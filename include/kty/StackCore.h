#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "FitsImage.h"        // 你已有的结构体

namespace kty {

enum class FrameType {
    Light,
    Dark,
    Flat,
    Bias
};

struct CalibConfig {
    bool useBias  = true;
    bool useDark  = true;
    bool useFlat  = true;

    // 未来可以加：dark scaling / exposure 比例 / 温度匹配之类
};

struct RejectConfig {
    // 参考 PixInsight/Siril 的像素拒绝思路，默认启用 SigmaClip
    enum class Method {
        None,
        SigmaClip
    };
    Method method      = Method::SigmaClip;
    float sigmaLow     = 3.0f;
    float sigmaHigh    = 3.0f;
    int   minSamples   = 3;
};

struct DenoiseConfig {
    enum class PreviewMode {
        Off,
        FastBilateral
    };
    enum class ExportMode {
        Off,
        HQWavelet,
        HQWaveletBm3dStyle
    };

    PreviewMode previewMode = PreviewMode::FastBilateral;
    ExportMode exportMode = ExportMode::HQWavelet;
    float strength = 0.35f;          // 0~1
    float backgroundSigma = 3.0f;    // 仅对 median + N*sigma 以下区域生效
    int   iterations = 1;            // 1~4
};

struct StackResult {
    FitsImage finalImage;           // 叠加结果（float、RGB 或单通道）
    std::string log;                // 处理日志（后面给 UI 用）
    int usedLights     = 0;
    int rejectedLights = 0;
};

class StackCore
{
public:
    using ProgressCallback = std::function<void(float, const std::string&)>;

    StackCore();
    ~StackCore();

    // 重置工程（清空所有帧和状态）
    void reset();

    // 添加帧（路径 + 类型）
    bool addFrame(const std::string& path, FrameType type);

    // 批量添加
    void addFrames(const std::vector<std::string>& paths, FrameType type);

    // 设置校准 / 拒绝策略
    void setCalibConfig(const CalibConfig& cfg)   { _calibCfg = cfg; }
    void setRejectConfig(const RejectConfig& cfg) { _rejCfg   = cfg; }
    void setDenoiseConfig(const DenoiseConfig& cfg) { _denoiseCfg = cfg; }

    const CalibConfig&  calibConfig()  const { return _calibCfg;  }
    const RejectConfig& rejectConfig() const { return _rejCfg;   }
    const DenoiseConfig& denoiseConfig() const { return _denoiseCfg; }

    // 预处理：生成 MasterBias / MasterDark / MasterFlat
    bool buildMasters();

    // 执行叠加：对所有 Light 做校准 + 叠加，返回结果
    bool runStack(StackResult& outResult, const ProgressCallback& progressCallback = {});

    // 可以单独导出 master 帧（供诊断）
    const FitsImage* masterBias() const { return _masterBias.get(); }
    const FitsImage* masterDark() const { return _masterDark.get(); }
    const FitsImage* masterFlat() const { return _masterFlat.get(); }

    // 简单日志访问
    const std::string& log() const { return _log; }

private:
    struct FrameEntry {
        std::string path;
        FrameType   type;
        std::unique_ptr<FitsImage> image; // 懒加载可以后加
    };

    std::vector<FrameEntry> _frames;

    CalibConfig  _calibCfg;
    RejectConfig _rejCfg;
    DenoiseConfig _denoiseCfg;

    std::unique_ptr<FitsImage> _masterBias;
    std::unique_ptr<FitsImage> _masterDark;
    std::unique_ptr<FitsImage> _masterFlat;

    std::string _log;

private:
    bool loadFrame(FrameEntry& entry);
    bool buildMasterBias();
    bool buildMasterDark();
    bool buildMasterFlat();
    bool validateCompatible(const FitsImage& ref,
                            const FitsImage& other,
                            const std::string& what);

    bool calibrateLight(const FitsImage& inLight, FitsImage& outCalib);
    bool combineLights(const std::vector<FitsImage>& calibratedLights,
                       const std::vector<double>& frameWeights,
                       FitsImage& outCombined);
};

} // namespace kty
