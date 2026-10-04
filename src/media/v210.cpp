#include "media/v210.hpp"

#include <algorithm>
#include <cstring>

namespace mwm
{
namespace
{
void pack6(std::uint8_t* dst, int y0, int y1, int y2, int y3, int y4, int y5, int cb0, int cb1, int cb2, int cr0, int cr1, int cr2)
{
    auto const word = [](int a, int b, int c) {
        return static_cast<std::uint32_t>((a & 0x3ff) | ((b & 0x3ff) << 10) | ((c & 0x3ff) << 20));
    };
    std::uint32_t const words[4] = {word(cb0, y0, cr0), word(y1, cb1, y2), word(cr1, y3, cb2), word(y4, cr2, y5)};
    std::memcpy(dst, words, sizeof(words));
}
} // namespace

std::size_t v210Stride(int width)
{
    if (width < 6)
    {
        width = 6;
    }
    // MXL (and SMPTE/DeckLink) v210 rows are padded to 48 pixels = 128 bytes.
    return static_cast<std::size_t>((width + 47) / 48) * 128;
}

std::size_t v210FrameBytes(int width, int height)
{
    if (height < 1)
    {
        height = 1;
    }
    return v210Stride(width) * static_cast<std::size_t>(height);
}

void fillV210Black(std::uint8_t* dst, int width, int height)
{
    auto const stride = v210Stride(width);
    std::vector<std::uint8_t> line(stride, 0);
    int x = 0;
    while (x + 6 <= width)
    {
        pack6(line.data() + static_cast<std::size_t>(x / 6) * 16, 64, 64, 64, 64, 64, 64, 512, 512, 512, 512, 512, 512);
        x += 6;
    }
    for (int y = 0; y < height; ++y)
    {
        std::memcpy(dst + static_cast<std::size_t>(y) * stride, line.data(), stride);
    }
}

void fillV210Bar(std::uint8_t* dst, int width, int height, int frame)
{
    fillV210Black(dst, width, height);
    int const bar = ((frame * 6) % std::max(width, 6));
    int const aligned = bar - (bar % 6);
    if (aligned + 6 > width)
    {
        return;
    }
    auto const stride = v210Stride(width);
    std::uint8_t group[16];
    pack6(group, 940, 940, 940, 940, 940, 940, 512, 512, 512, 512, 512, 512);
    for (int y = 0; y < height; ++y)
    {
        std::memcpy(dst + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(aligned / 6) * 16, group, sizeof(group));
    }
}

std::size_t previewBytes(int outWidth, int outHeight)
{
    return static_cast<std::size_t>(outWidth) * static_cast<std::size_t>(outHeight) * 3 / 2;
}

void v210ToPreview(std::uint8_t const* v210, int width, int height, bool interlaced, int outWidth, int outHeight, bool nv12, std::uint8_t* dst)
{
    if (v210 == nullptr || dst == nullptr || width < 2 || height < 1 || outWidth < 2 || outHeight < 2 || (outWidth & 1) != 0 || (outHeight & 1) != 0)
    {
        return;
    }
    std::size_t const stride = v210Stride(width);
    // Interlaced: the first field only (bob), so lines of the two fields never mix.
    int const rows = interlaced ? std::max(1, height / 2) : height;
    int const step = interlaced ? 2 : 1;
    int const chromaWidth = width / 2;
    int const outChromaWidth = outWidth / 2;
    // Source column range of each output luma column and each output chroma column.
    std::vector<int> lumaFirst(static_cast<std::size_t>(outWidth) + 1);
    std::vector<int> chromaFirst(static_cast<std::size_t>(outChromaWidth) + 1);
    for (int x = 0; x <= outWidth; ++x)
    {
        lumaFirst[static_cast<std::size_t>(x)] = static_cast<int>(static_cast<long long>(x) * width / outWidth);
    }
    for (int x = 0; x <= outChromaWidth; ++x)
    {
        chromaFirst[static_cast<std::size_t>(x)] = static_cast<int>(static_cast<long long>(x) * chromaWidth / outChromaWidth);
    }
    std::vector<std::uint16_t> y(static_cast<std::size_t>(width));
    std::vector<std::uint16_t> cb(static_cast<std::size_t>(chromaWidth));
    std::vector<std::uint16_t> cr(static_cast<std::size_t>(chromaWidth));
    std::vector<std::uint32_t> sumY(static_cast<std::size_t>(outWidth));
    std::vector<std::uint32_t> sumCb(static_cast<std::size_t>(outChromaWidth));
    std::vector<std::uint32_t> sumCr(static_cast<std::size_t>(outChromaWidth));
    std::uint8_t* planeY = dst;
    std::uint8_t* planeU = dst + static_cast<std::size_t>(outWidth) * static_cast<std::size_t>(outHeight);
    std::uint8_t* planeV = planeU + static_cast<std::size_t>(outChromaWidth) * static_cast<std::size_t>(outHeight / 2);
    auto unpackRow = [&](int row) {
        auto const* line = v210 + static_cast<std::size_t>(row) * stride;
        for (int group = 0; group * 6 < width; ++group)
        {
            std::uint32_t w[4];
            std::memcpy(w, line + static_cast<std::size_t>(group) * 16, sizeof(w));
            std::uint16_t const ys[6] = {static_cast<std::uint16_t>((w[0] >> 10) & 0x3ff), static_cast<std::uint16_t>(w[1] & 0x3ff),
                static_cast<std::uint16_t>((w[1] >> 20) & 0x3ff), static_cast<std::uint16_t>((w[2] >> 10) & 0x3ff), static_cast<std::uint16_t>(w[3] & 0x3ff),
                static_cast<std::uint16_t>((w[3] >> 20) & 0x3ff)};
            std::uint16_t const cbs[3] = {static_cast<std::uint16_t>(w[0] & 0x3ff), static_cast<std::uint16_t>((w[1] >> 10) & 0x3ff),
                static_cast<std::uint16_t>((w[2] >> 20) & 0x3ff)};
            std::uint16_t const crs[3] = {static_cast<std::uint16_t>((w[0] >> 20) & 0x3ff), static_cast<std::uint16_t>(w[2] & 0x3ff),
                static_cast<std::uint16_t>((w[3] >> 10) & 0x3ff)};
            for (int i = 0; i < 6 && group * 6 + i < width; ++i)
            {
                y[static_cast<std::size_t>(group * 6 + i)] = ys[i];
            }
            for (int i = 0; i < 3 && group * 3 + i < chromaWidth; ++i)
            {
                cb[static_cast<std::size_t>(group * 3 + i)] = cbs[i];
                cr[static_cast<std::size_t>(group * 3 + i)] = crs[i];
            }
        }
    };
    // Area average, 10 to 8 bit. Output rows are taken in pairs: the pair's chroma
    // row averages the source chroma of both (4:2:2 to 4:2:0).
    for (int pair = 0; pair < outHeight / 2; ++pair)
    {
        std::fill(sumCb.begin(), sumCb.end(), 0);
        std::fill(sumCr.begin(), sumCr.end(), 0);
        std::uint32_t chromaRows = 0;
        for (int half = 0; half < 2; ++half)
        {
            int const oy = pair * 2 + half;
            int const first = static_cast<int>(static_cast<long long>(oy) * rows / outHeight);
            int const last = std::max(first + 1, static_cast<int>(static_cast<long long>(oy + 1) * rows / outHeight));
            std::fill(sumY.begin(), sumY.end(), 0);
            for (int sy = first; sy < last && sy < rows; ++sy)
            {
                unpackRow(sy * step);
                for (int ox = 0; ox < outWidth; ++ox)
                {
                    for (int sx = lumaFirst[static_cast<std::size_t>(ox)]; sx < std::max(lumaFirst[static_cast<std::size_t>(ox) + 1], lumaFirst[static_cast<std::size_t>(ox)] + 1); ++sx)
                    {
                        sumY[static_cast<std::size_t>(ox)] += y[static_cast<std::size_t>(std::min(sx, width - 1))];
                    }
                }
                for (int ox = 0; ox < outChromaWidth; ++ox)
                {
                    for (int sx = chromaFirst[static_cast<std::size_t>(ox)];
                         sx < std::max(chromaFirst[static_cast<std::size_t>(ox) + 1], chromaFirst[static_cast<std::size_t>(ox)] + 1); ++sx)
                    {
                        sumCb[static_cast<std::size_t>(ox)] += cb[static_cast<std::size_t>(std::min(sx, chromaWidth - 1))];
                        sumCr[static_cast<std::size_t>(ox)] += cr[static_cast<std::size_t>(std::min(sx, chromaWidth - 1))];
                    }
                }
                ++chromaRows;
            }
            std::uint32_t const lines = static_cast<std::uint32_t>(std::max(1, std::min(last, rows) - first));
            for (int ox = 0; ox < outWidth; ++ox)
            {
                std::uint32_t const cols = static_cast<std::uint32_t>(std::max(1, lumaFirst[static_cast<std::size_t>(ox) + 1] - lumaFirst[static_cast<std::size_t>(ox)]));
                std::uint32_t const count = cols * lines * 4;
                planeY[static_cast<std::size_t>(oy) * static_cast<std::size_t>(outWidth) + static_cast<std::size_t>(ox)] =
                    static_cast<std::uint8_t>(std::min<std::uint32_t>(255, (sumY[static_cast<std::size_t>(ox)] + count / 2) / count));
            }
        }
        chromaRows = std::max<std::uint32_t>(1, chromaRows);
        for (int ox = 0; ox < outChromaWidth; ++ox)
        {
            std::uint32_t const cols =
                static_cast<std::uint32_t>(std::max(1, chromaFirst[static_cast<std::size_t>(ox) + 1] - chromaFirst[static_cast<std::size_t>(ox)]));
            std::uint32_t const count = cols * chromaRows * 4;
            auto const u = static_cast<std::uint8_t>(std::min<std::uint32_t>(255, (sumCb[static_cast<std::size_t>(ox)] + count / 2) / count));
            auto const v = static_cast<std::uint8_t>(std::min<std::uint32_t>(255, (sumCr[static_cast<std::size_t>(ox)] + count / 2) / count));
            if (nv12)
            {
                planeU[static_cast<std::size_t>(pair) * static_cast<std::size_t>(outWidth) + static_cast<std::size_t>(ox) * 2] = u;
                planeU[static_cast<std::size_t>(pair) * static_cast<std::size_t>(outWidth) + static_cast<std::size_t>(ox) * 2 + 1] = v;
            }
            else
            {
                planeU[static_cast<std::size_t>(pair) * static_cast<std::size_t>(outChromaWidth) + static_cast<std::size_t>(ox)] = u;
                planeV[static_cast<std::size_t>(pair) * static_cast<std::size_t>(outChromaWidth) + static_cast<std::size_t>(ox)] = v;
            }
        }
    }
}
} // namespace mwm
