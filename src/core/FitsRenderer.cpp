#include "kty/FitsRenderer.h"

#include "FitsImage.h"        // 你已有的 FITS 读入结构
#include "GlImageRenderer.h"  // 上面完整实现的渲染器
#include "Debayer.h"

#include <cmath>      // sqrt
#include <algorithm>
#include <iostream>
#include <array>

namespace kty {

static inline FitsImage*       asFits(void* p)       { return static_cast<FitsImage*>(p); }
static inline const FitsImage* asFits(const void* p) { return static_cast<const FitsImage*>(p); }
static inline GlImageRenderer*       asGl(void* p)   { return static_cast<GlImageRenderer*>(p); }
static inline const GlImageRenderer* asGl(const void* p) { return static_cast<const GlImageRenderer*>(p); }

static bool upload_monochrome_image(FitsImage* fits,
                                    GlImageRenderer* gl,
                                    int& imgWidth,
                                    int& imgHeight,
                                    bool& hasImage,
                                    kty::BayerPattern& bayer,
                                    const kty::WhiteBalance& wb,
                                    const kty::StretchParams& stretch,
                                    const std::vector<double>& raw,
                                    int width,
                                    int height,
                                    kty::BayerPattern bayerHint)
{
    if (!fits || !gl || width <= 0 || height <= 0)
        return false;

    const size_t expectedPixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (raw.size() != expectedPixels)
        return false;

    fits->width = width;
    fits->height = height;
    fits->channels = 1;
    fits->bayer = static_cast<::BayerPattern>(bayerHint);
    fits->raw = raw;
    fits->rgb.clear();

    imgWidth = width;
    imgHeight = height;
    hasImage = !raw.empty();
    bayer = bayerHint;

    std::vector<float> bayerNorm(raw.size());
    if (!raw.empty())
    {
        auto [itMin, itMax] = std::minmax_element(raw.begin(), raw.end());
        double mn = *itMin;
        double mx = *itMax;
        if (mn == mx)
        {
            mn = 0.0;
            mx = 1.0;
        }
        const double range = mx - mn;

        for (size_t i = 0; i < raw.size(); ++i)
        {
            const float v = static_cast<float>((raw[i] - mn) / range);
            bayerNorm[i] = std::clamp(v, 0.0f, 1.0f);
        }
    }

    gl->uploadBaseTexture(bayerNorm, width, height);
    gl->setBayerPattern(static_cast<int>(bayer));
    gl->setWhiteBalance(wb.r, wb.g, wb.b);
    gl->setStretchMode(static_cast<int>(stretch.mode));
    return true;
}

static inline float clamp01f(float v)
{
    return std::clamp(v, 0.0f, 1.0f);
}

static inline bool nearly_equal(float a, float b, float epsilon = 1.0e-6f)
{
    return std::abs(a - b) <= epsilon;
}

static bool same_white_balance(const WhiteBalance& lhs, const WhiteBalance& rhs)
{
    return nearly_equal(lhs.r, rhs.r) &&
           nearly_equal(lhs.g, rhs.g) &&
           nearly_equal(lhs.b, rhs.b);
}

static bool same_view_params(const ViewParams& lhs, const ViewParams& rhs)
{
    return nearly_equal(lhs.scale, rhs.scale) &&
           nearly_equal(lhs.panX, rhs.panX) &&
           nearly_equal(lhs.panY, rhs.panY);
}

static bool same_stretch_params(const StretchParams& lhs, const StretchParams& rhs)
{
    return lhs.autoStretch == rhs.autoStretch &&
           nearly_equal(lhs.blackClip, rhs.blackClip) &&
           nearly_equal(lhs.whiteClip, rhs.whiteClip) &&
           nearly_equal(lhs.strength, rhs.strength) &&
           lhs.mode == rhs.mode;
}

struct FloatStats {
    float median = 0.0f;
    float sigma = 1.0f;
};

static float median_of_copy(std::vector<float> values)
{
    if (values.empty())
        return 0.0f;

    const size_t mid = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + mid, values.end());
    float med = values[mid];
    if ((values.size() & 1U) == 0)
    {
        const float lower = *std::max_element(values.begin(), values.begin() + mid);
        med = 0.5f * (med + lower);
    }
    return med;
}

static FloatStats compute_float_stats(const std::vector<float>& values)
{
    FloatStats stats;
    if (values.empty())
        return stats;

    stats.median = median_of_copy(values);

    std::vector<float> absDev(values.size());
    for (size_t i = 0; i < values.size(); ++i)
        absDev[i] = std::abs(values[i] - stats.median);

    stats.sigma = median_of_copy(absDev) * 1.4826f;
    if (stats.sigma <= 1.0e-6f)
    {
        float meanAbs = 0.0f;
        for (float v : values)
            meanAbs += std::abs(v - stats.median);
        meanAbs /= static_cast<float>(values.size());
        stats.sigma = std::max(meanAbs * 1.2533141f, 1.0e-6f);
    }
    return stats;
}

static void dilated_blur5(const std::vector<float>& src,
                          int width,
                          int height,
                          int step,
                          std::vector<float>& dst)
{
    static constexpr std::array<float, 5> kernel = {1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f,
                                                    4.0f / 16.0f, 1.0f / 16.0f};
    std::vector<float> tmp(static_cast<size_t>(width) * height, 0.0f);
    dst.assign(static_cast<size_t>(width) * height, 0.0f);

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            float sum = 0.0f;
            for (int k = -2; k <= 2; ++k)
            {
                const int sx = std::clamp(x + k * step, 0, width - 1);
                sum += src[static_cast<size_t>(y) * width + sx] * kernel[static_cast<size_t>(k + 2)];
            }
            tmp[static_cast<size_t>(y) * width + x] = sum;
        }
    }

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            float sum = 0.0f;
            for (int k = -2; k <= 2; ++k)
            {
                const int sy = std::clamp(y + k * step, 0, height - 1);
                sum += tmp[static_cast<size_t>(sy) * width + x] * kernel[static_cast<size_t>(k + 2)];
            }
            dst[static_cast<size_t>(y) * width + x] = sum;
        }
    }
}

