#pragma once

#include <vector>

// 负责 GPU 渲染：
// - 保存 Bayer/灰度纹理（单通道）
// - shader 内完成：去拜耳 + 白平衡 + auto stretch + tone curve + 多种拉伸模式 + 缩放/平移
// - 提供 GPU 统计亮度 + GPU 导出 PNG 能力
class GlImageRenderer
{
public:
    GlImageRenderer() = default;
    ~GlImageRenderer() = default;

    // 在 OpenGL context 创建好之后调用
    bool init();
    void shutdown();

    // 上传 Bayer / 灰度 (0~1 float)，只在加载新图时调用一次
    void uploadBaseTexture(const std::vector<float>& bayerOrGray, int width, int height);

    // auto stretch 参数
    void setAutoParams(bool useAuto, float low, float high, float strength);

    // tone curve 参数
    void setCurveParams(bool useCurve, float black, float white, float gamma);

    // 拉伸模式：0: 线性, 1: arcsinh, 2: log, 3: sqrt
    void setStretchMode(int mode);

    // 白平衡（R/G/B 增益）
    void setWhiteBalance(float rGain, float gGain, float bGain);

    // Bayer 模式：0: NONE, 1: RGGB, 2: BGGR, 3: GRBG, 4: GBRG
    void setBayerPattern(int pattern) { _bayerPattern = pattern; }

    // 视图参数（缩放 + 平移）
    void setViewParams(float zoom, float panX, float panY);

    // 每帧调用，viewport 是当前帧缓冲大小
    void render(int viewportWidth, int viewportHeight);

    // 在 GPU 上生成亮度统计纹理，然后 CPU 上做 percentile + median/MAD
    // 输入：blackClip / whiteClip 百分比（%），输出：low/high in [0,1]
    bool computeAutoParamsGpu(bool useAuto,
                              float blackClip,
                              float whiteClip,
                              float& outLow,
                              float& outHigh);

    // 使用当前 shader 状态，把结果渲染到 outWidth x outHeight，然后读回 RGB8
    bool renderToImage(int outWidth, int outHeight, std::vector<unsigned char>& outRGB);

    bool hasImage() const { return _hasTexture; }
    int  imageWidth() const { return _imgWidth; }
    int  imageHeight() const { return _imgHeight; }

private:
    bool createQuad();
    bool createMainShader();
    bool createStatsShader();
    void destroyQuad();
    void destroyShaders();
    void updateUniforms(int viewportWidth, int viewportHeight);

private:
    // 主渲染资源
    unsigned int _baseTexture   = 0;  // Bayer/灰度纹理（单通道 float）
    unsigned int _quadVAO       = 0;
    unsigned int _quadVBO       = 0;
    unsigned int _quadEBO       = 0;
    unsigned int _shaderProgram = 0;

    // 主 shader uniform 位置
    int _uBaseTexLoc         = -1;
    int _uLowLoc             = -1;
    int _uHighLoc            = -1;
    int _uStretchStrengthLoc = -1;
    int _uUseAutoLoc         = -1;

    int _uCurveBlackLoc      = -1;
    int _uCurveWhiteLoc      = -1;
    int _uCurveGammaLoc      = -1;
    int _uUseCurveLoc        = -1;

    int _uTexSizeLoc         = -1;
    int _uViewportSizeLoc    = -1;

    int _uStretchModeLoc     = -1;
    int _uZoomLoc            = -1;
    int _uPanLoc             = -1;

    int _uWBGainLoc          = -1;   // vec3 白平衡增益
    int _uBayerPatternLoc    = -1;   // int Bayer 模式

    // 统计 FBO + 纹理 + shader
    unsigned int _statsFBO      = 0;
    unsigned int _statsTex      = 0;
    unsigned int _statsProgram  = 0;
    int          _statsSize     = 256;  // 统计纹理尺寸：256x256

    int _uStatsBaseTexLoc       = -1;
    int _uStatsTexSizeLoc       = -1;
    int _uStatsBayerPatternLoc  = -1;
    int _uStatsWBGainLoc        = -1;

    // 导出 FBO + 纹理（全分辨率）
    unsigned int _exportFBO = 0;
    unsigned int _exportTex = 0;

    // 图像尺寸
    int _imgWidth  = 0;
    int _imgHeight = 0;
    bool _hasTexture = false;

    // 当前参数（由上层设置）
    bool  _useAuto         = true;
    float _autoLow         = 0.0f;
    float _autoHigh        = 1.0f;
    float _stretchStrength = 5.0f;

    bool  _useCurve        = false;
    float _curveBlack      = 0.0f;
    float _curveWhite      = 1.0f;
    float _curveGamma      = 1.0f;

    int   _stretchMode     = 1;     // 0: linear, 1: asinh, 2: log, 3: sqrt

    float _zoom            = 1.0f;  // >1 放大
    float _panX            = 0.0f;
    float _panY            = 0.0f;

    // 白平衡
    float _wbR             = 1.0f;
    float _wbG             = 1.0f;
    float _wbB             = 1.0f;

    // Bayer 模式（GPU 去拜耳用）
    int   _bayerPattern    = 1;     // 默认 RGGB
};
