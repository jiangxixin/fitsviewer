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
    if (!_hasImage || !asGl(_gl))
        return false;

    float gR = 1.0f, gG = 1.0f, gB = 1.0f;
    if (!asGl(_gl)->computeAutoWhiteBalanceGpu(gR, gG, gB))
        return false;

    _wb.r = gR;
    _wb.g = gG;
    _wb.b = gB;

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
