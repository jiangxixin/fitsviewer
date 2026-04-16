#include "kty/StackCore.h"

#include "FitsImage.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <sstream>

namespace kty {

namespace {

struct ImageStats {
    double median = 0.0;
    double sigma = 1.0;
    double signalScale = 1.0;
};

struct AffineNormalization {
    double offset = 0.0;
    double scale = 1.0;
};

struct CfaStats {
    std::array<ImageStats, 4> phases{};
};

struct CfaNormalization {
    std::array<AffineNormalization, 4> phases{};
};

struct RgbStats {
    std::array<ImageStats, 3> channels{};
};

struct RgbNormalization {
    std::array<AffineNormalization, 3> channels{};
};

struct FrameQualityMetrics {
    ImageStats stats{};
    double sharpness = 1.0;
    double snr = 1.0;
    double backgroundPenalty = 1.0;
    int starCount = 0;
    double starFwhm = 0.0;
    double starRoundness = 0.0;
    double score = 1.0;
};

struct AutoSelectionDecision {
    float keepPercent = 100.0f;
    size_t keepCount = 0;
    double threshold = 0.0;
    double medianSelectionScore = 1.0;
};

struct AnalysisPlane {
    int width = 0;
    int height = 0;
    std::vector<double> values;
};

struct DetectedStar {
    double flux = 0.0;
    double fwhm = 0.0;
    double roundness = 0.0;
};

struct DarkOptimizationProfile {
    bool enabled = false;
    size_t sampleCount = 0;
    double monoScale = 1.0;
    std::array<double, 4> cfaScales{1.0, 1.0, 1.0, 1.0};
    std::array<double, 3> rgbScales{1.0, 1.0, 1.0};
};

struct LineDefectMap {
    std::vector<uint8_t> mask;
    int badColumns = 0;
    int badRows = 0;
};

struct PixelIntegrationStats {
    double activeWeightSum = 0.0;
    double effectiveSamples = 0.0;
    size_t activeSamples = 0;
    bool coverageFallback = false;
};

static constexpr size_t kCfaPhaseCount = 4;
static constexpr size_t kRgbChannelCount = 3;

static double median_of(std::vector<double> values)
{
    if (values.empty())
        return 0.0;

    const size_t mid = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + mid, values.end());
    double med = values[mid];
    if ((values.size() & 1U) == 0)
    {
        auto lowerMax = *std::max_element(values.begin(), values.begin() + mid);
        med = 0.5 * (med + lowerMax);
    }
    return med;
}

static double robust_sigma_from_values(const std::vector<double>& values, double center)
{
    if (values.size() < 2)
        return 1.0;

    std::vector<double> absDev(values.size());
    for (size_t i = 0; i < values.size(); ++i)
        absDev[i] = std::abs(values[i] - center);

    double mad = median_of(absDev) * 1.4826;
    if (mad > 1e-12)
        return mad;

    double aav = 0.0;
    for (double v : values)
        aav += std::abs(v - center);
    aav /= static_cast<double>(values.size());
    if (aav > 1e-12)
        return aav * 1.2533141373;

    return 1.0;
}

static ImageStats compute_image_stats(const FitsImage& image)
{
    ImageStats stats;
    if (image.raw.empty())
        return stats;

    std::vector<double> values = image.raw;
    stats.median = median_of(values);
    stats.sigma = robust_sigma_from_values(values, stats.median);

    std::sort(values.begin(), values.end());
    const size_t n = values.size();
    const size_t p90Index = (n > 1) ? static_cast<size_t>(0.90 * (n - 1)) : 0;
    const double p90 = values[p90Index];
    stats.signalScale = std::max(p90 - stats.median, stats.sigma);
    return stats;
}

static bool is_cfa_raw(const FitsImage& image)
{
    return image.channels == 1 &&
           image.bayer != BayerPattern::NONE &&
           image.width > 0 &&
           image.height > 0 &&
           image.raw.size() == static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
}

static bool is_rgb_planar(const FitsImage& image)
{
    return image.channels == 3 &&
           image.width > 0 &&
           image.height > 0 &&
           image.raw.size() == static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 3ULL;
}

static size_t cfa_phase_for_index(size_t idx, int width)
{
    const size_t x = idx % static_cast<size_t>(width);
    const size_t y = idx / static_cast<size_t>(width);
    return ((y & 1U) << 1U) | (x & 1U);
}

static CfaStats compute_cfa_stats(const FitsImage& image)
{
    CfaStats stats;
    if (!is_cfa_raw(image))
    {
        const ImageStats global = compute_image_stats(image);
        for (ImageStats& phase : stats.phases)
            phase = global;
        return stats;
    }

    std::array<std::vector<double>, kCfaPhaseCount> phaseValues;
    for (size_t phase = 0; phase < kCfaPhaseCount; ++phase)
        phaseValues[phase].reserve(image.raw.size() / kCfaPhaseCount + 1);

    for (size_t idx = 0; idx < image.raw.size(); ++idx)
        phaseValues[cfa_phase_for_index(idx, image.width)].push_back(image.raw[idx]);

    const ImageStats global = compute_image_stats(image);
    for (size_t phase = 0; phase < kCfaPhaseCount; ++phase)
    {
        if (phaseValues[phase].empty())
        {
            stats.phases[phase] = global;
            continue;
        }

        auto& values = phaseValues[phase];
        stats.phases[phase].median = median_of(values);
        stats.phases[phase].sigma = robust_sigma_from_values(values, stats.phases[phase].median);

        std::sort(values.begin(), values.end());
        const size_t n = values.size();
        const size_t p90Index = (n > 1) ? static_cast<size_t>(0.90 * (n - 1)) : 0;
        const double p90 = values[p90Index];
        stats.phases[phase].signalScale =
            std::max(p90 - stats.phases[phase].median, stats.phases[phase].sigma);
    }

    return stats;
}

static RgbStats compute_rgb_stats(const FitsImage& image)
{
    RgbStats stats;
    const ImageStats global = compute_image_stats(image);
    for (ImageStats& channel : stats.channels)
        channel = global;

    if (!is_rgb_planar(image))
        return stats;

    const size_t planeSize = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
    for (size_t channel = 0; channel < kRgbChannelCount; ++channel)
    {
        std::vector<double> values(image.raw.begin() + static_cast<std::ptrdiff_t>(channel * planeSize),
                                   image.raw.begin() + static_cast<std::ptrdiff_t>((channel + 1) * planeSize));
        stats.channels[channel].median = median_of(values);
        stats.channels[channel].sigma = robust_sigma_from_values(values, stats.channels[channel].median);
        std::sort(values.begin(), values.end());
        const size_t n = values.size();
        const size_t p90Index = (n > 1) ? static_cast<size_t>(0.90 * (n - 1)) : 0;
        const double p90 = values[p90Index];
        stats.channels[channel].signalScale =
            std::max(p90 - stats.channels[channel].median, stats.channels[channel].sigma);
    }

    return stats;
}

static std::array<double, kCfaPhaseCount> compute_phase_means(const std::vector<double>& values,
                                                              int width)
{
    std::array<double, kCfaPhaseCount> sums = {0.0, 0.0, 0.0, 0.0};
    std::array<size_t, kCfaPhaseCount> counts = {0, 0, 0, 0};

    for (size_t idx = 0; idx < values.size(); ++idx)
    {
        const size_t phase = cfa_phase_for_index(idx, width);
        sums[phase] += values[idx];
        ++counts[phase];
    }

    double globalSum = std::accumulate(values.begin(), values.end(), 0.0);
    double globalMean = values.empty() ? 1.0 : (globalSum / static_cast<double>(values.size()));
    if (std::abs(globalMean) < 1.0e-12)
        globalMean = 1.0;

    std::array<double, kCfaPhaseCount> means{};
    for (size_t phase = 0; phase < kCfaPhaseCount; ++phase)
    {
        means[phase] = (counts[phase] == 0) ? globalMean
                                            : (sums[phase] / static_cast<double>(counts[phase]));
        if (std::abs(means[phase]) < 1.0e-12)
            means[phase] = globalMean;
    }

    return means;
}

static int cfa_phase_color(BayerPattern pattern, size_t phase)
{
    switch (pattern)
    {
    case BayerPattern::RGGB:
        return (phase == 0) ? 0 : ((phase == 3) ? 2 : 1);
    case BayerPattern::BGGR:
        return (phase == 0) ? 2 : ((phase == 3) ? 0 : 1);
    case BayerPattern::GRBG:
        return (phase == 1) ? 0 : ((phase == 2) ? 2 : 1);
    case BayerPattern::GBRG:
        return (phase == 1) ? 2 : ((phase == 2) ? 0 : 1);
    case BayerPattern::NONE:
        break;
    }
    return 1;
}

static double rgb_neutral_background_from_cfa(const FitsImage& image, const CfaStats& referenceStats)
{
    std::array<double, kRgbChannelCount> colorMedians = {0.0, 0.0, 0.0};
    std::array<int, kRgbChannelCount> colorCounts = {0, 0, 0};
    for (size_t phase = 0; phase < kCfaPhaseCount; ++phase)
    {
        const int color = cfa_phase_color(image.bayer, phase);
        colorMedians[static_cast<size_t>(color)] += referenceStats.phases[phase].median;
        colorCounts[static_cast<size_t>(color)] += 1;
    }
    for (size_t color = 0; color < kRgbChannelCount; ++color)
    {
        if (colorCounts[color] > 0)
            colorMedians[color] /= static_cast<double>(colorCounts[color]);
    }

    return std::min({colorMedians[0], colorMedians[1], colorMedians[2]});
}

static double rgb_luma_at(const FitsImage& image, size_t pixelIndex)
{
    const size_t planeSize = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
    const double r = image.raw[pixelIndex];
    const double g = image.raw[pixelIndex + planeSize];
    const double b = image.raw[pixelIndex + 2 * planeSize];
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

static ImageStats compute_stats_from_values(std::vector<double> values)
{
    ImageStats stats;
    if (values.empty())
        return stats;

    stats.median = median_of(values);
    stats.sigma = robust_sigma_from_values(values, stats.median);
    std::sort(values.begin(), values.end());
    const size_t n = values.size();
    const size_t p90Index = (n > 1) ? static_cast<size_t>(0.90 * (n - 1)) : 0;
    const double p90 = values[p90Index];
    stats.signalScale = std::max(p90 - stats.median, stats.sigma);
    return stats;
}

static AnalysisPlane build_analysis_plane(const FitsImage& image)
{
    AnalysisPlane plane;
    if (image.raw.empty() || image.width <= 0 || image.height <= 0)
        return plane;

    if (is_cfa_raw(image))
    {
        plane.width = std::max(image.width / 2, 1);
        plane.height = std::max(image.height / 2, 1);
        plane.values.assign(static_cast<size_t>(plane.width) * static_cast<size_t>(plane.height), 0.0);
        for (int y = 0; y < plane.height; ++y)
        {
            for (int x = 0; x < plane.width; ++x)
            {
                double sum = 0.0;
                int count = 0;
                for (int dy = 0; dy < 2; ++dy)
                {
                    for (int dx = 0; dx < 2; ++dx)
                    {
                        const int sx = x * 2 + dx;
                        const int sy = y * 2 + dy;
                        if (sx >= image.width || sy >= image.height)
                            continue;
                        sum += image.raw[static_cast<size_t>(sy) * static_cast<size_t>(image.width) +
                                         static_cast<size_t>(sx)];
                        ++count;
                    }
                }
                plane.values[static_cast<size_t>(y) * static_cast<size_t>(plane.width) + static_cast<size_t>(x)] =
                    (count > 0) ? (sum / static_cast<double>(count)) : 0.0;
            }
        }
        return plane;
    }

    plane.width = image.width;
    plane.height = image.height;
    plane.values.assign(static_cast<size_t>(plane.width) * static_cast<size_t>(plane.height), 0.0);
    if (is_rgb_planar(image))
    {
        for (size_t idx = 0; idx < plane.values.size(); ++idx)
            plane.values[idx] = rgb_luma_at(image, idx);
    }
    else
    {
        plane.values = image.raw;
    }
    return plane;
}

static double plane_value_at(const AnalysisPlane& plane, int x, int y)
{
    return plane.values[static_cast<size_t>(y) * static_cast<size_t>(plane.width) + static_cast<size_t>(x)];
}

static std::vector<DetectedStar> detect_stars_for_quality(const AnalysisPlane& plane, const ImageStats& planeStats)
{
    std::vector<DetectedStar> stars;
    if (plane.width < 7 || plane.height < 7 || plane.values.empty())
        return stars;

    const double sigma = std::max(planeStats.sigma, 1.0e-6);
    const double threshold = planeStats.median + std::max(3.0 * sigma, 0.12 * std::max(planeStats.signalScale, sigma));
    std::vector<uint8_t> suppressed(static_cast<size_t>(plane.width) * static_cast<size_t>(plane.height), 0);

    for (int y = 3; y < plane.height - 3; ++y)
    {
        for (int x = 3; x < plane.width - 3; ++x)
        {
            const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(plane.width) + static_cast<size_t>(x);
            if (suppressed[idx] != 0)
                continue;

            const double center = plane.values[idx];
            if (center <= threshold)
                continue;

            bool localMaximum = true;
            for (int dy = -1; dy <= 1 && localMaximum; ++dy)
            {
                for (int dx = -1; dx <= 1; ++dx)
                {
                    if (dx == 0 && dy == 0)
                        continue;
                    if (plane_value_at(plane, x + dx, y + dy) >= center)
                    {
                        localMaximum = false;
                        break;
                    }
                }
            }
            if (!localMaximum)
                continue;

            double weightSum = 0.0;
            double sumX = 0.0;
            double sumY = 0.0;
            double sumXX = 0.0;
            double sumYY = 0.0;
            double sumXY = 0.0;
            for (int dy = -2; dy <= 2; ++dy)
            {
                for (int dx = -2; dx <= 2; ++dx)
                {
                    const double weight = std::max(plane_value_at(plane, x + dx, y + dy) - planeStats.median, 0.0);
                    if (weight <= 0.0)
                        continue;
                    const double fx = static_cast<double>(x + dx);
                    const double fy = static_cast<double>(y + dy);
                    weightSum += weight;
                    sumX += fx * weight;
                    sumY += fy * weight;
                }
            }
            if (weightSum <= 4.0 * sigma)
                continue;

            const double cx = sumX / weightSum;
            const double cy = sumY / weightSum;
            for (int dy = -2; dy <= 2; ++dy)
            {
                for (int dx = -2; dx <= 2; ++dx)
                {
                    const double weight = std::max(plane_value_at(plane, x + dx, y + dy) - planeStats.median, 0.0);
                    if (weight <= 0.0)
                        continue;
                    const double ddx = static_cast<double>(x + dx) - cx;
                    const double ddy = static_cast<double>(y + dy) - cy;
                    sumXX += ddx * ddx * weight;
                    sumYY += ddy * ddy * weight;
                    sumXY += ddx * ddy * weight;
                }
            }

            const double mxx = sumXX / weightSum;
            const double myy = sumYY / weightSum;
            const double mxy = sumXY / weightSum;
            const double trace = mxx + myy;
            const double detTerm = std::sqrt(std::max((mxx - myy) * (mxx - myy) + 4.0 * mxy * mxy, 0.0));
            const double lambdaMajor = std::max(0.5 * (trace + detTerm), 1.0e-6);
            const double lambdaMinor = std::max(0.5 * (trace - detTerm), 1.0e-6);
            const double sigmaMajor = std::sqrt(lambdaMajor);
            const double sigmaMinor = std::sqrt(lambdaMinor);
            const double fwhm = 2.354820045 * std::sqrt(0.5 * (lambdaMajor + lambdaMinor));
            const double roundness = std::clamp(sigmaMinor / std::max(sigmaMajor, 1.0e-6), 0.0, 1.0);
            const double peakSigma = (center - planeStats.median) / sigma;

            if (fwhm < 0.8 || fwhm > 8.5 || roundness < 0.35 || peakSigma < 4.0)
                continue;

            stars.push_back(DetectedStar{weightSum, fwhm, roundness});
            const int suppressRadius = std::max(2, static_cast<int>(std::ceil(fwhm)));
            for (int dy = -suppressRadius; dy <= suppressRadius; ++dy)
            {
                for (int dx = -suppressRadius; dx <= suppressRadius; ++dx)
                {
                    const int sx = x + dx;
                    const int sy = y + dy;
                    if (sx < 0 || sy < 0 || sx >= plane.width || sy >= plane.height)
                        continue;
                    suppressed[static_cast<size_t>(sy) * static_cast<size_t>(plane.width) + static_cast<size_t>(sx)] = 1;
                }
            }
        }
    }

    if (stars.size() > 256)
    {
        std::partial_sort(stars.begin(), stars.begin() + 256, stars.end(),
                          [](const DetectedStar& a, const DetectedStar& b) { return a.flux > b.flux; });
        stars.resize(256);
    }
    return stars;
}

static ImageStats compute_quality_stats(const FitsImage& image)
{
    if (!is_rgb_planar(image))
        return compute_image_stats(image);

    const size_t planeSize = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
    std::vector<double> luma(planeSize, 0.0);
    for (size_t idx = 0; idx < planeSize; ++idx)
        luma[idx] = rgb_luma_at(image, idx);

    FitsImage pseudo;
    pseudo.width = image.width;
    pseudo.height = image.height;
    pseudo.channels = 1;
    pseudo.raw = std::move(luma);
    return compute_image_stats(pseudo);
}

static double compute_sharpness_proxy(const FitsImage& image, const ImageStats& stats)
{
    if (image.width < 5 || image.height < 5)
        return 1.0;

    const double threshold = stats.median + 1.5 * std::max(stats.sigma, 1.0e-6);
    double sharpnessSum = 0.0;
    size_t sharpnessCount = 0;

    if (is_cfa_raw(image))
    {
        for (int y = 2; y < image.height - 2; ++y)
        {
            for (int x = 2; x < image.width - 2; ++x)
            {
                const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(image.width) + static_cast<size_t>(x);
                const double center = image.raw[idx];
                if (center <= threshold)
                    continue;

                const double left = image.raw[idx - 2];
                const double right = image.raw[idx + 2];
                const double up = image.raw[idx - 2 * static_cast<size_t>(image.width)];
                const double down = image.raw[idx + 2 * static_cast<size_t>(image.width)];
                const double lap = std::max(0.0, 4.0 * center - left - right - up - down);
                sharpnessSum += lap;
                ++sharpnessCount;
            }
        }
    }
    else if (is_rgb_planar(image))
    {
        for (int y = 1; y < image.height - 1; ++y)
        {
            for (int x = 1; x < image.width - 1; ++x)
            {
                const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(image.width) + static_cast<size_t>(x);
                const double center = rgb_luma_at(image, idx);
                if (center <= threshold)
                    continue;

                const double left = rgb_luma_at(image, idx - 1);
                const double right = rgb_luma_at(image, idx + 1);
                const double up = rgb_luma_at(image, idx - static_cast<size_t>(image.width));
                const double down = rgb_luma_at(image, idx + static_cast<size_t>(image.width));
                const double lap = std::max(0.0, 4.0 * center - left - right - up - down);
                sharpnessSum += lap;
                ++sharpnessCount;
            }
        }
    }
    else
    {
        for (int y = 1; y < image.height - 1; ++y)
        {
            for (int x = 1; x < image.width - 1; ++x)
            {
                const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(image.width) + static_cast<size_t>(x);
                const double center = image.raw[idx];
                if (center <= threshold)
                    continue;

                const double left = image.raw[idx - 1];
                const double right = image.raw[idx + 1];
                const double up = image.raw[idx - static_cast<size_t>(image.width)];
                const double down = image.raw[idx + static_cast<size_t>(image.width)];
                const double lap = std::max(0.0, 4.0 * center - left - right - up - down);
                sharpnessSum += lap;
                ++sharpnessCount;
            }
        }
    }

    if (sharpnessCount == 0)
        return 1.0;

    const double scale = std::max(stats.signalScale, std::max(stats.sigma, 1.0e-6));
    return std::max((sharpnessSum / static_cast<double>(sharpnessCount)) / scale, 1.0e-3);
}

static FrameQualityMetrics compute_frame_quality(const FitsImage& image)
{
    FrameQualityMetrics metrics;
    metrics.stats = compute_quality_stats(image);
    metrics.sharpness = compute_sharpness_proxy(image, metrics.stats);
    metrics.snr = std::max(metrics.stats.signalScale, 1.0e-6) / std::max(metrics.stats.sigma, 1.0e-6);
    const double backgroundReference = std::max(metrics.stats.signalScale, metrics.stats.sigma);
    metrics.backgroundPenalty = 1.0 /
        (1.0 + std::max(metrics.stats.median, 0.0) / std::max(backgroundReference, 1.0e-6));
    const AnalysisPlane plane = build_analysis_plane(image);
    const ImageStats planeStats = compute_stats_from_values(plane.values);
    const std::vector<DetectedStar> stars = detect_stars_for_quality(plane, planeStats);
    metrics.starCount = static_cast<int>(stars.size());
    if (!stars.empty())
    {
        std::vector<double> fwhmValues;
        std::vector<double> roundnessValues;
        fwhmValues.reserve(stars.size());
        roundnessValues.reserve(stars.size());
        for (const DetectedStar& star : stars)
        {
            fwhmValues.push_back(star.fwhm);
            roundnessValues.push_back(star.roundness);
        }
        metrics.starFwhm = median_of(fwhmValues);
        metrics.starRoundness = median_of(roundnessValues);
    }

    const double starCountFactor =
        (metrics.starCount > 0)
            ? std::clamp(std::log1p(static_cast<double>(metrics.starCount)) / std::log(12.0), 0.35, 2.5)
            : 0.55;
    const double roundnessFactor =
        (metrics.starCount > 0)
            ? std::clamp(metrics.starRoundness, 0.35, 1.0)
            : 0.65;
    const double fwhmFactor =
        (metrics.starCount > 0 && metrics.starFwhm > 0.0)
            ? std::clamp(2.8 / metrics.starFwhm, 0.35, 2.0)
            : 0.75;
    metrics.score =
        std::pow(std::max(metrics.snr, 1.0e-3), 0.55) *
        std::pow(std::max(metrics.sharpness, 1.0e-3), 0.55) *
        std::pow(starCountFactor, 0.80) *
        std::pow(roundnessFactor * fwhmFactor, 1.15) *
        metrics.backgroundPenalty;
    metrics.score = std::max(metrics.score, 1.0e-6);
    return metrics;
}

static double effective_sample_count_from_weights(const std::vector<double>& weights)
{
    double sumW = 0.0;
    double sumW2 = 0.0;
    for (double weight : weights)
    {
        const double w = std::max(weight, 0.0);
        sumW += w;
        sumW2 += w * w;
    }
    if (sumW <= 1.0e-12 || sumW2 <= 1.0e-12)
        return 0.0;
    return (sumW * sumW) / sumW2;
}

static AutoSelectionDecision compute_auto_selection_decision(const std::vector<FrameQualityMetrics>& qualities,
                                                             AutoQualityProfile profile)
{
    AutoSelectionDecision decision;
    if (qualities.size() < 4)
    {
        decision.keepCount = qualities.size();
        return decision;
    }

    std::vector<double> scoreValues;
    std::vector<double> starCounts;
    std::vector<double> fwhmValues;
    std::vector<double> roundnessValues;
    scoreValues.reserve(qualities.size());
    starCounts.reserve(qualities.size());
    fwhmValues.reserve(qualities.size());
    roundnessValues.reserve(qualities.size());
    for (const FrameQualityMetrics& quality : qualities)
    {
        scoreValues.push_back(quality.score);
        if (quality.starCount > 0)
            starCounts.push_back(static_cast<double>(quality.starCount));
        if (quality.starFwhm > 0.0)
            fwhmValues.push_back(quality.starFwhm);
        if (quality.starRoundness > 0.0)
            roundnessValues.push_back(quality.starRoundness);
    }

    const double medianScore = std::max(median_of(scoreValues), 1.0e-6);
    const double medianStarCount = starCounts.empty() ? 0.0 : median_of(starCounts);
    const double medianFwhm = fwhmValues.empty() ? 0.0 : median_of(fwhmValues);
    const double medianRoundness = roundnessValues.empty() ? 0.0 : median_of(roundnessValues);

    std::vector<double> selectionScores;
    selectionScores.reserve(qualities.size());
    for (const FrameQualityMetrics& quality : qualities)
    {
        const double baseFactor = std::clamp(quality.score / medianScore, 0.35, 2.5);
        const double starCountFactor =
            (quality.starCount > 0 && medianStarCount > 0.0)
                ? std::clamp(static_cast<double>(quality.starCount) / medianStarCount, 0.35, 2.2)
                : 0.8;
        const double fwhmFactor =
            (quality.starFwhm > 0.0 && medianFwhm > 0.0)
                ? std::clamp(medianFwhm / quality.starFwhm, 0.35, 2.0)
                : 0.8;
        const double roundnessFactor =
            (quality.starRoundness > 0.0 && medianRoundness > 0.0)
                ? std::clamp(quality.starRoundness / medianRoundness, 0.5, 1.5)
                : 0.85;
        const double selectionScore =
            std::pow(baseFactor, 0.55) *
            std::pow(starCountFactor, 0.85) *
            std::pow(fwhmFactor, 1.20) *
            std::pow(roundnessFactor, 0.90);
        selectionScores.push_back(std::max(selectionScore, 1.0e-6));
    }

    decision.medianSelectionScore = median_of(selectionScores);
    double thresholdRatio = 0.72;
    double minKeepRatio = 0.55;
    switch (profile)
    {
    case AutoQualityProfile::Conservative:
        thresholdRatio = 0.58;
        minKeepRatio = 0.75;
        break;
    case AutoQualityProfile::Balanced:
        thresholdRatio = 0.72;
        minKeepRatio = 0.55;
        break;
    case AutoQualityProfile::Aggressive:
        thresholdRatio = 0.88;
        minKeepRatio = 0.35;
        break;
    }

    const double absoluteThreshold = decision.medianSelectionScore * thresholdRatio;
    size_t keepCount = 0;
    for (double selectionScore : selectionScores)
    {
        if (selectionScore >= absoluteThreshold)
            ++keepCount;
    }

    const size_t minKeepCount = std::max<size_t>(
        3,
        static_cast<size_t>(std::ceil(static_cast<double>(qualities.size()) * minKeepRatio)));
    keepCount = std::clamp(keepCount, minKeepCount, qualities.size());
    decision.keepCount = keepCount;
    decision.keepPercent =
        std::clamp(100.0f * static_cast<float>(keepCount) / static_cast<float>(qualities.size()), 5.0f, 100.0f);
    decision.threshold = absoluteThreshold;
    return decision;
}

static std::vector<uint8_t> detect_hot_pixels(const FitsImage& image)
{
    std::vector<uint8_t> mask(image.raw.size(), 0);
    if (image.raw.empty())
        return mask;

    if (is_cfa_raw(image))
    {
        const CfaStats stats = compute_cfa_stats(image);
        std::array<double, kCfaPhaseCount> thresholds{};
        for (size_t phase = 0; phase < kCfaPhaseCount; ++phase)
            thresholds[phase] = stats.phases[phase].median + 16.0 * std::max(stats.phases[phase].sigma, 1.0e-6);

        for (size_t idx = 0; idx < image.raw.size(); ++idx)
        {
            const size_t phase = cfa_phase_for_index(idx, image.width);
            if (image.raw[idx] > thresholds[phase])
                mask[idx] = 1;
        }
        return mask;
    }

    if (is_rgb_planar(image))
    {
        const RgbStats stats = compute_rgb_stats(image);
        const size_t planeSize = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
        for (size_t channel = 0; channel < kRgbChannelCount; ++channel)
        {
            const double threshold =
                stats.channels[channel].median + 16.0 * std::max(stats.channels[channel].sigma, 1.0e-6);
            for (size_t idx = 0; idx < planeSize; ++idx)
            {
                const size_t planeIndex = channel * planeSize + idx;
                if (image.raw[planeIndex] > threshold)
                    mask[planeIndex] = 1;
            }
        }
        return mask;
    }

    const ImageStats stats = compute_image_stats(image);
    const double threshold = stats.median + 16.0 * std::max(stats.sigma, 1.0e-6);
    for (size_t idx = 0; idx < image.raw.size(); ++idx)
    {
        if (image.raw[idx] > threshold)
            mask[idx] = 1;
    }
    return mask;
}

static size_t count_masked_pixels(const std::vector<uint8_t>& mask)
{
    return static_cast<size_t>(std::count(mask.begin(), mask.end(), static_cast<uint8_t>(1)));
}

static size_t gray_index(int x, int y, int width)
{
    return static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
}

static std::vector<double> compute_plane_column_medians(const FitsImage& image, size_t planeOffset)
{
    std::vector<double> medians(static_cast<size_t>(image.width), 0.0);
    std::vector<double> values;
    values.reserve(static_cast<size_t>(image.height));
    for (int x = 0; x < image.width; ++x)
    {
        values.clear();
        for (int y = 0; y < image.height; ++y)
            values.push_back(image.raw[planeOffset + gray_index(x, y, image.width)]);
        medians[static_cast<size_t>(x)] = median_of(values);
    }
    return medians;
}

static std::vector<double> compute_plane_row_medians(const FitsImage& image, size_t planeOffset)
{
    std::vector<double> medians(static_cast<size_t>(image.height), 0.0);
    std::vector<double> values;
    values.reserve(static_cast<size_t>(image.width));
    for (int y = 0; y < image.height; ++y)
    {
        values.clear();
        for (int x = 0; x < image.width; ++x)
            values.push_back(image.raw[planeOffset + gray_index(x, y, image.width)]);
        medians[static_cast<size_t>(y)] = median_of(values);
    }
    return medians;
}

static std::vector<double> compute_plane_column_hot_fraction(const FitsImage& image,
                                                             const std::vector<uint8_t>& hotPixelMask,
                                                             size_t planeOffset)
{
    std::vector<double> fractions(static_cast<size_t>(image.width), 0.0);
    if (hotPixelMask.size() != image.raw.size())
        return fractions;

    const double denom = std::max(static_cast<double>(image.height), 1.0);
    for (int x = 0; x < image.width; ++x)
    {
        size_t hotCount = 0;
        for (int y = 0; y < image.height; ++y)
            hotCount += hotPixelMask[planeOffset + gray_index(x, y, image.width)] != 0 ? 1U : 0U;
        fractions[static_cast<size_t>(x)] = static_cast<double>(hotCount) / denom;
    }
    return fractions;
}

static std::vector<double> compute_plane_row_hot_fraction(const FitsImage& image,
                                                          const std::vector<uint8_t>& hotPixelMask,
                                                          size_t planeOffset)
{
    std::vector<double> fractions(static_cast<size_t>(image.height), 0.0);
    if (hotPixelMask.size() != image.raw.size())
        return fractions;

    const double denom = std::max(static_cast<double>(image.width), 1.0);
    for (int y = 0; y < image.height; ++y)
    {
        size_t hotCount = 0;
        for (int x = 0; x < image.width; ++x)
            hotCount += hotPixelMask[planeOffset + gray_index(x, y, image.width)] != 0 ? 1U : 0U;
        fractions[static_cast<size_t>(y)] = static_cast<double>(hotCount) / denom;
    }
    return fractions;
}

static std::vector<int> detect_bad_lines(const std::vector<double>& medians,
                                         const std::vector<double>& hotFractions,
                                         double globalSigma,
                                         int start,
                                         int step)
{
    std::vector<int> badLines;
    if (medians.size() < 5 || medians.size() != hotFractions.size())
        return badLines;

    std::vector<int> positions;
    positions.reserve(medians.size());
    for (int i = start; i < static_cast<int>(medians.size()); i += step)
        positions.push_back(i);

    if (positions.size() < 4)
        return badLines;

    std::vector<double> residuals;
    std::vector<int> residualPositions;
    residuals.reserve(positions.size());
    residualPositions.reserve(positions.size());
    for (size_t i = 0; i < positions.size(); ++i)
    {
        const bool hasPrev = i > 0;
        const bool hasNext = i + 1 < positions.size();
        if (!hasPrev && !hasNext)
            continue;

        double reference = 0.0;
        if (hasPrev && hasNext)
            reference = 0.5 * (medians[static_cast<size_t>(positions[i - 1])] +
                               medians[static_cast<size_t>(positions[i + 1])]);
        else if (hasPrev)
            reference = medians[static_cast<size_t>(positions[i - 1])];
        else
            reference = medians[static_cast<size_t>(positions[i + 1])];

        residuals.push_back(medians[static_cast<size_t>(positions[i])] - reference);
        residualPositions.push_back(positions[i]);
    }

    if (residuals.size() < 4)
        return badLines;

    const double residualCenter = median_of(residuals);
    const double residualSigma = robust_sigma_from_values(residuals, residualCenter);
    const double strongThreshold = std::max(6.0 * std::max(residualSigma, 1.0e-6),
                                            4.0 * std::max(globalSigma, 1.0e-6));
    const double assistedThreshold = std::max(3.5 * std::max(residualSigma, 1.0e-6),
                                              2.0 * std::max(globalSigma, 1.0e-6));
    const double densityThreshold = 0.025;

    for (size_t i = 0; i < residuals.size(); ++i)
    {
        const int lineIndex = residualPositions[i];
        const double residualDelta = std::abs(residuals[i] - residualCenter);
        const bool strongOutlier = residualDelta > strongThreshold;
        const bool densityAssisted = hotFractions[static_cast<size_t>(lineIndex)] > densityThreshold &&
                                     residualDelta > assistedThreshold;
        if (strongOutlier || densityAssisted)
            badLines.push_back(lineIndex);
    }

    return badLines;
}

static void mark_plane_column_mask(LineDefectMap& map,
                                   const FitsImage& image,
                                   size_t planeOffset,
                                   int column)
{
    for (int y = 0; y < image.height; ++y)
        map.mask[planeOffset + gray_index(column, y, image.width)] = 1;
    ++map.badColumns;
}

static void mark_plane_row_mask(LineDefectMap& map,
                                const FitsImage& image,
                                size_t planeOffset,
                                int row)
{
    for (int x = 0; x < image.width; ++x)
        map.mask[planeOffset + gray_index(x, row, image.width)] = 1;
    ++map.badRows;
}

static LineDefectMap detect_line_defects(const FitsImage& image,
                                         const std::vector<uint8_t>& hotPixelMask)
{
    LineDefectMap result;
    result.mask.assign(image.raw.size(), 0);
    if (image.raw.empty() || hotPixelMask.size() != image.raw.size())
        return result;

    if (is_rgb_planar(image))
    {
        const RgbStats stats = compute_rgb_stats(image);
        const size_t planeSize = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
        for (size_t channel = 0; channel < kRgbChannelCount; ++channel)
        {
            const size_t planeOffset = channel * planeSize;
            const auto columnMedians = compute_plane_column_medians(image, planeOffset);
            const auto rowMedians = compute_plane_row_medians(image, planeOffset);
            const auto columnFractions = compute_plane_column_hot_fraction(image, hotPixelMask, planeOffset);
            const auto rowFractions = compute_plane_row_hot_fraction(image, hotPixelMask, planeOffset);
            for (int column : detect_bad_lines(columnMedians, columnFractions, stats.channels[channel].sigma, 0, 1))
                mark_plane_column_mask(result, image, planeOffset, column);
            for (int row : detect_bad_lines(rowMedians, rowFractions, stats.channels[channel].sigma, 0, 1))
                mark_plane_row_mask(result, image, planeOffset, row);
        }
        return result;
    }

    const ImageStats stats = compute_image_stats(image);
    const auto columnMedians = compute_plane_column_medians(image, 0);
    const auto rowMedians = compute_plane_row_medians(image, 0);
    const auto columnFractions = compute_plane_column_hot_fraction(image, hotPixelMask, 0);
    const auto rowFractions = compute_plane_row_hot_fraction(image, hotPixelMask, 0);
    const bool cfaAware = is_cfa_raw(image);
    const int familyCount = cfaAware ? 2 : 1;
    const int step = cfaAware ? 2 : 1;
    for (int family = 0; family < familyCount; ++family)
    {
        for (int column : detect_bad_lines(columnMedians, columnFractions, stats.sigma, family, step))
            mark_plane_column_mask(result, image, 0, column);
        for (int row : detect_bad_lines(rowMedians, rowFractions, stats.sigma, family, step))
            mark_plane_row_mask(result, image, 0, row);
    }

    return result;
}

static double median_or_zero(std::vector<double>& values)
{
    return values.empty() ? 0.0 : median_of(values);
}

static double interpolate_gray_defect(const FitsImage& image,
                                      const std::vector<uint8_t>& defectMask,
                                      int x,
                                      int y)
{
    std::vector<double> samples;
    samples.reserve(8);
    for (int dy = -1; dy <= 1; ++dy)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            if (dx == 0 && dy == 0)
                continue;
            const int nx = x + dx;
            const int ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= image.width || ny >= image.height)
                continue;
            const size_t nidx = gray_index(nx, ny, image.width);
            if (nidx < defectMask.size() && defectMask[nidx] != 0)
                continue;
            samples.push_back(image.raw[nidx]);
        }
    }
    return median_or_zero(samples);
}

