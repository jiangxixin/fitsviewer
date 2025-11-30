#include "Debayer.h"
#include <algorithm>
#include <iostream>

static void compute_minmax(const std::vector<double>& v, double& mn, double& mx)
{
    if (v.empty())
    {
        mn = 0.0;
        mx = 1.0;
        return;
    }
    auto [itMin, itMax] = std::minmax_element(v.begin(), v.end());
    mn = *itMin;
    mx = *itMax;
    if (mn == mx)
    {
        mn = 0.0;
        mx = 1.0;
    }
}

// “概念 RGGB 坐标 (cx,cy)” -> 实际 raw 坐标 (px,py)
static void conceptual_to_physical(
    int cx, int cy,
    int W, int H,
    BayerPattern pattern,
    int& px, int& py)
{
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx >= W) cx = W - 1;
    if (cy >= H) cy = H - 1;

    switch (pattern)
    {
        case BayerPattern::RGGB:
            px = cx;
            py = cy;
            break;
        case BayerPattern::BGGR:
            // 旋转 180°
            px = (W - 1) - cx;
            py = (H - 1) - cy;
            break;
        case BayerPattern::GRBG:
            // 水平翻转
            px = (W - 1) - cx;
            py = cy;
            break;
        case BayerPattern::GBRG:
            // 垂直翻转
            px = cx;
            py = (H - 1) - cy;
            break;
        case BayerPattern::NONE:
        default:
            px = cx;
            py = cy;
            break;
    }

    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px >= W) px = W - 1;
    if (py >= H) py = H - 1;
}

bool debayer_bilinear(const FitsImage& in, FitsImage& out)
{
    if (!in.isValid())
        return false;

    // 非 Bayer 或 3 通道：灰度转 RGB
    if (in.bayer == BayerPattern::NONE || in.channels == 3)
    {
        out = in;
        out.channels = 3;
        out.bayer = BayerPattern::NONE;
        out.rgb.resize(static_cast<size_t>(out.width) * out.height * 3);

        double mn, mx;
        compute_minmax(in.raw, mn, mx);
        double range = mx - mn;

        for (int y = 0; y < out.height; ++y)
        {
            for (int x = 0; x < out.width; ++x)
            {
                size_t idx = static_cast<size_t>(y) * out.width + x;
                float v = static_cast<float>((in.raw[idx] - mn) / range);
                v = std::clamp(v, 0.0f, 1.0f);
                out.rgb[idx * 3 + 0] = v;
                out.rgb[idx * 3 + 1] = v;
                out.rgb[idx * 3 + 2] = v;
            }
        }
        return true;
    }

    if (in.bayer != BayerPattern::RGGB &&
        in.bayer != BayerPattern::BGGR &&
        in.bayer != BayerPattern::GRBG &&
        in.bayer != BayerPattern::GBRG)
    {
        std::cerr << "debayer_bilinear: unsupported bayer pattern.\n";
        return false;
    }

    const int W = in.width;
    const int H = in.height;

    out.width = W;
    out.height = H;
    out.channels = 3;
    out.bayer = BayerPattern::NONE;
    out.raw = in.raw;
    out.rgb.assign(static_cast<size_t>(W) * H * 3, 0.0f);

    double mn, mx;
    compute_minmax(in.raw, mn, mx);
    double range = mx - mn;

    auto norm = [&](double v) -> float {
        float t = static_cast<float>((v - mn) / range);
        return std::clamp(t, 0.0f, 1.0f);
    };

    auto get_norm = [&](int cx, int cy) -> float {
        int px = cx, py = cy;
        conceptual_to_physical(cx, cy, W, H, in.bayer, px, py);
        size_t idx = static_cast<size_t>(py) * W + px;
        return norm(in.raw[idx]);
    };

    for (int y = 0; y < H; ++y)
    {
        for (int x = 0; x < W; ++x)
        {
            bool yEven = (y % 2 == 0);
            bool xEven = (x % 2 == 0);

            float R = 0.0f;
            float G = 0.0f;
            float B = 0.0f;

            if (yEven && xEven)
            {
                // R
                R = get_norm(x, y);
                G = 0.25f * (get_norm(x - 1, y) +
                             get_norm(x + 1, y) +
                             get_norm(x, y - 1) +
                             get_norm(x, y + 1));
                B = 0.25f * (get_norm(x - 1, y - 1) +
                             get_norm(x + 1, y - 1) +
                             get_norm(x - 1, y + 1) +
                             get_norm(x + 1, y + 1));
            }
            else if (yEven && !xEven)
            {
                // G on R 行
                G = get_norm(x, y);
                R = 0.5f * (get_norm(x - 1, y) + get_norm(x + 1, y));
                B = 0.5f * (get_norm(x, y - 1) + get_norm(x, y + 1));
            }
            else if (!yEven && xEven)
            {
                // G on B 行
                G = get_norm(x, y);
                R = 0.5f * (get_norm(x, y - 1) + get_norm(x, y + 1));
                B = 0.5f * (get_norm(x - 1, y) + get_norm(x + 1, y));
            }
            else
            {
                // B
                B = get_norm(x, y);
                G = 0.25f * (get_norm(x - 1, y) +
                             get_norm(x + 1, y) +
                             get_norm(x, y - 1) +
                             get_norm(x, y + 1));
                R = 0.25f * (get_norm(x - 1, y - 1) +
                             get_norm(x + 1, y - 1) +
                             get_norm(x - 1, y + 1) +
                             get_norm(x + 1, y + 1));
            }

            size_t dst = static_cast<size_t>(y) * W + x;
            out.rgb[dst * 3 + 0] = R;
            out.rgb[dst * 3 + 1] = G;
            out.rgb[dst * 3 + 2] = B;
        }
    }

    return true;
}
