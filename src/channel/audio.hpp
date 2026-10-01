#pragma once

#include <cstddef>
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
} // namespace mwm
