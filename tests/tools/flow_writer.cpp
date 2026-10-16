#include "media/v210.hpp"

#include <mxl/flow.h>
#include <mxl/mxl.h>
#include <mxl/time.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{
void writeFile(std::filesystem::path const& path, std::string const& body)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    out << body;
}

// v210 line of one luma (chroma 512).
void fillLine(std::uint8_t* dst, int width, std::uint32_t y)
{
    std::uint32_t const c = 512;
    std::uint32_t const words[4] = {c | (y << 10) | (c << 20), y | (c << 10) | (y << 20), c | (y << 10) | (c << 20), y | (c << 10) | (y << 20)};
    for (int x = 0; x < width; x += 6)
    {
        std::memcpy(dst + static_cast<std::size_t>(x / 6) * 16, words, sizeof(words));
    }
}

// Field grain of an interlaced test flow: `order` 0 is the first field of the frame in time (grain
// 2k), 1 the second. The frame's upper half is 800 (first field) and 560 (second), its lower half
// 320 and 160, so a reader that takes the wrong field, misses the field order or shows a field as a
// frame (squashed into the upper half) gets other values.
void fillField(std::uint8_t* dst, int width, int height, int parity, int order)
{
    auto const stride = mwm::v210Stride(width);
    for (int row = 0; row < height / 2; ++row)
    {
        bool const upper = 2 * row + parity < height / 2;
        std::uint32_t const y = upper ? (order == 0 ? 800 : 560) : (order == 0 ? 320 : 160);
        fillLine(dst + stride * static_cast<std::size_t>(row), width, y);
    }
}

