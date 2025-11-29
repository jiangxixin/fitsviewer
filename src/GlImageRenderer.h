#pragma once

#include <vector>

// 负责 GPU 端渲染：
// - 保存线性 RGB 纹理
// - 使用 shader 做 auto stretch + tone curve
// - 画一个全屏 quad
class GlImageRenderer
{
public:
    GlImageRenderer() = default;
    ~GlImageRenderer() = default;

    // 在 OpenGL context 创建好之后调用
    bool init();
    void shutdown();

    // 上传线性 RGB (0~1 float)，只在加载新图时调用一次
    void uploadBaseTexture(const std::vector<float>& linearRgb, int width, int height);

    // auto stretch 参数
    void setAutoParams(bool useAuto, float low, float high, float strength);

    // tone curve 参数
    void setCurveParams(bool useCurve, float black, float white, float gamma);

    // 每帧调用，viewport 是当前帧缓冲大小
    void render(int viewportWidth, int viewportHeight);

    bool hasImage() const { return _hasTexture; }
    int  imageWidth() const { return _imgWidth; }
    int  imageHeight() const { return _imgHeight; }

private:
    bool createQuad();
    bool createShader();
    void destroyQuad();
    void destroyShader();
    void updateUniforms(int viewportWidth, int viewportHeight);

private:
    // OpenGL 资源
    unsigned int _baseTexture   = 0;
    unsigned int _quadVAO       = 0;
    unsigned int _quadVBO       = 0;
    unsigned int _quadEBO       = 0;
    unsigned int _shaderProgram = 0;

    // uniform 位置
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

    // 图像尺寸
    int _imgWidth  = 0;
    int _imgHeight = 0;
    bool _hasTexture = false;

    // 当前参数（由 ImageApp 设置）
    bool  _useAuto          = true;
    float _autoLow          = 0.0f;
    float _autoHigh         = 1.0f;
    float _stretchStrength  = 5.0f;

    bool  _useCurve         = false;
    float _curveBlack       = 0.0f;
    float _curveWhite       = 1.0f;
    float _curveGamma       = 1.0f;
};
