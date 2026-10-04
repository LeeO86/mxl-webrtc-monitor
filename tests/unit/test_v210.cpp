#include "doctest/doctest.h"

#include "media/v210.hpp"

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
} // namespace

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