static std::vector<float> compute_luminance(const std::vector<float>& rgb, int width, int height)
{
    std::vector<float> lum(static_cast<size_t>(width) * height, 0.0f);
    for (size_t i = 0; i < lum.size(); ++i)
    {
        const size_t base = i * 3;
        lum[i] = 0.2126f * rgb[base + 0] + 0.7152f * rgb[base + 1] + 0.0722f * rgb[base + 2];
    }
    return lum;
}

static void apply_white_balance(std::vector<float>& rgb, const WhiteBalance& wb)
{
    for (size_t i = 0; i + 2 < rgb.size(); i += 3)
    {
        rgb[i + 0] = std::max(0.0f, rgb[i + 0] * wb.r);
        rgb[i + 1] = std::max(0.0f, rgb[i + 1] * wb.g);
        rgb[i + 2] = std::max(0.0f, rgb[i + 2] * wb.b);
    }
}

static void build_denoise_masks(const std::vector<float>& lum,
                                int width,
                                int height,
                                float backgroundSigma,
                                std::vector<float>& backgroundMask,
                                std::vector<float>& starMask,
                                FloatStats& stats)
{
    stats = compute_float_stats(lum);

    std::vector<float> blur1;
    dilated_blur5(lum, width, height, 1, blur1);

    backgroundMask.assign(lum.size(), 0.0f);
    starMask.assign(lum.size(), 0.0f);

    const float sigma = std::max(stats.sigma, 1.0e-5f);
    const float bgThreshold = stats.median + std::max(0.5f, backgroundSigma) * sigma;
    const float starThreshold = bgThreshold + 2.0f * sigma;

    for (size_t i = 0; i < lum.size(); ++i)
    {
        const float detail = std::max(0.0f, lum[i] - blur1[i]);
        const float structure = std::abs(lum[i] - blur1[i]);
        const float starCore = clamp01f((lum[i] - starThreshold) / std::max(3.0f * sigma, 1.0e-5f));
        const float edgeProtect = clamp01f((structure - 0.55f * sigma) / std::max(3.0f * sigma, 1.0e-5f));
        const float detailProtect = clamp01f((detail - 0.35f * sigma) / std::max(2.8f * sigma, 1.0e-5f));
        starMask[i] = clamp01f(std::max(starCore, std::max(edgeProtect * 0.7f, detailProtect)));

        float bg = clamp01f((bgThreshold - lum[i]) / std::max(bgThreshold - stats.median, 1.0e-5f));
        bg *= bg;
        backgroundMask[i] = bg * (1.0f - 0.92f * starMask[i]);
    }

    std::vector<float> starBlur;
    std::vector<float> starHalo;
    dilated_blur5(starMask, width, height, 1, starBlur);
    dilated_blur5(starBlur, width, height, 2, starHalo);
    for (size_t i = 0; i < starMask.size(); ++i)
    {
        const float haloProtect = std::max(0.85f * starBlur[i], 0.70f * starHalo[i]);
        starMask[i] = clamp01f(std::max(starMask[i], haloProtect));
        const float bgSuppress = 1.0f - 0.92f * starMask[i];
        backgroundMask[i] *= bgSuppress * bgSuppress;
    }
}

static void multiscale_background_denoise(std::vector<float>& lum,
                                          int width,
                                          int height,
                                          float strength,
                                          int iterations,
                                          const std::vector<float>& backgroundMask,
                                          const std::vector<float>& starMask,
                                          float sigma)
{
    const int levels = std::clamp(iterations + 2, 3, 6);
    std::vector<std::vector<float>> details;
    details.reserve(static_cast<size_t>(levels));

    std::vector<float> current = lum;
    std::vector<float> low;
    for (int level = 0; level < levels; ++level)
    {
        dilated_blur5(current, width, height, 1 << level, low);
        std::vector<float> detail(current.size(), 0.0f);
        for (size_t i = 0; i < detail.size(); ++i)
            detail[i] = current[i] - low[i];
        details.push_back(std::move(detail));
        current.swap(low);
    }

    std::vector<float> recon = current;
    for (int level = levels - 1; level >= 0; --level)
    {
        const float baseThreshold =
            std::max(sigma * (0.90f + 0.35f * static_cast<float>(level)) * (0.8f + 2.4f * strength), 1.0e-5f);
        const auto& detail = details[static_cast<size_t>(level)];
        for (size_t i = 0; i < recon.size(); ++i)
        {
            const float localAmount = strength * backgroundMask[i] * (1.0f - 0.98f * starMask[i]);
            if (localAmount <= 1.0e-4f)
            {
                recon[i] += detail[i];
                continue;
            }

            const float softThreshold = baseThreshold * localAmount;
            const float absDetail = std::abs(detail[i]);
            const float shrink = (absDetail > softThreshold)
                               ? ((absDetail - softThreshold) / std::max(absDetail, 1.0e-6f))
                               : 0.0f;
            recon[i] += detail[i] * shrink;
        }
    }

    std::vector<float> coarseBlur;
    dilated_blur5(recon, width, height, 1 << std::max(levels - 1, 0), coarseBlur);
    for (size_t i = 0; i < recon.size(); ++i)
    {
        const float coarseBlend = 0.08f * strength * backgroundMask[i] * (1.0f - 0.97f * starMask[i]);
        recon[i] = recon[i] * (1.0f - coarseBlend) + coarseBlur[i] * coarseBlend;
        lum[i] = std::max(0.0f, recon[i]);
    }
}

