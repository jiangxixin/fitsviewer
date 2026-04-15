#include "kty/StackCore.h"

#include "FitsImage.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace kty {

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

    const size_t n = ref.raw.size();
    auto out = std::make_unique<FitsImage>();
    out->width = ref.width;
    out->height = ref.height;
    out->channels = 1;
    out->bayer = ref.bayer;
    out->raw.resize(n);

    std::vector<double> tmp(biasFrames.size());
    for (size_t idx = 0; idx < n; ++idx)
    {
        for (size_t i = 0; i < biasFrames.size(); ++i)
            tmp[i] = biasFrames[i]->raw[idx];

        std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end());
        out->raw[idx] = tmp[tmp.size() / 2];
    }

    _masterBias = std::move(out);
    _log += "Master Bias built.\n";
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
    auto out = std::make_unique<FitsImage>();
    out->width = ref.width;
    out->height = ref.height;
    out->channels = 1;
    out->bayer = ref.bayer;
    out->raw.resize(n);

    std::vector<double> tmp(darkFrames.size());
    for (size_t idx = 0; idx < n; ++idx)
    {
        for (size_t i = 0; i < darkFrames.size(); ++i)
        {
            double v = darkFrames[i]->raw[idx];
            if (_calibCfg.useBias && _masterBias)
                v -= _masterBias->raw[idx];
            tmp[i] = v;
        }

        std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end());
        out->raw[idx] = tmp[tmp.size() / 2];
    }

    _masterDark = std::move(out);
    _log += "Master Dark built.\n";
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

    std::vector<double> tmp(flatFrames.size());
    for (size_t idx = 0; idx < n; ++idx)
    {
        for (size_t i = 0; i < flatFrames.size(); ++i)
        {
            double v = flatFrames[i]->raw[idx];
            if (_calibCfg.useBias && _masterBias)
                v -= _masterBias->raw[idx];
            if (_calibCfg.useDark && _masterDark)
                v -= _masterDark->raw[idx];
            tmp[i] = v / perFlatNorm[i];
        }

        std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end());
        out->raw[idx] = tmp[tmp.size() / 2];
    }

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
    _log += "Master Flat built.\n";
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

    const size_t n = ref.raw.size();
    outCombined = ref;
    outCombined.raw.assign(n, 0.0);

    for (const auto& img : calibratedLights)
    {
        for (size_t idx = 0; idx < n; ++idx)
            outCombined.raw[idx] += img.raw[idx];
    }

    const double inv = 1.0 / static_cast<double>(calibratedLights.size());
    for (size_t idx = 0; idx < n; ++idx)
        outCombined.raw[idx] *= inv;

    return true;
}

bool StackCore::runStack(StackResult& outResult)
{
    _log.clear();
    outResult = StackResult{};

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

    if (!buildMasters())
    {
        _log += "buildMasters failed.\n";
        outResult.log = _log;
        return false;
    }

    std::vector<FitsImage> calibratedLights;
    calibratedLights.reserve(static_cast<size_t>(totalLights));

    int used = 0;
    int rejected = 0;

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
        ++used;
    }

    if (calibratedLights.empty())
    {
        _log += "No calibrated lights.\n";
        outResult.log = _log;
        outResult.rejectedLights = rejected;
        return false;
    }

    FitsImage combined;
    if (!combineLights(calibratedLights, combined))
    {
        _log += "combineLights failed.\n";
        outResult.log = _log;
        outResult.rejectedLights = rejected;
        return false;
    }

    std::ostringstream oss;
    oss << "Stack complete: used=" << used << ", rejected=" << rejected << "\n";
    _log += oss.str();

    outResult.finalImage = std::move(combined);
    outResult.usedLights = used;
    outResult.rejectedLights = rejected;
    outResult.log = _log;
    return true;
}

} // namespace kty