static double interpolate_cfa_defect(const FitsImage& image,
                                     const std::vector<uint8_t>& defectMask,
                                     int x,
                                     int y)
{
    std::vector<double> samples;
    samples.reserve(8);
    static constexpr int kOffsets[][2] = {
        {-2, 0}, {2, 0}, {0, -2}, {0, 2},
        {-2, -2}, {2, -2}, {-2, 2}, {2, 2}
    };
    for (const auto& offset : kOffsets)
    {
        const int nx = x + offset[0];
        const int ny = y + offset[1];
        if (nx < 0 || ny < 0 || nx >= image.width || ny >= image.height)
            continue;
        const size_t nidx = gray_index(nx, ny, image.width);
        if (nidx < defectMask.size() && defectMask[nidx] != 0)
            continue;
        samples.push_back(image.raw[nidx]);
    }
    return samples.empty() ? interpolate_gray_defect(image, defectMask, x, y)
                           : median_or_zero(samples);
}

static double interpolate_rgb_defect(const FitsImage& image,
                                     const std::vector<uint8_t>& defectMask,
                                     int x,
                                     int y,
                                     size_t channel)
{
    std::vector<double> samples;
    samples.reserve(8);
    const size_t planeSize = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
    for (int dy = -1; dy <= 1; ++dy)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            if (dx == 0 && dy == 0)
                continue;
            const int nx = x + dx;
            const int ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= image.width || ny >= image.height)
                continue;
            const size_t pixelIndex = gray_index(nx, ny, image.width);
            const size_t planeIndex = channel * planeSize + pixelIndex;
            if (planeIndex < defectMask.size() && defectMask[planeIndex] != 0)
                continue;
            samples.push_back(image.raw[planeIndex]);
        }
    }
    return median_or_zero(samples);
}