static void haar1d_inplace(std::vector<float>& data)
{
    std::vector<float> temp(data.size(), 0.0f);
    size_t length = data.size();
    while (length > 1)
    {
        const size_t half = length / 2;
        for (size_t i = 0; i < half; ++i)
        {
            const float a = data[2 * i];
            const float b = data[2 * i + 1];
            temp[i] = 0.70710678f * (a + b);
            temp[half + i] = 0.70710678f * (a - b);
        }
        for (size_t i = 0; i < length; ++i)
            data[i] = temp[i];
        length = half;
    }
}

static void inverse_haar1d_inplace(std::vector<float>& data)
{
    std::vector<float> temp(data.size(), 0.0f);
    for (size_t length = 1; length < data.size(); length *= 2)
    {
        for (size_t i = 0; i < length; ++i)
        {
            const float a = data[i];
            const float d = data[length + i];
            temp[2 * i] = 0.70710678f * (a + d);
            temp[2 * i + 1] = 0.70710678f * (a - d);
        }
        for (size_t i = 0; i < 2 * length; ++i)
            data[i] = temp[i];
    }
}

static void haar2d4_inplace(std::array<float, 16>& patch)
{
    for (int y = 0; y < 4; ++y)
    {
        std::vector<float> row(4, 0.0f);
        for (int x = 0; x < 4; ++x)
            row[static_cast<size_t>(x)] = patch[static_cast<size_t>(y * 4 + x)];
        haar1d_inplace(row);
        for (int x = 0; x < 4; ++x)
            patch[static_cast<size_t>(y * 4 + x)] = row[static_cast<size_t>(x)];
    }
    for (int x = 0; x < 4; ++x)
    {
        std::vector<float> col(4, 0.0f);
        for (int y = 0; y < 4; ++y)
            col[static_cast<size_t>(y)] = patch[static_cast<size_t>(y * 4 + x)];
        haar1d_inplace(col);
        for (int y = 0; y < 4; ++y)
            patch[static_cast<size_t>(y * 4 + x)] = col[static_cast<size_t>(y)];
    }
}

static void inverse_haar2d4_inplace(std::array<float, 16>& patch)
{
    for (int x = 0; x < 4; ++x)
    {
        std::vector<float> col(4, 0.0f);
        for (int y = 0; y < 4; ++y)
            col[static_cast<size_t>(y)] = patch[static_cast<size_t>(y * 4 + x)];
        inverse_haar1d_inplace(col);
        for (int y = 0; y < 4; ++y)
            patch[static_cast<size_t>(y * 4 + x)] = col[static_cast<size_t>(y)];
    }
    for (int y = 0; y < 4; ++y)
    {
        std::vector<float> row(4, 0.0f);
        for (int x = 0; x < 4; ++x)
            row[static_cast<size_t>(x)] = patch[static_cast<size_t>(y * 4 + x)];
        inverse_haar1d_inplace(row);
        for (int x = 0; x < 4; ++x)
            patch[static_cast<size_t>(y * 4 + x)] = row[static_cast<size_t>(x)];
    }
}

static std::array<float, 16> extract_patch4(const std::vector<float>& img, int width, int height, int x, int y)
{
    std::array<float, 16> patch{};
    for (int py = 0; py < 4; ++py)
    {
        for (int px = 0; px < 4; ++px)
        {
            const int sx = std::clamp(x + px, 0, width - 1);
            const int sy = std::clamp(y + py, 0, height - 1);
            patch[static_cast<size_t>(py * 4 + px)] = img[static_cast<size_t>(sy) * width + sx];
        }
    }
    return patch;
}

static float patch_distance4(const std::vector<float>& img, int width, int height, int x0, int y0, int x1, int y1)
{
    float dist = 0.0f;
    for (int py = 0; py < 4; ++py)
    {
        for (int px = 0; px < 4; ++px)
        {
            const int ax = std::clamp(x0 + px, 0, width - 1);
            const int ay = std::clamp(y0 + py, 0, height - 1);
            const int bx = std::clamp(x1 + px, 0, width - 1);
            const int by = std::clamp(y1 + py, 0, height - 1);
            const float diff = img[static_cast<size_t>(ay) * width + ax] -
                               img[static_cast<size_t>(by) * width + bx];
            dist += diff * diff;
        }
    }
    return dist / 16.0f;
}

static void aggregate_patch4(const std::array<float, 16>& patch,
                             int width,
                             int height,
                             int x,
                             int y,
                             float weight,
                             const std::vector<float>& backgroundMask,
                             const std::vector<float>& starMask,
                             std::vector<float>& accum,
                             std::vector<float>& weights)
{
    for (int py = 0; py < 4; ++py)
    {
        for (int px = 0; px < 4; ++px)
        {
            const int sx = std::clamp(x + px, 0, width - 1);
            const int sy = std::clamp(y + py, 0, height - 1);
            const size_t idx = static_cast<size_t>(sy) * width + sx;
            const float localWeight = weight * (0.12f + 0.88f * backgroundMask[idx]) * (1.0f - 0.96f * starMask[idx]);
            accum[idx] += patch[static_cast<size_t>(py * 4 + px)] * localWeight;
            weights[idx] += localWeight;
        }
    }
}

