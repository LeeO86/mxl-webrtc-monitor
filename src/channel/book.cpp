#include "channel/book.hpp"

namespace mwm
{
namespace
{
void refresh(LegRoute& leg)
{
    LegInput input;
    input.master_enable = leg.master_enable;
    input.domain_id = leg.domain_id;
    input.flow_id = leg.flow_id;
    input.domain_resolved = leg.domain_resolved;
    input.flow_open = leg.flow_open;
    input.grains_flowing = leg.grains_flowing;
    auto const eval = evaluateLeg(input);
    leg.state = eval.state;
    if (!eval.reason.empty())
    {
        leg.reason = eval.reason;
    }
    else if (leg.state != RunState::Waiting)
    {
        leg.reason.clear();
    }
}
} // namespace

ChannelBook::ChannelBook(Config const& cfg)
{
    reset(cfg);
}

void ChannelBook::reset(Config const& cfg)
{
    std::lock_guard const lock{mu_};
    std::vector<ChannelView> next;
    next.reserve(cfg.channels.size());
    for (auto const& settings : cfg.channels)
    {
        ChannelView view;
        view.settings = settings;
        for (auto const& previous : channels_)
        {
            if (previous.settings.index == settings.index)
            {
                view.video = previous.video;
                view.audio = previous.audio;
                view.format = previous.format;
                view.encoder = previous.encoder;
                view.width = previous.width;
                view.height = previous.height;
                view.rate_num = previous.rate_num;
                view.rate_den = previous.rate_den;
                view.interlaced = previous.interlaced;
                view.audio_channels = previous.audio_channels;
                view.peak_dbfs = previous.peak_dbfs;
                view.rms_dbfs = previous.rms_dbfs;
                view.grains_read = previous.grains_read;
                view.dropped_queue = previous.dropped_queue;
                view.dropped_late = previous.dropped_late;
                view.resyncs = previous.resyncs;
                view.read_lag = previous.read_lag;
                view.encode_fps = previous.encode_fps;
                view.bitrate_bps = previous.bitrate_bps;
                view.viewers_webrtc = previous.viewers_webrtc;
                view.viewers_hls = previous.viewers_hls;
                view.mediamtx_ready = previous.mediamtx_ready;
                view.mediamtx_tracks = previous.mediamtx_tracks;
                view.source_label = previous.source_label;
                view.latency_count = previous.latency_count;
                view.latency_sum = previous.latency_sum;
                view.encode_latency_seconds = previous.encode_latency_seconds;
                view.tsl_lh = previous.tsl_lh;
                view.tsl_rh = previous.tsl_rh;
                view.tsl_text_tally = previous.tsl_text_tally;
                view.tsl_text = previous.tsl_text;
            }
        }
        next.push_back(std::move(view));
    }
    channels_ = std::move(next);
}

ChannelView* ChannelBook::find(int channel)
{
    for (auto& view : channels_)
    {
        if (view.settings.index == channel)
        {
            return &view;
        }
    }
    return nullptr;
}

void ChannelBook::setSettings(int channel, ChannelSettings const& settings)
{
    std::lock_guard const lock{mu_};
    if (auto* view = find(channel))
    {
        view->settings = settings;
    }
}

void ChannelBook::setRoute(int channel, LegKind kind, bool enable, std::string domainId, std::string flowId, std::string senderId)
{
    std::lock_guard const lock{mu_};
    auto* view = find(channel);
    if (view == nullptr)
    {
        return;
    }
    auto& leg = kind == LegKind::Video ? view->video : view->audio;
    bool const changed = leg.domain_id != domainId || leg.flow_id != flowId || leg.master_enable != enable;
    leg.master_enable = enable;
    leg.domain_id = std::move(domainId);
    leg.flow_id = std::move(flowId);
    if (!senderId.empty() || enable)
    {
        leg.sender_id = std::move(senderId);
    }
    if (changed)
    {
        leg.domain_resolved = false;
        leg.flow_open = false;
        leg.grains_flowing = false;
    }
    refresh(leg);
}

void ChannelBook::setProbe(int channel, LegKind kind, bool domainResolved, bool flowOpen, bool flowing, std::string const& reason)
{
    std::lock_guard const lock{mu_};
    auto* view = find(channel);
    if (view == nullptr)
    {
        return;
    }
    auto& leg = kind == LegKind::Video ? view->video : view->audio;
    leg.domain_resolved = domainResolved;
    leg.flow_open = flowOpen;
    leg.grains_flowing = flowing;
    leg.reason = reason;
    refresh(leg);
}

void ChannelBook::setFormat(int channel, int width, int height, int rateNum, int rateDen, bool interlaced, std::string const& format, int audioChannels)
{
    std::lock_guard const lock{mu_};
    if (auto* view = find(channel))
    {
        if (width > 0 && height > 0)
        {
            view->width = width;
            view->height = height;
            view->rate_num = rateNum;
            view->rate_den = rateDen;
            view->interlaced = interlaced;
            if (!format.empty())
            {
                view->format = format;
            }
        }
        if (audioChannels >= 0)
        {
            view->audio_channels = audioChannels;
        }
    }
}

void ChannelBook::setEncoder(int channel, std::string const& encoder)
{
    std::lock_guard const lock{mu_};
    if (auto* view = find(channel))
    {
        view->encoder = encoder;
    }
}

void ChannelBook::setCounters(int channel, std::uint64_t grains, std::uint64_t droppedQueue, std::uint64_t droppedLate, std::uint64_t resyncs, double lag, double fps, double bitrate)
{
    std::lock_guard const lock{mu_};
    if (auto* view = find(channel))
    {
        view->grains_read = grains;
        view->dropped_queue = droppedQueue;
        view->dropped_late = droppedLate;
        view->resyncs = resyncs;
        view->read_lag = lag;
        view->encode_fps = fps;
        view->bitrate_bps = bitrate;
    }
}

void ChannelBook::setLevels(int channel, std::vector<double> peak, std::vector<double> rms)
{
    std::lock_guard const lock{mu_};
    if (auto* view = find(channel))
    {
        view->peak_dbfs = std::move(peak);
        view->rms_dbfs = std::move(rms);
    }
}

void ChannelBook::setSourceLabel(int channel, std::string const& label)
{
    std::lock_guard const lock{mu_};
    if (auto* view = find(channel))
    {
        view->source_label = label;
    }
}

void ChannelBook::setViewers(int channel, int webrtc, int hls)
{
    std::lock_guard const lock{mu_};
    if (auto* view = find(channel))
    {
        view->viewers_webrtc = webrtc;
        view->viewers_hls = hls;
    }
}

void ChannelBook::setMediamtxPath(int channel, bool ready, std::vector<std::string> tracks)
{
    std::lock_guard const lock{mu_};
    if (auto* view = find(channel))
    {
        view->mediamtx_ready = ready;
        view->mediamtx_tracks = std::move(tracks);
    }
}

void ChannelBook::setTally(int channel, int lh, int rh, int text, std::string const& label)
{
    std::lock_guard const lock{mu_};
    if (auto* view = find(channel))
    {
        view->tsl_lh = lh;
        view->tsl_rh = rh;
        view->tsl_text_tally = text;
        view->tsl_text = label;
    }
}

void ChannelBook::observeLatency(int channel, double seconds)
{
    std::lock_guard const lock{mu_};
    if (auto* view = find(channel))
    {
        view->latency_count += 1;
        view->latency_sum += seconds;
        view->encode_latency_seconds = seconds;
    }
}

std::vector<ChannelView> ChannelBook::snapshot() const
{
    std::lock_guard const lock{mu_};
    return channels_;
}

ChannelSettings ChannelBook::settings(int channel) const
{
    std::lock_guard const lock{mu_};
    for (auto const& view : channels_)
    {
        if (view.settings.index == channel)
        {
            return view.settings;
        }
    }
    return {};
}

LegRoute ChannelBook::route(int channel, LegKind kind) const
{
    std::lock_guard const lock{mu_};
    for (auto const& view : channels_)
    {
        if (view.settings.index == channel)
        {
            return kind == LegKind::Video ? view.video : view.audio;
        }
    }
    return {};
}
} // namespace mwm