static size_t correct_hot_pixels(FitsImage& image, const std::vector<uint8_t>& defectMask)
{
    if (image.raw.empty() || defectMask.size() != image.raw.size())
        return 0;

    FitsImage source = image;
    size_t corrected = 0;

    if (is_cfa_raw(image))
    {
        for (int y = 0; y < image.height; ++y)
        {
            for (int x = 0; x < image.width; ++x)
            {
                const size_t idx = gray_index(x, y, image.width);
                if (defectMask[idx] == 0)
                    continue;
                const double replacement = interpolate_cfa_defect(source, defectMask, x, y);
                if (replacement == 0.0 && source.raw[idx] == 0.0)
                    continue;
                image.raw[idx] = replacement;
                ++corrected;
            }
        }
        return corrected;
    }

    if (is_rgb_planar(image))
    {
        const size_t planeSize = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
        for (size_t channel = 0; channel < kRgbChannelCount; ++channel)
        {
            for (int y = 0; y < image.height; ++y)
            {
                for (int x = 0; x < image.width; ++x)
                {
                    const size_t pixelIndex = gray_index(x, y, image.width);
                    const size_t idx = channel * planeSize + pixelIndex;
                    if (defectMask[idx] == 0)
                        continue;
                    const double replacement = interpolate_rgb_defect(source, defectMask, x, y, channel);
                    if (replacement == 0.0 && source.raw[idx] == 0.0)
                        continue;
                    image.raw[idx] = replacement;
                    ++corrected;
                }
            }
        }
        return corrected;
    }

    for (int y = 0; y < image.height; ++y)
    {
        for (int x = 0; x < image.width; ++x)
        {
            const size_t idx = gray_index(x, y, image.width);
            if (defectMask[idx] == 0)
                continue;
            const double replacement = interpolate_gray_defect(source, defectMask, x, y);
            if (replacement == 0.0 && source.raw[idx] == 0.0)
                continue;
            image.raw[idx] = replacement;
            ++corrected;
        }
    }

    return corrected;
}

