#pragma once

#include <vector>

class GlImageRenderer
{
public:
    GlImageRenderer() = default;
    ~GlImageRenderer() = default;

    bool init();
    void shutdown();

    // 上传归一化 Bayer/灰度数据（0~1）
    void uploadBaseTexture(const std::vector<float>& bayerOrGray, int width, int height);

    // 0: NONE, 1: RGGB, 2: BGGR, 3: GRBG, 4: GBRG
    void setBayerPattern(int pattern);

    // 白平衡
    void setWhiteBalance(float r, float g, float b);

    // 0: Linear, 1: Asinh, 2: Log, 3: Sqrt
    void setStretchMode(int mode);

    // 自动拉伸参数（是否启用 + low/high + strength）
    void setAutoParams(bool useAuto, float low, float high, float strength);

    // 视图参数（缩放 + 平移）
    void setViewParams(float scale, float panX, float panY);

    // 在当前 FBO 上渲染（viewport 由调用者设置）
    void render(int viewportWidth, int viewportHeight);

    // GPU 统计 auto stretch 参数 + 更新直方图
    bool computeAutoParamsGpu(bool useAuto,
                              float blackClip,
                              float whiteClip,
                              float& outLow,
                              float& outHigh);

    // 全分辨率渲染到离屏 FBO 并读回 RGB8（用于导出 PNG）
    bool renderToImage(int width, int height, std::vector<unsigned char>& outRGB);

    // 预览渲染：在内部 FBO 生成预览纹理（Image 面板用）
    bool renderPreview(int width, int height);

    // 预览纹理 ID（OpenGL 纹理句柄）
    unsigned int previewTextureId() const { return _previewTex; }

    // 获取亮度直方图（已经归一化到 0~1）
    bool getLuminanceHistogram(std::vector<float>& outHist) const;

    bool hasImage() const { return _hasTexture; }
    int  imageWidth() const { return _imgWidth; }
    int  imageHeight() const { return _imgHeight; }

private:
    // OpenGL 资源
    unsigned int _baseTexture   = 0;
    unsigned int _quadVAO       = 0;
    unsigned int _quadVBO       = 0;
    unsigned int _quadEBO       = 0;
    unsigned int _shaderProgram = 0;

    // 主 shader uniform
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
    int _uWBGainLoc          = -1;
    int _uBayerPatternLoc    = -1;

    // 统计 FBO + 纹理 + shader
    unsigned int _statsFBO      = 0;
    unsigned int _statsTex      = 0;
    unsigned int _statsProgram  = 0;
    int          _statsSize     = 256;  // 亮度统计纹理尺寸
    int _uStatsBaseTexLoc       = -1;
    int _uStatsTexSizeLoc       = -1;
    int _uStatsBayerPatternLoc  = -1;
    int _uStatsWBGainLoc        = -1;

    // 导出 FBO + 纹理（全分辨率）
    unsigned int _exportFBO = 0;
    unsigned int _exportTex = 0;

    // 预览 FBO + 纹理（ImGui::Image 用）
    unsigned int _previewFBO = 0;
    unsigned int _previewTex = 0;
    int          _previewW   = 0;
    int          _previewH   = 0;

    // 图像尺寸
    int  _imgWidth  = 0;
    int  _imgHeight = 0;
    bool _hasTexture = false;

    // 当前参数
    bool  _useAuto         = true;
    float _autoLow         = 0.0f;
    float _autoHigh        = 1.0f;
    float _stretchStrength = 5.0f;

    bool  _useCurve        = false;
    float _curveBlack      = 0.0f;
    float _curveWhite      = 1.0f;
    float _curveGamma      = 1.0f;

    int   _stretchMode     = 1;    // 默认 Asinh

    float _zoom            = 1.0f;
    float _panX            = 0.0f;
    float _panY            = 0.0f;

    float _wbR             = 1.0f;
    float _wbG             = 1.0f;
    float _wbB             = 1.0f;

    int   _bayerPattern    = 1;    // 默认 RGGB

    // 亮度直方图
    static constexpr int _histBins = 64;
    std::vector<float>   _histogram;

private:
    bool createQuad();
    bool createMainShader();
    bool createStatsShader();
    void destroyQuad();
    void destroyShaders();
    void updateUniforms(int viewportWidth, int viewportHeight);
};
