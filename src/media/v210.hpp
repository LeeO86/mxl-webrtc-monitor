#pragma once

#include <cstdint>
#include <vector>

namespace mwm
{
// Bytes in one v210 line. Width should be a multiple of 6.
std::size_t v210Stride(int width);
std::size_t v210FrameBytes(int width, int height);

// 10-bit studio black (Y=64, Cb=Cr=512).
void fillV210Black(std::uint8_t* dst, int width, int height);

// A moving vertical bar so successive frames are not identical.
void fillV210Bar(std::uint8_t* dst, int width, int height, int frame);
} // namespace mwm
