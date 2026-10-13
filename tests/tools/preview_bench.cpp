// mwm-preview-bench: v210ToPreview per frame, scalar against AVX2, and whether both give the
// same bytes. Not part of the image; CI runs it to print the runner's numbers.
//
// Usage: mwm-preview-bench [seconds per run] [frames in rotation] [case]
//   rotation 8 (default): a new grain per call (8 x 5.5 MB at 1080p is more than a server's L3),
//   as in production; 1: warm cache. case: one of the names below, default all.
//   MONITOR_SIMD=scalar or avx2 runs that level only (for perf stat); unset runs both.
// Prints per case and level: ms per frame and cores per channel at 50 frames/s.

#include "media/v210.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
struct Case
{
    char const* name;
    int w, h;
    bool interlaced;
    int ow, oh;
    bool nv12;
};

// Random legal words; integer code takes the same time for any content.
std::vector<std::uint8_t> randomFrame(int w, int h, std::uint32_t seed)
{
    std::vector<std::uint8_t> out(mwm::v210FrameBytes(w, h));
    for (std::size_t i = 0; i + 4 <= out.size(); i += 4)
    {
        seed = seed * 1664525u + 1013904223u;
        std::uint32_t const word = seed & 0x3fffffffu;
        std::memcpy(out.data() + i, &word, sizeof(word));
    }
    return out;
}

struct Result
{
    long frames = 0;
    double ms = 0;
};

Result run(Case const& c, std::vector<std::vector<std::uint8_t>> const& src, std::vector<std::uint8_t>& dst, double seconds)
{
    using clock = std::chrono::steady_clock;
    auto const rotation = static_cast<long>(src.size());
    auto const convert = [&](long n) {
        mwm::v210ToPreview(src[static_cast<std::size_t>(n % rotation)].data(), c.w, c.h, c.interlaced, c.ow, c.oh, c.nv12, dst.data());
    };
    // Warm-up (page faults, caches, clock), then the timed loop.
    auto const warmEnd = clock::now() + std::chrono::milliseconds(500);
    for (long n = 0; clock::now() < warmEnd; ++n)
    {
        convert(n);
    }
    Result result;
    auto const t0 = clock::now();
    auto const end = t0 + std::chrono::duration<double>(seconds);
    auto now = t0;
    while (now < end)
    {
        convert(result.frames++);
        now = clock::now();
    }
    result.ms = std::chrono::duration<double, std::milli>(now - t0).count() / static_cast<double>(result.frames);
    return result;
}
} // namespace

int main(int argc, char** argv)
{
    double const seconds = argc > 1 ? std::atof(argv[1]) : 2.0;
    int const rotation = argc > 2 ? std::max(1, std::atoi(argv[2])) : 8;
    std::string const only = argc > 3 ? argv[3] : "all";
    Case const cases[] = {
        {"1080p-nv12", 1920, 1080, false, 960, 540, true},
        {"1080i-nv12", 1920, 1080, true, 960, 540, true},
        {"1080p-i420", 1920, 1080, false, 960, 540, false},
        {"720p-nv12", 1280, 720, false, 960, 540, true},
        {"2160p-nv12", 3840, 2160, false, 960, 540, true},
    };
    std::vector<mwm::PreviewSimd> levels;
    if (std::getenv("MONITOR_SIMD") != nullptr)
    {
        auto const choice = mwm::applySimdEnvironment();
        if (!choice.warning.empty())
        {
            std::fprintf(stderr, "%s\n", choice.warning.c_str());
        }
        levels.push_back(choice.level);
    }
    else
    {
        levels.push_back(mwm::PreviewSimd::Scalar);
        if (mwm::cpuHasAvx2())
        {
            levels.push_back(mwm::PreviewSimd::Avx2);
        }
    }
    std::printf("cpu_avx2=%s rotation=%d seconds=%.1f\n", mwm::cpuHasAvx2() ? "yes" : "no", rotation, seconds);
    bool same = true;
    for (Case const& c : cases)
    {
        if (only != "all" && only != c.name)
        {
            continue;
        }
        std::vector<std::vector<std::uint8_t>> src;
        for (int i = 0; i < rotation; ++i)
        {
            src.push_back(randomFrame(c.w, c.h, static_cast<std::uint32_t>(i + 1)));
        }
        std::vector<std::vector<std::uint8_t>> outputs;
        std::vector<double> ms;
        for (auto const level : levels)
        {
            mwm::setPreviewSimd(level);
            std::vector<std::uint8_t> dst(mwm::previewBytes(c.ow, c.oh));
            auto const result = run(c, src, dst, seconds);
            std::printf("%-11s %-6s rot=%d frames=%ld ms/frame=%.3f cores@50fps=%.3f\n", c.name, mwm::simdName(level), rotation, result.frames, result.ms,
                result.ms * 50.0 / 1000.0);
            mwm::v210ToPreview(src[0].data(), c.w, c.h, c.interlaced, c.ow, c.oh, c.nv12, dst.data());
            outputs.push_back(dst);
            ms.push_back(result.ms);
        }
        if (outputs.size() == 2)
        {
            bool const equal = outputs[0] == outputs[1];
            same = same && equal;
            std::printf("%-11s speedup=%.2fx same_bytes=%s\n", c.name, ms[0] / ms[1], equal ? "yes" : "NO");
        }
    }
    return same ? 0 : 1;
}