static void bm3d_style_refine(std::vector<float>& lum,
                              int width,
                              int height,
                              float strength,
                              float sigma,
                              const std::vector<float>& backgroundMask,
                              const std::vector<float>& starMask)
{
    struct Candidate {
        int x = 0;
        int y = 0;
        float dist = 0.0f;
    };

    const int patchSize = 4;
    const int searchRadius = 8;
    const int refStep = 2;
    const int searchStep = 2;
    const int groupSize = 8;
    const float sigmaSafe = std::max(sigma, 1.0e-5f);
    const float matchLimit = std::max(14.0f * sigmaSafe * sigmaSafe * (0.8f + 1.5f * strength), 1.0e-6f);
    const float coeffBase = std::max(2.4f * sigmaSafe * (0.75f + 1.35f * strength), 1.0e-6f);

    std::vector<float> accum(lum.size(), 0.0f);
    std::vector<float> weightMap(lum.size(), 0.0f);

    for (int y = 0; y < height; y += refStep)
    {
        for (int x = 0; x < width; x += refStep)
        {
            const size_t refIdx = static_cast<size_t>(y) * width + x;
            const float refBg = backgroundMask[refIdx];
            const float refStar = starMask[refIdx];
            if (refBg < 0.24f || refStar > 0.30f)
                continue;

            std::vector<Candidate> candidates;
            candidates.reserve(64);
            candidates.push_back({x, y, 0.0f});

            for (int sy = std::max(0, y - searchRadius); sy <= std::min(height - 1, y + searchRadius); sy += searchStep)
            {
                for (int sx = std::max(0, x - searchRadius); sx <= std::min(width - 1, x + searchRadius); sx += searchStep)
                {
                    if (sx == x && sy == y)
                        continue;
                    const size_t cidx = static_cast<size_t>(sy) * width + sx;
                    if (backgroundMask[cidx] < 0.20f || starMask[cidx] > 0.35f)
                        continue;

                    const float dist = patch_distance4(lum, width, height, x, y, sx, sy);
                    if (dist <= matchLimit)
                        candidates.push_back({sx, sy, dist});
                }
            }

            std::sort(candidates.begin(), candidates.end(),
                      [](const Candidate& a, const Candidate& b) { return a.dist < b.dist; });
            if (static_cast<int>(candidates.size()) > groupSize)
                candidates.resize(static_cast<size_t>(groupSize));

            while (static_cast<int>(candidates.size()) < groupSize)
                candidates.push_back(candidates.front());

            std::array<std::array<float, 16>, groupSize> coeffs{};
            std::array<int, groupSize> patchX{};
            std::array<int, groupSize> patchY{};
            for (int i = 0; i < groupSize; ++i)
            {
                patchX[static_cast<size_t>(i)] = candidates[static_cast<size_t>(i)].x;
                patchY[static_cast<size_t>(i)] = candidates[static_cast<size_t>(i)].y;
                auto patch = extract_patch4(lum, width, height, patchX[static_cast<size_t>(i)], patchY[static_cast<size_t>(i)]);
                haar2d4_inplace(patch);
                coeffs[static_cast<size_t>(i)] = patch;
            }

            int nonZeroCount = 0;
            const float localStrength = strength * (0.22f + 0.52f * refBg) * (1.0f - 0.97f * refStar);
            for (int coeffIndex = 0; coeffIndex < patchSize * patchSize; ++coeffIndex)
            {
                std::vector<float> group(groupSize, 0.0f);
                for (int i = 0; i < groupSize; ++i)
                    group[static_cast<size_t>(i)] = coeffs[static_cast<size_t>(i)][static_cast<size_t>(coeffIndex)];

                haar1d_inplace(group);
                const float coeffScale = (coeffIndex == 0) ? 0.55f : (1.0f + 0.07f * static_cast<float>(coeffIndex));
                const float hardThreshold = coeffBase * coeffScale * std::max(localStrength, 0.15f);
                for (int i = 1; i < groupSize; ++i)
                {
                    if (std::abs(group[static_cast<size_t>(i)]) < hardThreshold)
                    {
                        group[static_cast<size_t>(i)] = 0.0f;
                    }
                    else
                    {
                        ++nonZeroCount;
                    }
                }
                inverse_haar1d_inplace(group);
                for (int i = 0; i < groupSize; ++i)
                    coeffs[static_cast<size_t>(i)][static_cast<size_t>(coeffIndex)] = group[static_cast<size_t>(i)];
            }

            const float collaborativeWeight =
                (0.12f + 0.32f * localStrength) / std::max(1.0f + 0.02f * static_cast<float>(nonZeroCount), 1.0f);
            for (int i = 0; i < groupSize; ++i)
            {
                auto filteredPatch = coeffs[static_cast<size_t>(i)];
                inverse_haar2d4_inplace(filteredPatch);
                aggregate_patch4(filteredPatch,
                                 width,
                                 height,
                                 patchX[static_cast<size_t>(i)],
                                 patchY[static_cast<size_t>(i)],
                                 collaborativeWeight,
                                 backgroundMask,
                                 starMask,
                                 accum,
                                 weightMap);
            }
        }
    }

    for (size_t i = 0; i < lum.size(); ++i)
    {
        if (weightMap[i] <= 1.0e-6f)
            continue;
        const float filtered = accum[i] / weightMap[i];
        const float blend = 0.07f * strength * backgroundMask[i] * (1.0f - 0.98f * starMask[i]);
        lum[i] = std::max(0.0f, lum[i] * (1.0f - blend) + filtered * blend);
    }
}

static void apply_luminance_ratio(std::vector<float>& rgb,
                                  const std::vector<float>& originalLum,
                                  const std::vector<float>& newLum,
                                  const std::vector<float>& starMask)
{
    for (size_t i = 0; i < originalLum.size(); ++i)
    {
        const float rawRatio = newLum[i] / std::max(originalLum[i], 1.0e-5f);
        const float protectedRatio = 1.0f + (rawRatio - 1.0f) * (1.0f - 0.97f * starMask[i]);
        const size_t base = i * 3;
        rgb[base + 0] = std::max(0.0f, rgb[base + 0] * protectedRatio);
        rgb[base + 1] = std::max(0.0f, rgb[base + 1] * protectedRatio);
        rgb[base + 2] = std::max(0.0f, rgb[base + 2] * protectedRatio);
    }
}

