#pragma once

#include <vector>

// NINA 风格 auto stretch（背景 + 百分位 + arcsinh）
void auto_stretch(
    std::vector<float>& rgb,
    float black_clip = 0.1f,      // %
    float white_clip = 0.1f,      // %
    float stretch_strength = 5.0f // arcsinh 强度
);

// 手动 tone curve：黑点 / 白点 / gamma
inline float tone_curve(float x, float black, float white, float gamma)
{
    if (x <= black) return 0.0f;
    if (x >= white) return 1.0f;
    float t = (x - black) / (white - black);
    if (gamma <= 0.0f) gamma = 1.0f;
    float ginv = 1.0f / gamma;
    float y = std::pow(t, ginv);
    if (y < 0.0f) y = 0.0f;
    if (y > 1.0f) y = 1.0f;
    return y;
}
