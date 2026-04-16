#include "GlImageRenderer.h"

#include <glad/glad.h>
#include <iostream>
#include <cmath>
#include <algorithm>

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
    if (!createMainShader())
        return false;
    if (!createStatsShader())
        return false;
    if (!createDenoiseShader())
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
    if (_statsTex)
    {
        glDeleteTextures(1, &_statsTex);
        _statsTex = 0;
    }
    if (_statsFBO)
    {
        glDeleteFramebuffers(1, &_statsFBO);
        _statsFBO = 0;
    }
    if (_statsProgram)
    {
        glDeleteProgram(_statsProgram);
        _statsProgram = 0;
    }
    if (_exportTex)
    {
        glDeleteTextures(1, &_exportTex);
        _exportTex = 0;
    }
    if (_exportFBO)
    {
        glDeleteFramebuffers(1, &_exportFBO);
        _exportFBO = 0;
    }
    if (_previewTex)
    {
        glDeleteTextures(1, &_previewTex);
        _previewTex = 0;
    }
    if (_previewFBO)
    {
        glDeleteFramebuffers(1, &_previewFBO);
        _previewFBO = 0;
    }
    _previewDisplayTex = 0;
    if (_denoiseTex)
    {
        glDeleteTextures(1, &_denoiseTex);
        _denoiseTex = 0;
    }
    if (_denoiseFBO)
    {
        glDeleteFramebuffers(1, &_denoiseFBO);
        _denoiseFBO = 0;
    }

    destroyQuad();
    destroyShaders();

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

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

    glBindVertexArray(0);
    return true;
}

// 主渲染 shader：debayer + 白平衡 + 拉伸 + 曲线 + 缩放
bool GlImageRenderer::createMainShader()
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

uniform sampler2D uBaseTex;

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

uniform vec3  uWBGain;
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

// 概念 RGGB 坐标→实际
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

