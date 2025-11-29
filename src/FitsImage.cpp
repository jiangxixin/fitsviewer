#include "FitsImage.h"

#include <fitsio.h>
#include <algorithm>
#include <iostream>

bool load_fits(const std::string& path, FitsImage& outImage, BayerPattern bayerHint)
{
    fitsfile* fptr = nullptr;
    int status = 0;

    if (fits_open_file(&fptr, path.c_str(), READONLY, &status))
    {
        fits_report_error(stderr, status);
        return false;
    }

    int bitpix = 0, naxis = 0;
    long naxes[3] = {1, 1, 1};

    if (fits_get_img_param(fptr, 3, &bitpix, &naxis, naxes, &status))
    {
        fits_report_error(stderr, status);
        fits_close_file(fptr, &status);
        return false;
    }

    if (naxis < 2)
    {
        std::cerr << "Image has less than 2 dimensions.\n";
        fits_close_file(fptr, &status);
        return false;
    }

    long width = naxes[0];
    long height = naxes[1];
    long depth = (naxis >= 3) ? naxes[2] : 1;

    outImage.width = static_cast<int>(width);
    outImage.height = static_cast<int>(height);
    outImage.channels = 1;
    outImage.bayer = bayerHint;
    outImage.raw.clear();

    long npixels = width * height * depth;
    outImage.raw.resize(npixels);

    long fpixel[3] = {1, 1, 1};

    if (fits_read_pix(fptr, TDOUBLE, fpixel, npixels, nullptr,
                      outImage.raw.data(), nullptr, &status))
    {
        fits_report_error(stderr, status);
        fits_close_file(fptr, &status);
        return false;
    }

    fits_close_file(fptr, &status);

    if (depth == 3)
    {
        outImage.channels = 3;
        outImage.bayer = BayerPattern::NONE;
    }

    return true;
}

std::vector<unsigned char> rgb_to_u8(const std::vector<float>& rgb, int width, int height)
{
    std::vector<unsigned char> out;
    out.resize(static_cast<size_t>(width) * height * 3);

    for (size_t i = 0; i < out.size(); ++i)
    {
        float v = rgb[i];
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        out[i] = static_cast<unsigned char>(v * 255.0f + 0.5f);
    }

    return out;
}