static double clamp_dark_optimization_scale(double scale)
{
    if (!std::isfinite(scale))
        return 1.0;
    return std::clamp(scale, 0.5, 2.5);
}

static double robust_dark_scale_from_ratios(std::vector<double>& ratios)
{
    if (ratios.size() < 8)
        return 1.0;

    const double medianRatio = median_of(ratios);
    const double sigma = robust_sigma_from_values(ratios, medianRatio);
    const double low = std::max(0.2, medianRatio - 2.5 * sigma);
    const double high = medianRatio + 2.5 * sigma;

    std::vector<double> filtered;
    filtered.reserve(ratios.size());
    for (double ratio : ratios)
    {
        if (ratio >= low && ratio <= high)
            filtered.push_back(ratio);
    }

    const double refinedRatio = filtered.empty() ? medianRatio : median_of(filtered);
    return clamp_dark_optimization_scale(refinedRatio);
}

static DarkOptimizationProfile compute_dark_optimization_profile(const FitsImage& biasAdjustedLight,
                                                                 const FitsImage& masterDark,
                                                                 const std::vector<uint8_t>& hotPixelMask)
{
    DarkOptimizationProfile profile;
    if (biasAdjustedLight.raw.empty() ||
        masterDark.raw.empty() ||
        biasAdjustedLight.raw.size() != masterDark.raw.size() ||
        hotPixelMask.size() != masterDark.raw.size())
    {
        return profile;
    }

    if (is_cfa_raw(biasAdjustedLight) && is_cfa_raw(masterDark))
    {
        std::array<std::vector<double>, kCfaPhaseCount> phaseRatios;
        std::vector<double> allRatios;
        allRatios.reserve(256);
        for (size_t idx = 0; idx < hotPixelMask.size(); ++idx)
        {
            if (hotPixelMask[idx] == 0)
                continue;

            const int x = static_cast<int>(idx % static_cast<size_t>(biasAdjustedLight.width));
            const int y = static_cast<int>(idx / static_cast<size_t>(biasAdjustedLight.width));
            const size_t phase = cfa_phase_for_index(idx, biasAdjustedLight.width);
            const double darkBaseline = interpolate_cfa_defect(masterDark, hotPixelMask, x, y);
            const double lightBaseline = interpolate_cfa_defect(biasAdjustedLight, hotPixelMask, x, y);
            const double darkSignal = masterDark.raw[idx] - darkBaseline;
            const double lightSignal = biasAdjustedLight.raw[idx] - lightBaseline;
            if (darkSignal <= 1.0e-6 || lightSignal <= 0.0)
                continue;

            const double ratio = lightSignal / darkSignal;
            if (!std::isfinite(ratio) || ratio < 0.2 || ratio > 6.0)
                continue;

            phaseRatios[phase].push_back(ratio);
            allRatios.push_back(ratio);
        }

        const double globalScale = robust_dark_scale_from_ratios(allRatios);
        size_t usedSamples = 0;
        for (size_t phase = 0; phase < kCfaPhaseCount; ++phase)
        {
            usedSamples += phaseRatios[phase].size();
            profile.cfaScales[phase] = (phaseRatios[phase].size() >= 8)
                ? robust_dark_scale_from_ratios(phaseRatios[phase])
                : globalScale;
        }
        profile.sampleCount = usedSamples;
        profile.enabled = usedSamples >= 8;
        return profile;
    }

    if (is_rgb_planar(biasAdjustedLight) && is_rgb_planar(masterDark))
    {
        std::array<std::vector<double>, kRgbChannelCount> channelRatios;
        std::vector<double> allRatios;
        allRatios.reserve(256);
        const size_t planeSize = static_cast<size_t>(biasAdjustedLight.width) * static_cast<size_t>(biasAdjustedLight.height);
        for (size_t idx = 0; idx < hotPixelMask.size(); ++idx)
        {
            if (hotPixelMask[idx] == 0)
                continue;

            const size_t channel = idx / planeSize;
            const size_t pixelIndex = idx % planeSize;
            const int x = static_cast<int>(pixelIndex % static_cast<size_t>(biasAdjustedLight.width));
            const int y = static_cast<int>(pixelIndex / static_cast<size_t>(biasAdjustedLight.width));
            const double darkBaseline = interpolate_rgb_defect(masterDark, hotPixelMask, x, y, channel);
            const double lightBaseline = interpolate_rgb_defect(biasAdjustedLight, hotPixelMask, x, y, channel);
            const double darkSignal = masterDark.raw[idx] - darkBaseline;
            const double lightSignal = biasAdjustedLight.raw[idx] - lightBaseline;
            if (darkSignal <= 1.0e-6 || lightSignal <= 0.0)
                continue;

            const double ratio = lightSignal / darkSignal;
            if (!std::isfinite(ratio) || ratio < 0.2 || ratio > 6.0)
                continue;

            channelRatios[channel].push_back(ratio);
            allRatios.push_back(ratio);
        }

        const double globalScale = robust_dark_scale_from_ratios(allRatios);
        size_t usedSamples = 0;
        for (size_t channel = 0; channel < kRgbChannelCount; ++channel)
        {
            usedSamples += channelRatios[channel].size();
            profile.rgbScales[channel] = (channelRatios[channel].size() >= 8)
                ? robust_dark_scale_from_ratios(channelRatios[channel])
                : globalScale;
        }
        profile.sampleCount = usedSamples;
        profile.enabled = usedSamples >= 8;
        return profile;
    }

    std::vector<double> ratios;
    ratios.reserve(256);
    for (size_t idx = 0; idx < hotPixelMask.size(); ++idx)
    {
        if (hotPixelMask[idx] == 0)
            continue;

        const int x = static_cast<int>(idx % static_cast<size_t>(biasAdjustedLight.width));
        const int y = static_cast<int>(idx / static_cast<size_t>(biasAdjustedLight.width));
        const double darkBaseline = interpolate_gray_defect(masterDark, hotPixelMask, x, y);
        const double lightBaseline = interpolate_gray_defect(biasAdjustedLight, hotPixelMask, x, y);
        const double darkSignal = masterDark.raw[idx] - darkBaseline;
        const double lightSignal = biasAdjustedLight.raw[idx] - lightBaseline;
        if (darkSignal <= 1.0e-6 || lightSignal <= 0.0)
            continue;

        const double ratio = lightSignal / darkSignal;
        if (!std::isfinite(ratio) || ratio < 0.2 || ratio > 6.0)
            continue;
        ratios.push_back(ratio);
    }

    profile.sampleCount = ratios.size();
    profile.monoScale = robust_dark_scale_from_ratios(ratios);
    profile.enabled = profile.sampleCount >= 8;
    return profile;
}

