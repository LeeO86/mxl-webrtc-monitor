#pragma once

#include <cstdint>
#include <vector>

namespace mwm
{
// Bytes in one v210 line: 48 pixels per 128 bytes, padded like MXL and DeckLink.
std::size_t v210Stride(int width);
std::size_t v210FrameBytes(int width, int height);

// 10-bit studio black (Y=64, Cb=Cr=512).
void fillV210Black(std::uint8_t* dst, int width, int height);

// A moving vertical bar so successive frames are not identical.
void fillV210Bar(std::uint8_t* dst, int width, int height, int frame);

// Bytes of an 8-bit 4:2:0 picture (I420 or NV12).
std::size_t previewBytes(int outWidth, int outHeight);

// The encoder's picture straight from packed v210: area average to outWidth x
// outHeight (both even), 10 to 8 bit, 4:2:0 as I420 or NV12. Interlaced sources use
// their first field (bob). One pass over the source and no full-size intermediate,
// instead of converting and scaling the full frame in GStreamer.
void v210ToPreview(std::uint8_t const* v210, int width, int height, bool interlaced, int outWidth, int outHeight, bool nv12, std::uint8_t* dst);
} // namespace mwm
