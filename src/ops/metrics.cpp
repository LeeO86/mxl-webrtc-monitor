#include "ops/metrics.hpp"

#include "version.hpp"

#include <sstream>

namespace mwm
{
void LatencyHistogram::observe(double seconds)
{
    std::lock_guard const lock{mu_};
    ++count_;
    sum_ += seconds;
    for (std::size_t i = 0; i < sizeof(kBounds) / sizeof(kBounds[0]); ++i)
    {
        if (seconds <= kBounds[i])
        {
            ++counts_[i];
            break;
        }
    }
}

std::string LatencyHistogram::render(std::string const& name, std::string const& labels) const
{
    std::lock_guard const lock{mu_};
    std::ostringstream out;
    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < sizeof(kBounds) / sizeof(kBounds[0]); ++i)
    {
        cumulative += counts_[i];
        out << name << "_bucket{" << labels << "le=\"" << kBounds[i] << "\"} " << cumulative << "\n";
    }
    out << name << "_bucket{" << labels << "le=\"+Inf\"} " << count_ << "\n";
    out << name << "_sum{" << labels << "} " << sum_ << "\n";
    out << name << "_count{" << labels << "} " << count_ << "\n";
    return out.str();
}

namespace
{
char const* kStates[] = {"not_routed", "waiting", "no_signal", "running"};
char const* kPublishStates[] = {"connecting", "publishing", "error"};

void gaugeState(std::ostringstream& out, int channel, char const* kind, RunState state)
{
    for (auto const* name : kStates)
    {
        int const value = std::string(name) == stateName(state) ? 1 : 0;
        out << "mxl_webrtc_monitor_channel_state{channel=\"" << channel << "\",kind=\"" << kind << "\",state=\"" << name << "\"} " << value << "\n";
    }
}
} // namespace

std::string renderMetrics(Config const& cfg, std::vector<ChannelView> const& channels, std::string const& mxlVersion, std::string const& gstVersion,
    std::string const& encoderAvailable, std::vector<LatencyHistogram const*> const& latency, std::vector<std::uint64_t> const& fallbacks,
    std::string const& previewMode)
{
    (void)gstVersion;
    std::ostringstream out;
    out << "# HELP mxl_webrtc_monitor_info Build information.\n";
    out << "# TYPE mxl_webrtc_monitor_info gauge\n";
    out << "mxl_webrtc_monitor_info{version=\"" << kVersion << "\",mxl_version=\"" << mxlVersion << "\",encoder_available=\"" << encoderAvailable
        << "\"} 1\n";
    out << "# HELP mxl_webrtc_monitor_channel_state Current channel state.\n";
    out << "# TYPE mxl_webrtc_monitor_channel_state gauge\n";
    out << "# TYPE mxl_webrtc_monitor_grains_read_total counter\n";
    out << "# TYPE mxl_webrtc_monitor_grains_dropped_total counter\n";
    out << "# TYPE mxl_webrtc_monitor_resyncs_total counter\n";
    out << "# TYPE mxl_webrtc_monitor_read_lag_grains gauge\n";
    out << "# TYPE mxl_webrtc_monitor_encode_fps gauge\n";
    out << "# TYPE mxl_webrtc_monitor_encode_latency_seconds histogram\n";
    out << "# TYPE mxl_webrtc_monitor_encoder_fallbacks_total counter\n";
    out << "# TYPE mxl_webrtc_monitor_output_bitrate_bps gauge\n";
    out << "# TYPE mxl_webrtc_monitor_viewers gauge\n";
    out << "# HELP mxl_webrtc_monitor_preview_mode Where the previews are published: own (built-in MediaMTX) or shared.\n";
    out << "# TYPE mxl_webrtc_monitor_preview_mode gauge\n";
    for (auto const* mode : {"own", "shared"})
    {
        out << "mxl_webrtc_monitor_preview_mode{mode=\"" << mode << "\"} " << (previewMode == mode ? 1 : 0) << "\n";
    }
    out << "# HELP mxl_webrtc_monitor_preview_publish_state State of each channel's RTSP publish.\n";
    out << "# TYPE mxl_webrtc_monitor_preview_publish_state gauge\n";
    if (cfg.metrics_audio_peak)
    {
        out << "# TYPE mxl_webrtc_monitor_audio_peak_dbfs gauge\n";
    }
    for (std::size_t i = 0; i < channels.size(); ++i)
    {
        auto const& channel = channels[i];
        int const n = channel.settings.index;
        gaugeState(out, n, "video", channel.video.state);
        gaugeState(out, n, "audio", channel.audio.state);
        out << "mxl_webrtc_monitor_grains_read_total{channel=\"" << n << "\"} " << channel.grains_read << "\n";
        out << "mxl_webrtc_monitor_grains_dropped_total{channel=\"" << n << "\",reason=\"queue_full\"} " << channel.dropped_queue << "\n";
        out << "mxl_webrtc_monitor_grains_dropped_total{channel=\"" << n << "\",reason=\"too_late\"} " << channel.dropped_late << "\n";
        out << "mxl_webrtc_monitor_resyncs_total{channel=\"" << n << "\"} " << channel.resyncs << "\n";
        out << "mxl_webrtc_monitor_read_lag_grains{channel=\"" << n << "\"} " << channel.read_lag << "\n";
        out << "mxl_webrtc_monitor_encode_fps{channel=\"" << n << "\"} " << channel.encode_fps << "\n";
        std::string const labels = "channel=\"" + std::to_string(n) + "\",encoder=\"" + (channel.encoder.empty() ? "none" : channel.encoder) + "\",";
        if (i < latency.size() && latency[i] != nullptr)
        {
            out << latency[i]->render("mxl_webrtc_monitor_encode_latency_seconds", labels);
        }
        std::uint64_t const fallback = i < fallbacks.size() ? fallbacks[i] : 0;
        out << "mxl_webrtc_monitor_encoder_fallbacks_total{channel=\"" << n << "\"} " << fallback << "\n";
        out << "mxl_webrtc_monitor_output_bitrate_bps{channel=\"" << n << "\"} " << channel.bitrate_bps << "\n";
        out << "mxl_webrtc_monitor_viewers{channel=\"" << n << "\",protocol=\"webrtc\"} " << channel.viewers_webrtc << "\n";
        out << "mxl_webrtc_monitor_viewers{channel=\"" << n << "\",protocol=\"hls\"} " << channel.viewers_hls << "\n";
        for (auto const* state : kPublishStates)
        {
            out << "mxl_webrtc_monitor_preview_publish_state{channel=\"" << n << "\",state=\"" << state << "\"} " << (channel.publish_state == state ? 1 : 0)
                << "\n";
        }
        if (cfg.metrics_audio_peak)
        {
            for (std::size_t ch = 0; ch < channel.peak_dbfs.size(); ++ch)
            {
                out << "mxl_webrtc_monitor_audio_peak_dbfs{channel=\"" << n << "\",input_channel=\"" << (ch + 1) << "\"} " << channel.peak_dbfs[ch] << "\n";
            }
        }
    }
    return out.str();
}
} // namespace mwm
