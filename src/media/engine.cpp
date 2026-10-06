#include "media/engine.hpp"

#include "channel/audio.hpp"
#include "domain/scan.hpp"
#include "media/v210.hpp"
#include "util/jsonutil.hpp"
#include "util/logging.hpp"

#include <mxl/flow.h>
#include <mxl/mxl.h>
#include <mxl/time.h>

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace mwm
{
namespace
{
std::once_flag gGstOnce;

void initGst()
{
    std::call_once(gGstOnce, [] { gst_init(nullptr, nullptr); });
}

bool elementReady(char const* factoryName)
{
    initGst();
    GstElementFactory* factory = gst_element_factory_find(factoryName);
    if (factory == nullptr)
    {
        return false;
    }
    GstElement* element = gst_element_factory_create(factory, nullptr);
    gst_object_unref(factory);
    if (element == nullptr)
    {
        return false;
    }
    auto const ret = gst_element_set_state(element, GST_STATE_READY);
    gst_element_set_state(element, GST_STATE_NULL);
    gst_object_unref(element);
    return ret != GST_STATE_CHANGE_FAILURE;
}

struct VideoFormat
{
    int width = 960;
    int height = 540;
    int rateNum = 25;
    int rateDen = 1;
    bool interlaced = false;
    bool alpha = false;
    std::string label;
    std::string signature() const
    {
        return std::to_string(width) + "x" + std::to_string(height) + "@" + std::to_string(rateNum) + "/" + std::to_string(rateDen) +
               (interlaced ? "i" : "p") + (alpha ? "a" : "");
    }
};

struct AudioFormat
{
    int channels = 0;
    std::string label;
};

VideoFormat parseVideoDef(std::string const& text)
{
    VideoFormat format;
    std::string err;
    auto const root = json::parse(text, &err);
    if (!err.empty() || !root.is<picojson::object>())
    {
        return format;
    }
    auto const& obj = root.get<picojson::object>();
    if (obj.count("frame_width") && obj.at("frame_width").is<double>())
    {
        format.width = static_cast<int>(obj.at("frame_width").get<double>());
    }
    if (obj.count("frame_height") && obj.at("frame_height").is<double>())
    {
        format.height = static_cast<int>(obj.at("frame_height").get<double>());
    }
    if (obj.count("grain_rate") && obj.at("grain_rate").is<picojson::object>())
    {
        auto const& rate = obj.at("grain_rate").get<picojson::object>();
        if (rate.count("numerator") && rate.at("numerator").is<double>())
        {
            format.rateNum = static_cast<int>(rate.at("numerator").get<double>());
        }
        if (rate.count("denominator") && rate.at("denominator").is<double>())
        {
            format.rateDen = std::max(1, static_cast<int>(rate.at("denominator").get<double>()));
        }
    }
    auto const mode = json::fieldString(root, "interlace_mode");
    format.interlaced = mode.rfind("interlaced", 0) == 0;
    auto const media = json::fieldString(root, "media_type");
    format.alpha = media == "video/v210a";
    format.label = json::fieldString(root, "label");
    if (format.width < 6)
    {
        format.width = 960;
    }
    if (format.height < 2)
    {
        format.height = 540;
    }
    return format;
}

AudioFormat parseAudioDef(std::string const& text)
{
    AudioFormat format;
    std::string err;
    auto const root = json::parse(text, &err);
    if (!err.empty() || !root.is<picojson::object>())
    {
        return format;
    }
    auto const& obj = root.get<picojson::object>();
    if (obj.count("channel_count") && obj.at("channel_count").is<double>())
    {
        format.channels = static_cast<int>(obj.at("channel_count").get<double>());
    }
    format.label = json::fieldString(root, "label");
    return format;
}

std::string readFlowDef(mxlInstance instance, std::string const& flowId)
{
    std::size_t size = 0;
    auto const first = mxlGetFlowDef(instance, flowId.c_str(), nullptr, &size);
    if (first != MXL_ERR_INVALID_ARG && first != MXL_STATUS_OK)
    {
        return {};
    }
    if (size == 0)
    {
        size = 8192;
    }
    std::string buffer(size, '\0');
    auto const status = mxlGetFlowDef(instance, flowId.c_str(), buffer.data(), &size);
    if (status != MXL_STATUS_OK)
    {
        return {};
    }
    buffer.resize(std::strlen(buffer.c_str()));
    return buffer;
}

void copySamples(mxlWrappedMultiBufferSlice const& slice, std::size_t count, std::vector<std::vector<float>>& channels)
{
    int const nch = static_cast<int>(std::min<std::size_t>(slice.count, 64));
    channels.assign(static_cast<std::size_t>(nch), std::vector<float>(count, 0.f));
    for (int ch = 0; ch < nch; ++ch)
    {
        std::size_t filled = 0;
        for (int frag = 0; frag < 2 && filled < count; ++frag)
        {
            auto const* pointer = static_cast<char const*>(slice.base.fragments[frag].pointer);
            if (pointer == nullptr || slice.base.fragments[frag].size == 0)
            {
                continue;
            }
            auto const* src = reinterpret_cast<float const*>(pointer + static_cast<std::size_t>(ch) * slice.stride);
            std::size_t const available = slice.base.fragments[frag].size / sizeof(float);
            std::size_t const take = std::min(available, count - filled);
            std::memcpy(channels[static_cast<std::size_t>(ch)].data() + filled, src, take * sizeof(float));
            filled += take;
        }
    }
}

class Slot
{
public:
    Slot(int index, Config cfg, ChannelBook& book, LatencyHistogram& latency, std::string const& forcedEncoder)
        : index_(index)
        , cfg_(std::move(cfg))
        , book_(book)
        , latency_(latency)
        , forcedEncoder_(forcedEncoder)
    {
    }

    std::uint64_t fallbackCount() const
    {
        return fallbacks_.load();
    }

    void start()
    {
        running_.store(true);
        videoThread_ = std::thread([this] { videoLoop(); });
        audioThread_ = std::thread([this] { audioLoop(); });
    }

    void stop()
    {
        running_.store(false);
        if (videoThread_.joinable())
        {
            videoThread_.join();
        }
        if (audioThread_.joinable())
        {
            audioThread_.join();
        }
        closeVideo();
        closeAudio();
        destroyPipeline();
    }

private:
    void sleepMs(int total, LegRoute const& baseline, LegKind kind)
    {
        int left = total;
        while (left > 0 && running_.load())
        {
            int const step = std::min(left, 50);
            std::this_thread::sleep_for(std::chrono::milliseconds(step));
            left -= step;
            auto const now = book_.route(index_, kind);
            if (now.master_enable != baseline.master_enable || now.domain_id != baseline.domain_id || now.flow_id != baseline.flow_id)
            {
                break;
            }
        }
    }

    void closeVideo()
    {
        if (videoReader_ != nullptr && videoInstance_ != nullptr)
        {
            mxlReleaseFlowReader(videoInstance_, videoReader_);
            videoReader_ = nullptr;
        }
        if (videoInstance_ != nullptr)
        {
            mxlDestroyInstance(videoInstance_);
            videoInstance_ = nullptr;
        }
        videoFlow_.clear();
        videoPath_.clear();
    }

    void closeAudio()
    {
        if (audioReader_ != nullptr && audioInstance_ != nullptr)
        {
            mxlReleaseFlowReader(audioInstance_, audioReader_);
            audioReader_ = nullptr;
        }
        if (audioInstance_ != nullptr)
        {
            mxlDestroyInstance(audioInstance_);
            audioInstance_ = nullptr;
        }
        audioFlow_.clear();
        audioPath_.clear();
    }

    bool openInstance(std::string const& path, mxlInstance& instance, std::string& currentPath)
    {
        if (instance != nullptr && currentPath == path)
        {
            return true;
        }
        if (instance != nullptr)
        {
            mxlDestroyInstance(instance);
            instance = nullptr;
        }
        instance = mxlCreateInstance(path.c_str(), "");
        currentPath = instance != nullptr ? path : std::string{};
        return instance != nullptr;
    }

    void destroyPipeline()
    {
        std::lock_guard const lock{pipeMu_};
        if (pipeline_ == nullptr)
        {
            return;
        }
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        if (vsrc_ != nullptr)
        {
            gst_object_unref(vsrc_);
            vsrc_ = nullptr;
        }
        if (asrc_ != nullptr)
        {
            gst_object_unref(asrc_);
            asrc_ = nullptr;
        }
        if (overlay_ != nullptr)
        {
            gst_object_unref(overlay_);
            overlay_ = nullptr;
        }
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        pipeSig_.clear();
        pipeAudio_ = false;
        videoFrames_ = 0;
        audioSamplesPushed_ = 0;
    }

    std::string encoderName(bool allowNvenc) const
    {
        if (forcedEncoder_ == "x264")
        {
            return "x264";
        }
        if (forcedEncoder_ == "nvenc" && allowNvenc && elementReady("nvcudah264enc"))
        {
            return "nvenc";
        }
        if (forcedEncoder_ == "auto" && allowNvenc && elementReady("nvcudah264enc"))
        {
            return "nvenc";
        }
        return "x264";
    }

    bool buildPipeline(VideoFormat const& format, ChannelSettings const& settings, bool withAudio, bool allowNvenc)
    {
        std::lock_guard const lock{pipeMu_};
        auto const sig = format.signature() + "/" + std::to_string(settings.preview_height) + "/" + std::to_string(settings.max_fps) + "/" +
                         std::to_string(settings.video_bitrate_kbps) + "/" + std::to_string(settings.audio_bitrate_kbps);
        if (pipeline_ != nullptr && pipeSig_ == sig && pipeAudio_ == withAudio && !(wantX264_ && pipeEncoder_ == "nvenc"))
        {
            return true;
        }
        if (pipeline_ != nullptr)
        {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
            if (vsrc_ != nullptr)
            {
                gst_object_unref(vsrc_);
            }
            if (asrc_ != nullptr)
            {
                gst_object_unref(asrc_);
            }
            if (overlay_ != nullptr)
            {
                gst_object_unref(overlay_);
            }
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
            vsrc_ = nullptr;
            asrc_ = nullptr;
            overlay_ = nullptr;
        }
        auto const encoder = encoderName(allowNvenc && !wantX264_);
        int const fps = std::max(1, (format.rateNum + format.rateDen / 2) / std::max(format.rateDen, 1));
        int const gop = settings.max_fps > 0 ? settings.max_fps : fps;
        std::string launch = "appsrc name=vsrc is-live=true format=time do-timestamp=false block=false max-buffers=2 leaky-type=downstream ! ";
        int previewH = settings.preview_height;
        if (previewH % 2 != 0)
        {
            ++previewH;
        }
        int previewW = format.height > 0 ? format.width * previewH / format.height : format.width;
        if (previewW < 2)
        {
            previewW = 2;
        }
        if (previewW % 2 != 0)
        {
            ++previewW;
        }
        // pushVideo hands over the encoder's picture (preview size, 8-bit 4:2:0) made
        // straight from v210, so no full-size frame goes through GStreamer.
        previewW_ = previewW;
        previewH_ = previewH;
        previewNv12_ = encoder == "nvenc";
        launch += "queue max-size-buffers=2 leaky=downstream ! ";
        if (settings.max_fps > 0)
        {
            launch += "videorate drop-only=true ! video/x-raw,framerate=" + std::to_string(settings.max_fps) + "/1 ! ";
        }
        launch += "textoverlay name=ovl text=\"\" valignment=top halignment=left font-desc=\"Sans 18\" ! ";
        if (encoder == "nvenc")
        {
            // nvcudah264enc takes the P1-P7 presets that current drivers require; the
            // legacy nvh264enc presets fail at caps time ("Selected preset not supported").
            // It only takes NV12 or Y444 in system memory.
            launch += "videoconvert ! video/x-raw,format=NV12 ! ";
            launch += "nvcudah264enc name=enc bitrate=" + std::to_string(settings.video_bitrate_kbps) + " gop-size=" + std::to_string(gop) +
                      " rate-control=cbr preset=p1 tune=ultra-low-latency zero-reorder-delay=true ! ";
        }
        else
        {
            launch += "x264enc name=enc tune=zerolatency speed-preset=ultrafast bitrate=" + std::to_string(settings.video_bitrate_kbps) +
                      " key-int-max=" + std::to_string(gop) + " bframes=0 byte-stream=true aud=true option-string=scenecut=0 ! ";
        }
        launch += "h264parse config-interval=-1 ! queue ! rtspclientsink name=sink location=\"" + cfg_.mediamtx_rtsp_url + "/ch" + std::to_string(index_) +
                  "\" protocols=tcp latency=0 ";
        if (withAudio)
        {
            launch += "appsrc name=asrc is-live=true format=time do-timestamp=false block=false max-buffers=4 leaky-type=downstream ! ";
            launch += "audioconvert ! audioresample ! audio/x-raw,format=S16LE,rate=48000,channels=2,layout=interleaved ! ";
            launch += "opusenc bitrate=" + std::to_string(settings.audio_bitrate_kbps * 1000) + " ! sink. ";
        }
        GError* error = nullptr;
        pipeline_ = gst_parse_launch(launch.c_str(), &error);
        if (pipeline_ == nullptr || error != nullptr)
        {
            std::string message = error != nullptr ? error->message : "parse failed";
            if (error != nullptr)
            {
                g_error_free(error);
            }
            if (pipeline_ != nullptr)
            {
                gst_object_unref(pipeline_);
                pipeline_ = nullptr;
            }
            log::warn("pipeline_build_failed", {{"channel", std::to_string(index_)}, {"encoder", encoder}, {"error", message}});
            if (encoder == "nvenc")
            {
                wantX264_ = true;
                fallbacks_.fetch_add(1);
                return false;
            }
            return false;
        }
        vsrc_ = gst_bin_get_by_name(GST_BIN(pipeline_), "vsrc");
        overlay_ = gst_bin_get_by_name(GST_BIN(pipeline_), "ovl");
        overlayText_.reset();
        asrc_ =withAudio ? gst_bin_get_by_name(GST_BIN(pipeline_), "asrc") : nullptr;
        auto* encoderElement = gst_bin_get_by_name(GST_BIN(pipeline_), "enc");
        if (encoderElement != nullptr)
        {
            auto* pad = gst_element_get_static_pad(encoderElement, "src");
            if (pad != nullptr)
            {
                gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, &Slot::onEncoded, this, nullptr);
                gst_object_unref(pad);
            }
            gst_object_unref(encoderElement);
        }
        std::string caps = std::string("video/x-raw,format=") + (previewNv12_ ? "NV12" : "I420") + ",width=" + std::to_string(previewW_) +
                           ",height=" + std::to_string(previewH_) + ",framerate=" + std::to_string(format.rateNum) + "/" + std::to_string(format.rateDen) +
                           ",pixel-aspect-ratio=1/1,interlace-mode=progressive";
        if (vsrc_ != nullptr)
        {
            GstCaps* videoCaps = gst_caps_from_string(caps.c_str());
            gst_app_src_set_caps(GST_APP_SRC(vsrc_), videoCaps);
            gst_caps_unref(videoCaps);
        }
        if (asrc_ != nullptr)
        {
            GstCaps* audioCaps = gst_caps_from_string("audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved");
            gst_app_src_set_caps(GST_APP_SRC(asrc_), audioCaps);
            gst_caps_unref(audioCaps);
        }
        // Queue one buffer before PLAYING. rtspclientsink waits for media, and this
        // thread is the only video producer, so the wait must not need a later push.
        gst_element_set_state(pipeline_, GST_STATE_READY);
        if (vsrc_ != nullptr)
        {
            // Black in 8-bit 4:2:0: Y 16, chroma 128.
            auto const bytes = previewBytes(previewW_, previewH_);
            auto const luma = static_cast<std::size_t>(previewW_) * static_cast<std::size_t>(previewH_);
            GstBuffer* buffer = gst_buffer_new_allocate(nullptr, bytes, nullptr);
            gst_buffer_memset(buffer, 0, 16, luma);
            gst_buffer_memset(buffer, luma, 128, bytes - luma);
            int const den = std::max(format.rateDen, 1);
            int const num = std::max(format.rateNum, 1);
            GST_BUFFER_PTS(buffer) = 0;
            GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(1, GST_SECOND * static_cast<std::uint64_t>(den), static_cast<std::uint64_t>(num));
            videoFrames_ = 1;
            gst_app_src_push_buffer(GST_APP_SRC(vsrc_), buffer);
        }
        if (asrc_ != nullptr)
        {
            std::size_t const frames = static_cast<std::size_t>(std::max(1, 48000 * std::max(format.rateDen, 1) / std::max(format.rateNum, 1)));
            std::vector<float> silence(frames * 2, 0.f);
            GstBuffer* buffer = gst_buffer_new_allocate(nullptr, silence.size() * sizeof(float), nullptr);
            gst_buffer_fill(buffer, 0, silence.data(), silence.size() * sizeof(float));
            GST_BUFFER_PTS(buffer) = 0;
            GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(frames, GST_SECOND, 48000);
            audioSamplesPushed_ = frames;
            gst_app_src_push_buffer(GST_APP_SRC(asrc_), buffer);
        }
        auto const ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
        if (ret == GST_STATE_CHANGE_FAILURE)
        {
            log::warn("pipeline_state_failed", {{"channel", std::to_string(index_)}, {"encoder", encoder}});
            if (encoder == "nvenc")
            {
                wantX264_ = true;
                fallbacks_.fetch_add(1);
            }
            gst_element_set_state(pipeline_, GST_STATE_NULL);
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
            vsrc_ = nullptr;
            asrc_ = nullptr;
            overlay_ = nullptr;
            return false;
        }
        pipeSig_ = sig;
        pipeAudio_ = withAudio;
        pipeEncoder_ = encoder;
        videoFormat_ = format;
        book_.setEncoder(index_, encoder);
        log::info("pipeline_playing", {{"channel", std::to_string(index_)}, {"encoder", encoder}, {"format", format.signature()}, {"audio", withAudio ? "yes" : "no"}});
        return true;
    }

    bool drainBus()
    {
        bool broken = false;
        GstElement* pipeline = nullptr;
        {
            std::lock_guard const lock{pipeMu_};
            if (pipeline_ == nullptr)
            {
                return false;
            }
            pipeline = pipeline_;
            gst_object_ref(pipeline);
        }
        GstBus* bus = gst_element_get_bus(pipeline);
        while (GstMessage* message = gst_bus_pop(bus))
        {
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR)
            {
                GError* error = nullptr;
                gchar* debug = nullptr;
                gst_message_parse_error(message, &error, &debug);
                log::warn("pipeline_bus_error", {{"channel", std::to_string(index_)}, {"error", error != nullptr ? error->message : ""}, {"debug", debug != nullptr ? debug : ""}});
                // An NVENC session can also fail after PLAYING (caps negotiation).
                // Rebuild that channel with x264 instead of retrying NVENC.
                if (std::strcmp(GST_OBJECT_NAME(GST_MESSAGE_SRC(message)), "enc") == 0)
                {
                    std::lock_guard const lock{pipeMu_};
                    if (pipeEncoder_ == "nvenc" && !wantX264_)
                    {
                        wantX264_ = true;
                        fallbacks_.fetch_add(1);
                        log::warn("encoder_fallback", {{"channel", std::to_string(index_)}, {"encoder", "x264"}});
                    }
                }
                if (error != nullptr)
                {
                    g_error_free(error);
                }
                g_free(debug);
                pipeBroken_.store(true);
            }
            gst_message_unref(message);
        }
        gst_object_unref(bus);
        gst_object_unref(pipeline);
        if (pipeBroken_.exchange(false))
        {
            destroyPipeline();
            broken = true;
        }
        return broken;
    }

    static GstPadProbeReturn onEncoded(GstPad*, GstPadProbeInfo* info, gpointer user)
    {
        auto* self = static_cast<Slot*>(user);
        auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        if (buffer != nullptr)
        {
            self->encodedBytes_.fetch_add(gst_buffer_get_size(buffer));
            self->encodedFrames_.fetch_add(1);
            auto const pushed = self->lastPushNs_.load();
            if (pushed != 0)
            {
                auto const now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                if (now > pushed)
                {
                    self->latency_.observe(static_cast<double>(now - pushed) / 1e9);
                }
            }
        }
        return GST_PAD_PROBE_OK;
    }

    void setOverlay(ChannelSettings const& settings, LegRoute const& route, std::string const& formatText)
    {
        if (overlay_ == nullptr || !settings.overlay)
        {
            if (overlay_ != nullptr)
            {
                applyOverlayText("");
            }
            return;
        }
        std::string text;
        if (settings.overlay_label)
        {
            text += settings.video_label;
        }
        if (settings.overlay_source)
        {
            auto const view = book_.snapshot();
            for (auto const& channel : view)
            {
                if (channel.settings.index == index_ && !channel.source_label.empty())
                {
                    if (!text.empty())
                    {
                        text += "\n";
                    }
                    text += channel.source_label;
                }
            }
        }
        if (settings.overlay_format && !formatText.empty())
        {
            if (!text.empty())
            {
                text += "\n";
            }
            text += formatText;
        }
        if (route.state != RunState::Running)
        {
            if (!text.empty())
            {
                text += "\n";
            }
            text += stateName(route.state);
            if (!route.reason.empty())
            {
                text += " ";
                text += route.reason;
            }
        }
        applyOverlayText(text);
    }

    // textoverlay lays out and renders its text again on every "text" set, and the video
    // loop sets it once per grain: only a change is passed on.
    void applyOverlayText(std::string const& text)
    {
        if (overlayText_ == text)
        {
            return;
        }
        g_object_set(overlay_, "text", text.c_str(), nullptr);
        overlayText_ = text;
    }

    bool pushVideo(std::uint8_t const* data, std::size_t size, VideoFormat const& format)
    {
        std::lock_guard const lock{pipeMu_};
        if (vsrc_ == nullptr)
        {
            return false;
        }
        guint64 level = 0;
        g_object_get(vsrc_, "current-level-buffers", &level, nullptr);
        if (level >= 2)
        {
            droppedQueue_.fetch_add(1);
        }
        if (size < v210FrameBytes(format.width, format.height) || previewW_ < 2 || previewH_ < 2)
        {
            return false;
        }
        GstBuffer* buffer = gst_buffer_new_allocate(nullptr, previewBytes(previewW_, previewH_), nullptr);
        GstMapInfo map{};
        if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE))
        {
            gst_buffer_unref(buffer);
            return false;
        }
        v210ToPreview(data, format.width, format.height, format.interlaced, previewW_, previewH_, previewNv12_, map.data);
        gst_buffer_unmap(buffer, &map);
        int const den = std::max(format.rateDen, 1);
        int const num = std::max(format.rateNum, 1);
        GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(videoFrames_, GST_SECOND * static_cast<std::uint64_t>(den), static_cast<std::uint64_t>(num));
        GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(1, GST_SECOND * static_cast<std::uint64_t>(den), static_cast<std::uint64_t>(num));
        lastVideoPts_.store(GST_BUFFER_PTS(buffer));
        ++videoFrames_;
        lastPushNs_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        auto const flow = gst_app_src_push_buffer(GST_APP_SRC(vsrc_), buffer);
        return flow == GST_FLOW_OK;
    }

    bool pushAudio(float const* interleaved, std::size_t frames)
    {
        std::lock_guard const lock{pipeMu_};
        if (asrc_ == nullptr || frames == 0)
        {
            return false;
        }
        guint64 level = 0;
        g_object_get(asrc_, "current-level-buffers", &level, nullptr);
        if (level >= 4)
        {
            droppedQueue_.fetch_add(1);
        }
        std::size_t const bytes = frames * 2 * sizeof(float);
        GstBuffer* buffer = gst_buffer_new_allocate(nullptr, bytes, nullptr);
        gst_buffer_fill(buffer, 0, interleaved, bytes);
        GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(audioSamplesPushed_, GST_SECOND, 48000);
        GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(frames, GST_SECOND, 48000);
        audioSamplesPushed_ += frames;
        auto const flow = gst_app_src_push_buffer(GST_APP_SRC(asrc_), buffer);
        return flow == GST_FLOW_OK;
    }

    void publishCounters(double lag)
    {
        auto const now = std::chrono::steady_clock::now();
        double fps = encodeFps_;
        double bitrate = bitrateBps_;
        if (rateTick_.time_since_epoch().count() == 0)
        {
            rateTick_ = now;
            rateFrames_ = encodedFrames_.load();
            rateBytes_ = encodedBytes_.load();
        }
        else
        {
            auto const elapsed = std::chrono::duration<double>(now - rateTick_).count();
            if (elapsed >= 1.0)
            {
                auto const frames = encodedFrames_.load();
                auto const bytes = encodedBytes_.load();
                fps = static_cast<double>(frames - rateFrames_) / elapsed;
                bitrate = static_cast<double>(bytes - rateBytes_) * 8.0 / elapsed;
                encodeFps_ = fps;
                bitrateBps_ = bitrate;
                rateTick_ = now;
                rateFrames_ = frames;
                rateBytes_ = bytes;
            }
        }
        book_.setCounters(index_, grainsRead_.load(), droppedQueue_.load(), droppedLate_.load(), resyncs_.load(), lag, fps, bitrate);
    }

    void videoLoop()
    {
        initGst();
        int attempt = 0;
        VideoFormat format;
        std::vector<std::uint8_t> slate;
        std::uint64_t lastIndex = 0;
        bool haveIndex = false;
        auto lastGood = std::chrono::steady_clock::now() - std::chrono::seconds(5);
        while (running_.load())
        {
            if (drainBus())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            auto const route = book_.route(index_, LegKind::Video);
            auto const settings = book_.settings(index_);
            auto const audioRoute = book_.route(index_, LegKind::Audio);
            bool const wantAudio = audioRoute.master_enable && !audioRoute.flow_id.empty();
            bool built = buildPipeline(format, settings, wantAudio, true);
            if (!built && wantX264_)
            {
                built = buildPipeline(format, settings, wantAudio, false);
            }
            if (!built)
            {
                sleepMs(250, route, LegKind::Video);
            }
            setOverlay(settings, route, book_.snapshot().empty() ? std::string{} : formatString(format.height, format.interlaced, format.rateNum, format.rateDen));
            if (!route.master_enable || route.domain_id.empty() || route.flow_id.empty())
            {
                book_.setProbe(index_, LegKind::Video, false, false, false, "");
                closeVideo();
                ensureSlate(format, slate);
                pushVideo(slate.data(), slate.size(), format);
                publishCounters(0);
                sleepMs(40, route, LegKind::Video);
                continue;
            }
            auto const domain = resolveDomain(cfg_.mxl_domain_scan_path, route.domain_id);
            if (!domain)
            {
                book_.setProbe(index_, LegKind::Video, false, false, false, "domain_not_found");
                closeVideo();
                ensureSlate(format, slate);
                pushVideo(slate.data(), slate.size(), format);
                publishCounters(0);
                sleepMs(backoffMs(attempt++), route, LegKind::Video);
                continue;
            }
            if (videoInstance_ != nullptr && videoPath_ != domain->path && videoReader_ != nullptr)
            {
                mxlReleaseFlowReader(videoInstance_, videoReader_);
                videoReader_ = nullptr;
                videoFlow_.clear();
            }
            if (videoReader_ != nullptr && videoFlow_ != route.flow_id)
            {
                mxlReleaseFlowReader(videoInstance_, videoReader_);
                videoReader_ = nullptr;
                videoFlow_.clear();
            }
            if (!openInstance(domain->path, videoInstance_, videoPath_) || videoInstance_ == nullptr)
            {
                book_.setProbe(index_, LegKind::Video, false, false, false, "domain_not_found");
                sleepMs(backoffMs(attempt++), route, LegKind::Video);
                continue;
            }
            if (videoReader_ == nullptr)
            {
                auto const status = mxlCreateFlowReader(videoInstance_, route.flow_id.c_str(), nullptr, &videoReader_);
                if (status != MXL_STATUS_OK)
                {
                    videoReader_ = nullptr;
                    book_.setProbe(index_, LegKind::Video, true, false, false, "flow_not_found");
                    ensureSlate(format, slate);
                    pushVideo(slate.data(), slate.size(), format);
                    publishCounters(0);
                    sleepMs(backoffMs(attempt++), route, LegKind::Video);
                    continue;
                }
                videoFlow_ = route.flow_id;
                auto const def = readFlowDef(videoInstance_, route.flow_id);
                auto const parsed = parseVideoDef(def);
                if (parsed.signature() != format.signature())
                {
                    format = parsed;
                    haveIndex = false;
                    destroyPipeline();
                }
                book_.setFormat(index_, format.width, format.height, format.rateNum, format.rateDen, format.interlaced,
                    formatString(format.height, format.interlaced, format.rateNum, format.rateDen), -1);
                attempt = 0;
            }
            mxlFlowRuntimeInfo runtime{};
            mxlFlowConfigInfo info{};
            mxlFlowReaderGetRuntimeInfo(videoReader_, &runtime);
            mxlFlowReaderGetConfigInfo(videoReader_, &info);
            auto const nowTai = mxlGetTime();
            std::uint64_t const frameNs = format.rateNum > 0 ? (static_cast<std::uint64_t>(format.rateDen) * 1000000000ULL) / static_cast<std::uint64_t>(format.rateNum) : 40000000ULL;
            bool const fresh = runtime.headIndex != MXL_UNDEFINED_INDEX && runtime.headIndex != 0 && runtime.lastWriteTime != 0 &&
                               nowTai >= runtime.lastWriteTime && (nowTai - runtime.lastWriteTime) < frameNs * 3;
            if (!fresh)
            {
                book_.setProbe(index_, LegKind::Video, true, true, false, "");
                ensureSlate(format, slate);
                pushVideo(slate.data(), slate.size(), format);
                publishCounters(0);
                sleepMs(static_cast<int>(std::min<std::uint64_t>(frameNs / 1000000ULL, 80)), route, LegKind::Video);
                continue;
            }
            std::uint64_t const head = runtime.headIndex;
            std::uint64_t const offset = static_cast<std::uint64_t>(std::max(0, cfg_.read_offset_grains));
            std::uint64_t target = head > offset ? head - offset : 0;
            if (haveIndex && head > lastIndex && info.discrete.grainCount > 0 && head - lastIndex > info.discrete.grainCount)
            {
                resyncs_.fetch_add(1);
                droppedLate_.fetch_add(1);
                target = head > offset ? head - offset : head;
                haveIndex = false;
            }
            std::uint64_t index = haveIndex ? lastIndex + 1 : target;
            if (index < target)
            {
                index = target;
            }
            if (index > head)
            {
                book_.setProbe(index_, LegKind::Video, true, true, std::chrono::steady_clock::now() - lastGood < std::chrono::milliseconds(500), "");
                publishCounters(static_cast<double>(head - (haveIndex ? lastIndex : target)));
                sleepMs(5, route, LegKind::Video);
                continue;
            }
            mxlGrainInfo grain{};
            std::uint8_t* payload = nullptr;
            auto const status = mxlFlowReaderGetGrainNonBlocking(videoReader_, index, &grain, &payload);
            if (status == MXL_ERR_OUT_OF_RANGE_TOO_LATE || status == MXL_ERR_FLOW_INVALID)
            {
                droppedLate_.fetch_add(1);
                resyncs_.fetch_add(1);
                haveIndex = false;
                if (status == MXL_ERR_FLOW_INVALID && videoReader_ != nullptr)
                {
                    mxlReleaseFlowReader(videoInstance_, videoReader_);
                    videoReader_ = nullptr;
                    videoFlow_.clear();
                }
                continue;
            }
            if (status == MXL_ERR_OUT_OF_RANGE_TOO_EARLY || status == MXL_ERR_TIMEOUT)
            {
                sleepMs(5, route, LegKind::Video);
                continue;
            }
            if (status != MXL_STATUS_OK || payload == nullptr)
            {
                sleepMs(10, route, LegKind::Video);
                continue;
            }
            lastIndex = index;
            haveIndex = true;
            bool const valid = (grain.flags & MXL_GRAIN_FLAG_INVALID) == 0 && grain.validSlices == grain.totalSlices;
            double const lag = static_cast<double>(head - index);
            if (!valid)
            {
                book_.setProbe(index_, LegKind::Video, true, true, false, "");
                ensureSlate(format, slate);
                pushVideo(slate.data(), slate.size(), format);
                publishCounters(lag);
                continue;
            }
            auto const ts = mxlIndexToTimestamp(&info.common.grainRate, index);
            alignTimestamp_.store(ts);
            frameSamples_.store(std::max(1, 48000 * format.rateDen / std::max(format.rateNum, 1)));
            grainsRead_.fetch_add(1);
            lastGood = std::chrono::steady_clock::now();
            book_.setProbe(index_, LegKind::Video, true, true, true, "");
            book_.setFormat(index_, format.width, format.height, format.rateNum, format.rateDen, format.interlaced,
                formatString(format.height, format.interlaced, format.rateNum, format.rateDen), -1);
            ensureSlate(format, slate);
            std::size_t const expect = slate.size();
            std::size_t const got = std::min(expect, static_cast<std::size_t>(grain.grainSize));
            if (got < expect)
            {
                std::memcpy(slate.data(), payload, got);
                pushVideo(slate.data(), expect, format);
                ensureSlate(format, slate);
            }
            else
            {
                pushVideo(payload, expect, format);
            }
            publishCounters(lag);
        }
    }

    void ensureSlate(VideoFormat const& format, std::vector<std::uint8_t>& slate)
    {
        auto const bytes = v210FrameBytes(format.width, format.height);
        if (slate.size() != bytes)
        {
            slate.assign(bytes, 0);
            fillV210Black(slate.data(), format.width, format.height);
        }
    }

    void audioLoop()
    {
        int attempt = 0;
        auto lastLevels = std::chrono::steady_clock::now();
        while (running_.load())
        {
            auto const route = book_.route(index_, LegKind::Audio);
            auto const settings = book_.settings(index_);
            if (!route.master_enable || route.domain_id.empty() || route.flow_id.empty())
            {
                book_.setProbe(index_, LegKind::Audio, false, false, false, "");
                closeAudio();
                audioChannels_ = 0;
                sleepMs(40, route, LegKind::Audio);
                continue;
            }
            auto const domain = resolveDomain(cfg_.mxl_domain_scan_path, route.domain_id);
            if (!domain)
            {
                book_.setProbe(index_, LegKind::Audio, false, false, false, "domain_not_found");
                closeAudio();
                sleepMs(backoffMs(attempt++), route, LegKind::Audio);
                continue;
            }
            if (audioInstance_ != nullptr && audioPath_ != domain->path && audioReader_ != nullptr)
            {
                mxlReleaseFlowReader(audioInstance_, audioReader_);
                audioReader_ = nullptr;
                audioFlow_.clear();
            }
            if (audioReader_ != nullptr && audioFlow_ != route.flow_id)
            {
                mxlReleaseFlowReader(audioInstance_, audioReader_);
                audioReader_ = nullptr;
                audioFlow_.clear();
            }
            if (!openInstance(domain->path, audioInstance_, audioPath_))
            {
                book_.setProbe(index_, LegKind::Audio, false, false, false, "domain_not_found");
                sleepMs(backoffMs(attempt++), route, LegKind::Audio);
                continue;
            }
            if (audioReader_ == nullptr)
            {
                auto const status = mxlCreateFlowReader(audioInstance_, route.flow_id.c_str(), nullptr, &audioReader_);
                if (status != MXL_STATUS_OK)
                {
                    audioReader_ = nullptr;
                    book_.setProbe(index_, LegKind::Audio, true, false, false, "flow_not_found");
                    sleepMs(backoffMs(attempt++), route, LegKind::Audio);
                    continue;
                }
                audioFlow_ = route.flow_id;
                audioHeadSeen_ = MXL_UNDEFINED_INDEX;
                auto const parsed = parseAudioDef(readFlowDef(audioInstance_, route.flow_id));
                audioChannels_ = parsed.channels;
                attempt = 0;
            }
            mxlFlowRuntimeInfo runtime{};
            mxlFlowConfigInfo info{};
            mxlFlowReaderGetRuntimeInfo(audioReader_, &runtime);
            mxlFlowReaderGetConfigInfo(audioReader_, &info);
            int const channels = static_cast<int>(info.continuous.channelCount);
            if (channels > 0)
            {
                audioChannels_ = channels;
            }
            // MXL's continuous writer does not set lastWriteTime (only the discrete one does), so an
            // audio flow is live while its head moves.
            auto const steadyNow = std::chrono::steady_clock::now();
            if (runtime.headIndex != audioHeadSeen_)
            {
                if (audioHeadSeen_ != MXL_UNDEFINED_INDEX)
                {
                    audioHeadAt_ = steadyNow;
                }
                audioHeadSeen_ = runtime.headIndex;
            }
            bool const fresh = runtime.headIndex != MXL_UNDEFINED_INDEX && runtime.headIndex > 480 && steadyNow - audioHeadAt_ < std::chrono::milliseconds(100);
            if (!fresh)
            {
                book_.setProbe(index_, LegKind::Audio, true, true, false, "");
                int const frameSamples = std::max(1, frameSamples_.load());
                std::vector<float> silence(static_cast<std::size_t>(frameSamples) * 2, 0.f);
                pushAudio(silence.data(), static_cast<std::size_t>(frameSamples));
                sleepMs(20, route, LegKind::Audio);
                continue;
            }
            int const frameSamples = std::max(1, frameSamples_.load());
            std::uint64_t end = runtime.headIndex;
            std::uint64_t const align = alignTimestamp_.load();
            if (align != 0)
            {
                end = (align / 1000000000ULL) * 48000ULL + ((align % 1000000000ULL) * 48000ULL) / 1000000000ULL;
                if (end > runtime.headIndex)
                {
                    end = runtime.headIndex;
                }
            }
            else if (end > static_cast<std::uint64_t>(frameSamples * std::max(1, cfg_.read_offset_grains)))
            {
                end -= static_cast<std::uint64_t>(frameSamples * std::max(1, cfg_.read_offset_grains));
            }
            if (end <= static_cast<std::uint64_t>(frameSamples))
            {
                sleepMs(10, route, LegKind::Audio);
                continue;
            }
            if (haveAudioIndex_ && end <= lastAudioIndex_)
            {
                book_.setProbe(index_, LegKind::Audio, true, true, true, "");
                sleepMs(5, route, LegKind::Audio);
                continue;
            }
            mxlWrappedMultiBufferSlice slice{};
            auto const status = mxlFlowReaderGetSamplesNonBlocking(audioReader_, end, static_cast<std::size_t>(frameSamples), &slice);
            if (status == MXL_ERR_OUT_OF_RANGE_TOO_LATE)
            {
                droppedLate_.fetch_add(1);
                resyncs_.fetch_add(1);
                haveAudioIndex_ = false;
                continue;
            }
            if (status != MXL_STATUS_OK)
            {
                book_.setProbe(index_, LegKind::Audio, true, true, false, "");
                sleepMs(10, route, LegKind::Audio);
                continue;
            }
            std::vector<std::vector<float>> planar;
            copySamples(slice, static_cast<std::size_t>(frameSamples), planar);
            std::vector<float const*> ptrs(planar.size());
            for (std::size_t i = 0; i < planar.size(); ++i)
            {
                ptrs[i] = planar[i].data();
            }
            if (std::chrono::steady_clock::now() - lastLevels > std::chrono::milliseconds(100))
            {
                auto const levels = measureLevels(ptrs.data(), static_cast<int>(ptrs.size()), static_cast<std::size_t>(frameSamples));
                book_.setLevels(index_, levels.peak_dbfs, levels.rms_dbfs);
                lastLevels = std::chrono::steady_clock::now();
            }
            auto const selection = selectAudioPair(static_cast<int>(ptrs.size()), settings.audio_pair, settings.downmix);
            std::vector<float> interleaved(static_cast<std::size_t>(frameSamples) * 2, 0.f);
            renderAudioPair(ptrs.data(), static_cast<int>(ptrs.size()), static_cast<std::size_t>(frameSamples), selection, interleaved.data());
            pushAudio(interleaved.data(), static_cast<std::size_t>(frameSamples));
            lastAudioIndex_ = end;
            haveAudioIndex_ = true;
            book_.setProbe(index_, LegKind::Audio, true, true, true, "");
            book_.setFormat(index_, 0, 0, 0, 1, false, "", audioChannels_);
        }
    }

    int index_;
    Config cfg_;
    ChannelBook& book_;
    LatencyHistogram& latency_;
    std::atomic<std::uint64_t> fallbacks_{0};
    std::string forcedEncoder_;
    std::atomic<bool> running_{false};
    std::thread videoThread_;
    std::thread audioThread_;
    mxlInstance videoInstance_ = nullptr;
    mxlFlowReader videoReader_ = nullptr;
    std::string videoPath_;
    std::string videoFlow_;
    mxlInstance audioInstance_ = nullptr;
    mxlFlowReader audioReader_ = nullptr;
    std::string audioPath_;
    std::string audioFlow_;
    std::uint64_t audioHeadSeen_ = MXL_UNDEFINED_INDEX;           // audio head at the last pass
    std::chrono::steady_clock::time_point audioHeadAt_{};         // when it last moved
    int audioChannels_ = 0;
    std::mutex pipeMu_;
    GstElement* pipeline_ = nullptr;
    GstElement* vsrc_ = nullptr;
    GstElement* asrc_ = nullptr;
    // The picture appsrc carries: preview size, NV12 for NVENC, I420 for x264.
    int previewW_ = 0;
    int previewH_ = 0;
    bool previewNv12_ = false;
    GstElement* overlay_ = nullptr;
    std::optional<std::string> overlayText_; // the text overlay_ shows
    std::string pipeSig_;
    std::string pipeEncoder_;
    bool pipeAudio_ = false;
    bool wantX264_ = false;
    std::atomic<bool> pipeBroken_{false};
    VideoFormat videoFormat_{};
    std::uint64_t videoFrames_ = 0;
    std::uint64_t audioSamplesPushed_ = 0;
    std::atomic<std::int64_t> lastVideoPts_{0};
    std::atomic<std::int64_t> lastPushNs_{0};
    std::atomic<std::uint64_t> encodedBytes_{0};
    std::atomic<std::uint64_t> encodedFrames_{0};
    std::atomic<std::uint64_t> grainsRead_{0};
    std::atomic<std::uint64_t> droppedQueue_{0};
    std::atomic<std::uint64_t> droppedLate_{0};
    std::atomic<std::uint64_t> resyncs_{0};
    std::atomic<std::uint64_t> alignTimestamp_{0};
    std::atomic<int> frameSamples_{1920};
    std::chrono::steady_clock::time_point rateTick_{};
    std::uint64_t rateFrames_ = 0;
    std::uint64_t rateBytes_ = 0;
    double encodeFps_ = 0;
    double bitrateBps_ = 0;
    std::uint64_t lastAudioIndex_ = 0;
    bool haveAudioIndex_ = false;
};
} // namespace

