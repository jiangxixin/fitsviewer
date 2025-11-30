#pragma once

#include "FitsImage.h"

// 全分辨率双线性去拜耳：支持 RGGB / BGGR / GRBG / GBRG
bool debayer_bilinear(const FitsImage& in, FitsImage& out);