static void downsample_linear_rgb_2x(const std::vector<float>& src,
                                     int srcW,
                                     int srcH,
                                     std::vector<float>& dst,
                                     int& dstW,
                                     int& dstH)
{
    dstW = std::max(1, srcW / 2);
    dstH = std::max(1, srcH / 2);
    dst.assign(static_cast<size_t>(dstW) * dstH * 3, 0.0f);
    for (int y = 0; y < dstH; ++y)
    {
        for (int x = 0; x < dstW; ++x)
        {
            const int sx0 = std::min(srcW - 1, x * 2);
            const int sy0 = std::min(srcH - 1, y * 2);
            const int sx1 = std::min(srcW - 1, sx0 + 1);
            const int sy1 = std::min(srcH - 1, sy0 + 1);
            for (int c = 0; c < 3; ++c)
            {
                const float v00 = src[(static_cast<size_t>(sy0) * srcW + sx0) * 3 + c];
                const float v10 = src[(static_cast<size_t>(sy0) * srcW + sx1) * 3 + c];
                const float v01 = src[(static_cast<size_t>(sy1) * srcW + sx0) * 3 + c];
                const float v11 = src[(static_cast<size_t>(sy1) * srcW + sx1) * 3 + c];
                dst[(static_cast<size_t>(y) * dstW + x) * 3 + c] = 0.25f * (v00 + v10 + v01 + v11);
            }
        }
    }
}

static bool compute_histogram_from_linear_rgb(const std::vector<float>& linearRgb,
                                              int width,
                                              int height,
                                              const StretchParams& stretch,
                                              float autoLow,
                                              float autoHigh,
                                              std::vector<float>& outHist)
{
    if (width <= 0 || height <= 0 || linearRgb.empty())
        return false;

    constexpr int histBins = 64;
    std::vector<float> lum = compute_luminance(linearRgb, width, height);
    outHist.assign(histBins, 0.0f);

    const float range = std::max(autoHigh - autoLow, 1.0e-3f);
    const float s = std::max(stretch.strength, 1.0f);
    const float asinhDenom = std::max(std::asinh(s), 1.0e-6f);
    const float logDenom = std::max(std::log(1.0f + s), 1.0e-6f);

    for (float v : lum)
    {
        float y = v;
        if (stretch.autoStretch)
        {
            const float t = clamp01f((v - autoLow) / range);
            switch (stretch.mode)
            {
                case StretchMode::Linear:
                    y = t;
                    break;
                case StretchMode::Asinh:
                    y = clamp01f(std::asinh(s * t) / asinhDenom);
                    break;
                case StretchMode::Log:
                    y = clamp01f(std::log(1.0f + s * t) / logDenom);
                    break;
                case StretchMode::Sqrt:
                    y = std::sqrt(t);
                    break;
            }
        }
        else
        {
            y = clamp01f(v);
        }
        int bin = static_cast<int>(y * histBins);
        bin = std::clamp(bin, 0, histBins - 1);
        outHist[static_cast<size_t>(bin)] += 1.0f;
    }

    float maxCount = 0.0f;
    for (float c : outHist)
        maxCount = std::max(maxCount, c);
    if (maxCount > 0.0f)
    {
        for (float& c : outHist)
        {
            c /= maxCount;
            c = std::sqrt(c);
        }
    }
    return true;
}

static void apply_cpu_stretch(std::vector<float>& rgb,
                              const StretchParams& stretch,
                              float autoLow,
                              float autoHigh)
{
    const float range = std::max(autoHigh - autoLow, 1.0e-3f);
    const float s = std::max(stretch.strength, 1.0f);
    const float asinhDenom = std::max(std::asinh(s), 1.0e-6f);
    const float logDenom = std::max(std::log(1.0f + s), 1.0e-6f);

    for (float& channel : rgb)
    {
        float v = channel;
        if (stretch.autoStretch)
        {
            float t = clamp01f((v - autoLow) / range);
            switch (stretch.mode)
            {
                case StretchMode::Linear:
                    v = t;
                    break;
                case StretchMode::Asinh:
                    v = clamp01f(std::asinh(s * t) / asinhDenom);
                    break;
                case StretchMode::Log:
                    v = clamp01f(std::log(1.0f + s * t) / logDenom);
                    break;
                case StretchMode::Sqrt:
                    v = std::sqrt(t);
                    break;
            }
        }
        channel = clamp01f(v);
    }
}

static bool build_hq_linear_rgb(const FitsImage& source,
                                const WhiteBalance& wb,
                                int exportMode,
                                float strength,
                                float backgroundSigma,
                                int iterations,
                                std::vector<float>& outLinearRgb,
                                int& outWidth,
                                int& outHeight)
{
    FitsImage debayered;
    if (!debayer_bilinear(source, debayered))
        return false;

    outWidth = debayered.width;
    outHeight = debayered.height;

    outLinearRgb = debayered.rgb;
    apply_white_balance(outLinearRgb, wb);

    std::vector<float> lum = compute_luminance(outLinearRgb, outWidth, outHeight);
    const std::vector<float> originalLum = lum;

    std::vector<float> backgroundMask;
    std::vector<float> starMask;
    FloatStats stats;
    build_denoise_masks(lum, outWidth, outHeight, backgroundSigma, backgroundMask, starMask, stats);

    if (exportMode >= 1 && strength > 1.0e-4f)
    {
        multiscale_background_denoise(lum,
                                      outWidth,
                                      outHeight,
                                      strength,
                                      iterations,
                                      backgroundMask,
                                      starMask,
                                      std::max(stats.sigma, 1.0e-5f));
    }

    if (exportMode >= 2 && strength > 1.0e-4f)
    {
        bm3d_style_refine(lum,
                          outWidth,
                          outHeight,
                          strength,
                          std::max(stats.sigma, 1.0e-5f),
                          backgroundMask,
                          starMask);
    }

    apply_luminance_ratio(outLinearRgb, originalLum, lum, starMask);
    return true;
}