// 双线性去拜耳
vec3 debayer_bilinear(vec2 uv, sampler2D tex, vec2 texSize, int pattern)
{
    ivec2 size = ivec2(int(texSize.x + 0.5), int(texSize.y + 0.5));
    float fx = uv.x * texSize.x;
    float fy = uv.y * texSize.y;
    int cx = int(floor(fx + 0.5));
    int cy = int(floor(fy + 0.5));

    if (pattern == 0)
    {
        float v = sample_raw_bayer(ivec2(cx, cy), size, 1, tex);
        return vec3(v);
    }

    bool yEven = (cy & 1) == 0;
    bool xEven = (cx & 1) == 0;

    float R = 0.0;
    float G = 0.0;
    float B = 0.0;

    if (yEven && xEven)
    {
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
    // 保持长宽比 + 缩放/平移
    float texAspect    = uTexSize.x / uTexSize.y;
    float screenAspect = uViewportSize.x / uViewportSize.y;

    vec2 uv = vTexCoord;

    if (screenAspect > texAspect)
    {
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
        float scale = screenAspect / texAspect;
        float y = (uv.y - 0.5) * scale + 0.5;
        if (y < 0.0 || y > 1.0)
        {
            FragColor = vec4(0.0, 0.0, 0.0, 1.0);
            return;
        }
        uv.y = y;
    }

    vec2 uvCentered = uv - vec2(0.5);
    uvCentered /= max(uZoom, 0.1);
    uvCentered += vec2(0.5) + uPan;

    if (uvCentered.x < 0.0 || uvCentered.x > 1.0 ||
        uvCentered.y < 0.0 || uvCentered.y > 1.0)
    {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 c = debayer_bilinear(uvCentered, uBaseTex, uTexSize, uBayerPattern);

    // 白平衡
    c *= uWBGain;
    c = clamp(c, 0.0, 1.0);

    // auto stretch
    if (uUseAuto)
    {
        float range = max(uHigh - uLow, 1e-3);
        vec3 t = (c - vec3(uLow)) / range;
        t = clamp(t, 0.0, 1.0);

        if (uStretchMode == 0)
        {
            c = t;
        }
        else if (uStretchMode == 1)
        {
            float s = max(uStretchStrength, 1.0);
            float denom = asinh(s);
            vec3 stretched = asinh(s * t) / denom;
            c = clamp(stretched, 0.0, 1.0);
        }
        else if (uStretchMode == 2)
        {
            float k = max(uStretchStrength, 1.0);
            float denom = log(1.0 + k);
            vec3 stretched = log(1.0 + k * t) / denom;
            c = clamp(stretched, 0.0, 1.0);
        }
        else if (uStretchMode == 3)
        {
            c = sqrt(t);
        }
        else
        {
            c = t;
        }
    }

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

// 统计 shader：输出白平衡后的亮度到 RED
bool GlImageRenderer::createStatsShader()
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

uniform sampler2D uBaseTex;
uniform vec2  uTexSize;
uniform int   uBayerPattern;
uniform vec3  uWBGain;

float clamp01(float x) { return clamp(x, 0.0, 1.0); }

// 与主 shader 相同的去拜耳辅助
ivec2 conceptual_to_physical(ivec2 c, ivec2 size, int pattern)
{
    int cx = clamp(c.x, 0, size.x - 1);
    int cy = clamp(c.y, 0, size.y - 1);
    int px = cx;
    int py = cy;

    if (pattern == 1) {
    } else if (pattern == 2) {
        px = (size.x - 1) - cx;
        py = (size.y - 1) - cy;
    } else if (pattern == 3) {
        px = (size.x - 1) - cx;
        py = cy;
    } else if (pattern == 4) {
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

vec3 debayer_bilinear(vec2 uv, sampler2D tex, vec2 texSize, int pattern)
{
    ivec2 size = ivec2(int(texSize.x + 0.5), int(texSize.y + 0.5));
    float fx = uv.x * texSize.x;
    float fy = uv.y * texSize.y;
    int cx = int(floor(fx + 0.5));
    int cy = int(floor(fy + 0.5));

    if (pattern == 0)
    {
        float v = sample_raw_bayer(ivec2(cx, cy), size, 1, tex);
        return vec3(v);
    }

    bool yEven = (cy & 1) == 0;
    bool xEven = (cx & 1) == 0;

    float R = 0.0;
    float G = 0.0;
    float B = 0.0;

    if (yEven && xEven)
    {
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
    vec2 uv = vTexCoord;
    vec3 c = debayer_bilinear(uv, uBaseTex, uTexSize, uBayerPattern);

    c *= uWBGain;
    c = clamp(c, 0.0, 1.0);

    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    l = clamp01(l);

    FragColor = vec4(l, 0.0, 0.0, 1.0);
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

    _statsProgram = linkProgram(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);

    if (!_statsProgram)
        return false;

    glUseProgram(_statsProgram);
    _uStatsBaseTexLoc      = glGetUniformLocation(_statsProgram, "uBaseTex");
    _uStatsTexSizeLoc      = glGetUniformLocation(_statsProgram, "uTexSize");
    _uStatsBayerPatternLoc = glGetUniformLocation(_statsProgram, "uBayerPattern");
    _uStatsWBGainLoc       = glGetUniformLocation(_statsProgram, "uWBGain");
    glUniform1i(_uStatsBaseTexLoc, 0);
    glUseProgram(0);

    // stats FBO + tex
    glGenFramebuffers(1, &_statsFBO);
    glGenTextures(1, &_statsTex);
    glBindTexture(GL_TEXTURE_2D, _statsTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, _statsSize, _statsSize,
                 0, GL_RED, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glBindFramebuffer(GL_FRAMEBUFFER, _statsFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, _statsTex, 0);
    GLenum drawBuf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &drawBuf);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        std::cerr << "Stats FBO incomplete\n";
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    return true;
}

bool GlImageRenderer::createDenoiseShader()
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

uniform sampler2D uSourceTex;
uniform vec2 uInvTexSize;
uniform float uStrength;
uniform float uBgThreshold;
uniform float uRangeSigma;

float luminance(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

void main()
{
    vec3 center = texture(uSourceTex, vTexCoord).rgb;
    float centerL = luminance(center);

    float spatial[9] = float[9](
        1.0, 2.0, 1.0,
        2.0, 4.0, 2.0,
        1.0, 2.0, 1.0
    );

    float rangeDenom = max(2.0 * uRangeSigma * uRangeSigma, 1e-5);
    vec3 accum = vec3(0.0);
    float wsum = 0.0;
    int k = 0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec2 uv = vTexCoord + vec2(float(x), float(y)) * uInvTexSize;
            vec3 s = texture(uSourceTex, uv).rgb;
            float dl = luminance(s) - centerL;
            float w = spatial[k] * exp(-(dl * dl) / rangeDenom);
            accum += s * w;
            wsum += w;
            k++;
        }
    }

    vec3 filtered = (wsum > 0.0) ? (accum / wsum) : center;
    float backgroundness = clamp((uBgThreshold - centerL) / max(uBgThreshold, 1e-4), 0.0, 1.0);
    float blend = uStrength * backgroundness * backgroundness;
    vec3 outColor = mix(center, filtered, blend);
    FragColor = vec4(outColor, 1.0);
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

    _denoiseProgram = linkProgram(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);

    if (!_denoiseProgram)
        return false;

    glUseProgram(_denoiseProgram);
    _uDenoiseSourceTexLoc = glGetUniformLocation(_denoiseProgram, "uSourceTex");
    _uDenoiseInvTexSizeLoc = glGetUniformLocation(_denoiseProgram, "uInvTexSize");
    _uDenoiseStrengthLoc = glGetUniformLocation(_denoiseProgram, "uStrength");
    _uDenoiseBgThresholdLoc = glGetUniformLocation(_denoiseProgram, "uBgThreshold");
    _uDenoiseRangeSigmaLoc = glGetUniformLocation(_denoiseProgram, "uRangeSigma");
    glUniform1i(_uDenoiseSourceTexLoc, 0);
    glUseProgram(0);
    return true;
}

bool GlImageRenderer::ensureDenoiseResources(int width, int height)
{
    if (!_denoiseFBO)
        glGenFramebuffers(1, &_denoiseFBO);
    if (!_denoiseTex)
        glGenTextures(1, &_denoiseTex);

    if (width != _denoiseW || height != _denoiseH)
    {
        _denoiseW = width;
        _denoiseH = height;
        glBindTexture(GL_TEXTURE_2D, _denoiseTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    return _denoiseTex != 0 && _denoiseFBO != 0;
}

unsigned int GlImageRenderer::applyDenoisePass(unsigned int sourceTex, int width, int height)
{
    if (!_denoiseEnabled || !_denoiseProgram || sourceTex == 0 || width <= 0 || height <= 0)
        return sourceTex;
    if (_denoiseStrength <= 1e-4f || _denoiseIterations <= 0)
        return sourceTex;
    if (!ensureDenoiseResources(width, height))
        return sourceTex;

    GLuint currentRead = sourceTex;
    GLuint currentWrite = _denoiseTex;

    const float bgThreshold = std::clamp(0.08f + 0.06f * _denoiseBackgroundSigma, 0.08f, 0.50f);
    const float rangeSigma = std::clamp(0.015f + 0.09f * _denoiseStrength, 0.015f, 0.12f);

    GLint prevFBO = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    GLint prevViewport[4];
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    glUseProgram(_denoiseProgram);
    glUniform2f(_uDenoiseInvTexSizeLoc, 1.0f / std::max(width, 1), 1.0f / std::max(height, 1));
    glUniform1f(_uDenoiseStrengthLoc, _denoiseStrength);
    glUniform1f(_uDenoiseBgThresholdLoc, bgThreshold);
    glUniform1f(_uDenoiseRangeSigmaLoc, rangeSigma);

    for (int iter = 0; iter < _denoiseIterations; ++iter)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, _denoiseFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, currentWrite, 0);
        GLenum drawBuf = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &drawBuf);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            break;

        glViewport(0, 0, width, height);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, currentRead);
        glBindVertexArray(_quadVAO);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

        std::swap(currentRead, currentWrite);
        if (currentWrite != sourceTex)
            currentWrite = sourceTex;
        else
            currentWrite = _denoiseTex;
    }

    glBindVertexArray(0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    return currentRead;
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

void GlImageRenderer::destroyShaders()
{
    if (_shaderProgram)
    {
        glDeleteProgram(_shaderProgram);
        _shaderProgram = 0;
    }
    if (_statsProgram)
    {
        glDeleteProgram(_statsProgram);
        _statsProgram = 0;
    }
    if (_denoiseProgram)
    {
        glDeleteProgram(_denoiseProgram);
        _denoiseProgram = 0;
    }
}

void GlImageRenderer::uploadBaseTexture(const std::vector<float>& bayerOrGray, int width, int height)
{
    if (bayerOrGray.empty() || width <= 0 || height <= 0 || !_baseTexture)
    {
        _hasTexture = false;
        _previewDisplayTex = 0;
        return;
    }

    _imgWidth  = width;
    _imgHeight = height;
    _previewDisplayTex = 0;

    glBindTexture(GL_TEXTURE_2D, _baseTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // 归一化 float → 单通道 RED（float）
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, width, height,
                 0, GL_RED, GL_FLOAT, bayerOrGray.data());

    _hasTexture = true;
}

void GlImageRenderer::setBayerPattern(int pattern)
{
    _bayerPattern = pattern;
}

void GlImageRenderer::setWhiteBalance(float r, float g, float b)
{
    _wbR = r;
    _wbG = g;
    _wbB = b;
}

void GlImageRenderer::setLinearDenoiseConfig(bool enabled,
                                             float strength,
                                             float backgroundSigma,
                                             int iterations)
{
    _denoiseEnabled = enabled;
    _denoiseStrength = std::clamp(strength, 0.0f, 1.0f);
    _denoiseBackgroundSigma = std::max(backgroundSigma, 0.5f);
    _denoiseIterations = std::clamp(iterations, 1, 4);
}

bool GlImageRenderer::computeWhiteBalanceGpuImpl(float& outGainR,
                                                 float& outGainG,
                                                 float& outGainB,
                                                 bool backgroundNeutralization)
{
    if (!_hasTexture || !_shaderProgram || !_quadVAO)
        return false;

    // ==== 1. 备份当前状态（AutoStretch + WhiteBalance） ====
    bool  oldUseAuto   = _useAuto;
    float oldLow       = _autoLow;
    float oldHigh      = _autoHigh;
    float oldStrength  = _stretchStrength;
    float oldWbR       = _wbR;
    float oldWbG       = _wbG;
    float oldWbB       = _wbB;
    float oldZoom      = _zoom;
    float oldPanX      = _panX;
    float oldPanY      = _panY;
    bool  oldDenoiseEnabled = _denoiseEnabled;

    auto restorePreviewState = [&]() {
        _zoom = oldZoom;
        _panX = oldPanX;
        _panY = oldPanY;
    };

    auto restoreAllState = [&]() {
        _wbR = oldWbR;
        _wbG = oldWbG;
        _wbB = oldWbB;
        _useAuto = oldUseAuto;
        _autoLow = oldLow;
        _autoHigh = oldHigh;
        _stretchStrength = oldStrength;
        _denoiseEnabled = oldDenoiseEnabled;
        restorePreviewState();
        setWhiteBalance(_wbR, _wbG, _wbB);
        setAutoParams(_useAuto, _autoLow, _autoHigh, _stretchStrength);
    };

    // 临时设置白平衡为 1,1,1，关闭 AutoStretch，保持线性
    _wbR = _wbG = _wbB = 1.0f;
    _useAuto = false;
    _autoLow = 0.0f;
    _autoHigh = 1.0f;
    _zoom = 1.0f;
    _panX = 0.0f;
    _panY = 0.0f;
    _denoiseEnabled = false;
    setWhiteBalance(_wbR, _wbG, _wbB);
    setAutoParams(_useAuto, _autoLow, _autoHigh, _stretchStrength);

    // ==== 2. 用小尺寸预览渲染一张 RGB 图（GPU debayer 完成） ====
    const int S = 128;  // 统计用预览尺寸，降低 readback 与 CPU 统计开销
    if (!renderPreview(S, S))
    {
        restoreAllState();
        return false;
    }

    // 从预览 FBO 读回图像
    if (!_previewFBO || !_previewTex)
    {
        restoreAllState();
        return false;
    }

    GLint prevFBO = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);

    glBindFramebuffer(GL_FRAMEBUFFER, _previewFBO);

    std::vector<float> pixels;
    pixels.resize((size_t)S * S * 3);

    // 预览纹理是 GL_RGB8，将其作为 float 读回会自动归一化到 [0,1]
    glReadPixels(0, 0, S, S, GL_RGB, GL_FLOAT, pixels.data());

    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);

    // ==== 3. 在 CPU 上对这张 debayer 后的小图做稳健白平衡估计 ====
    struct Sample
    {
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
        float l = 0.0f;
    };

    std::vector<Sample> samples;
    samples.reserve((size_t)S * S);
    std::vector<float> luminanceValues;
    luminanceValues.reserve((size_t)S * S);

    for (int y = 0; y < S; ++y)
    {
        for (int x = 0; x < S; ++x)
        {
            size_t idx = ((size_t)y * S + x) * 3;
            float r = pixels[idx + 0];
            float g = pixels[idx + 1];
            float b = pixels[idx + 2];
            float l = 0.2126f * r + 0.7152f * g + 0.0722f * b;

            if (l <= 1e-5f)
                continue;

            samples.push_back({r, g, b, l});
            luminanceValues.push_back(l);
        }
    }

    if (samples.empty())
    {
        restoreAllState();
        return false;
    }

    std::sort(luminanceValues.begin(), luminanceValues.end());
    auto percentile = [&](float p) -> float
    {
        if (luminanceValues.empty())
            return 0.0f;
        p = std::clamp(p, 0.0f, 1.0f);
        const size_t idx = static_cast<size_t>(p * (luminanceValues.size() - 1));
        return luminanceValues[idx];
    };

    const float lumLow = percentile(0.10f);
    const float lumHigh = percentile(0.98f);
    const float bgLow = percentile(0.05f);
    const float bgHigh = percentile(0.45f);

    auto accumulate_neutral = [](const std::vector<Sample>& src,
                                 float low,
                                 float high,
                                 bool backgroundMode,
                                 double& sumR,
                                 double& sumG,
                                 double& sumB,
                                 size_t& count)
    {
        sumR = 0.0;
        sumG = 0.0;
        sumB = 0.0;
        count = 0;

        for (const Sample& s : src)
        {
            if (s.l < low || s.l > high)
                continue;

            const float maxc = std::max({s.r, s.g, s.b});
            const float minc = std::min({s.r, s.g, s.b});
            if (maxc <= 1e-6f)
                continue;

            const float saturation = (maxc - minc) / maxc;
            const float satLimit = backgroundMode
                                 ? (0.10f + 0.10f * std::clamp(s.l, 0.0f, 1.0f))
                                 : (0.20f + 0.25f * std::clamp(s.l, 0.0f, 1.0f));
            if (saturation > satLimit)
                continue;

            float weight = 1.0f - 0.7f * saturation;
            if (backgroundMode)
            {
                // 背景中性化更偏向暗部、低饱和区域，避免星点和主体颜色主导增益。
                const float normalizedL = std::clamp((s.l - low) / std::max(high - low, 1e-4f), 0.0f, 1.0f);
                weight *= (1.20f - 0.70f * normalizedL);
            }
            sumR += s.r * weight;
            sumG += s.g * weight;
            sumB += s.b * weight;
            if (weight > 1e-5f)
                ++count;
        }
    };

    double sumR = 0.0;
    double sumG = 0.0;
    double sumB = 0.0;
    size_t count = 0;
    if (backgroundNeutralization)
        accumulate_neutral(samples, bgLow, bgHigh, true, sumR, sumG, sumB, count);
    else
        accumulate_neutral(samples, lumLow, lumHigh, false, sumR, sumG, sumB, count);

    if (count < 128)
    {
        // 回退：放宽条件，避免在窄带或极暗画面上完全失效。
        sumR = 0.0;
        sumG = 0.0;
        sumB = 0.0;
        count = 0;
        for (const Sample& s : samples)
        {
            const float low = backgroundNeutralization ? percentile(0.02f) : percentile(0.03f);
            const float high = backgroundNeutralization ? percentile(0.70f) : percentile(0.995f);
            if (s.l < low || s.l > high)
                continue;
            const float maxc = std::max({s.r, s.g, s.b});
            const float minc = std::min({s.r, s.g, s.b});
            if (maxc <= 1e-6f)
                continue;
            const float saturation = (maxc - minc) / maxc;
            if (backgroundNeutralization && saturation > 0.22f)
                continue;
            sumR += s.r;
            sumG += s.g;
            sumB += s.b;
            ++count;
        }
    }

    if (count == 0)
    {
        restoreAllState();
        return false;
    }

    double meanR = sumR / static_cast<double>(count);
    double meanG = sumG / static_cast<double>(count);
    double meanB = sumB / static_cast<double>(count);

    if (meanR <= 0.0 || meanG <= 0.0 || meanB <= 0.0)
    {
        restoreAllState();
        return false;
    }

    double meanGrey = (meanR + meanG + meanB) / 3.0;

    float gR = static_cast<float>(meanGrey / meanR);
    float gG = static_cast<float>(meanGrey / meanG);
    float gB = static_cast<float>(meanGrey / meanB);

    auto clampGain = [backgroundNeutralization](float g) {
        if (backgroundNeutralization)
        {
            if (g < 0.55f) g = 0.55f;
            if (g > 1.80f) g = 1.80f;
        }
        else
        {
            if (g < 0.4f) g = 0.4f;
            if (g > 2.5f) g = 2.5f;
        }
        return g;
    };

    gR = clampGain(gR);
    gG = clampGain(gG);
    gB = clampGain(gB);

    // ==== 4. 把计算结果写回，并恢复 AutoStretch/视图状态 ====
    _wbR = gR;
    _wbG = gG;
    _wbB = gB;
    setWhiteBalance(_wbR, _wbG, _wbB);

    restorePreviewState();
    _useAuto         = oldUseAuto;
    _autoLow         = oldLow;
    _autoHigh        = oldHigh;
    _stretchStrength = oldStrength;
    _denoiseEnabled  = oldDenoiseEnabled;
    setAutoParams(_useAuto, _autoLow, _autoHigh, _stretchStrength);

    outGainR = gR;
    outGainG = gG;
    outGainB = gB;
    return true;
}

bool GlImageRenderer::computeAutoWhiteBalanceGpu(float& outGainR,
                                                 float& outGainG,
                                                 float& outGainB)
{
    return computeWhiteBalanceGpuImpl(outGainR, outGainG, outGainB, false);
}

bool GlImageRenderer::computeBackgroundNeutralizationGpu(float& outGainR,
                                                         float& outGainG,
                                                         float& outGainB)
{
    return computeWhiteBalanceGpuImpl(outGainR, outGainG, outGainB, true);
}

void GlImageRenderer::setStretchMode(int mode)
{
    _stretchMode = mode;
}

void GlImageRenderer::setAutoParams(bool useAuto, float low, float high, float strength)
{
    _useAuto         = useAuto;
    _autoLow         = low;
    _autoHigh        = high;
    _stretchStrength = strength;
}

void GlImageRenderer::setViewParams(float scale, float panX, float panY)
{
    _zoom = scale;
    _panX = panX;
    _panY = panY;
}

void GlImageRenderer::updateUniforms(int viewportWidth, int viewportHeight)
{
    glUniform1f(_uLowLoc,  _autoLow);
    glUniform1f(_uHighLoc, _autoHigh);
    glUniform1f(_uStretchStrengthLoc, _stretchStrength);
    glUniform1i(_uUseAutoLoc, _useAuto ? 1 : 0);

    glUniform1f(_uCurveBlackLoc, _curveBlack);
    glUniform1f(_uCurveWhiteLoc, _curveWhite);
    glUniform1f(_uCurveGammaLoc, _curveGamma);
    glUniform1i(_uUseCurveLoc, _useCurve ? 1 : 0);

    glUniform2f(_uTexSizeLoc,      (float)_imgWidth,  (float)_imgHeight);
    glUniform2f(_uViewportSizeLoc, (float)viewportWidth, (float)viewportHeight);

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

bool GlImageRenderer::computeAutoParamsGpu(bool useAuto,
                                           float blackClip,
                                           float whiteClip,
                                           float& outLow,
                                           float& outHigh)
{
    if (!_hasTexture || !_statsFBO || !_statsProgram || _imgWidth <= 0 || _imgHeight <= 0)
    {
        outLow = 0.0f;
        outHigh = 1.0f;
        return false;
    }

    GLint prevFBO = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    GLint prevViewport[4];
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    glBindFramebuffer(GL_FRAMEBUFFER, _statsFBO);
    glViewport(0, 0, _statsSize, _statsSize);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(_statsProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, _baseTexture);

    glUniform2f(_uStatsTexSizeLoc, (float)_imgWidth, (float)_imgHeight);
    glUniform1i(_uStatsBayerPatternLoc, _bayerPattern);
    glUniform3f(_uStatsWBGainLoc, _wbR, _wbG, _wbB);

    glBindVertexArray(_quadVAO);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);

    std::vector<float> lum;
    lum.resize((size_t)_statsSize * _statsSize);
    glReadPixels(0, 0, _statsSize, _statsSize, GL_RED, GL_FLOAT, lum.data());

    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    glUseProgram(0);

    if (lum.empty())
    {
        outLow = 0.0f;
        outHigh = 1.0f;
        return false;
    }

    // 百分位统计
    std::vector<float> sorted = lum;
    std::sort(sorted.begin(), sorted.end());

    size_t n = sorted.size();
    if (n == 0)
    {
        outLow = 0.0f;
        outHigh = 1.0f;
        return false;
    }

    auto clampPercent = [](float v) {
        if (v < 0.0f) return 0.0f;
        if (v > 100.0f) return 100.0f;
        return v;
    };

    blackClip = clampPercent(blackClip);
    whiteClip = clampPercent(whiteClip);

    float pLow  = useAuto ? (blackClip / 100.0f) : 0.0f;
    float pHigh = useAuto ? ((100.0f - whiteClip) / 100.0f) : 1.0f;

    size_t idxLow  = (size_t)(pLow  * (n - 1));
    size_t idxHigh = (size_t)(pHigh * (n - 1));
    if (idxLow >= n) idxLow = n - 1;
    if (idxHigh >= n) idxHigh = n - 1;
    if (idxLow > idxHigh) idxLow = 0;

    float low  = sorted[idxLow];
    float high = sorted[idxHigh];

    if (high <= low + 1e-4f)
        high = low + 1e-3f;

    outLow  = clamp01(low);
    outHigh = clamp01(high);

    // 直方图（基于拉伸后的亮度）
    _histogram.assign(_histBins, 0.0f);

    float range = std::max(outHigh - outLow, 1e-3f);
    float s = std::max(_stretchStrength, 1.0f);
    float asinhDenom = std::asinh(s);
    if (asinhDenom < 1e-6f) asinhDenom = 1e-6f;
    float logDenom = std::log(1.0f + s);
    if (logDenom < 1e-6f) logDenom = 1e-6f;

    for (float v : lum)
    {
        float t = (v - outLow) / range;
        t = clamp01(t);

        float y = t;
        if (_stretchMode == 0)
        {
            y = t;
        }
        else if (_stretchMode == 1)
        {
            y = std::asinh(s * t) / asinhDenom;
        }
        else if (_stretchMode == 2)
        {
            y = std::log(1.0f + s * t) / logDenom;
        }
        else if (_stretchMode == 3)
        {
            y = std::sqrt(t);
        }
        y = clamp01(y);

        int bin = (int)(y * _histBins);
        if (bin < 0) bin = 0;
        if (bin >= _histBins) bin = _histBins - 1;
        _histogram[bin] += 1.0f;
    }

    float maxCount = 0.0f;
    for (float c : _histogram)
        if (c > maxCount) maxCount = c;

    if (maxCount > 0.0f)
    {
        for (float& c : _histogram)
        {
            c /= maxCount;
            c = std::sqrt(c);   // 增强小值
        }
    }

    return true;
}

bool GlImageRenderer::getLuminanceHistogram(std::vector<float>& outHist) const
{
    if (_histogram.empty())
        return false;
    outHist = _histogram;
    return true;
}

bool GlImageRenderer::renderToImage(int width, int height, std::vector<unsigned char>& outRGB)
{
    if (!_hasTexture || !_shaderProgram || !_quadVAO || width <= 0 || height <= 0)
        return false;

    if (!_exportFBO)
        glGenFramebuffers(1, &_exportFBO);
    if (!_exportTex)
        glGenTextures(1, &_exportTex);

    glBindTexture(GL_TEXTURE_2D, _exportTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, width, height,
                 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    GLint prevFBO = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    GLint prevViewport[4];
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    glBindFramebuffer(GL_FRAMEBUFFER, _exportFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, _exportTex, 0);
    GLenum drawBuf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &drawBuf);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        std::cerr << "Export FBO incomplete\n";
        glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
        glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
        return false;
    }

    glViewport(0, 0, width, height);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(_shaderProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, _baseTexture);

    updateUniforms(width, height);

    glBindVertexArray(_quadVAO);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);

    GLuint exportReadTex = applyDenoisePass(_exportTex, width, height);
    if (exportReadTex != 0 && exportReadTex != _exportTex)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, _exportFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, exportReadTex, 0);
    }

    outRGB.resize((size_t)width * height * 3);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, outRGB.data());

    // 垂直翻转（OpenGL 左下原点，PNG 左上原点）
    int rowBytes = width * 3;
    for (int y = 0; y < height / 2; ++y)
    {
        unsigned char* row1 = outRGB.data() + y * rowBytes;
        unsigned char* row2 = outRGB.data() + (height - 1 - y) * rowBytes;
        for (int x = 0; x < rowBytes; ++x)
            std::swap(row1[x], row2[x]);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    glUseProgram(0);

    return true;
}

