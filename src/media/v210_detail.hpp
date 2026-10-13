#pragma once

// Shared by the scalar (v210.cpp) and the AVX2 (v210_avx2.cpp) preview conversion.

#include <cstddef>
#include <cstdint>

namespace mwm::detail
{
// Largest count a Divider takes.
constexpr long long kMaxDivisor = 1LL << 22;

// (sum + count/2) / count by one multiply and a shift (Granlund and Montgomery): with
// 2^l >= count and m = ceil(2^(2l+9) / count) it is exact for every n < 2^(l+9), and n is
// at most 256.25·count (samples are at most 1023, count is four times their number).
// n·m stays below 2^64 for counts up to kMaxDivisor. Every output pixel divides, and the
// div instruction was most of the preview's time.
struct Divider
{
    std::uint64_t magic = 1;
    std::uint32_t shift = 0;
    std::uint32_t half = 0;

    explicit Divider(std::uint32_t d)
        : half(d / 2)
    {
        std::uint32_t l = 0;
        while ((std::uint64_t{1} << l) < d)
        {
            ++l;
        }
        shift = 2 * l + 9;
        magic = ((std::uint64_t{1} << shift) + d - 1) / d;
    }

    [[nodiscard]] std::uint32_t rounded(std::uint32_t sum) const
    {
        return static_cast<std::uint32_t>((std::uint64_t{sum + half} * magic) >> shift);
    }
};

#if defined(__x86_64__)
// v210ToPreview's downscale path with AVX2: the same integer sums and dividers as the scalar
// version, so the same bytes. Call it only when cpuHasAvx2().
void downscaleToPreviewAvx2(std::uint8_t const* v210, int width, int rows, int step, std::size_t stride, int outWidth, int outHeight, bool nv12,
    std::uint8_t* dst);
#endif
} // namespace mwm::detail
