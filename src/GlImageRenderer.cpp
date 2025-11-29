#include "GlImageRenderer.h"

#include <glad/glad.h>
#include <iostream>
#include <cmath>

static inline float clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static GLuint compileShader(GLenum type, const char* src)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success)
    {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::cerr << "Shader compile error: " << log << "\n";
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint linkProgram(GLuint vs, GLuint fs)
{
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint success = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &success);
    if (!success)
    {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        std::cerr << "Program link error: " << log << "\n";
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

bool GlImageRenderer::init()
{
    if (!createQuad())
        return false;
    if (!createShader())
        return false;

    glGenTextures(1, &_baseTexture);
    return true;
}

void GlImageRenderer::shutdown()
{
    if (_baseTexture)
    {
        glDeleteTextures(1, &_baseTexture);
        _baseTexture = 0;
    }
    destroyQuad();
    destroyShader();
    _hasTexture = false;
    _imgWidth = _imgHeight = 0;
}

bool GlImageRenderer::createQuad()
{
    float quadVertices[] = {
        // pos      // uv
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 1.0f,
        -1.0f,  1.0f,  0.0f, 1.0f,
    };
    unsigned int indices[] = {0, 1, 2, 0, 2, 3};

    glGenVertexArrays(1, &_quadVAO);
    glGenBuffers(1, &_quadVBO);
    glGenBuffers(1, &_quadEBO);

    glBindVertexArray(_quadVAO);

    glBindBuffer(GL_ARRAY_BUFFER, _quadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _quadEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    // layout(location=0): pos, layout(location=1): uv
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

    glBindVertexArray(0);
    return true;
}

bool GlImageRenderer::createShader()
{
    const char* vs_src = R"(#version 330 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec2 aUV;

out vec2 vTexCoord;

void main()
{
    vTexCoord = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

    const char* fs_src = R"(#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;

uniform sampler2D uBaseTex;   // 单通道 Bayer/灰度

uniform float uLow;
uniform float uHigh;
uniform float uStretchStrength;
uniform bool  uUseAuto;

uniform float uCurveBlack;
uniform float uCurveWhite;
uniform float uCurveGamma;
uniform bool  uUseCurve;

uniform vec2  uTexSize;
uniform vec2  uViewportSize;

uniform int   uStretchMode;   // 0: linear, 1: asinh, 2: log, 3: sqrt
uniform float uZoom;
uniform vec2  uPan;

uniform vec3  uWBGain;        // 白平衡: (R,G,B) 增益
uniform int   uBayerPattern;  // 0: NONE, 1: RGGB, 2: BGGR, 3: GRBG, 4: GBRG

float clamp01(float x) { return clamp(x, 0.0, 1.0); }

float toneCurve(float x, float black, float white, float gamma)
{
    if (x <= black) return 0.0;
    if (x >= white) return 1.0;
    float t = (x - black) / (white - black);
    if (gamma <= 0.0) gamma = 1.0;
    float ginv = 1.0 / gamma;
    float y = pow(t, ginv);
    return clamp01(y);
}

// 将概念 RGGB 坐标 (cx,cy) 映射为真实像素坐标
ivec2 conceptual_to_physical(ivec2 c, ivec2 size, int pattern)
{
    int cx = clamp(c.x, 0, size.x - 1);
    int cy = clamp(c.y, 0, size.y - 1);
    int px = cx;
    int py = cy;

    if (pattern == 1) {
        // RGGB
    } else if (pattern == 2) {
        // BGGR = RGGB 旋转 180°
        px = (size.x - 1) - cx;
        py = (size.y - 1) - cy;
    } else if (pattern == 3) {
        // GRBG = RGGB 水平翻转
        px = (size.x - 1) - cx;
        py = cy;
    } else if (pattern == 4) {
        // GBRG = RGGB 垂直翻转
        px = cx;
        py = (size.y - 1) - cy;
    }

    px = clamp(px, 0, size.x - 1);
    py = clamp(py, 0, size.y - 1);
    return ivec2(px, py);
}

float sample_raw_bayer(ivec2 c, ivec2 size, int pattern, sampler2D tex)
{
    ivec2 p = conceptual_to_physical(c, size, pattern);
    return texelFetch(tex, p, 0).r;
}

// 基于 RGGB 概念坐标的双线性去拜耳
vec3 debayer_bilinear(vec2 uv, sampler2D tex, vec2 texSize, int pattern)
{
    ivec2 size = ivec2(int(texSize.x + 0.5), int(texSize.y + 0.5));

    float fx = uv.x * texSize.x;
    float fy = uv.y * texSize.y;
    int cx = int(floor(fx + 0.5));
    int cy = int(floor(fy + 0.5));

    // pattern==0: 当灰度图处理
    if (pattern == 0)
    {
        float v = sample_raw_bayer(ivec2(cx, cy), size, 1, tex); // 用 RGGB 采样即可
        return vec3(v);
    }

    bool yEven = (cy & 1) == 0;
    bool xEven = (cx & 1) == 0;

    float R = 0.0;
    float G = 0.0;
    float B = 0.0;

    if (yEven && xEven)
    {
        // R 像素
        R = sample_raw_bayer(ivec2(cx, cy), size, pattern, tex);
        G = 0.25 * (
            sample_raw_bayer(ivec2(cx - 1, cy),     size, pattern, tex) +
            sample_raw_bayer(ivec2(cx + 1, cy),     size, pattern, tex) +
            sample_raw_bayer(ivec2(cx,     cy - 1), size, pattern, tex) +
            sample_raw_bayer(ivec2(cx,     cy + 1), size, pattern, tex));
        B = 0.25 * (
            sample_raw_bayer(ivec2(cx - 1, cy - 1), size, pattern, tex) +
            sample_raw_bayer(ivec2(cx + 1, cy - 1), size, pattern, tex) +
            sample_raw_bayer(ivec2(cx - 1, cy + 1), size, pattern, tex) +
            sample_raw_bayer(ivec2(cx + 1, cy + 1), size, pattern, tex));
    }
    else if (yEven && !xEven)
    {
        // G (R 行)
        G = sample_raw_bayer(ivec2(cx, cy), size, pattern, tex);
        R = 0.5 * (
            sample_raw_bayer(ivec2(cx - 1, cy), size, pattern, tex) +
            sample_raw_bayer(ivec2(cx + 1, cy), size, pattern, tex));
        B = 0.5 * (
            sample_raw_bayer(ivec2(cx, cy - 1), size, pattern, tex) +
            sample_raw_bayer(ivec2(cx, cy + 1), size, pattern, tex));
    }
    else if (!yEven && xEven)
    {
        // G (B 行)
        G = sample_raw_bayer(ivec2(cx, cy), size, pattern, tex);
        R = 0.5 * (
            sample_raw_bayer(ivec2(cx, cy - 1), size, pattern, tex) +
            sample_raw_bayer(ivec2(cx, cy + 1), size, pattern, tex));
        B = 0.5 * (
            sample_raw_bayer(ivec2(cx - 1, cy), size, pattern, tex) +
            sample_raw_bayer(ivec2(cx + 1, cy), size, pattern, tex));
    }
    else
    {
        // B 像素
        B = sample_raw_bayer(ivec2(cx, cy), size, pattern, tex);
        G = 0.25 * (
            sample_raw_bayer(ivec2(cx - 1, cy),     size, pattern, tex) +
            sample_raw_bayer(ivec2(cx + 1, cy),     size, pattern, tex) +
            sample_raw_bayer(ivec2(cx,     cy - 1), size, pattern, tex) +
            sample_raw_bayer(ivec2(cx,     cy + 1), size, pattern, tex));
        R = 0.25 * (
            sample_raw_bayer(ivec2(cx - 1, cy - 1), size, pattern, tex) +
            sample_raw_bayer(ivec2(cx + 1, cy - 1), size, pattern, tex) +
            sample_raw_bayer(ivec2(cx - 1, cy + 1), size, pattern, tex) +
            sample_raw_bayer(ivec2(cx + 1, cy + 1), size, pattern, tex));
    }

    return vec3(R, G, B);
}

void main()
{
    // 先保持长宽比，把 vTexCoord 映射到“裁剪后的纹理 uv”，再做缩放/平移
    float texAspect    = uTexSize.x / uTexSize.y;
    float screenAspect = uViewportSize.x / uViewportSize.y;

    vec2 uv = vTexCoord;

    if (screenAspect > texAspect)
    {
        // 屏幕比纹理宽：左右留黑
        float scale = texAspect / screenAspect;
        float x = (uv.x - 0.5) * scale + 0.5;
        if (x < 0.0 || x > 1.0)
        {
            FragColor = vec4(0.0, 0.0, 0.0, 1.0);
            return;
        }
        uv.x = x;
    }
    else
    {
        // 屏幕比纹理高：上下留黑
        float scale = screenAspect / texAspect;
        float y = (uv.y - 0.5) * scale + 0.5;
        if (y < 0.0 || y > 1.0)
        {
            FragColor = vec4(0.0, 0.0, 0.0, 1.0);
            return;
        }
        uv.y = y;
    }

    // 再以纹理中心为基准做缩放/平移
    vec2 uvCentered = uv - vec2(0.5);
    uvCentered /= max(uZoom, 0.1);
    uvCentered += vec2(0.5) + uPan;

    if (uvCentered.x < 0.0 || uvCentered.x > 1.0 ||
        uvCentered.y < 0.0 || uvCentered.y > 1.0)
    {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // === GPU 去拜耳 ===
    vec3 c = debayer_bilinear(uvCentered, uBaseTex, uTexSize, uBayerPattern);

    // === 白平衡 ===
    c *= uWBGain;
    c = clamp(c, 0.0, 1.0);

    // === auto stretch ===
    if (uUseAuto)
    {
        float range = max(uHigh - uLow, 1e-3);
        vec3 t = (c - vec3(uLow)) / range;
        t = clamp(t, 0.0, 1.0);

        if (uStretchMode == 0)
        {
            // 线性
            c = t;
        }
        else if (uStretchMode == 1)
        {
            // asinh
            float s = max(uStretchStrength, 1.0);
            float denom = asinh(s);
            vec3 stretched = asinh(s * t) / denom;
            c = clamp(stretched, 0.0, 1.0);
        }
        else if (uStretchMode == 2)
        {
            // log: log(1 + k t) / log(1 + k)
            float k = max(uStretchStrength, 1.0);
            float denom = log(1.0 + k);
            vec3 stretched = log(1.0 + k * t) / denom;
            c = clamp(stretched, 0.0, 1.0);
        }
        else if (uStretchMode == 3)
        {
            // sqrt
            c = sqrt(t);
        }
        else
        {
            c = t;
        }
    }

    // === Tone curve ===
    if (uUseCurve)
    {
        c.r = toneCurve(c.r, uCurveBlack, uCurveWhite, uCurveGamma);
        c.g = toneCurve(c.g, uCurveBlack, uCurveWhite, uCurveGamma);
        c.b = toneCurve(c.b, uCurveBlack, uCurveWhite, uCurveGamma);
    }

    FragColor = vec4(c, 1.0);
}
)";

    GLuint vs = compileShader(GL_VERTEX_SHADER, vs_src);
    if (!vs) return false;
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fs_src);
    if (!fs)
    {
        glDeleteShader(vs);
        return false;
    }

    _shaderProgram = linkProgram(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);

    if (!_shaderProgram)
        return false;

    glUseProgram(_shaderProgram);

    _uBaseTexLoc         = glGetUniformLocation(_shaderProgram, "uBaseTex");
    _uLowLoc             = glGetUniformLocation(_shaderProgram, "uLow");
    _uHighLoc            = glGetUniformLocation(_shaderProgram, "uHigh");
    _uStretchStrengthLoc = glGetUniformLocation(_shaderProgram, "uStretchStrength");
    _uUseAutoLoc         = glGetUniformLocation(_shaderProgram, "uUseAuto");

    _uCurveBlackLoc      = glGetUniformLocation(_shaderProgram, "uCurveBlack");
    _uCurveWhiteLoc      = glGetUniformLocation(_shaderProgram, "uCurveWhite");
    _uCurveGammaLoc      = glGetUniformLocation(_shaderProgram, "uCurveGamma");
    _uUseCurveLoc        = glGetUniformLocation(_shaderProgram, "uUseCurve");

    _uTexSizeLoc         = glGetUniformLocation(_shaderProgram, "uTexSize");
    _uViewportSizeLoc    = glGetUniformLocation(_shaderProgram, "uViewportSize");

    _uStretchModeLoc     = glGetUniformLocation(_shaderProgram, "uStretchMode");
    _uZoomLoc            = glGetUniformLocation(_shaderProgram, "uZoom");
    _uPanLoc             = glGetUniformLocation(_shaderProgram, "uPan");

    _uWBGainLoc          = glGetUniformLocation(_shaderProgram, "uWBGain");
    _uBayerPatternLoc    = glGetUniformLocation(_shaderProgram, "uBayerPattern");

    glUniform1i(_uBaseTexLoc, 0);

    glUseProgram(0);
    return true;
}

void GlImageRenderer::destroyQuad()
{
    if (_quadVAO)
    {
        glDeleteVertexArrays(1, &_quadVAO);
        _quadVAO = 0;
    }
    if (_quadVBO)
    {
        glDeleteBuffers(1, &_quadVBO);
        _quadVBO = 0;
    }
    if (_quadEBO)
    {
        glDeleteBuffers(1, &_quadEBO);
        _quadEBO = 0;
    }
}

void GlImageRenderer::destroyShader()
{
    if (_shaderProgram)
    {
        glDeleteProgram(_shaderProgram);
        _shaderProgram = 0;
    }
}

void GlImageRenderer::uploadBaseTexture(const std::vector<float>& bayerOrGray, int width, int height)
{
    if (bayerOrGray.empty() || width <= 0 || height <= 0 || !_baseTexture)
    {
        _hasTexture = false;
        return;
    }

    _imgWidth  = width;
    _imgHeight = height;

    glBindTexture(GL_TEXTURE_2D, _baseTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // 保存单通道 Bayer / 灰度
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, width, height,
                 0, GL_RED, GL_FLOAT, bayerOrGray.data());

    _hasTexture = true;
}

void GlImageRenderer::setAutoParams(bool useAuto, float low, float high, float strength)
{
    _useAuto         = useAuto;
    _autoLow         = low;
    _autoHigh        = high;
    _stretchStrength = strength;
}

void GlImageRenderer::setCurveParams(bool useCurve, float black, float white, float gamma)
{
    _useCurve   = useCurve;
    _curveBlack = black;
    _curveWhite = white;
    _curveGamma = gamma;
}

void GlImageRenderer::setStretchMode(int mode)
{
    if (mode < 0) mode = 0;
    if (mode > 3) mode = 3;
    _stretchMode = mode;
}

void GlImageRenderer::setWhiteBalance(float rGain, float gGain, float bGain)
{
    _wbR = rGain;
    _wbG = gGain;
    _wbB = bGain;
}

void GlImageRenderer::setViewParams(float zoom, float panX, float panY)
{
    if (zoom < 0.1f) zoom = 0.1f;
    if (zoom > 20.0f) zoom = 20.0f;
    _zoom = zoom;
    _panX = panX;
    _panY = panY;
}

void GlImageRenderer::updateUniforms(int viewportWidth, int viewportHeight)
{
    if (!_shaderProgram)
        return;

    glUniform1f(_uLowLoc,  _autoLow);
    glUniform1f(_uHighLoc, _autoHigh);
    glUniform1f(_uStretchStrengthLoc, _stretchStrength);
    glUniform1i(_uUseAutoLoc, _useAuto ? 1 : 0);

    glUniform1f(_uCurveBlackLoc, _curveBlack);
    glUniform1f(_uCurveWhiteLoc, _curveWhite);
    glUniform1f(_uCurveGammaLoc, _curveGamma);
    glUniform1i(_uUseCurveLoc, _useCurve ? 1 : 0);

    glUniform2f(_uTexSizeLoc,      (float)_imgWidth,      (float)_imgHeight);
    glUniform2f(_uViewportSizeLoc, (float)viewportWidth,  (float)viewportHeight);

    glUniform1i(_uStretchModeLoc, _stretchMode);
    glUniform1f(_uZoomLoc,        _zoom);
    glUniform2f(_uPanLoc,         _panX, _panY);

    glUniform3f(_uWBGainLoc, _wbR, _wbG, _wbB);
    glUniform1i(_uBayerPatternLoc, _bayerPattern);
}

void GlImageRenderer::render(int viewportWidth, int viewportHeight)
{
    if (!_hasTexture || !_shaderProgram || !_quadVAO)
        return;

    glUseProgram(_shaderProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, _baseTexture);

    updateUniforms(viewportWidth, viewportHeight);

    glBindVertexArray(_quadVAO);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);

    glUseProgram(0);
}
