#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mwm
{
struct AudioSelection
{
    int left = 0;
    int right = 1;
    bool mono = false;
    bool valid = false;
};

// pair is 1-based. Pair 1 selects input channels 1 and 2.
AudioSelection selectAudioPair(int channelCount, int pair, std::string const& downmix);

// Planar float input. Writes interleaved stereo frames into dst (2 * frames).
void renderAudioPair(float const* const* channels, int channelCount, std::size_t frames, AudioSelection const& selection, float* dst);

struct AudioLevels
{
    std::vector<double> peak_dbfs;
    std::vector<double> rms_dbfs;
};

AudioLevels measureLevels(float const* const* channels, int channelCount, std::size_t frames);

// Audio samples (48 kHz) the stream still owes: as many as the pushed video frames cover. Audio
// timestamps count pushed samples, so pushing more than this would run ahead of the pipeline clock.
std::uint64_t audioSamplesDue(std::uint64_t videoFrames, int rateNum, int rateDen, std::uint64_t samplesPushed);

enum class AudioStep
{
    Read,    // read [start, start + count) from the flow
    Wait,    // the head has not reached start + count yet
    Silence, // push count samples of silence instead
};

struct AudioPlan
{
    AudioStep step = AudioStep::Wait;
    std::uint64_t start = 0;
    std::uint64_t count = 0;
    bool resync = false; // the cursor was moved to the target
};

// Where the next `want` samples of a continuous flow come from. cursor: the sample after the last
// pushed one (nullopt: not placed yet). target: where the audio should end now (the video
// grain's timestamp, or the head when the audio arrives later than the video). A cursor further
// than `slack` samples from the target is moved there. waited: the head was already awaited for
// about a frame, so silence fills the gap.
AudioPlan planAudioRead(std::optional<std::uint64_t> cursor, std::uint64_t target, std::uint64_t head, std::uint64_t want, std::uint64_t slack, bool waited);
} // namespace mwm
