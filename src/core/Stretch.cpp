#include "Stretch.h"
#include <algorithm>
#include <cmath>

static float clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static float percentile(const std::vector<float>& data, float percent)
{
    if (data.empty())
        return 0.0f;

    std::vector<float> tmp = data;
    std::sort(tmp.begin(), tmp.end());

    percent = std::clamp(percent, 0.0f, 100.0f);
    float p = percent / 100.0f;
    size_t n = tmp.size();
    size_t idx = static_cast<size_t>(p * (n - 1));
    if (idx >= n) idx = n - 1;
    return tmp[idx];
}

static void median_and_mad(const std::vector<float>& data, float& median, float& mad)
{
    if (data.empty())
    {
        median = 0.0f;
        mad = 0.0f;
        return;
    }

    std::vector<float> tmp = data;
    std::sort(tmp.begin(), tmp.end());
    size_t n = tmp.size();
    size_t mid = n / 2;

    if (n % 2 == 0)
        median = 0.5f * (tmp[mid - 1] + tmp[mid]);
    else
        median = tmp[mid];

    std::vector<float> devs;
    devs.reserve(n);
    for (float v : data)
        devs.push_back(std::fabs(v - median));

    std::sort(devs.begin(), devs.end());
    if (n % 2 == 0)
        mad = 0.5f * (devs[mid - 1] + devs[mid]);
    else
        mad = devs[mid];

    if (mad < 1e-6f)
        mad = 1e-6f;
}

void auto_stretch(
    std::vector<float>& rgb,
    float black_clip,
    float white_clip,
    float stretch_strength)
{
    if (rgb.empty())
        return;

    std::vector<float> lum;
    lum.reserve(rgb.size() / 3);
    for (size_t i = 0; i + 2 < rgb.size(); i += 3)
    {
        float r = rgb[i];
        float g = rgb[i + 1];
        float b = rgb[i + 2];
        float l = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        lum.push_back(clamp01(l));
    }

    float lowP = percentile(lum, black_clip);
    float highP = percentile(lum, 100.0f - white_clip);

    float median = 0.0f, mad = 0.0f;
    median_and_mad(lum, median, mad);

    const float kSigma = 1.5f;
    float candidateLow = clamp01(median - kSigma * mad);

    float low = std::max(candidateLow, lowP);
    float high = std::max(highP, low + 1e-3f);

    if (high <= low + 1e-4f)
    {
        low = lowP;
        high = std::max(highP, low + 1e-3f);
    }

    float range = high - low;

    if (stretch_strength < 1.0f)
        stretch_strength = 1.0f;

    float denom = std::asinh(stretch_strength);
    if (denom < 1e-6f)
        denom = 1e-6f;

    auto stretch = [&](float v) -> float {
        float t = (v - low) / range;
        t = clamp01(t);
        float s = std::asinh(stretch_strength * t) / denom;
        return clamp01(s);
    };

    for (float& v : rgb)
        v = stretch(v);
}