struct MediaHost::Impl
{
    Impl(Config c, ChannelBook& b)
        : cfg(std::move(c))
        , book(b)
    {
    }

    Config cfg;
    ChannelBook& book;
    std::string available = "x264";
    std::vector<std::unique_ptr<LatencyHistogram>> latency;
    std::vector<std::unique_ptr<Slot>> slots;
};

MediaHost::MediaHost(Config cfg, ChannelBook& book)
{
    initGst();
    bool const nv = elementReady("nvcudah264enc");
    auto impl = std::make_unique<Impl>(std::move(cfg), book);
    impl->available = nv ? "nvenc,x264" : "x264";
    std::string forced = impl->cfg.encoder;
    if (forced == "nvenc" && !nv)
    {
        log::warn("nvenc_unavailable", {{"fallback", "x264"}});
        forced = "x264";
    }
    for (int i = 1; i <= impl->cfg.monitor_channels; ++i)
    {
        impl->latency.push_back(std::make_unique<LatencyHistogram>());
        impl->slots.push_back(std::make_unique<Slot>(i, impl->cfg, impl->book, *impl->latency.back(), forced));
    }
    impl_ = std::move(impl);
}

MediaHost::~MediaHost()
{
    stop();
}

void MediaHost::start()
{
    for (auto& slot : impl_->slots)
    {
        slot->start();
    }
}

void MediaHost::stop()
{
    for (auto& slot : impl_->slots)
    {
        slot->stop();
    }
}

std::string MediaHost::encoderAvailable() const
{
    return impl_->available;
}

std::vector<std::uint64_t> MediaHost::fallbacks() const
{
    std::vector<std::uint64_t> out;
    out.reserve(impl_->slots.size());
    for (auto const& slot : impl_->slots)
    {
        out.push_back(slot->fallbackCount());
    }
    return out;
}

std::vector<LatencyHistogram const*> MediaHost::latency() const
{
    std::vector<LatencyHistogram const*> out;
    out.reserve(impl_->latency.size());
    for (auto const& item : impl_->latency)
    {
        out.push_back(item.get());
    }
    return out;
}
} // namespace mwm
