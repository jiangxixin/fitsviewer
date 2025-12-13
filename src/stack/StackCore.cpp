#include "kty/StackCore.h"

#include "FitsImage.h"

#include <iostream>
#include <sstream>
#include <algorithm>
#include <cmath>

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
    // 注意：这里直接用你已有的 load_fits(...) 函数
    FitsImage tmp;
    if (!load_fits(entry.path, tmp, BayerPattern::NONE)) // 叠加一般用校准后的线性图，BayerPattern 可根据需求调整
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
        if (f.type == FrameType::Bias)
        {
            if (!loadFrame(f)) return false;
            biasFrames.push_back(f.image.get());
        }
    }

    if (biasFrames.empty())
        return false;

    // 简化：直接做逐像素中值
    const int W = biasFrames[0]->width;
    const int H = biasFrames[0]->height;
    const size_t N = (size_t)W * H;

    auto out = std::make_unique<FitsImage>();
    out->width  = W;
    out->height = H;
    out->channels = 1;
    out->bayer    = biasFrames[0]->bayer;
    out->raw.resize(N);

    std::vector<double> tmp(biasFrames.size());
    for (size_t idx = 0; idx < N; ++idx)
    {
        for (size_t i = 0; i < biasFrames.size(); ++i)
            tmp[i] = biasFrames[i]->raw[idx];
        std::nth_element(tmp.begin(), tmp.begin() + tmp.size()/2, tmp.end());
        out->raw[idx] = tmp[tmp.size()/2];
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
        if (f.type == FrameType::Dark)
        {
            if (!loadFrame(f)) return false;
            darkFrames.push_back(f.image.get());
        }
    }

    if (darkFrames.empty())
        return false;

    const int W = darkFrames[0]->width;
    const int H = darkFrames[0]->height;
    const size_t N = (size_t)W * H;

    auto out = std::make_unique<FitsImage>();
    out->width  = W;
    out->height = H;
    out->channels = 1;
    out->bayer    = darkFrames[0]->bayer;
    out->raw.resize(N);

    std::vector<double> tmp(darkFrames.size());
    for (size_t idx = 0; idx < N; ++idx)
    {
        for (size_t i = 0; i < darkFrames.size(); ++i)
            tmp[i] = darkFrames[i]->raw[idx];

        // TODO: 将来可以加 dark scaling，这里先直接中值
        std::nth_element(tmp.begin(), tmp.begin() + tmp.size()/2, tmp.end());
        out->raw[idx] = tmp[tmp.size()/2];
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
        if (f.type == FrameType::Flat)
        {
            if (!loadFrame(f)) return false;
            flatFrames.push_back(f.image.get());
        }
    }

    if (flatFrames.empty())
        return false;

    const int W = flatFrames[0]->width;
    const int H = flatFrames[0]->height;
    const size_t N = (size_t)W * H;

    auto out = std::make_unique<FitsImage>();
    out->width  = W;
    out->height = H;
    out->channels = 1;
    out->bayer    = flatFrames[0]->bayer;
    out->raw.resize(N);

    std::vector<double> tmp(flatFrames.size());

    // 先做每个 flat 的 mean 归一化，然后再逐像素中值
    // 简化起步：不减 bias/dark_flat，后面再加
    for (size_t idx = 0; idx < N; ++idx)
    {
        for (size_t i = 0; i < flatFrames.size(); ++i)
            tmp[i] = flatFrames[i]->raw[idx];
        std::nth_element(tmp.begin(), tmp.begin() + tmp.size()/2, tmp.end());
        out->raw[idx] = tmp[tmp.size()/2];
    }

    // 归一化 MasterFlat，使 mean ≈ 1
    double sum = 0.0;
    for (size_t i = 0; i < N; ++i) sum += out->raw[i];
    double mean = sum / (double)N;
    if (mean != 0.0)
    {
        for (size_t i = 0; i < N; ++i)
            out->raw[i] /= mean;
    }

    _masterFlat = std::move(out);
    _log += "Master Flat built.\n";
    return true;
}

bool StackCore::calibrateLight(const FitsImage& inLight, FitsImage& outCalib)
{
    const int W = inLight.width;
    const int H = inLight.height;
    const size_t N = (size_t)W * H;

    outCalib = inLight; // 拷贝基础属性
    outCalib.raw.resize(N);

    for (size_t idx = 0; idx < N; ++idx)
    {
        double v = inLight.raw[idx];

        if (_calibCfg.useBias && _masterBias)
            v -= _masterBias->raw[idx];

        if (_calibCfg.useDark && _masterDark)
            v -= _masterDark->raw[idx];  // TODO: dark scaling

        if (_calibCfg.useFlat && _masterFlat)
        {
            double flat = _masterFlat->raw[idx];
            if (flat != 0.0)
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

    const int W = calibratedLights[0].width;
    const int H = calibratedLights[0].height;
    const size_t N = (size_t)W * H;

    outCombined = calibratedLights[0];
    outCombined.raw.assign(N, 0.0);

    // 简化：均值叠加；后续可以根据 _rejCfg 实现 SigmaClip 等
    for (const auto& img : calibratedLights)
    {
        for (size_t idx = 0; idx < N; ++idx)
            outCombined.raw[idx] += img.raw[idx];
    }

    double inv = 1.0 / (double)calibratedLights.size();
    for (size_t idx = 0; idx < N; ++idx)
        outCombined.raw[idx] *= inv;

    return true;
}

bool StackCore::runStack(StackResult& outResult)
{
    // 1. 确保 master 都已生成
    if (!buildMasters())
    {
        _log += "buildMasters failed.\n";
        return false;
    }

    // 2. 收集 Light 并逐张校准
    std::vector<FitsImage> calibratedLights;
    int used = 0;

    for (auto& f : _frames)
    {
        if (f.type != FrameType::Light)
            continue;

        if (!loadFrame(f))
            continue;

        FitsImage calib;
        if (!calibrateLight(*f.image, calib))
            continue;

        calibratedLights.push_back(std::move(calib));
        ++used;
    }

    if (calibratedLights.empty())
    {
        _log += "No calibrated lights.\n";
        return false;
    }

    // 3. 叠加
    FitsImage combined;
    if (!combineLights(calibratedLights, combined))
    {
        _log += "combineLights failed.\n";
        return false;
    }

    outResult.finalImage   = std::move(combined);
    outResult.usedLights   = used;
    outResult.rejectedLights = 0; // TODO: 当实现拒绝算法时更新
    outResult.log          = _log;

    return true;
}

} // namespace kty