static double dark_scale_for_index(const FitsImage& image,
                                   const DarkOptimizationProfile& profile,
                                   size_t idx)
{
    if (!profile.enabled)
        return 1.0;

    if (is_cfa_raw(image))
        return profile.cfaScales[cfa_phase_for_index(idx, image.width)];

    if (is_rgb_planar(image))
    {
        const size_t planeSize = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
        const size_t channel = idx / planeSize;
        return (channel < kRgbChannelCount) ? profile.rgbScales[channel] : 1.0;
    }

    return profile.monoScale;
}

static double background_target_for_cfa_phase(BackgroundCalibrationMode mode,
                                              const FitsImage& image,
                                              const CfaStats& referenceStats,
                                              const CfaStats& currentStats,
                                              size_t phase)
{
    switch (mode)
    {
    case BackgroundCalibrationMode::None:
        return currentStats.phases[phase].median;
    case BackgroundCalibrationMode::PerChannel:
        return referenceStats.phases[phase].median;
    case BackgroundCalibrationMode::RgbChannels:
        return rgb_neutral_background_from_cfa(image, referenceStats);
    }
    return referenceStats.phases[phase].median;
}

static double background_target_for_rgb_channel(BackgroundCalibrationMode mode,
                                                const RgbStats& referenceStats,
                                                const RgbStats& currentStats,
                                                size_t channel)
{
    switch (mode)
    {
    case BackgroundCalibrationMode::None:
        return currentStats.channels[channel].median;
    case BackgroundCalibrationMode::PerChannel:
        return referenceStats.channels[channel].median;
    case BackgroundCalibrationMode::RgbChannels:
    {
        const double neutral = std::min({referenceStats.channels[0].median,
                                         referenceStats.channels[1].median,
                                         referenceStats.channels[2].median});
        return neutral;
    }
    }
    return referenceStats.channels[channel].median;
}

static double weighted_average(const std::vector<double>& values,
                               const std::vector<double>& weights)
{
    if (values.empty())
        return 0.0;

    double weightedSum = 0.0;
    double weightSum = 0.0;
    for (size_t i = 0; i < values.size(); ++i)
    {
        const double w = (i < weights.size() && weights[i] > 0.0) ? weights[i] : 1.0;
        weightedSum += values[i] * w;
        weightSum += w;
    }

    if (weightSum <= 0.0)
        return values.front();
    return weightedSum / weightSum;
}

static double sample_weight_at(const std::vector<double>& weights, size_t index)
{
    return (index < weights.size() && weights[index] > 0.0) ? weights[index] : 1.0;
}

static void winsorized_mean_sigma(const std::vector<double>& values,
                                  double& center,
                                  double& sigma)
{
    if (values.empty())
    {
        center = 0.0;
        sigma = 1.0;
        return;
    }

    center = median_of(values);
    sigma = robust_sigma_from_values(values, center);

    constexpr double kHuber = 1.5;
    constexpr double kScale = 1.134;
    constexpr double kConvergence = 5.0e-4;

    std::vector<double> wins(values.size());
    for (int iter = 0; iter < 8; ++iter)
    {
        const double t0 = center - kHuber * sigma;
        const double t1 = center + kHuber * sigma;

        for (size_t i = 0; i < values.size(); ++i)
            wins[i] = std::clamp(values[i], t0, t1);

        double mean = std::accumulate(wins.begin(), wins.end(), 0.0) /
                      static_cast<double>(wins.size());

        double var = 0.0;
        for (double v : wins)
        {
            const double d = v - mean;
            var += d * d;
        }
        var /= static_cast<double>(wins.size());

        const double sigmaNew = std::max(std::sqrt(var) * kScale, 1.0e-12);
        const double rel = std::abs(sigmaNew - sigma) / std::max(sigma, 1.0e-12);

        center = mean;
        sigma = sigmaNew;
        if (rel <= kConvergence)
            break;
    }
}

static double auto_adaptive_weighted_average(const std::vector<double>& values,
                                             const std::vector<double>& weights,
                                             int iterations)
{
    if (values.empty())
        return 0.0;
    if (values.size() < 3 || iterations <= 0)
        return weighted_average(values, weights);

    double runningAverage = weighted_average(values, weights);
    constexpr double kMinVariance = 1.0e-12;
    constexpr double kConvergence = 1.0e-5;

    for (int iter = 0; iter < iterations; ++iter)
    {
        double variance = 0.0;
        for (double value : values)
        {
            const double delta = value - runningAverage;
            variance += delta * delta;
        }
        variance /= static_cast<double>(values.size());

        if (variance <= kMinVariance)
            return runningAverage;

        double weightedSum = 0.0;
        double weightSum = 0.0;
        for (size_t i = 0; i < values.size(); ++i)
        {
            const double delta = values[i] - runningAverage;
            const double adaptiveWeight = variance / (variance + delta * delta);
            const double totalWeight = sample_weight_at(weights, i) * adaptiveWeight;
            weightedSum += values[i] * totalWeight;
            weightSum += totalWeight;
        }

        if (weightSum <= 0.0)
            return runningAverage;

        const double nextAverage = weightedSum / weightSum;
        const double rel = std::abs(nextAverage - runningAverage) /
                           std::max(std::abs(runningAverage), 1.0);
        runningAverage = nextAverage;
        if (rel <= kConvergence)
            break;
    }

    return runningAverage;
}

static double sigma_clip_average(const std::vector<double>& values,
                                 const std::vector<double>& weights,
                                 const RejectConfig& rejectConfig,
                                 size_t& rejectedSamples)
{
    std::vector<double> keptValues = values;
    std::vector<double> keptWeights(values.size(), 1.0);
    for (size_t i = 0; i < keptWeights.size(); ++i)
        keptWeights[i] = sample_weight_at(weights, i);

    const size_t minSamples = static_cast<size_t>(std::max(rejectConfig.minSamples, 3));
    for (int iter = 0; iter < std::max(rejectConfig.iterations, 1); ++iter)
    {
        if (keptValues.size() < minSamples)
            break;

        double center = 0.0;
        double sigma = 1.0;
        winsorized_mean_sigma(keptValues, center, sigma);

        const double low = center - rejectConfig.sigmaLow * sigma;
        const double high = center + rejectConfig.sigmaHigh * sigma;

        std::vector<double> nextValues;
        std::vector<double> nextWeights;
        nextValues.reserve(keptValues.size());
        nextWeights.reserve(keptWeights.size());

        bool rejectedThisRound = false;
        for (size_t i = 0; i < keptValues.size(); ++i)
        {
            if (keptValues[i] < low || keptValues[i] > high)
            {
                ++rejectedSamples;
                rejectedThisRound = true;
                continue;
            }

            nextValues.push_back(keptValues[i]);
            nextWeights.push_back(keptWeights[i]);
        }

        if (!rejectedThisRound || nextValues.size() < static_cast<size_t>(std::max(rejectConfig.minSamples, 1)))
            break;

        keptValues.swap(nextValues);
        keptWeights.swap(nextWeights);
    }

    if (keptValues.size() < static_cast<size_t>(std::max(rejectConfig.minSamples, 1)))
        return weighted_average(values, weights);

    return weighted_average(keptValues, keptWeights);
}

static double integrate_pixel_stack(const std::vector<double>& values,
                                    const std::vector<double>& weights,
                                    const RejectConfig& rejectConfig,
                                    size_t& rejectedSamples,
                                    PixelIntegrationStats* outStats = nullptr)
{
    if (values.empty())
        return 0.0;

    std::vector<double> filteredValues;
    std::vector<double> filteredWeights;
    size_t nonZeroCount = 0;
    for (double value : values)
    {
        if (value != 0.0)
            ++nonZeroCount;
    }
    const bool ignoreZeroSamples = nonZeroCount > 0 && nonZeroCount < values.size();
    if (ignoreZeroSamples)
    {
        filteredValues.reserve(nonZeroCount);
        filteredWeights.reserve(nonZeroCount);
        for (size_t i = 0; i < values.size(); ++i)
        {
            if (values[i] == 0.0)
                continue;
            filteredValues.push_back(values[i]);
            filteredWeights.push_back(sample_weight_at(weights, i));
        }
    }

    const std::vector<double>& activeValues = ignoreZeroSamples ? filteredValues : values;
    const std::vector<double>& activeWeights = ignoreZeroSamples ? filteredWeights : weights;
    const double activeWeightSum = std::accumulate(activeWeights.begin(), activeWeights.end(), 0.0);
    const double effectiveSamples = effective_sample_count_from_weights(activeWeights);
    if (outStats)
    {
        outStats->activeWeightSum = activeWeightSum;
        outStats->effectiveSamples = effectiveSamples;
        outStats->activeSamples = activeValues.size();
    }
    const size_t minSamples = static_cast<size_t>(std::max(rejectConfig.minSamples, 3));
    if (activeValues.size() < minSamples || effectiveSamples + 1.0e-6 < static_cast<double>(minSamples))
    {
        if (outStats)
            outStats->coverageFallback = true;
        return weighted_average(activeValues, activeWeights);
    }

    switch (rejectConfig.method)
    {
    case RejectConfig::Method::None:
        return weighted_average(activeValues, activeWeights);
    case RejectConfig::Method::SigmaClip:
        return sigma_clip_average(activeValues, activeWeights, rejectConfig, rejectedSamples);
    case RejectConfig::Method::AutoAdaptiveWeightedAverage:
        return auto_adaptive_weighted_average(activeValues, activeWeights, rejectConfig.iterations);
    }

    return weighted_average(activeValues, activeWeights);
}

static bool integrate_frames(const std::vector<const FitsImage*>& frames,
                             const std::vector<double>& frameWeights,
                             const RejectConfig& rejectConfig,
                             FitsImage& outImage,
                             size_t& rejectedSamples)
{
    if (frames.empty() || !frames.front())
        return false;

    const FitsImage& ref = *frames.front();
    const size_t n = ref.raw.size();
    outImage = ref;
    outImage.raw.assign(n, 0.0);

    std::vector<double> pixelStack;
    pixelStack.reserve(frames.size());

    for (size_t idx = 0; idx < n; ++idx)
    {
        pixelStack.clear();
        for (const FitsImage* frame : frames)
            pixelStack.push_back(frame->raw[idx]);

        outImage.raw[idx] = integrate_pixel_stack(pixelStack, frameWeights, rejectConfig, rejectedSamples);
    }

    return true;
}

static AffineNormalization compute_affine_normalization(const ImageStats& referenceStats,
                                                        const ImageStats& currentStats)
{
    AffineNormalization normalization;
    const double currentScale = std::max(currentStats.signalScale, 1.0e-6);
    const double referenceScale = std::max(referenceStats.signalScale, 1.0e-6);

    normalization.scale = referenceScale / currentScale;
    normalization.scale = std::clamp(normalization.scale, 0.5, 2.0);
    normalization.offset = currentStats.median;
    return normalization;
}

static CfaNormalization compute_cfa_normalization(const CfaStats& referenceStats,
                                                  const CfaStats& currentStats)
{
    CfaNormalization normalization;
    for (size_t phase = 0; phase < kCfaPhaseCount; ++phase)
        normalization.phases[phase] =
            compute_affine_normalization(referenceStats.phases[phase], currentStats.phases[phase]);
    return normalization;
}