static std::array<float, 3> sample_bilinear_rgb(const std::vector<float>& rgb,
                                                int width,
                                                int height,
                                                float u,
                                                float v)
{
    const float x = std::clamp(u, 0.0f, 1.0f) * static_cast<float>(width - 1);
    const float y = std::clamp(v, 0.0f, 1.0f) * static_cast<float>(height - 1);
    const int x0 = std::clamp(static_cast<int>(std::floor(x)), 0, width - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(y)), 0, height - 1);
    const int x1 = std::clamp(x0 + 1, 0, width - 1);
    const int y1 = std::clamp(y0 + 1, 0, height - 1);
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);

    std::array<float, 3> color = {0.0f, 0.0f, 0.0f};
    for (int c = 0; c < 3; ++c)
    {
        const float c00 = rgb[(static_cast<size_t>(y0) * width + x0) * 3 + c];
        const float c10 = rgb[(static_cast<size_t>(y0) * width + x1) * 3 + c];
        const float c01 = rgb[(static_cast<size_t>(y1) * width + x0) * 3 + c];
        const float c11 = rgb[(static_cast<size_t>(y1) * width + x1) * 3 + c];
        const float top = c00 * (1.0f - tx) + c10 * tx;
        const float bottom = c01 * (1.0f - tx) + c11 * tx;
        color[static_cast<size_t>(c)] = top * (1.0f - ty) + bottom * ty;
    }
    return color;
}

static bool render_rgb_view_to_u8(const std::vector<float>& linearRgb,
                                  int sourceWidth,
                                  int sourceHeight,
                                  const StretchParams& stretch,
                                  float autoLow,
                                  float autoHigh,
                                  const ViewParams& view,
                                  int viewportWidth,
                                  int viewportHeight,
                                  std::vector<unsigned char>& outRgb)
{
    if (sourceWidth <= 0 || sourceHeight <= 0 || viewportWidth <= 0 || viewportHeight <= 0)
        return false;

    const float texAspect = static_cast<float>(sourceWidth) / static_cast<float>(sourceHeight);
    const float screenAspect = static_cast<float>(viewportWidth) / static_cast<float>(viewportHeight);
    const float range = std::max(autoHigh - autoLow, 1.0e-3f);
    const float s = std::max(stretch.strength, 1.0f);
    const float asinhDenom = std::max(std::asinh(s), 1.0e-6f);
    const float logDenom = std::max(std::log(1.0f + s), 1.0e-6f);

    outRgb.assign(static_cast<size_t>(viewportWidth) * viewportHeight * 3, 0);
    for (int y = 0; y < viewportHeight; ++y)
    {
        for (int x = 0; x < viewportWidth; ++x)
        {
            float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(viewportWidth);
            float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(viewportHeight);

            if (screenAspect > texAspect)
            {
                const float scale = texAspect / screenAspect;
                const float mappedX = (u - 0.5f) * scale + 0.5f;
                if (mappedX < 0.0f || mappedX > 1.0f)
                    continue;
                u = mappedX;
            }
            else
            {
                const float scale = screenAspect / texAspect;
                const float mappedY = (v - 0.5f) * scale + 0.5f;
                if (mappedY < 0.0f || mappedY > 1.0f)
                    continue;
                v = mappedY;
            }

            u = ((u - 0.5f) / std::max(view.scale, 0.1f)) + 0.5f + view.panX;
            v = ((v - 0.5f) / std::max(view.scale, 0.1f)) + 0.5f + view.panY;
            if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
                continue;

            auto color = sample_bilinear_rgb(linearRgb, sourceWidth, sourceHeight, u, v);
            for (float& channel : color)
            {
                float out = channel;
                if (stretch.autoStretch)
                {
                    const float t = clamp01f((channel - autoLow) / range);
                    switch (stretch.mode)
                    {
                        case StretchMode::Linear:
                            out = t;
                            break;
                        case StretchMode::Asinh:
                            out = clamp01f(std::asinh(s * t) / asinhDenom);
                            break;
                        case StretchMode::Log:
                            out = clamp01f(std::log(1.0f + s * t) / logDenom);
                            break;
                        case StretchMode::Sqrt:
                            out = std::sqrt(t);
                            break;
                    }
                }
                channel = clamp01f(out);
            }

            const size_t outRow = static_cast<size_t>(viewportHeight - 1 - y);
            const size_t base = (outRow * viewportWidth + static_cast<size_t>(x)) * 3;
            outRgb[base + 0] = static_cast<unsigned char>(std::lround(color[0] * 255.0f));
            outRgb[base + 1] = static_cast<unsigned char>(std::lround(color[1] * 255.0f));
            outRgb[base + 2] = static_cast<unsigned char>(std::lround(color[2] * 255.0f));
        }
    }
    return true;
}

