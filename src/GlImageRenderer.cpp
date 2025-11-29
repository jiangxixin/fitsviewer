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

uniform sampler2D uBaseTex;

uniform float uLow;
uniform float uHigh;
uniform float uStretchStrength;
uniform bool  uUseAuto;

uniform float uCurveBlack;
uniform float uCurveWhite;
uniform float uCurveGamma;
uniform bool  uUseCurve;

uniform vec2 uTexSize;
uniform vec2 uViewportSize;

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

void main()
{
    // 保持长宽比：根据纹理和视口比例缩放 uv
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

    vec3 c = texture(uBaseTex, uv).rgb;

    if (uUseAuto)
    {
        float range = max(uHigh - uLow, 1e-3);
        vec3 t = (c - vec3(uLow)) / range;
        t = clamp(t, 0.0, 1.0);

        float s = max(uStretchStrength, 1.0);
        float denom = asinh(s);
        vec3 stretched = asinh(s * t) / denom;
        c = clamp(stretched, 0.0, 1.0);
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

void GlImageRenderer::uploadBaseTexture(const std::vector<float>& linearRgb, int width, int height)
{
    if (linearRgb.empty() || width <= 0 || height <= 0 || !_baseTexture)
    {
        _hasTexture = false;
        return;
    }

    _imgWidth  = width;
    _imgHeight = height;

    glBindTexture(GL_TEXTURE_2D, _baseTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // 用 float 纹理保存线性 RGB 数据
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, width, height,
                 0, GL_RGB, GL_FLOAT, linearRgb.data());

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
    _useCurve    = useCurve;
    _curveBlack  = black;
    _curveWhite  = white;
    _curveGamma  = gamma;
}

void GlImageRenderer::updateUniforms(int viewportWidth, int viewportHeight)
{
    if (!_shaderProgram)
        return;

    glUseProgram(_shaderProgram);

    glUniform1f(_uLowLoc,  _autoLow);
    glUniform1f(_uHighLoc, _autoHigh);
    glUniform1f(_uStretchStrengthLoc, _stretchStrength);
    glUniform1i(_uUseAutoLoc, _useAuto ? 1 : 0);

    glUniform1f(_uCurveBlackLoc, _curveBlack);
    glUniform1f(_uCurveWhiteLoc, _curveWhite);
    glUniform1f(_uCurveGammaLoc, _curveGamma);
    glUniform1i(_uUseCurveLoc, _useCurve ? 1 : 0);

    glUniform2f(_uTexSizeLoc,      (float)_imgWidth,      (float)_imgHeight);
    glUniform2f(_uViewportSizeLoc, (float)viewportWidth, (float)viewportHeight);
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