static RgbNormalization compute_rgb_normalization(const RgbStats& referenceStats,
                                                  const RgbStats& currentStats)
{
    RgbNormalization normalization;
    for (size_t channel = 0; channel < kRgbChannelCount; ++channel)
        normalization.channels[channel] =
            compute_affine_normalization(referenceStats.channels[channel], currentStats.channels[channel]);
    return normalization;
}

static RejectConfig master_reject_config(const RejectConfig& baseConfig, size_t frameCount)
{
    RejectConfig config = baseConfig;
    config.minSamples = std::min<int>(std::max(config.minSamples, 3), static_cast<int>(std::max<size_t>(frameCount, 1)));

    if (frameCount < 5)
    {
        config.method = RejectConfig::Method::None;
        config.iterations = 1;
        return config;
    }

    if (config.method == RejectConfig::Method::AutoAdaptiveWeightedAverage)
        config.method = RejectConfig::Method::SigmaClip;

    if (frameCount >= 15)
        config.iterations = std::max(config.iterations, 5);
    else
        config.iterations = std::max(config.iterations, 3);

    return config;
}

static int clamp_index(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

} // namespace

StackCore::StackCore() = default;
StackCore::~StackCore() = default;

void StackCore::reset()
{
    _frames.clear();
    _masterBias.reset();
    _masterDark.reset();
    _masterFlat.reset();
    _hotPixelMap.clear();
    _lineDefectMap.clear();
    _coverageMap.clear();
    _effectiveSamplesMap.clear();
    _log.clear();
}

bool StackCore::addFrame(const std::string& path, FrameType type)
{
    FrameEntry ent;
    ent.path = path;
    ent.type = type;
    _frames.push_back(std::move(ent));
    return true;
}

void StackCore::addFrames(const std::vector<std::string>& paths, FrameType type)
{
    for (const auto& p : paths)
        addFrame(p, type);
}

bool StackCore::loadFrame(FrameEntry& entry)
{
    if (entry.image)
        return true;

    auto img = std::make_unique<FitsImage>();
    FitsImage tmp;
    if (!load_fits(entry.path, tmp, _bayerPattern))
    {
        std::ostringstream oss;
        oss << "load_fits failed: " << entry.path << "\n";
        _log += oss.str();
        return false;
    }

    *img = std::move(tmp);
    entry.image = std::move(img);
    return true;
}

bool StackCore::validateCompatible(const FitsImage& ref,
                                   const FitsImage& other,
                                   const std::string& what)
{
    if (ref.width != other.width || ref.height != other.height)
    {
        std::ostringstream oss;
        oss << "Geometry mismatch in " << what
            << ": ref=" << ref.width << "x" << ref.height
            << ", got=" << other.width << "x" << other.height << "\n";
        _log += oss.str();
        return false;
    }

    if (ref.raw.size() != other.raw.size())
    {
        std::ostringstream oss;
        oss << "Raw buffer mismatch in " << what
            << ": ref=" << ref.raw.size()
            << ", got=" << other.raw.size() << "\n";
        _log += oss.str();
        return false;
    }

    if (ref.channels != other.channels)
    {
        std::ostringstream oss;
        oss << "Channel mismatch in " << what
            << ": ref=" << ref.channels
            << ", got=" << other.channels << "\n";
        _log += oss.str();
        return false;
    }

    if (ref.channels == 1 && ref.bayer != other.bayer)
    {
        std::ostringstream oss;
        oss << "Bayer pattern mismatch in " << what
            << ": ref=" << static_cast<int>(ref.bayer)
            << ", got=" << static_cast<int>(other.bayer) << "\n";
        _log += oss.str();
        return false;
    }

    if (ref.raw.empty() || other.raw.empty())
    {
        std::ostringstream oss;
        oss << "Empty raw buffer in " << what << "\n";
        _log += oss.str();
        return false;
    }

    return true;
}

bool StackCore::buildMasters()
{
    _masterBias.reset();
    _masterDark.reset();
    _masterFlat.reset();
    _hotPixelMap.clear();
    _lineDefectMap.clear();
    _coverageMap.clear();
    _effectiveSamplesMap.clear();

    bool ok = true;
    if (_calibCfg.useBias)
        ok = ok && buildMasterBias();
    if (_calibCfg.useDark)
        ok = ok && buildMasterDark();
    if (_calibCfg.useFlat)
        ok = ok && buildMasterFlat();
    return ok;
}

bool StackCore::buildMasterBias()
{
    std::vector<const FitsImage*> biasFrames;
    for (auto& f : _frames)
    {
        if (f.type != FrameType::Bias)
            continue;
        if (!loadFrame(f))
            return false;
        biasFrames.push_back(f.image.get());
    }

    if (biasFrames.empty())
    {
        _log += "Skip Master Bias: no bias frames.\n";
        return true;
    }

    const FitsImage& ref = *biasFrames.front();
    for (size_t i = 1; i < biasFrames.size(); ++i)
    {
        if (!validateCompatible(ref, *biasFrames[i], "bias frames"))
            return false;
    }

    auto out = std::make_unique<FitsImage>();
    size_t rejectedSamples = 0;
    const std::vector<double> unitWeights(biasFrames.size(), 1.0);
    const RejectConfig effectiveReject = master_reject_config(_rejCfg, biasFrames.size());
    if (!integrate_frames(biasFrames, unitWeights, effectiveReject, *out, rejectedSamples))
        return false;

    _masterBias = std::move(out);
    std::ostringstream oss;
    oss << "Master Bias built";
    if (!biasFrames.empty())
    {
        const double total = static_cast<double>(ref.raw.size()) * static_cast<double>(biasFrames.size());
        const double rate = (total > 0.0) ? (100.0 * static_cast<double>(rejectedSamples) / total) : 0.0;
        oss << " (rejected " << rate << "%)";
    }
    oss << ".\n";
    _log += oss.str();
    return true;
}

bool StackCore::buildMasterDark()
{
    std::vector<const FitsImage*> darkFrames;
    for (auto& f : _frames)
    {
        if (f.type != FrameType::Dark)
            continue;
        if (!loadFrame(f))
            return false;
        darkFrames.push_back(f.image.get());
    }

    if (darkFrames.empty())
    {
        _log += "Skip Master Dark: no dark frames.\n";
        return true;
    }

    const FitsImage& ref = *darkFrames.front();
    for (size_t i = 1; i < darkFrames.size(); ++i)
    {
        if (!validateCompatible(ref, *darkFrames[i], "dark frames"))
            return false;
    }

    if (_calibCfg.useBias && _masterBias && !validateCompatible(ref, *_masterBias, "dark vs master bias"))
        return false;

    const size_t n = ref.raw.size();
    std::vector<FitsImage> correctedDarks(darkFrames.size());
    std::vector<const FitsImage*> correctedPtrs;
    correctedPtrs.reserve(darkFrames.size());

    for (size_t i = 0; i < darkFrames.size(); ++i)
    {
        correctedDarks[i] = *darkFrames[i];
        for (size_t idx = 0; idx < n; ++idx)
        {
            if (_calibCfg.useBias && _masterBias)
                correctedDarks[i].raw[idx] -= _masterBias->raw[idx];
        }
        correctedPtrs.push_back(&correctedDarks[i]);
    }

    auto out = std::make_unique<FitsImage>();
    size_t rejectedSamples = 0;
    const std::vector<double> unitWeights(correctedPtrs.size(), 1.0);
    const RejectConfig effectiveReject = master_reject_config(_rejCfg, correctedPtrs.size());
    if (!integrate_frames(correctedPtrs, unitWeights, effectiveReject, *out, rejectedSamples))
        return false;

    _masterDark = std::move(out);
    if (_masterDark)
    {
        _hotPixelMap = detect_hot_pixels(*_masterDark);
        const LineDefectMap lineDefects = detect_line_defects(*_masterDark, _hotPixelMap);
        _lineDefectMap = lineDefects.mask;
        std::ostringstream defectLog;
        defectLog << "Hot pixel map built from master dark: "
                  << count_masked_pixels(_hotPixelMap) << " defective pixel(s).\n";
        if (lineDefects.badColumns > 0 || lineDefects.badRows > 0)
        {
            defectLog << "Line defect map built from master dark: "
                      << lineDefects.badColumns << " bad column(s), "
                      << lineDefects.badRows << " bad row(s), "
                      << count_masked_pixels(_lineDefectMap) << " defective sample(s).\n";
        }
        _log += defectLog.str();
    }
    std::ostringstream oss;
    oss << "Master Dark built";
    if (!darkFrames.empty())
    {
        const double total = static_cast<double>(ref.raw.size()) * static_cast<double>(darkFrames.size());
        const double rate = (total > 0.0) ? (100.0 * static_cast<double>(rejectedSamples) / total) : 0.0;
        oss << " (rejected " << rate << "%)";
    }
    oss << ".\n";
    _log += oss.str();
    return true;
}

bool StackCore::buildMasterFlat()
{
    std::vector<const FitsImage*> flatFrames;
    for (auto& f : _frames)
    {
        if (f.type != FrameType::Flat)
            continue;
        if (!loadFrame(f))
            return false;
        flatFrames.push_back(f.image.get());
    }

    if (flatFrames.empty())
    {
        _log += "Skip Master Flat: no flat frames.\n";
        return true;
    }

    const FitsImage& ref = *flatFrames.front();
    for (size_t i = 1; i < flatFrames.size(); ++i)
    {
        if (!validateCompatible(ref, *flatFrames[i], "flat frames"))
            return false;
    }

    if (_calibCfg.useBias && _masterBias && !validateCompatible(ref, *_masterBias, "flat vs master bias"))
        return false;
    if (_calibCfg.useDark && _masterDark && !validateCompatible(ref, *_masterDark, "flat vs master dark"))
        return false;

    const size_t n = ref.raw.size();
    auto out = std::make_unique<FitsImage>();
    out->width = ref.width;
    out->height = ref.height;
    out->channels = ref.channels;
    out->bayer = ref.bayer;
    out->raw.resize(n);

    const bool cfaAwareFlat = is_cfa_raw(ref);
    std::vector<double> perFlatNorm(flatFrames.size(), 1.0);
    std::vector<std::array<double, kCfaPhaseCount>> perFlatPhaseNorm(flatFrames.size());
    for (size_t i = 0; i < flatFrames.size(); ++i)
    {
        double sum = 0.0;
        std::vector<double> correctedValues(n, 0.0);
        for (size_t idx = 0; idx < n; ++idx)
        {
            double v = flatFrames[i]->raw[idx];
            if (_calibCfg.useBias && _masterBias)
                v -= _masterBias->raw[idx];
            if (_calibCfg.useDark && _masterDark)
                v -= _masterDark->raw[idx];
            correctedValues[idx] = v;
            sum += v;
        }

        double mean = sum / static_cast<double>(n);
        if (std::abs(mean) < 1e-12)
        {
            _log += "Flat frame mean is near zero; fallback normalization factor applied.\n";
            mean = 1.0;
        }
        perFlatNorm[i] = mean;
        perFlatPhaseNorm[i] = cfaAwareFlat ? compute_phase_means(correctedValues, ref.width)
                                           : std::array<double, kCfaPhaseCount>{mean, mean, mean, mean};
    }

    std::vector<FitsImage> normalizedFlats(flatFrames.size());
    std::vector<const FitsImage*> normalizedPtrs;
    normalizedPtrs.reserve(flatFrames.size());
    for (size_t i = 0; i < flatFrames.size(); ++i)
    {
        normalizedFlats[i] = *flatFrames[i];
        for (size_t idx = 0; idx < n; ++idx)
        {
            double v = flatFrames[i]->raw[idx];
            if (_calibCfg.useBias && _masterBias)
                v -= _masterBias->raw[idx];
            if (_calibCfg.useDark && _masterDark)
                v -= _masterDark->raw[idx];
            const double denom = cfaAwareFlat
                ? perFlatPhaseNorm[i][cfa_phase_for_index(idx, ref.width)]
                : perFlatNorm[i];
            normalizedFlats[i].raw[idx] = v / denom;
        }
        normalizedPtrs.push_back(&normalizedFlats[i]);
    }

    size_t rejectedSamples = 0;
    const std::vector<double> unitWeights(normalizedPtrs.size(), 1.0);
    const RejectConfig effectiveReject = master_reject_config(_rejCfg, normalizedPtrs.size());
    if (!integrate_frames(normalizedPtrs, unitWeights, effectiveReject, *out, rejectedSamples))
        return false;

    if (cfaAwareFlat)
    {
        const auto phaseMeans = compute_phase_means(out->raw, ref.width);
        for (size_t idx = 0; idx < n; ++idx)
        {
            const double phaseMean = phaseMeans[cfa_phase_for_index(idx, ref.width)];
            if (std::abs(phaseMean) > 1.0e-12)
                out->raw[idx] /= phaseMean;
        }
    }
    else
    {
        double sum = 0.0;
        for (double v : out->raw)
            sum += v;

        double mean = sum / static_cast<double>(n);
        if (std::abs(mean) < 1e-12)
        {
            _log += "Master Flat mean is near zero; skip final normalization.\n";
        }
        else
        {
            for (double& v : out->raw)
                v /= mean;
        }
    }

    _masterFlat = std::move(out);
    std::ostringstream oss;
    oss << "Master Flat built";
    if (!flatFrames.empty())
    {
        const double total = static_cast<double>(ref.raw.size()) * static_cast<double>(flatFrames.size());
        const double rate = (total > 0.0) ? (100.0 * static_cast<double>(rejectedSamples) / total) : 0.0;
        oss << " (rejected " << rate << "%)";
    }
    oss << ".\n";
    _log += oss.str();
    return true;
}

