#include "kty/StackCore.h"

#include "FitsImage.h"

#include <algorithm>
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

static double integrate_pixel_stack(const std::vector<double>& values,
                                    const std::vector<double>& weights,
                                    const RejectConfig& rejectConfig,
                                    size_t& rejectedSamples)
{
    if (values.empty())
        return 0.0;

    if (rejectConfig.method != RejectConfig::Method::SigmaClip ||
        values.size() < static_cast<size_t>(std::max(rejectConfig.minSamples, 3)))
    {
        return weighted_average(values, weights);
    }

    std::vector<double> keptValues = values;
    std::vector<double> keptWeights(values.size(), 1.0);
    for (size_t i = 0; i < keptWeights.size(); ++i)
        keptWeights[i] = (i < weights.size() && weights[i] > 0.0) ? weights[i] : 1.0;

    bool rejectedAny = false;
    for (int iter = 0; iter < 3; ++iter)
    {
        if (keptValues.size() < static_cast<size_t>(std::max(rejectConfig.minSamples, 3)))
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
                rejectedAny = true;
                continue;
            }

            nextValues.push_back(keptValues[i]);
            nextWeights.push_back(keptWeights[i]);
        }

        if (!rejectedThisRound)
            break;
        if (nextValues.size() < static_cast<size_t>(std::max(rejectConfig.minSamples, 1)))
            break;

        keptValues.swap(nextValues);
        keptWeights.swap(nextWeights);
    }

    if (keptValues.size() < static_cast<size_t>(std::max(rejectConfig.minSamples, 1)))
        return weighted_average(values, weights);

    if (!rejectedAny)
        return weighted_average(keptValues, keptWeights);

    return weighted_average(keptValues, keptWeights);
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
    if (!load_fits(entry.path, tmp, BayerPattern::NONE))
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
    if (!integrate_frames(biasFrames, unitWeights, _rejCfg, *out, rejectedSamples))
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
    if (!integrate_frames(correctedPtrs, unitWeights, _rejCfg, *out, rejectedSamples))
        return false;

    _masterDark = std::move(out);
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
    out->channels = 1;
    out->bayer = ref.bayer;
    out->raw.resize(n);

    std::vector<double> perFlatNorm(flatFrames.size(), 1.0);
    for (size_t i = 0; i < flatFrames.size(); ++i)
    {
        double sum = 0.0;
        for (size_t idx = 0; idx < n; ++idx)
        {
            double v = flatFrames[i]->raw[idx];
            if (_calibCfg.useBias && _masterBias)
                v -= _masterBias->raw[idx];
            if (_calibCfg.useDark && _masterDark)
                v -= _masterDark->raw[idx];
            sum += v;
        }

        double mean = sum / static_cast<double>(n);
        if (std::abs(mean) < 1e-12)
        {
            _log += "Flat frame mean is near zero; fallback normalization factor applied.\n";
            mean = 1.0;
        }
        perFlatNorm[i] = mean;
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
            normalizedFlats[i].raw[idx] = v / perFlatNorm[i];
        }
        normalizedPtrs.push_back(&normalizedFlats[i]);
    }

    size_t rejectedSamples = 0;
    const std::vector<double> unitWeights(normalizedPtrs.size(), 1.0);
    if (!integrate_frames(normalizedPtrs, unitWeights, _rejCfg, *out, rejectedSamples))
        return false;

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

    for (size_t idx = 0; idx < n; ++idx)
    {
        double v = inLight.raw[idx];

        if (_calibCfg.useBias && _masterBias)
            v -= _masterBias->raw[idx];
        if (_calibCfg.useDark && _masterDark)
            v -= _masterDark->raw[idx];
        if (_calibCfg.useFlat && _masterFlat)
        {
            const double flat = _masterFlat->raw[idx];
            if (std::abs(flat) > 1e-12)
                v /= flat;
        }

        outCalib.raw[idx] = v;
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

    std::vector<ImageStats> stats(calibratedLights.size());
    std::vector<double> medians;
    std::vector<double> signalScales;
    medians.reserve(calibratedLights.size());
    signalScales.reserve(calibratedLights.size());
    for (size_t i = 0; i < calibratedLights.size(); ++i)
    {
        stats[i] = compute_image_stats(calibratedLights[i]);
        medians.push_back(stats[i].median);
        signalScales.push_back(stats[i].signalScale);
    }

    const double referenceMedian = median_of(medians);
    ImageStats referenceStats = stats.front();
    referenceStats.median = referenceMedian;
    referenceStats.signalScale = median_of(signalScales);
    const size_t n = ref.raw.size();
    outCombined = ref;
    outCombined.raw.assign(n, 0.0);

    std::vector<AffineNormalization> normalizations(calibratedLights.size());
    std::vector<double> normalizedWeights(calibratedLights.size(), 1.0);
    for (size_t i = 0; i < calibratedLights.size(); ++i)
    {
        normalizations[i] = compute_affine_normalization(referenceStats, stats[i]);
        const double explicitWeight = (i < frameWeights.size() && frameWeights[i] > 0.0) ? frameWeights[i] : 1.0;
        normalizedWeights[i] = explicitWeight /
                               std::max(normalizations[i].scale * normalizations[i].scale, 1.0e-6);
    }

    std::vector<double> pixelStack;
    pixelStack.reserve(calibratedLights.size());
    size_t rejectedSamples = 0;

    for (size_t idx = 0; idx < n; ++idx)
    {
        pixelStack.clear();
        for (size_t i = 0; i < calibratedLights.size(); ++i)
        {
            const AffineNormalization& norm = normalizations[i];
            const double normalized =
                (calibratedLights[i].raw[idx] - norm.offset) * norm.scale + referenceMedian;
            pixelStack.push_back(normalized);
        }

        outCombined.raw[idx] = integrate_pixel_stack(pixelStack, normalizedWeights, _rejCfg, rejectedSamples);
    }

    const double total = static_cast<double>(n) * static_cast<double>(calibratedLights.size());
    const double rate = (total > 0.0) ? (100.0 * static_cast<double>(rejectedSamples) / total) : 0.0;
    std::ostringstream oss;
    oss << "Light integration: affine-normalized weighted average";
    if (_rejCfg.method == RejectConfig::Method::SigmaClip)
        oss << " + iterative sigma clip";
    oss << ", rejected " << rate << "% of samples.\n";
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

    int used = 0;
    int rejected = 0;

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
        const ImageStats stats = compute_image_stats(calibratedLights.back());
        const double sigma = std::max(stats.sigma, 1.0e-6);
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

    FitsImage combined;
    report(0.55f, "Integrating calibrated lights...");
    if (!combineLights(calibratedLights, frameWeights, combined))
    {
        _log += "combineLights failed.\n";
        outResult.log = _log;
        outResult.rejectedLights = rejected;
        return false;
    }

    report(0.85f, "Finalizing integrated image...");

    if (_denoiseCfg.previewMode != DenoiseConfig::PreviewMode::Off ||
        _denoiseCfg.exportMode != DenoiseConfig::ExportMode::Off)
    {
        _log += "Denoise deferred to preview/export pipeline.\n";
    }

    std::ostringstream oss;
    oss << "Stack complete: used=" << used << ", rejected=" << rejected << "\n";
    _log += oss.str();

    outResult.finalImage = std::move(combined);
    outResult.usedLights = used;
    outResult.rejectedLights = rejected;
    outResult.log = _log;
    report(1.0f, "Stack complete.");
    return true;
}

} // namespace kty
