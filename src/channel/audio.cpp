#include "channel/audio.hpp"

#include <cmath>
#include <algorithm>

namespace mwm
{
namespace
{
float sampleAt(float const* const* channels, int channelCount, int channel, std::size_t frame)
{
    if (channel < 0 || channel >= channelCount || channels == nullptr || channels[channel] == nullptr)
    {
        return 0.f;
    }
    return channels[channel][frame];
}

double toDbfs(double amplitude)
{
    if (amplitude <= 1e-9)
    {
        return -120.0;
    }
    return 20.0 * std::log10(amplitude);
}
} // namespace

AudioSelection selectAudioPair(int channelCount, int pair, std::string const& downmix)
{
    AudioSelection out;
    out.mono = downmix == "mono";
    if (channelCount <= 0 || pair < 1)
    {
        return out;
    }
    int const left = (pair - 1) * 2;
    int const right = left + 1;
    if (left >= channelCount)
    {
        out.left = 0;
        out.right = channelCount > 1 ? 1 : 0;
    }
    else if (right >= channelCount)
    {
        out.left = left;
        out.right = left;
    }
    else
    {
        out.left = left;
        out.right = right;
    }
    out.valid = true;
    return out;
}

void renderAudioPair(float const* const* channels, int channelCount, std::size_t frames, AudioSelection const& selection, float* dst)
{
    if (dst == nullptr)
    {
        return;
    }
    for (std::size_t i = 0; i < frames; ++i)
    {
        float const l = sampleAt(channels, channelCount, selection.left, i);
        float const r = sampleAt(channels, channelCount, selection.right, i);
        if (selection.mono)
        {
            float const sum = 0.5f * (l + r);
            dst[i * 2] = sum;
            dst[i * 2 + 1] = sum;
        }
        else
        {
            dst[i * 2] = l;
            dst[i * 2 + 1] = r;
        }
    }
}

AudioLevels measureLevels(float const* const* channels, int channelCount, std::size_t frames)
{
    AudioLevels levels;
    levels.peak_dbfs.assign(static_cast<std::size_t>(std::max(channelCount, 0)), -120.0);
    levels.rms_dbfs.assign(static_cast<std::size_t>(std::max(channelCount, 0)), -120.0);
    if (frames == 0 || channelCount <= 0)
    {
        return levels;
    }
    for (int ch = 0; ch < channelCount; ++ch)
    {
        double peak = 0.0;
        double sum = 0.0;
        for (std::size_t i = 0; i < frames; ++i)
        {
            double const sample = sampleAt(channels, channelCount, ch, i);
            double const abs = std::fabs(sample);
            peak = std::max(peak, abs);
            sum += sample * sample;
        }
        levels.peak_dbfs[static_cast<std::size_t>(ch)] = toDbfs(peak);
        levels.rms_dbfs[static_cast<std::size_t>(ch)] = toDbfs(std::sqrt(sum / static_cast<double>(frames)));
    }
    return levels;
}
} // namespace mwm