bool StackCore::calibrateLight(const FitsImage& inLight, FitsImage& outCalib)
{
    if (inLight.raw.empty())
    {
        _log += "Light frame has empty raw buffer.\n";
        return false;
    }

    if (_calibCfg.useBias && _masterBias && !validateCompatible(inLight, *_masterBias, "light vs master bias"))
        return false;
    if (_calibCfg.useDark && _masterDark && !validateCompatible(inLight, *_masterDark, "light vs master dark"))
        return false;
    if (_calibCfg.useFlat && _masterFlat && !validateCompatible(inLight, *_masterFlat, "light vs master flat"))
        return false;

    const size_t n = inLight.raw.size();
    outCalib = inLight;
    outCalib.raw.resize(n);

    DarkOptimizationProfile darkOptimization;
    if (_calibCfg.useDark && _calibCfg.optimizeDark && _masterDark && !_hotPixelMap.empty())
    {
        FitsImage biasAdjustedLight = inLight;
        if (_calibCfg.useBias && _masterBias)
        {
            for (size_t idx = 0; idx < n; ++idx)
                biasAdjustedLight.raw[idx] -= _masterBias->raw[idx];
        }
        darkOptimization = compute_dark_optimization_profile(biasAdjustedLight, *_masterDark, _hotPixelMap);
    }

    for (size_t idx = 0; idx < n; ++idx)
    {
        double v = inLight.raw[idx];

        if (_calibCfg.useBias && _masterBias)
            v -= _masterBias->raw[idx];
        if (_calibCfg.useDark && _masterDark)
            v -= _masterDark->raw[idx] * dark_scale_for_index(inLight, darkOptimization, idx);
        if (_calibCfg.useFlat && _masterFlat)
        {
            const double flat = _masterFlat->raw[idx];
            if (std::abs(flat) > 1e-12)
                v /= flat;
        }

        outCalib.raw[idx] = v;
    }

    if (darkOptimization.enabled)
    {
        std::ostringstream oss;
        oss << "Dark optimization: ";
        if (is_cfa_raw(inLight))
        {
            oss << "scales=["
                << darkOptimization.cfaScales[0] << ", "
                << darkOptimization.cfaScales[1] << ", "
                << darkOptimization.cfaScales[2] << ", "
                << darkOptimization.cfaScales[3] << "]";
        }
        else if (is_rgb_planar(inLight))
        {
            oss << "scales=["
                << darkOptimization.rgbScales[0] << ", "
                << darkOptimization.rgbScales[1] << ", "
                << darkOptimization.rgbScales[2] << "]";
        }
        else
        {
            oss << "scale=" << darkOptimization.monoScale;
        }
        oss << ", hot samples=" << darkOptimization.sampleCount << ".\n";
        _log += oss.str();
    }

    if (_calibCfg.removeLineDefects && !_lineDefectMap.empty())
    {
        const size_t correctedSamples = correct_hot_pixels(outCalib, _lineDefectMap);
        if (correctedSamples > 0)
        {
            std::ostringstream oss;
            oss << "Bad column/row correction applied: " << correctedSamples << " sample(s).\n";
            _log += oss.str();
        }
    }

    if (_calibCfg.removeHotPixels && !_hotPixelMap.empty())
    {
        const size_t correctedPixels = correct_hot_pixels(outCalib, _hotPixelMap);
        if (correctedPixels > 0)
        {
            std::ostringstream oss;
            oss << "Hot pixel correction applied: " << correctedPixels << " pixel(s).\n";
            _log += oss.str();
        }
    }

    return true;
}

bool StackCore::combineLights(const std::vector<FitsImage>& calibratedLights,
                              const std::vector<double>& frameWeights,
                              FitsImage& outCombined)
{
    if (calibratedLights.empty())
        return false;

    const FitsImage& ref = calibratedLights.front();
    for (size_t i = 1; i < calibratedLights.size(); ++i)
    {
        if (!validateCompatible(ref, calibratedLights[i], "calibrated lights"))
            return false;
    }

    const bool cfaAware = is_cfa_raw(ref);
    const bool rgbAware = is_rgb_planar(ref);
    std::vector<ImageStats> stats(calibratedLights.size());
    std::vector<CfaStats> cfaStats(calibratedLights.size());
    std::vector<RgbStats> rgbStats(calibratedLights.size());
    std::vector<double> medians;
    std::vector<double> signalScales;
    medians.reserve(calibratedLights.size());
    signalScales.reserve(calibratedLights.size());
    std::array<std::vector<double>, kCfaPhaseCount> phaseMedians;
    std::array<std::vector<double>, kCfaPhaseCount> phaseSignalScales;
    for (size_t phase = 0; phase < kCfaPhaseCount; ++phase)
    {
        phaseMedians[phase].reserve(calibratedLights.size());
        phaseSignalScales[phase].reserve(calibratedLights.size());
    }

    for (size_t i = 0; i < calibratedLights.size(); ++i)
    {
        if (cfaAware)
        {
            cfaStats[i] = compute_cfa_stats(calibratedLights[i]);
            for (size_t phase = 0; phase < kCfaPhaseCount; ++phase)
            {
                phaseMedians[phase].push_back(cfaStats[i].phases[phase].median);
                phaseSignalScales[phase].push_back(cfaStats[i].phases[phase].signalScale);
            }
        }
        else if (rgbAware)
        {
            rgbStats[i] = compute_rgb_stats(calibratedLights[i]);
            stats[i] = compute_image_stats(calibratedLights[i]);
            medians.push_back(stats[i].median);
            signalScales.push_back(stats[i].signalScale);
        }
        else
        {
            stats[i] = compute_image_stats(calibratedLights[i]);
            medians.push_back(stats[i].median);
            signalScales.push_back(stats[i].signalScale);
        }
    }

    const double referenceMedian = cfaAware ? 0.0 : median_of(medians);
    ImageStats referenceStats = stats.front();
    referenceStats.median = referenceMedian;
    referenceStats.signalScale = cfaAware ? 1.0 : median_of(signalScales);
    CfaStats referenceCfaStats;
    RgbStats referenceRgbStats;
    if (cfaAware)
    {
        for (size_t phase = 0; phase < kCfaPhaseCount; ++phase)
        {
            referenceCfaStats.phases[phase] = cfaStats.front().phases[phase];
            referenceCfaStats.phases[phase].median = median_of(phaseMedians[phase]);
            referenceCfaStats.phases[phase].signalScale = median_of(phaseSignalScales[phase]);
        }
    }
    else if (rgbAware)
    {
        for (size_t channel = 0; channel < kRgbChannelCount; ++channel)
        {
            std::vector<double> channelMedians;
            std::vector<double> channelScales;
            channelMedians.reserve(calibratedLights.size());
            channelScales.reserve(calibratedLights.size());
            for (size_t i = 0; i < calibratedLights.size(); ++i)
            {
                channelMedians.push_back(rgbStats[i].channels[channel].median);
                channelScales.push_back(rgbStats[i].channels[channel].signalScale);
            }
            referenceRgbStats.channels[channel] = rgbStats.front().channels[channel];
            referenceRgbStats.channels[channel].median = median_of(channelMedians);
            referenceRgbStats.channels[channel].signalScale = median_of(channelScales);
        }
    }
    const size_t n = ref.raw.size();
    outCombined = ref;
    outCombined.raw.assign(n, 0.0);

    std::vector<AffineNormalization> normalizations(calibratedLights.size());
    std::vector<CfaNormalization> cfaNormalizations(calibratedLights.size());
    std::vector<RgbNormalization> rgbNormalizations(calibratedLights.size());
    std::vector<double> normalizedWeights(calibratedLights.size(), 1.0);
    for (size_t i = 0; i < calibratedLights.size(); ++i)
    {
        if (cfaAware)
            cfaNormalizations[i] = compute_cfa_normalization(referenceCfaStats, cfaStats[i]);
        else if (rgbAware)
            rgbNormalizations[i] = compute_rgb_normalization(referenceRgbStats, rgbStats[i]);
        else
            normalizations[i] = compute_affine_normalization(referenceStats, stats[i]);
        const double explicitWeight = (i < frameWeights.size() && frameWeights[i] > 0.0) ? frameWeights[i] : 1.0;
        double scaleForWeight = 1.0;
        if (cfaAware)
        {
            double phaseScaleSum = 0.0;
            for (size_t phase = 0; phase < kCfaPhaseCount; ++phase)
                phaseScaleSum += cfaNormalizations[i].phases[phase].scale;
            scaleForWeight = phaseScaleSum / static_cast<double>(kCfaPhaseCount);
        }
        else if (rgbAware)
        {
            double channelScaleSum = 0.0;
            for (size_t channel = 0; channel < kRgbChannelCount; ++channel)
                channelScaleSum += rgbNormalizations[i].channels[channel].scale;
            scaleForWeight = channelScaleSum / static_cast<double>(kRgbChannelCount);
        }
        else
        {
            scaleForWeight = normalizations[i].scale;
        }
        normalizedWeights[i] = explicitWeight /
                               std::max(scaleForWeight * scaleForWeight, 1.0e-6);
    }

    std::vector<double> pixelStack;
    pixelStack.reserve(calibratedLights.size());
    size_t rejectedSamples = 0;
    const double totalFrameWeight = std::accumulate(normalizedWeights.begin(), normalizedWeights.end(), 0.0);
    _coverageMap.assign(n, 0.0f);
    _effectiveSamplesMap.assign(n, 0.0f);
    double coverageSum = 0.0;
    double effectiveSampleSum = 0.0;
    size_t lowCoveragePixels = 0;
    size_t fallbackPixels = 0;

    for (size_t idx = 0; idx < n; ++idx)
    {
        pixelStack.clear();
        const size_t phase = cfaAware ? cfa_phase_for_index(idx, ref.width) : 0;
        const size_t channel = rgbAware
            ? (idx / (static_cast<size_t>(ref.width) * static_cast<size_t>(ref.height)))
            : 0;
        for (size_t i = 0; i < calibratedLights.size(); ++i)
        {
            double normalized = calibratedLights[i].raw[idx];
            if (cfaAware)
            {
                const AffineNormalization& norm = cfaNormalizations[i].phases[phase];
                const double backgroundTarget = background_target_for_cfa_phase(_calibCfg.backgroundCalibration,
                                                                               ref,
                                                                               referenceCfaStats,
                                                                               cfaStats[i],
                                                                               phase);
                normalized =
                    (normalized - norm.offset) * norm.scale + backgroundTarget;
            }
            else if (rgbAware)
            {
                const AffineNormalization& norm = rgbNormalizations[i].channels[channel];
                const double backgroundTarget = background_target_for_rgb_channel(_calibCfg.backgroundCalibration,
                                                                                  referenceRgbStats,
                                                                                  rgbStats[i],
                                                                                  channel);
                normalized = (normalized - norm.offset) * norm.scale + backgroundTarget;
            }
            else
            {
                const AffineNormalization& norm = normalizations[i];
                const double backgroundTarget =
                    (_calibCfg.backgroundCalibration == BackgroundCalibrationMode::None)
                        ? stats[i].median
                        : referenceMedian;
                normalized = (normalized - norm.offset) * norm.scale + backgroundTarget;
            }
            pixelStack.push_back(normalized);
        }

        PixelIntegrationStats pixelStats;
        outCombined.raw[idx] = integrate_pixel_stack(pixelStack, normalizedWeights, _rejCfg, rejectedSamples, &pixelStats);
        const double coverageFraction =
            (totalFrameWeight > 1.0e-12) ? (pixelStats.activeWeightSum / totalFrameWeight) : 0.0;
        _coverageMap[idx] = static_cast<float>(std::clamp(coverageFraction, 0.0, 1.0));
        _effectiveSamplesMap[idx] = static_cast<float>(std::max(pixelStats.effectiveSamples, 0.0));
        coverageSum += _coverageMap[idx];
        effectiveSampleSum += _effectiveSamplesMap[idx];
        if (coverageFraction < 0.5 || pixelStats.effectiveSamples + 1.0e-6 < static_cast<double>(_rejCfg.minSamples))
            ++lowCoveragePixels;
        if (pixelStats.coverageFallback)
            ++fallbackPixels;
    }

    const double total = static_cast<double>(n) * static_cast<double>(calibratedLights.size());
    const double rate = (total > 0.0) ? (100.0 * static_cast<double>(rejectedSamples) / total) : 0.0;
    std::ostringstream oss;
    oss << "Light integration: affine-normalized weighted average";
    if (_calibCfg.backgroundCalibration == BackgroundCalibrationMode::PerChannel)
        oss << " + DSS per-channel background calibration";
    else if (_calibCfg.backgroundCalibration == BackgroundCalibrationMode::RgbChannels)
        oss << " + DSS RGB background calibration";
    if (_rejCfg.method == RejectConfig::Method::SigmaClip)
    {
        oss << " + iterative sigma clip";
        oss << ", rejected " << rate << "% of samples";
    }
    else if (_rejCfg.method == RejectConfig::Method::AutoAdaptiveWeightedAverage)
    {
        oss << " + DSS auto-adaptive weighted average"
            << " (" << std::max(_rejCfg.iterations, 1) << " iterations)";
    }
    if (!_coverageMap.empty())
    {
        const double pixelCount = static_cast<double>(_coverageMap.size());
        oss << ", avg coverage=" << (coverageSum / pixelCount)
            << ", avg effective samples=" << (effectiveSampleSum / pixelCount)
            << ", low coverage pixels=" << lowCoveragePixels;
        if (fallbackPixels > 0)
            oss << ", coverage fallbacks=" << fallbackPixels;
    }
    oss << ".\n";
    _log += oss.str();
    return true;
}

