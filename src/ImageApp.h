#pragma once

#include "FitsImage.h"
#include "GlImageRenderer.h"
#include <string>
#include <vector>

class ImageApp
{
public:
    ImageApp();
    ~ImageApp();

    void run();

private:
    bool init();
    void shutdown();

    void main_loop();
    void render_ui();

    // 图像 & GPU 渲染
    void load_fits_file(const std::string& path);

    void build_luminance_stats();   // 一次排序
    void recompute_auto_params();   // 计算 _autoLow/_autoHigh

    // 导出 PNG 时 CPU 做一次拉伸 + 曲线 + 白平衡
    void compute_processed_cpu(std::vector<float>& outRgb);
    void export_png(const std::string& path);

    // 文件对话框
    void open_file_dialog();
    void render_file_dialog();
    void refresh_file_list();

private:
    // 图像数据
    FitsImage _fits;
    std::vector<float> _linearRgb;  // CPU 去拜耳后的 RGB，用于统计 & 导出
    bool _hasImage = false;

    // 亮度统计
    std::vector<float> _lum;
    std::vector<float> _lumSorted;
    float _medianLum = 0.0f;
    float _madLum    = 0.0f;

    // auto stretch 结果黑/白点
    float _autoLow  = 0.0f;
    float _autoHigh = 1.0f;

    int _imgWidth  = 0;
    int _imgHeight = 0;

    // GPU 渲染器
    GlImageRenderer _renderer;

    // UI 参数
    std::string _currentPath;
    bool  _autoStretch      = true;
    float _blackClip        = 0.1f;
    float _whiteClip        = 0.1f;
    float _stretchStrength  = 5.0f;
    BayerPattern _bayerHint = BayerPattern::RGGB;

    // 拉伸模式：0 线性，1 arcsinh，2 log，3 sqrt
    int   _stretchMode      = 1;     // 默认 arcsinh

    // 手动曲线
    bool  _useManualCurve   = false;
    float _curveBlack       = 0.0f;
    float _curveWhite       = 1.0f;
    float _curveGamma       = 1.0f;

    // 白平衡 (R/G/B 增益)
    float _wbR              = 1.0f;
    float _wbG              = 1.0f;
    float _wbB              = 1.0f;

    // 视图状态（传给 GPU）
    float _zoom             = 1.0f;
    float _panX             = 0.0f;
    float _panY             = 0.0f;

    // 文件对话框
    bool _showFileDialog    = false;
    std::string _fileDialogDir;
    std::vector<std::string> _fileEntries;
    bool _fileListDirty     = true;

    struct GLFWwindow* _window = nullptr;
};