static bool render_hq_export(const std::vector<float>& linearRgb,
                             int width,
                             int height,
                             const StretchParams& stretch,
                             float autoLow,
                             float autoHigh,
                             std::vector<unsigned char>& outRGB)
{
    std::vector<float> stretched = linearRgb;
    apply_cpu_stretch(stretched, stretch, autoLow, autoHigh);

    outRGB.resize(static_cast<size_t>(width) * height * 3);
    for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i)
    {
        const size_t base = i * 3;
        outRGB[base + 0] = static_cast<unsigned char>(std::lround(clamp01f(stretched[base + 0]) * 255.0f));
        outRGB[base + 1] = static_cast<unsigned char>(std::lround(clamp01f(stretched[base + 1]) * 255.0f));
        outRGB[base + 2] = static_cast<unsigned char>(std::lround(clamp01f(stretched[base + 2]) * 255.0f));
    }

    return true;
}

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

void FitsRenderer::invalidateHqCaches()
{
    _hqLinearCacheValid = false;
    _hqLinearCacheW = 0;
    _hqLinearCacheH = 0;
    _hqLinearRgbCache.clear();
    _hqLinearLowResCacheValid = false;
    _hqLinearLowResCacheW = 0;
    _hqLinearLowResCacheH = 0;
    _hqLinearLowResRgbCache.clear();
    _hqPreviewCacheValid = false;
    _hqPreviewCacheW = 0;
    _hqPreviewCacheH = 0;
    _hqPreviewNeedsHighRes = false;
}

bool FitsRenderer::ensureHqLinearCache() const
{
    if (_hqLinearCacheValid)
        return true;
    if (!asFits(_fits))
        return false;

    std::vector<float> linearRgb;
    int width = 0;
    int height = 0;
    if (!build_hq_linear_rgb(*asFits(_fits),
                             _wb,
                             _exportDenoiseMode,
                             _exportDenoiseStrength,
                             _exportBackgroundSigma,
                             _exportDenoiseIterations,
                             linearRgb,
                             width,
                             height))
    {
        return false;
    }

    _hqLinearRgbCache = std::move(linearRgb);
    _hqLinearCacheW = width;
    _hqLinearCacheH = height;
    _hqLinearCacheValid = true;
    if (_hqLinearCacheW > 1024 || _hqLinearCacheH > 1024)
    {
        downsample_linear_rgb_2x(_hqLinearRgbCache,
                                 _hqLinearCacheW,
                                 _hqLinearCacheH,
                                 _hqLinearLowResRgbCache,
                                 _hqLinearLowResCacheW,
                                 _hqLinearLowResCacheH);
        _hqLinearLowResCacheValid = true;
    }
    else
    {
        _hqLinearLowResCacheValid = false;
        _hqLinearLowResRgbCache.clear();
        _hqLinearLowResCacheW = 0;
        _hqLinearLowResCacheH = 0;
    }
    return true;
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
    invalidateHqCaches();
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

    const size_t expectedPixels = static_cast<size_t>(img.width) * static_cast<size_t>(img.height);
    if (img.raw.empty() || img.channels != 1 || img.raw.size() != expectedPixels)
    {
        if (img.channels != 1)
            std::cerr << "Unsupported FITS channels for renderer: " << img.channels << "\n";
        if (img.raw.size() != expectedPixels)
            std::cerr << "Unsupported FITS buffer size for renderer: got " << img.raw.size()
                      << ", expected " << expectedPixels << "\n";
        _hasImage = false;
        return false;
    }

    _view.scale = 1.0f;
    _view.panX  = 0.0f;
    _view.panY  = 0.0f;

    if (!upload_monochrome_image(asFits(_fits), asGl(_gl),
                                 _imgWidth, _imgHeight, _hasImage, _bayer,
                                 _wb, _stretch, img.raw, img.width, img.height, bayerHint))
    {
        _hasImage = false;
        return false;
    }

    invalidateHqCaches();
    recomputeAutoStretch();

    return true;
}

bool FitsRenderer::loadMonochromeImage(const std::vector<double>& raw,
                                       int width,
                                       int height,
                                       BayerPattern bayerHint)
{
    if (!asFits(_fits) || !asGl(_gl))
        return false;

    _view.scale = 1.0f;
    _view.panX  = 0.0f;
    _view.panY  = 0.0f;

    if (!upload_monochrome_image(asFits(_fits), asGl(_gl),
                                 _imgWidth, _imgHeight, _hasImage, _bayer,
                                 _wb, _stretch, raw, width, height, bayerHint))
    {
        _hasImage = false;
        return false;
    }

    invalidateHqCaches();
    recomputeAutoStretch();
    return true;
}

void FitsRenderer::setStretchParams(const StretchParams& p)
{
    if (same_stretch_params(_stretch, p))
        return;
    _stretch = p;
    _hqPreviewCacheValid = false;
    asGl(_gl)->setStretchMode(static_cast<int>(_stretch.mode));
    asGl(_gl)->setAutoParams(_stretch.autoStretch, _autoLow, _autoHigh, _stretch.strength);
}

void FitsRenderer::setWhiteBalance(const WhiteBalance& wb)
{
    if (same_white_balance(_wb, wb))
        return;
    _wb = wb;
    invalidateHqCaches();
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
    invalidateHqCaches();

    return true;
}

bool FitsRenderer::computeBackgroundNeutralization()
{
    if (!_hasImage || !asGl(_gl))
        return false;

    float gR = 1.0f, gG = 1.0f, gB = 1.0f;
    if (!asGl(_gl)->computeBackgroundNeutralizationGpu(gR, gG, gB))
        return false;

    _wb.r = gR;
    _wb.g = gG;
    _wb.b = gB;
    invalidateHqCaches();

    return true;
}

void FitsRenderer::setViewParams(const ViewParams& vp)
{
    if (same_view_params(_view, vp))
        return;
    _view = vp;
    _hqPreviewCacheValid = false;
    asGl(_gl)->setViewParams(_view.scale, _view.panX, _view.panY);
}

