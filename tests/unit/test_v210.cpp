#include "doctest/doctest.h"

#include "media/v210.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace
{
// One v210 frame where every pixel has the given 10-bit values.
std::vector<std::uint8_t> solid(int width, int height, int y, int cb, int cr)
{
    std::vector<std::uint8_t> frame(mwm::v210FrameBytes(width, height), 0);
    auto const word = [](int a, int b, int c) {
        return static_cast<std::uint32_t>((a & 0x3ff) | ((b & 0x3ff) << 10) | ((c & 0x3ff) << 20));
    };
    std::uint32_t const group[4] = {word(cb, y, cr), word(y, cb, y), word(cr, y, cb), word(y, cr, y)};
    for (int row = 0; row < height; ++row)
    {
        for (int g = 0; g * 6 < width; ++g)
        {
            std::memcpy(frame.data() + static_cast<std::size_t>(row) * mwm::v210Stride(width) + static_cast<std::size_t>(g) * 16, group, sizeof(group));
        }
    }
    return frame;
}
// v210ToPreview before the per-row accumulators, as the reference.
void referencePreview(std::uint8_t const* v210, int width, int height, bool interlaced, int outWidth, int outHeight, bool nv12, std::uint8_t* dst)
{
    if (v210 == nullptr || dst == nullptr || width < 2 || height < 1 || outWidth < 2 || outHeight < 2 || (outWidth & 1) != 0 || (outHeight & 1) != 0)
    {
        return;
    }
    std::size_t const stride = mwm::v210Stride(width);
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

} // namespace

TEST_CASE("the downscale path gives the same bytes as the reference")
{
    std::uint32_t seed = 5;
    struct Case
    {
        int w, h;
        bool interlaced;
        int ow, oh;
    };
    for (Case c : {Case{1920, 1080, false, 640, 360}, Case{1920, 1080, true, 640, 360}, Case{1280, 720, false, 640, 360},
             Case{3840, 2160, false, 640, 360}, Case{1920, 1080, false, 426, 240}})
    {
        std::vector<std::uint8_t> frame(mwm::v210FrameBytes(c.w, c.h));
        for (auto& b : frame)
        {
            seed = seed * 1664525u + 1013904223u;
            b = static_cast<std::uint8_t>(seed >> 24);
        }
        for (bool nv12 : {false, true})
        {
            std::vector<std::uint8_t> expected(mwm::previewBytes(c.ow, c.oh)), actual(mwm::previewBytes(c.ow, c.oh));
            referencePreview(frame.data(), c.w, c.h, c.interlaced, c.ow, c.oh, nv12, expected.data());
            mwm::v210ToPreview(frame.data(), c.w, c.h, c.interlaced, c.ow, c.oh, nv12, actual.data());
            CHECK(expected == actual);
        }
    }
}

TEST_CASE("v210 rows are padded to 128 bytes like MXL")
{
    CHECK(mwm::v210Stride(1920) == 5120);
    CHECK(mwm::v210Stride(1280) == 3456);
    CHECK(mwm::v210Stride(720) == 1920);
}

TEST_CASE("preview from v210 keeps a solid colour and lays out I420 and NV12")
{
    auto const frame = solid(1920, 1080, 940, 320, 700);
    std::vector<std::uint8_t> i420(mwm::previewBytes(640, 360));
    mwm::v210ToPreview(frame.data(), 1920, 1080, false, 640, 360, false, i420.data());
    std::size_t const luma = 640 * 360;
    std::size_t const chroma = 320 * 180;
    CHECK(i420[0] == 235);
    CHECK(i420[luma - 1] == 235);
    CHECK(i420[luma] == 80);
    CHECK(i420[luma + chroma - 1] == 80);
    CHECK(i420[luma + chroma] == 175);
    CHECK(i420[luma + 2 * chroma - 1] == 175);

    std::vector<std::uint8_t> nv12(mwm::previewBytes(640, 360));
    mwm::v210ToPreview(frame.data(), 1920, 1080, true, 640, 360, true, nv12.data());
    CHECK(nv12[1234] == 235);
    CHECK(nv12[luma] == 80);
    CHECK(nv12[luma + 1] == 175);
    CHECK(nv12[nv12.size() - 2] == 80);
    CHECK(nv12[nv12.size() - 1] == 175);
}

TEST_CASE("preview averages the source area of each output pixel")
{
    // Left half black (64), right half white (940): a 2:1 preview splits exactly.
    auto frame = solid(96, 4, 64, 512, 512);
    auto const white = solid(96, 4, 940, 512, 512);
    for (int row = 0; row < 4; ++row)
    {
        std::memcpy(frame.data() + static_cast<std::size_t>(row) * mwm::v210Stride(96) + 128, white.data() + 128, 128);
    }
    std::vector<std::uint8_t> out(mwm::previewBytes(48, 2));
    mwm::v210ToPreview(frame.data(), 96, 4, false, 48, 2, false, out.data());
    CHECK(out[0] == 16);
    CHECK(out[23] == 16);
    CHECK(out[24] == 235);
    CHECK(out[47] == 235);
}