bool StackCore::runStack(StackResult& outResult, const ProgressCallback& progressCallback)
{
    _log.clear();
    outResult = StackResult{};

    auto report = [&](float progress, const std::string& message) {
        if (progressCallback)
            progressCallback(std::clamp(progress, 0.0f, 1.0f), message);
    };

    report(0.0f, "Preparing stack...");

    int totalLights = 0;
    for (const auto& f : _frames)
    {
        if (f.type == FrameType::Light)
            ++totalLights;
    }

    if (totalLights == 0)
    {
        _log += "No light frames.\n";
        outResult.log = _log;
        return false;
    }

    report(0.05f, "Building calibration masters...");
    if (!buildMasters())
    {
        _log += "buildMasters failed.\n";
        outResult.log = _log;
        return false;
    }

    std::vector<FitsImage> calibratedLights;
    calibratedLights.reserve(static_cast<size_t>(totalLights));
    std::vector<double> frameWeights;
    frameWeights.reserve(static_cast<size_t>(totalLights));
    std::vector<FrameQualityMetrics> frameQualities;
    frameQualities.reserve(static_cast<size_t>(totalLights));

    int used = 0;
    int rejected = 0;
    int qualityRejected = 0;

    report(0.20f, "Calibrating light frames...");
    for (auto& f : _frames)
    {
        if (f.type != FrameType::Light)
            continue;

        if (!loadFrame(f))
        {
            ++rejected;
            continue;
        }

        FitsImage calib;
        if (!calibrateLight(*f.image, calib))
        {
            ++rejected;
            continue;
        }

        calibratedLights.push_back(std::move(calib));
        const FrameQualityMetrics quality = compute_frame_quality(calibratedLights.back());
        frameQualities.push_back(quality);
        const double sigma = std::max(quality.stats.sigma, 1.0e-6);
        frameWeights.push_back(1.0 / (sigma * sigma));
        ++used;

        const float progress = 0.20f + 0.30f * (static_cast<float>(used + rejected) / static_cast<float>(std::max(totalLights, 1)));
        report(progress, "Calibrating light frames...");
    }

    if (calibratedLights.empty())
    {
        _log += "No calibrated lights.\n";
        outResult.log = _log;
        outResult.rejectedLights = rejected;
        return false;
    }

    if (_calibCfg.qualityWeighting && !frameQualities.empty())
    {
        std::vector<double> scores;
        scores.reserve(frameQualities.size());
        for (const FrameQualityMetrics& quality : frameQualities)
            scores.push_back(quality.score);
        const double medianScore = std::max(median_of(scores), 1.0e-6);
        for (size_t i = 0; i < frameWeights.size(); ++i)
        {
            const double qualityFactor = std::clamp(frameQualities[i].score / medianScore, 0.35, 3.0);
            frameWeights[i] *= qualityFactor;
        }
    }

    float keepBestPercent = std::clamp(_calibCfg.keepBestPercent, 5.0f, 100.0f);
    AutoSelectionDecision autoSelection;
    const bool autoSelectionEnabled = _calibCfg.frameSelectionMode == FrameSelectionMode::AutoStarQuality;
    if (autoSelectionEnabled)
    {
        autoSelection = compute_auto_selection_decision(frameQualities, _calibCfg.autoQualityProfile);
        keepBestPercent = autoSelection.keepPercent;
    }
    if (keepBestPercent < 99.999f && calibratedLights.size() > 3)
    {
        const size_t keepCount = std::max<size_t>(
            3,
            static_cast<size_t>(std::ceil(static_cast<double>(calibratedLights.size()) *
                                          static_cast<double>(keepBestPercent) / 100.0)));

        if (keepCount < calibratedLights.size())
        {
            std::vector<size_t> order(calibratedLights.size());
            std::iota(order.begin(), order.end(), 0);
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                return frameQualities[a].score > frameQualities[b].score;
            });
            order.resize(keepCount);
            std::sort(order.begin(), order.end());

            std::vector<FitsImage> filteredLights;
            std::vector<double> filteredWeights;
            std::vector<FrameQualityMetrics> filteredQualities;
            filteredLights.reserve(keepCount);
            filteredWeights.reserve(keepCount);
            filteredQualities.reserve(keepCount);
            for (size_t index : order)
            {
                filteredLights.push_back(std::move(calibratedLights[index]));
                filteredWeights.push_back(frameWeights[index]);
                filteredQualities.push_back(frameQualities[index]);
            }

            qualityRejected = static_cast<int>(calibratedLights.size() - keepCount);
            calibratedLights = std::move(filteredLights);
            frameWeights = std::move(filteredWeights);
            frameQualities = std::move(filteredQualities);
        }
    }

    if (!frameQualities.empty())
    {
        std::vector<double> scores;
        std::vector<double> fwhmValues;
        std::vector<double> roundnessValues;
        int maxStarCount = 0;
        scores.reserve(frameQualities.size());
        fwhmValues.reserve(frameQualities.size());
        roundnessValues.reserve(frameQualities.size());
        for (const FrameQualityMetrics& quality : frameQualities)
        {
            scores.push_back(quality.score);
            if (quality.starCount > 0)
            {
                fwhmValues.push_back(quality.starFwhm);
                roundnessValues.push_back(quality.starRoundness);
                maxStarCount = std::max(maxStarCount, quality.starCount);
            }
        }

        const auto [scoreMinIt, scoreMaxIt] = std::minmax_element(scores.begin(), scores.end());
        std::ostringstream qualityLog;
        qualityLog << "Light quality: kept " << calibratedLights.size()
                   << "/" << (calibratedLights.size() + static_cast<size_t>(qualityRejected))
                   << ", score range=[" << *scoreMinIt << ", " << *scoreMaxIt << "]";
        if (!fwhmValues.empty())
        {
            qualityLog << ", median FWHM=" << median_of(fwhmValues)
                       << ", median roundness=" << median_of(roundnessValues)
                       << ", max stars=" << maxStarCount;
        }
        if (_calibCfg.qualityWeighting)
            qualityLog << ", quality weighting enabled";
        if (autoSelectionEnabled)
        {
            const char* profileName = "Balanced";
            switch (_calibCfg.autoQualityProfile)
            {
            case AutoQualityProfile::Conservative: profileName = "Conservative"; break;
            case AutoQualityProfile::Balanced: profileName = "Balanced"; break;
            case AutoQualityProfile::Aggressive: profileName = "Aggressive"; break;
            }
            qualityLog << ", auto star selection (" << profileName << ") => keep "
                       << keepBestPercent << "%";
            if (autoSelection.threshold > 0.0)
                qualityLog << ", threshold=" << autoSelection.threshold;
        }
        else if (qualityRejected > 0)
        {
            qualityLog << ", best " << keepBestPercent << "% selected";
        }
        qualityLog << ".\n";
        _log += qualityLog.str();
    }

    FitsImage combined;
    report(0.55f, "Integrating calibrated lights...");
    if (!combineLights(calibratedLights, frameWeights, combined))
    {
        _log += "combineLights failed.\n";
        outResult.log = _log;
        outResult.rejectedLights = rejected + qualityRejected;
        return false;
    }

    report(0.85f, "Finalizing integrated image...");

    if (_denoiseCfg.previewMode != DenoiseConfig::PreviewMode::Off ||
        _denoiseCfg.exportMode != DenoiseConfig::ExportMode::Off)
    {
        _log += "Denoise deferred to preview/export pipeline.\n";
    }

    std::ostringstream oss;
    oss << "Stack complete: used=" << calibratedLights.size()
        << ", rejected=" << (rejected + qualityRejected) << "\n";
    _log += oss.str();

    outResult.finalImage = std::move(combined);
    outResult.coverageMap = _coverageMap;
    outResult.effectiveSamplesMap = _effectiveSamplesMap;
    outResult.usedLights = static_cast<int>(calibratedLights.size());
    outResult.rejectedLights = rejected + qualityRejected;
    outResult.log = _log;
    report(1.0f, "Stack complete.");
    return true;
}

} // namespace kty