void FitsRenderer::setBayerPattern(BayerPattern bayer)
{
    if (_bayer == bayer)
        return;
    _bayer = bayer;
    invalidateHqCaches();
    asGl(_gl)->setBayerPattern(static_cast<int>(_bayer));
}

void FitsRenderer::setLinearDenoiseConfig(bool enabled,
                                          float strength,
                                          float backgroundSigma,
                                          int iterations)
{
    if (!asGl(_gl))
        return;
    _hqPreviewCacheValid = false;
    asGl(_gl)->setLinearDenoiseConfig(enabled, strength, backgroundSigma, iterations);
}

void FitsRenderer::setExportDenoiseConfig(int mode,
                                          float strength,
                                          float backgroundSigma,
                                          int iterations)
{
    const int newMode = mode;
    const float newStrength = std::clamp(strength, 0.0f, 1.0f);
    const float newBackgroundSigma = std::max(backgroundSigma, 0.5f);
    const int newIterations = std::clamp(iterations, 1, 6);
    if (_exportDenoiseMode == newMode &&
        nearly_equal(_exportDenoiseStrength, newStrength) &&
        nearly_equal(_exportBackgroundSigma, newBackgroundSigma) &&
        _exportDenoiseIterations == newIterations)
    {
        return;
    }
    _exportDenoiseMode = newMode;
    _exportDenoiseStrength = newStrength;
    _exportBackgroundSigma = newBackgroundSigma;
    _exportDenoiseIterations = newIterations;
    invalidateHqCaches();
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
        _hqPreviewCacheValid = false;
        asGl(_gl)->setAutoParams(_stretch.autoStretch, _autoLow, _autoHigh, _stretch.strength);
        return true;
    }
    else
    {
        _autoLow  = 0.0f;
        _autoHigh = 1.0f;
        _hqPreviewCacheValid = false;
        asGl(_gl)->setAutoParams(_stretch.autoStretch, _autoLow, _autoHigh, _stretch.strength);
        return false;
    }
}

bool FitsRenderer::getLumaHistogram(std::vector<float>& outHist) const
{
    if (_exportDenoiseMode > 0)
    {
        if (!ensureHqLinearCache())
            return false;
        return compute_histogram_from_linear_rgb(_hqLinearRgbCache,
                                                 _hqLinearCacheW,
                                                 _hqLinearCacheH,
                                                 _stretch,
                                                 _autoLow,
                                                 _autoHigh,
                                                 outHist);
    }
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
    if (!_hasImage || !asGl(_gl) || !asFits(_fits))
        return false;

    if (_exportDenoiseMode > 0)
    {
        if (!ensureHqLinearCache())
            return false;
        outWidth = _hqLinearCacheW;
        outHeight = _hqLinearCacheH;
        return render_hq_export(_hqLinearRgbCache,
                                outWidth,
                                outHeight,
                                _stretch,
                                _autoLow,
                                _autoHigh,
                                outRGB);
    }

    outWidth = _imgWidth;
    outHeight = _imgHeight;
    return asGl(_gl)->renderToImage(outWidth, outHeight, outRGB);
}

bool FitsRenderer::renderPreview(int width, int height)
{
    if (!_hasImage || !asGl(_gl))
        return false;
    if (_exportDenoiseMode > 0)
    {
        const bool samePreview =
            _hqPreviewCacheValid &&
            _hqPreviewCacheW == width &&
            _hqPreviewCacheH == height &&
            _hqPreviewAutoLow == _autoLow &&
            _hqPreviewAutoHigh == _autoHigh &&
            _hqPreviewView.scale == _view.scale &&
            _hqPreviewView.panX == _view.panX &&
            _hqPreviewView.panY == _view.panY &&
            _hqPreviewStretch.autoStretch == _stretch.autoStretch &&
            _hqPreviewStretch.blackClip == _stretch.blackClip &&
            _hqPreviewStretch.whiteClip == _stretch.whiteClip &&
            _hqPreviewStretch.strength == _stretch.strength &&
            _hqPreviewStretch.mode == _stretch.mode;

        if (!samePreview || _hqPreviewNeedsHighRes)
        {
            if (!ensureHqLinearCache())
                return false;

            std::vector<unsigned char> previewRgb;
            const bool useLowResFirst = !samePreview &&
                _hqLinearLowResCacheValid &&
                (width >= 512 || height >= 512);
            const std::vector<float>& sourceRgb =
                (useLowResFirst || _hqPreviewNeedsHighRes == false) && useLowResFirst
                ? _hqLinearLowResRgbCache
                : _hqLinearRgbCache;
            const int sourceW =
                (useLowResFirst || _hqPreviewNeedsHighRes == false) && useLowResFirst
                ? _hqLinearLowResCacheW
                : _hqLinearCacheW;
            const int sourceH =
                (useLowResFirst || _hqPreviewNeedsHighRes == false) && useLowResFirst
                ? _hqLinearLowResCacheH
                : _hqLinearCacheH;

            if (!render_rgb_view_to_u8(sourceRgb,
                                       sourceW,
                                       sourceH,
                                       _stretch,
                                       _autoLow,
                                       _autoHigh,
                                       _view,
                                       width,
                                       height,
                                       previewRgb))
            {
                return false;
            }
            if (!asGl(_gl)->uploadPreviewRgb(previewRgb, width, height))
                return false;

            _hqPreviewCacheValid = true;
            _hqPreviewCacheW = width;
            _hqPreviewCacheH = height;
            _hqPreviewStretch = _stretch;
            _hqPreviewView = _view;
            _hqPreviewAutoLow = _autoLow;
            _hqPreviewAutoHigh = _autoHigh;
            _hqPreviewNeedsHighRes = useLowResFirst;
        }
        return true;
    }

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
