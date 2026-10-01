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
    int const groups = (width + 5) / 6;
    return static_cast<std::size_t>(groups) * 16;
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
} // namespace mwm
