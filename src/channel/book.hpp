#pragma once

#include "channel/state.hpp"
#include "config/config.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace mwm
{
struct LegRoute
{
    bool master_enable = false;
    std::string domain_id;
    std::string flow_id;
    std::string sender_id;
    bool domain_resolved = false;
    bool flow_open = false;
    bool grains_flowing = false;
    std::string reason;
    RunState state = RunState::NotRouted;
};

struct ChannelView
{
    ChannelSettings settings;
    LegRoute video;
    LegRoute audio;
    std::string format;
    std::string encoder;
    int width = 0;
    int height = 0;
    int rate_num = 25;
    int rate_den = 1;
    bool interlaced = false;
    int audio_channels = 0;
    std::vector<double> peak_dbfs;
    std::vector<double> rms_dbfs;
    std::uint64_t grains_read = 0;
    std::uint64_t dropped_queue = 0;
    std::uint64_t dropped_late = 0;
    std::uint64_t resyncs = 0;
    double read_lag = 0;
    double encode_fps = 0;
    double encode_latency_seconds = 0;
    double bitrate_bps = 0;
    int viewers_webrtc = 0;
    int viewers_hls = 0;
    bool mediamtx_ready = false;
    std::vector<std::string> mediamtx_tracks;
    std::string source_label;
    std::uint64_t latency_count = 0;
    double latency_sum = 0;
    // TSL tally as received: 0 off, 1 red, 2 green, 3 amber, and the display's text.
    int tsl_lh = 0;
    int tsl_rh = 0;
    int tsl_text_tally = 0;
    std::string tsl_text;
    // The MediaMTX path the channel publishes to (fixed at start), and that RTSP publish: "connecting",
    // "publishing" or "error"; publish_error is the last error until the channel publishes again.
    std::string preview_path;
    std::string publish_state = "connecting";
    std::string publish_error;
};

class ChannelBook
{
public:
    explicit ChannelBook(Config const& cfg);

    void reset(Config const& cfg);
    void setSettings(int channel, ChannelSettings const& settings);
    void setRoute(int channel, LegKind kind, bool enable, std::string domainId, std::string flowId, std::string senderId);
    void setProbe(int channel, LegKind kind, bool domainResolved, bool flowOpen, bool flowing, std::string const& reason);
    void setFormat(int channel, int width, int height, int rateNum, int rateDen, bool interlaced, std::string const& format, int audioChannels);
    void setEncoder(int channel, std::string const& encoder);
    void setCounters(int channel, std::uint64_t grains, std::uint64_t droppedQueue, std::uint64_t droppedLate, std::uint64_t resyncs, double lag, double fps, double bitrate);
    void setLevels(int channel, std::vector<double> peak, std::vector<double> rms);
    void setSourceLabel(int channel, std::string const& label);
    void setViewers(int channel, int webrtc, int hls);
    void setMediamtxPath(int channel, bool ready, std::vector<std::string> tracks);
    void setTally(int channel, int lh, int rh, int text, std::string const& label);
    void setPublish(int channel, std::string const& state, std::string const& error);
    void observeLatency(int channel, double seconds);

    std::vector<ChannelView> snapshot() const;
    ChannelSettings settings(int channel) const;
    LegRoute route(int channel, LegKind kind) const;

private:
    ChannelView* find(int channel);

    mutable std::mutex mu_;
    std::vector<ChannelView> channels_;
};
} // namespace mwm
