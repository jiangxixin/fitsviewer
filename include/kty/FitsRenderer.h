#pragma once
#include <string>
#include <vector>

namespace kty {

enum class BayerPattern {
    None = 0,
    RGGB = 1,
    BGGR = 2,
    GRBG = 3,
    GBRG = 4,
};

enum class StretchMode {
    Linear = 0,
    Asinh  = 1,
    Log    = 2,
    Sqrt   = 3,
};

struct StretchParams {
    bool  autoStretch   = true;
    float blackClip     = 0.1f;   // %
    float whiteClip     = 0.1f;   // %
    float strength      = 5.0f;
    StretchMode mode    = StretchMode::Asinh;
};

struct WhiteBalance {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
};

struct ViewParams {
    float scale = 1.0f;
    float panX  = 0.0f;
    float panY  = 0.0f;
};

class FitsRenderer
{
public:
    FitsRenderer();
    ~FitsRenderer();

    bool init();
    void shutdown();

    bool loadFits(const std::string& path, BayerPattern bayerHint);

    void setStretchParams(const StretchParams& p);
    void setWhiteBalance(const WhiteBalance& wb);
    void setViewParams(const ViewParams& vp);
    void setBayerPattern(BayerPattern bayer);

    const StretchParams& stretchParams() const { return _stretch; }
    const WhiteBalance&  whiteBalance() const { return _wb; }
    const ViewParams&    viewParams()   const { return _view; }
    BayerPattern         bayerPattern() const { return _bayer; }

    bool recomputeAutoStretch();
    bool getLumaHistogram(std::vector<float>& outHist) const;

    void render(int viewportWidth, int viewportHeight);

    bool renderToImage(std::vector<unsigned char>& outRGB, int& outWidth, int& outHeight) const;

    // 预览纹理
    bool renderPreview(int width, int height);
    unsigned int previewTextureId() const;

    // 计算自动白平衡（更新内部 _wb 并同步到 GL），true=成功
    bool computeAutoWhiteBalance();

    bool hasImage() const { return _hasImage; }
    int  width()   const { return _imgWidth; }
    int  height()  const { return _imgHeight; }

private:
    bool  _hasImage = false;
    int   _imgWidth = 0;
    int   _imgHeight = 0;

    StretchParams _stretch;
    WhiteBalance  _wb;
    ViewParams    _view;
    BayerPattern  _bayer = BayerPattern::RGGB;

    float _autoLow  = 0.0f;
    float _autoHigh = 1.0f;

    void* _fits = nullptr;   // FitsImage*
    void* _gl   = nullptr;   // GlImageRenderer*
};

} // namespace kty