bool GlImageRenderer::renderPreview(int width, int height)
{
    if (!_hasTexture || !_shaderProgram || !_quadVAO)
        return false;
    if (width <= 0 || height <= 0)
        return false;

    if (!_previewFBO)
        glGenFramebuffers(1, &_previewFBO);
    if (!_previewTex)
        glGenTextures(1, &_previewTex);

    if (width != _previewW || height != _previewH)
    {
        _previewW = width;
        _previewH = height;

        glBindTexture(GL_TEXTURE_2D, _previewTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, _previewW, _previewH,
                     0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    GLint prevFBO = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    GLint prevViewport[4];
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    glBindFramebuffer(GL_FRAMEBUFFER, _previewFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, _previewTex, 0);
    GLenum drawBuf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &drawBuf);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        std::cerr << "Preview FBO incomplete\n";
        glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
        glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
        return false;
    }

    glViewport(0, 0, _previewW, _previewH);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(_shaderProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, _baseTexture);

    updateUniforms(_previewW, _previewH);

    glBindVertexArray(_quadVAO);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);

    _previewDisplayTex = _previewTex;
    GLuint denoisedPreviewTex = applyDenoisePass(_previewTex, _previewW, _previewH);
    if (denoisedPreviewTex != 0)
        _previewDisplayTex = denoisedPreviewTex;

    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    glUseProgram(0);

    return true;
}

bool GlImageRenderer::uploadPreviewRgb(const std::vector<unsigned char>& rgb, int width, int height)
{
    if (width <= 0 || height <= 0)
        return false;
    if (rgb.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 3)
        return false;

    if (!_previewTex)
        glGenTextures(1, &_previewTex);

    if (width != _previewW || height != _previewH)
    {
        _previewW = width;
        _previewH = height;
        glBindTexture(GL_TEXTURE_2D, _previewTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, _previewW, _previewH,
                     0, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    else
    {
        glBindTexture(GL_TEXTURE_2D, _previewTex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, _previewW, _previewH,
                        GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
    }

    _previewDisplayTex = _previewTex;
    return true;
}