std::string videoDef(std::string const& id, int width, int height, int rate, std::string const& interlace)
{
    return std::string(R"({"id":")") + id + R"(","label":"monitor-test-video","description":"integration","format":"urn:x-nmos:format:video","media_type":"video/v210","grain_rate":{"numerator":)" +
           std::to_string(rate) + R"(,"denominator":1},"frame_width":)" + std::to_string(width) + R"(,"frame_height":)" + std::to_string(height) +
           R"(,"interlace_mode":")" + interlace + R"(","colorspace":"BT709","tags":{"urn:x-nmos:tag:grouphint/v1.0":["test:Video"]}})";
}

std::string audioDef(std::string const& id, int channels)
{
    return std::string(R"({"id":")") + id + R"(","label":"monitor-test-audio","description":"integration","format":"urn:x-nmos:format:audio","media_type":"audio/float32","sample_rate":{"numerator":48000,"denominator":1},"channel_count":)" +
           std::to_string(channels) + R"(,"bit_depth":32,"tags":{"urn:x-nmos:tag:grouphint/v1.0":["test:Audio"]}})";
}
} // namespace

int main(int argc, char** argv)
{
    std::string domain = argc > 1 ? argv[1] : "/dev/shm/mwm-domain";
    std::string domainId = argc > 2 ? argv[2] : "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
    std::string videoId = argc > 3 ? argv[3] : "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
    std::string audioId = argc > 4 ? argv[4] : "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
    int seconds = argc > 5 ? std::atoi(argv[5]) : 30;
    int width = argc > 6 ? std::atoi(argv[6]) : 480;
    int height = argc > 7 ? std::atoi(argv[7]) : 270;
    int rate = argc > 8 ? std::atoi(argv[8]) : 25;
    // p (default), tff or bff. Interlaced: MXL stores one field per grain at twice `rate`; grain 2k is
    // the first field of frame k (the top field for tff, the bottom field for bff).
    std::string const scan = argc > 9 ? argv[9] : "p";
    bool const fields = scan == "tff" || scan == "bff";
    bool const bff = scan == "bff";
    if (scan != "p" && !fields)
    {
        std::cerr << "scan is p, tff or bff\n";
        return 2;
    }
    if (fields && height % 2 != 0)
    {
        ++height;
    }
    if (width % 6 != 0)
    {
        width += 6 - (width % 6);
    }
    std::filesystem::create_directories(domain);
    writeFile(std::filesystem::path(domain) / "domain_def.json", std::string("{\"id\":\"") + domainId + "\"}");
    auto* instance = mxlCreateInstance(domain.c_str(), "");
    if (instance == nullptr)
    {
        std::cerr << "mxlCreateInstance failed\n";
        return 1;
    }
    mxlFlowWriter video = nullptr;
    mxlFlowWriter audio = nullptr;
    bool created = false;
    if (mxlCreateFlowWriter(instance, videoDef(videoId, width, height, rate, fields ? (bff ? "interlaced_bff" : "interlaced_tff") : "progressive").c_str(), nullptr, &video, nullptr, &created) != MXL_STATUS_OK)
    {
        std::cerr << "video writer failed\n";
        return 1;
    }
    if (mxlCreateFlowWriter(instance, audioDef(audioId, 2).c_str(), nullptr, &audio, nullptr, &created) != MXL_STATUS_OK)
    {
        std::cerr << "audio writer failed\n";
        return 1;
    }
    std::size_t maxWrite = 0;
    mxlFlowWriterGetMaxWriteLengthSamples(audio, &maxWrite);
    if (maxWrite == 0)
    {
        maxWrite = 1920;
    }
    std::vector<std::uint8_t> frame(mwm::v210FrameBytes(width, height));
    mxlRational videoRate{fields ? rate * 2 : rate, 1};
    mxlRational audioRate{48000, 1};
    int const samplesPerFrame = 48000 / static_cast<int>(videoRate.numerator);
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    int frameNumber = 0;
    std::cout << "writing " << scan << " " << videoId << " and " << audioId << " for " << seconds << "s\n";
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto const index = mxlGetCurrentIndex(&videoRate);
        mxlGrainInfo grain{};
        std::uint8_t* payload = nullptr;
        if (mxlFlowWriterOpenGrain(video, index, &grain, &payload) == MXL_STATUS_OK && payload != nullptr)
        {
            if (fields)
            {
                int const order = static_cast<int>(index & 1u);
                fillField(frame.data(), width, height, order ^ (bff ? 1 : 0), order);
            }
            else
            {
                mwm::fillV210Bar(frame.data(), width, height, frameNumber);
            }
            auto const n = std::min(frame.size(), static_cast<std::size_t>(grain.grainSize));
            std::memcpy(payload, frame.data(), n);
            grain.flags = 0;
            grain.validSlices = grain.totalSlices;
            mxlFlowWriterCommitGrain(video, &grain);
        }
        auto const sampleEnd = mxlIndexToTimestamp(&videoRate, index + 1);
        std::uint64_t end = (sampleEnd / 1000000000ULL) * 48000ULL + ((sampleEnd % 1000000000ULL) * 48000ULL) / 1000000000ULL;
        if (end > static_cast<std::uint64_t>(samplesPerFrame))
        {
            std::size_t const count = static_cast<std::size_t>(samplesPerFrame);
            if (count <= maxWrite)
            {
                mxlMutableWrappedMultiBufferSlice slices{};
                if (mxlFlowWriterOpenSamples(audio, end, count, &slices) == MXL_STATUS_OK)
                {
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        std::size_t filled = 0;
                        for (int frag = 0; frag < 2 && filled < count; ++frag)
                        {
                            auto* pointer = static_cast<char*>(slices.base.fragments[frag].pointer);
                            if (pointer == nullptr)
                            {
                                continue;
                            }
                            auto* dst = reinterpret_cast<float*>(pointer + static_cast<std::size_t>(ch) * slices.stride);
                            std::size_t const available = slices.base.fragments[frag].size / sizeof(float);
                            std::size_t const take = std::min(available, count - filled);
                            for (std::size_t i = 0; i < take; ++i)
                            {
                                float const phase = static_cast<float>((frameNumber * samplesPerFrame + static_cast<int>(filled + i)) % 48) / 48.f;
                                dst[i] = (ch == 0 ? 0.2f : 0.1f) * (phase < 0.5f ? 1.f : -1.f);
                            }
                            filled += take;
                        }
                    }
                    mxlFlowWriterCommitSamples(audio);
                }
            }
        }
        ++frameNumber;
        auto const wait = mxlGetNsUntilIndex(index + 1, &videoRate);
        if (wait != MXL_UNDEFINED_INDEX && wait < 1000000000ULL)
        {
            mxlSleepForNs(wait);
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000 / std::max(1, static_cast<int>(videoRate.numerator))));
        }
        (void)audioRate;
    }
    mxlReleaseFlowWriter(instance, video);
    mxlReleaseFlowWriter(instance, audio);
    mxlDestroyInstance(instance);
    std::cout << "wrote " << frameNumber << " frames\n";
    return 0;
}
