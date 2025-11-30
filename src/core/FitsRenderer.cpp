#include "kty/FitsRenderer.h"

#include "FitsImage.h"        // 你已有的 FITS 读入结构
#include "GlImageRenderer.h"  // 上面完整实现的渲染器
#include "Debayer.h"

#include <cmath>      // sqrt
#include <algorithm>
#include <iostream>

namespace kty {

static inline FitsImage*       asFits(void* p)       { return static_cast<FitsImage*>(p); }
static inline const FitsImage* asFits(const void* p) { return static_cast<const FitsImage*>(p); }
static inline GlImageRenderer*       asGl(void* p)   { return static_cast<GlImageRenderer*>(p); }
static inline const GlImageRenderer* asGl(const void* p) { return static_cast<const GlImageRenderer*>(p); }

FitsRenderer::FitsRenderer()
{
    _fits = new FitsImage();
    _gl   = new GlImageRenderer();

    _stretch.autoStretch = true;
    _stretch.blackClip   = 0.1f;
    _stretch.whiteClip   = 0.1f;
    _stretch.strength    = 5.0f;
    _stretch.mode        = StretchMode::Asinh;

    _wb.r = _wb.g = _wb.b = 1.0f;

    _view.scale = 1.0f;
    _view.panX  = 0.0f;
    _view.panY  = 0.0f;
}

FitsRenderer::~FitsRenderer()
{
    shutdown();
    delete asGl(_gl);
    delete asFits(_fits);
    _gl   = nullptr;
    _fits = nullptr;
}

bool FitsRenderer::init()
{
    if (!asGl(_gl)) return false;
    return asGl(_gl)->init();
}

void FitsRenderer::shutdown()
{
    if (asGl(_gl))
        asGl(_gl)->shutdown();
    _hasImage = false;
}

bool FitsRenderer::loadFits(const std::string& path, BayerPattern bayerHint)
{
    if (!asFits(_fits) || !asGl(_gl))
        return false;

    FitsImage img;
    // 注意这里的 BayerPattern 类型要和 FitsImage.h 中的保持一致
    if (!load_fits(path, img, static_cast<::BayerPattern>(bayerHint)))
    {
        std::cerr << "Failed to load FITS: " << path << "\n";
        _hasImage = false;
        return false;
    }

    *asFits(_fits) = img;
    _imgWidth  = asFits(_fits)->width;
    _imgHeight = asFits(_fits)->height;
    _hasImage  = !asFits(_fits)->raw.empty();
    _bayer     = bayerHint;

    if (!_hasImage)
        return false;

    _view.scale = 1.0f;
    _view.panX  = 0.0f;
    _view.panY  = 0.0f;

    std::vector<float> bayerNorm(asFits(_fits)->raw.size());

    if (!asFits(_fits)->raw.empty())
    {
        auto [itMin, itMax] = std::minmax_element(asFits(_fits)->raw.begin(),
                                                  asFits(_fits)->raw.end());
        double mn = *itMin;
        double mx = *itMax;
        if (mn == mx)
        {
            mn = 0.0;
            mx = 1.0;
        }
        double range = mx - mn;

        for (size_t i = 0; i < asFits(_fits)->raw.size(); ++i)
        {
            float v = static_cast<float>((asFits(_fits)->raw[i] - mn) / range);
            bayerNorm[i] = std::clamp(v, 0.0f, 1.0f);
        }
    }

    asGl(_gl)->uploadBaseTexture(bayerNorm, asFits(_fits)->width, asFits(_fits)->height);
    asGl(_gl)->setBayerPattern(static_cast<int>(_bayer));
    asGl(_gl)->setWhiteBalance(_wb.r, _wb.g, _wb.b);
    asGl(_gl)->setStretchMode(static_cast<int>(_stretch.mode));

    recomputeAutoStretch();

    return true;
}

void FitsRenderer::setStretchParams(const StretchParams& p)
{
    _stretch = p;
    asGl(_gl)->setStretchMode(static_cast<int>(_stretch.mode));
    asGl(_gl)->setAutoParams(_stretch.autoStretch, _autoLow, _autoHigh, _stretch.strength);
}

void FitsRenderer::setWhiteBalance(const WhiteBalance& wb)
{
    _wb = wb;
    asGl(_gl)->setWhiteBalance(_wb.r, _wb.g, _wb.b);
}

bool FitsRenderer::computeAutoWhiteBalance()
{
    if (!_hasImage || !asFits(_fits))
        return false;

    const FitsImage* fi = asFits(_fits);
    int W = fi->width;
    int H = fi->height;
    if (W <= 0 || H <= 0)
        return false;

    // ==== 1. 准备一个可用于统计的 RGB 图像 ====
    // 优先使用已有的 fi->rgb（如果你已经在别处做过 CPU 去拜耳）
    FitsImage tempRgb;
    const FitsImage* srcRgb = nullptr;

    if (!fi->rgb.empty() && fi->channels == 3)
    {
        // 假设 fi->rgb 已经是 [0,1] 的 RGB
        srcRgb = fi;
    }
    else
    {
        // 没有现成 rgb，就用已有的 debayer_bilinear 做一次 CPU 去拜耳（只为统计）
        // 注意：这里假设你有 debayer_bilinear(const FitsImage&, FitsImage&)，否则可以根据你现在的 Debayer 接口替换
        if (!debayer_bilinear(*fi, tempRgb))
        {
            // 去拜耳失败了，就没法做自动白平衡
            return false;
        }
        srcRgb = &tempRgb;
    }

    if (!srcRgb || srcRgb->rgb.empty() || srcRgb->channels != 3)
        return false;

    const std::vector<float>& rgb = srcRgb->rgb;
    int width  = srcRgb->width;
    int height = srcRgb->height;

    // ==== 2. 再做一次简单的归一化（防御性，避免某些路径下 rgb 不在 0~1）====
    float minVal = 1e9f;
    float maxVal = -1e9f;
    for (size_t i = 0; i < rgb.size(); ++i)
    {
        float v = rgb[i];
        if (v < minVal) minVal = v;
        if (v > maxVal) maxVal = v;
    }
    if (maxVal <= minVal)
    {
        minVal = 0.0f;
        maxVal = 1.0f;
    }
    float invRange = 1.0f / (maxVal - minVal);

    auto norm = [&](float v) -> float {
        float t = (v - minVal) * invRange;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        return t;
    };

    // ==== 3. 灰度世界算法（Grey-World） + 亮度过滤 ====
    const size_t targetSamples = 200000;
    size_t total = (size_t)width * (size_t)height;
    int step = 1;
    if (total > targetSamples)
    {
        step = (int)std::sqrt((double)total / (double)targetSamples);
        if (step < 1) step = 1;
    }

    double sumR = 0.0;
    double sumG = 0.0;
    double sumB = 0.0;
    size_t count = 0;

    for (int y = 0; y < height; y += step)
    {
        for (int x = 0; x < width; x += step)
        {
            size_t idx = ((size_t)y * width + x) * 3;
            float r = norm(rgb[idx + 0]);
            float g = norm(rgb[idx + 1]);
            float b = norm(rgb[idx + 2]);

            // 计算亮度，用于剔除太暗/太亮的像素
            float l = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            if (l < 0.10f || l > 0.90f)
                continue;

            sumR += r;
            sumG += g;
            sumB += b;
            ++count;
        }
    }

    if (count == 0)
        return false;

    double meanR = sumR / (double)count;
    double meanG = sumG / (double)count;
    double meanB = sumB / (double)count;

    if (meanR <= 0.0 || meanG <= 0.0 || meanB <= 0.0)
        return false;

    double meanGrey = (meanR + meanG + meanB) / 3.0;

    float gR = (float)(meanGrey / meanR);
    float gG = (float)(meanGrey / meanG);
    float gB = (float)(meanGrey / meanB);

    // 防止增益过分极端
    auto clampGain = [](float g) {
        if (g < 0.25f) g = 0.25f;
        if (g > 4.0f)  g = 4.0f;
        return g;
    };

    gR = clampGain(gR);
    gG = clampGain(gG);
    gB = clampGain(gB);

    // ==== 4. 更新内部白平衡参数，并同步到 GL ====
    _wb.r = gR;
    _wb.g = gG;
    _wb.b = gB;

    asGl(_gl)->setWhiteBalance(_wb.r, _wb.g, _wb.b);

    return true;
}

void FitsRenderer::setViewParams(const ViewParams& vp)
{
    _view = vp;
    asGl(_gl)->setViewParams(_view.scale, _view.panX, _view.panY);
}

void FitsRenderer::setBayerPattern(BayerPattern bayer)
{
    _bayer = bayer;
    asGl(_gl)->setBayerPattern(static_cast<int>(_bayer));
}

bool FitsRenderer::recomputeAutoStretch()
{
    if (!_hasImage || !asGl(_gl))
        return false;

    float low = 0.0f, high = 1.0f;
    if (asGl(_gl)->computeAutoParamsGpu(_stretch.autoStretch,
                                        _stretch.blackClip,
                                        _stretch.whiteClip,
                                        low, high))
    {
        _autoLow  = low;
        _autoHigh = high;
        asGl(_gl)->setAutoParams(_stretch.autoStretch, _autoLow, _autoHigh, _stretch.strength);
        return true;
    }
    else
    {
        _autoLow  = 0.0f;
        _autoHigh = 1.0f;
        asGl(_gl)->setAutoParams(_stretch.autoStretch, _autoLow, _autoHigh, _stretch.strength);
        return false;
    }
}

bool FitsRenderer::getLumaHistogram(std::vector<float>& outHist) const
{
    if (!asGl(_gl))
        return false;
    return asGl(_gl)->getLuminanceHistogram(outHist);
}

void FitsRenderer::render(int viewportWidth, int viewportHeight)
{
    if (!_hasImage || !asGl(_gl))
        return;
    asGl(_gl)->setViewParams(_view.scale, _view.panX, _view.panY);
    asGl(_gl)->render(viewportWidth, viewportHeight);
}

bool FitsRenderer::renderToImage(std::vector<unsigned char>& outRGB,
                                 int& outWidth, int& outHeight) const
{
    if (!_hasImage || !asGl(_gl))
        return false;

    outWidth  = _imgWidth;
    outHeight = _imgHeight;
    return asGl(_gl)->renderToImage(outWidth, outHeight, outRGB);
}

bool FitsRenderer::renderPreview(int width, int height)
{
    if (!_hasImage || !asGl(_gl))
        return false;
    asGl(_gl)->setViewParams(_view.scale, _view.panX, _view.panY);
    return asGl(_gl)->renderPreview(width, height);
}

unsigned int FitsRenderer::previewTextureId() const
{
    if (!asGl(_gl))
        return 0;
    return asGl(_gl)->previewTextureId();
}

} // namespace kty
