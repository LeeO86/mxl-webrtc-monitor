#pragma once

#include <cstdint>
#include <string>
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

// CPU code of v210ToPreview's downscale path, the one every real preview takes (the
// other cases stay scalar). Both give the same bytes.
enum class PreviewSimd
{
    Scalar,
    Avx2,
};

struct SimdChoice
{
    PreviewSimd level = PreviewSimd::Scalar;
    std::string requested; // MONITOR_SIMD in lower case, "auto" when unset
    std::string warning;   // set when the request is not followed
};

// "scalar" or "avx2".
char const* simdName(PreviewSimd level);

// True when the CPU has AVX2 and the OS saves its registers. The image is built for
// plain x86-64, so the AVX2 code runs only after this check.
bool cpuHasAvx2();

// The level for a MONITOR_SIMD value: auto (or empty) takes AVX2 when the CPU has it,
// scalar forces the scalar code. avx2 on a CPU without AVX2 gives scalar and an unknown
// value gives auto, both with a warning.
SimdChoice chooseSimd(std::string const& request, bool avx2);

// Reads MONITOR_SIMD, sets the level and returns the choice (logged at start).
SimdChoice applySimdEnvironment();

// The level in use; until it is set, the best the CPU runs.
PreviewSimd previewSimd();

// Sets the level (tests, benchmarks). AVX2 on a CPU without it gives scalar.
void setPreviewSimd(PreviewSimd level);
} // namespace mwm
