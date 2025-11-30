#pragma once

#include <string>
#include <vector>

enum class BayerPattern {
    NONE = 0,
    RGGB = 1,
    BGGR = 2,
    GRBG = 3,
    GBRG = 4
};

struct FitsImage {
    int width = 0;
    int height = 0;
    int channels = 1;          // 1: 单通道, 3: RGB
    BayerPattern bayer = BayerPattern::NONE;

    // 原始 FITS 数据，统一用 double 存
    std::vector<double> raw;

    // 显示用 RGB，0~1 浮点
    std::vector<float> rgb;

    bool isValid() const {
        return width > 0 && height > 0 && !raw.empty();
    }
};

// 从 FITS 文件读取数据
bool load_fits(const std::string& path, FitsImage& outImage, BayerPattern bayerHint);

// 把 0~1 RGB 映射到 8bit
std::vector<unsigned char> rgb_to_u8(const std::vector<float>& rgb, int width, int height);
