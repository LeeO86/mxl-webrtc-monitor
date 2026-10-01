#include "nmos/node.hpp"

#include "nmos/ids.hpp"
#include "util/httpclient.hpp"
#include "util/logging.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#if defined(MWM_WITH_NMOS)
#include "cpprest/host_utils.h"
#include "nmos/capabilities.h"
#include "nmos/clock_name.h"
#include "nmos/connection_api.h"
#include "nmos/connection_resources.h"
#include "nmos/format.h"
#include "nmos/transport.h"
#include "nmos/group_hint.h"
#include "nmos/interlace_mode.h"
#include "nmos/log_gate.h"
#include "nmos/media_type.h"
#include "nmos/model.h"
#include "nmos/mxl.h"
#include "nmos/node_interfaces.h"
#include "nmos/node_resource.h"
#include "nmos/node_resources.h"
#include "nmos/node_server.h"
#include "nmos/server.h"
#include "nmos/settings.h"
#include "nmos/slog.h"
#include "sdp/json.h"
#endif

namespace mwm
{
struct NmosNode::Impl
{
    Impl(Config c, ChannelBook& b)
        : cfg(std::move(c))
        , book(b)
        , ids(makeNmosIds(cfg.nmos_seed))
    {
    }

    Config cfg;
    ChannelBook& book;
    NmosIds ids;
    std::atomic<bool> running{false};
    std::mutex readyMu;
    std::condition_variable readyCv;
    bool ready = false;
    std::string error;
#if defined(MWM_WITH_NMOS)
    std::thread thread;
#endif
};

NmosNode::NmosNode(Config cfg, ChannelBook& book)
    : impl_(std::unique_ptr<Impl>(new Impl(std::move(cfg), book)))
{
}

NmosNode::~NmosNode()
{
    stop();
}

std::string NmosNode::nodeId() const
{
    return impl_->ids.node;
}

bool NmosNode::running() const
{
    return impl_->running.load();
}

bool NmosNode::registered() const
{
    if (!impl_->cfg.nmos_enable)
    {
        return true;
    }
    if (!impl_->running.load())
    {
        return false;
    }
    if (impl_->cfg.nmos_registry_address.empty())
    {
        return impl_->cfg.nmos_dns_sd;
    }
    auto const url = "http://" + impl_->cfg.nmos_registry_address + ":" + std::to_string(impl_->cfg.nmos_registry_port + 1) + "/x-nmos/query/v1.3/nodes/" +
                     impl_->ids.node;
    auto const response = httpGet(url, 700);
    return response.status == 200;
}

std::string NmosNode::summary() const
{
    std::string out = std::string("{\"enabled\":") + (impl_->cfg.nmos_enable ? "true" : "false") + ",\"node_id\":\"" + impl_->ids.node + "\",\"device_id\":\"" +
                      impl_->ids.device + "\",\"device_label\":\"MXL WebRTC Monitor\",\"registry\":\"" + impl_->cfg.nmos_registry_address + "\",\"registry_port\":" +
                      std::to_string(impl_->cfg.nmos_registry_port) + ",\"port\":" + std::to_string(impl_->cfg.nmos_port) + ",\"dns_sd\":" +
                      (impl_->cfg.nmos_dns_sd ? "true" : "false") + ",\"registered\":" + (registered() ? "true" : "false") + ",\"receivers\":[";
    auto const views = impl_->book.snapshot();
    bool first = true;
    for (auto const& view : views)
    {
        auto add = [&](char const* kind, LegRoute const& route, std::string const& id, std::string const& label) {
            if (!first)
            {
                out += ',';
            }
            first = false;
            out += std::string("{\"id\":\"") + id + "\",\"kind\":\"" + kind + "\",\"label\":\"" + label + "\",\"state\":\"" + stateName(route.state) +
                   "\",\"sender_id\":" + (route.sender_id.empty() ? "null" : std::string("\"") + route.sender_id + "\"") + ",\"active\":" +
                   (route.master_enable ? "true" : "false") + ",\"mxl_domain_id\":" + (route.domain_id.empty() ? "null" : std::string("\"") + route.domain_id + "\"") +
                   ",\"mxl_flow_id\":" + (route.flow_id.empty() ? "null" : std::string("\"") + route.flow_id + "\"") + "}";
        };
        add("video", view.video, impl_->ids.videoReceiver(view.settings.index), view.settings.video_label);
        add("audio", view.audio, impl_->ids.audioReceiver(view.settings.index), view.settings.audio_label);
    }
    out += "]}";
    return out;
}

#if !defined(MWM_WITH_NMOS)
void NmosNode::start()
{
    if (impl_->cfg.nmos_enable)
    {
        throw std::runtime_error("built without nmos-cpp");
    }
}

void NmosNode::stop() {}
#else
namespace
{
utility::string_t us(std::string const& text)
{
    return utility::conversions::to_string_t(text);
}

std::string su(utility::string_t const& text)
{
    return utility::conversions::to_utf8string(text);
}

web::json::value capsWith(web::json::value sets, utility::string_t const& mediaTypesJoined)
{
    (void)mediaTypesJoined;
    web::json::value caps = web::json::value::object();
    caps[U("constraint_sets")] = std::move(sets);
    caps[U("version")] = web::json::value::string(nmos::make_version());
    return caps;
}

void tagGroup(nmos::resource& resource, std::string const& group, std::string const& role)
{
    if (!resource.data.has_field(nmos::fields::tags))
    {
        resource.data[U("tags")] = web::json::value::object();
    }
    web::json::push_back(resource.data[U("tags")][U("urn:x-nmos:tag:grouphint/v1.0")], nmos::make_group_hint({us(group), us(role)}));
}
} // namespace

void NmosNode::start()
{
    if (!impl_->cfg.nmos_enable)
    {
        return;
    }
    impl_->thread = std::thread([this] {
        nmos::experimental::log_model logModel;
        std::ostream errorLog(std::cerr.rdbuf());
        std::filebuf discarded;
        std::ostream accessLog(&discarded);
        nmos::experimental::log_gate gate(errorLog, accessLog, logModel);
        try
        {
            nmos::node_model nodeModel;
            web::json::value settings = web::json::value::object();
            settings[U("http_port")] = impl_->cfg.nmos_port;
            settings[U("label")] = web::json::value::string(us(impl_->cfg.host_id));
            settings[U("description")] = web::json::value::string(U("mxl-webrtc-monitor"));
            settings[U("seed_id")] = web::json::value::string(us(impl_->ids.node));
            settings[U("service_name_prefix")] = web::json::value::string(U("mxl-webrtc-monitor"));
            settings[U("logging_level")] = 20;
            settings[U("control_protocol_ws_port")] = -1;
            settings[U("host_address")] = web::json::value::string(us(impl_->cfg.monitor_public_ip));
            if (!impl_->cfg.nmos_dns_sd)
            {
                settings[U("pri")] = std::numeric_limits<int>::max();
                settings[U("highest_pri")] = std::numeric_limits<int>::max();
            }
            if (!impl_->cfg.nmos_registry_address.empty())
            {
                settings[U("registry_address")] = web::json::value::string(us(impl_->cfg.nmos_registry_address));
                settings[U("registration_port")] = impl_->cfg.nmos_registry_port;
                settings[U("query_port")] = impl_->cfg.nmos_registry_port + 1;
            }
            nodeModel.settings = settings;
            nmos::insert_node_default_settings(nodeModel.settings);
            logModel.settings = nodeModel.settings;
            logModel.level = nmos::fields::logging_level(logModel.settings);

            auto implementation =
                nmos::experimental::node_implementation()
                    .on_parse_transport_file([](nmos::resource const&, nmos::resource const&, utility::string_t const&, utility::string_t const&,
                                                 slog::base_gate&) -> web::json::value { throw std::runtime_error("MXL does not use a transport file"); })
                    .on_resolve_auto([](nmos::resource const&, nmos::resource const&, web::json::value& params) {
                        if (!params.is_array() || params.size() == 0)
                        {
                            return;
                        }
                        auto& leg = params.at(0);
                        nmos::details::resolve_auto(leg, U("mxl_domain_id"), [] { return web::json::value::string(U("00000000-0000-0000-0000-000000000000")); });
                        nmos::details::resolve_auto(leg, U("mxl_flow_id"), [] { return web::json::value::null(); });
                    })
                    .on_set_transportfile([](nmos::resource const&, nmos::resource const&, web::json::value& transportFile) { transportFile = web::json::value::null(); })
                    .on_connection_activated([this](nmos::resource const&, nmos::resource const& connection) {
                        auto const id = su(connection.id);
                        int channel = 0;
                        LegKind kind = LegKind::Video;
                        bool matched = false;
                        for (int i = 1; i <= impl_->cfg.monitor_channels; ++i)
                        {
                            if (id == impl_->ids.videoReceiver(i))
                            {
                                channel = i;
                                kind = LegKind::Video;
                                matched = true;
                            }
                            else if (id == impl_->ids.audioReceiver(i))
                            {
                                channel = i;
                                kind = LegKind::Audio;
                                matched = true;
                            }
                        }
                        if (!matched || !connection.data.has_field(U("active")))
                        {
                            return;
                        }
                        auto const& active = connection.data.at(U("active"));
                        bool enable = active.has_field(U("master_enable")) && active.at(U("master_enable")).is_boolean() && active.at(U("master_enable")).as_bool();
                        std::string domain;
                        std::string flow;
                        std::string sender;
                        if (active.has_field(U("sender_id")) && active.at(U("sender_id")).is_string())
                        {
                            sender = su(active.at(U("sender_id")).as_string());
                        }
                        if (active.has_field(U("transport_params")) && active.at(U("transport_params")).is_array() && active.at(U("transport_params")).size() > 0)
                        {
                            auto const& params = active.at(U("transport_params")).at(0);
                            if (params.has_field(U("mxl_domain_id")) && params.at(U("mxl_domain_id")).is_string())
                            {
                                domain = su(params.at(U("mxl_domain_id")).as_string());
                                if (domain == "00000000-0000-0000-0000-000000000000")
                                {
                                    domain.clear();
                                }
                            }
                            if (params.has_field(U("mxl_flow_id")) && params.at(U("mxl_flow_id")).is_string())
                            {
                                flow = su(params.at(U("mxl_flow_id")).as_string());
                            }
                        }
                        impl_->book.setRoute(channel, kind, enable, domain, flow, sender);
                        log::info("nmos_activation", {{"channel", std::to_string(channel)}, {"kind", kind == LegKind::Video ? "video" : "audio"}, {"enable", enable ? "true" : "false"},
                                                        {"domain", domain}, {"flow", flow}});
                    });

            auto server = nmos::experimental::make_node_server(nodeModel, implementation, logModel, gate);
            server.thread_functions.push_back([this, &nodeModel] {
                try
                {
                    {
                        auto lock = nodeModel.write_lock();
                        using web::json::value;
                        auto const clocks = web::json::value_of({nmos::make_internal_clock(nmos::clock_names::clk0)});
                        auto const interfaces = nmos::experimental::node_interfaces(nmos::get_host_interfaces(nodeModel.settings));
                        auto node = nmos::make_node(us(impl_->ids.node), clocks, nmos::make_node_interfaces(interfaces), nodeModel.settings);
                        node.data[U("label")] = value::string(us(impl_->cfg.host_id));
                        node.data[U("description")] = value::string(U("MXL WebRTC monitor"));
                        nmos::insert_resource(nodeModel.node_resources, std::move(node));

                        std::vector<nmos::id> receivers;
                        for (int i = 1; i <= impl_->cfg.monitor_channels; ++i)
                        {
                            receivers.push_back(us(impl_->ids.videoReceiver(i)));
                            receivers.push_back(us(impl_->ids.audioReceiver(i)));
                        }
                        auto device = nmos::make_device(us(impl_->ids.device), us(impl_->ids.node), {}, receivers, nodeModel.settings);
                        device.data[U("label")] = value::string(U("MXL WebRTC Monitor"));
                        device.data[U("description")] = value::string(U("Preview receivers"));
                        nmos::insert_resource(nodeModel.node_resources, std::move(device));

                        std::vector<nmos::rational> rates{{25, 1}, {30000, 1001}, {50, 1}, {60000, 1001}};
                        for (int i = 1; i <= impl_->cfg.monitor_channels; ++i)
                        {
                            auto const group = "Monitor " + std::to_string(i);
                            auto const videoId = impl_->ids.videoReceiver(i);
                            auto const audioId = impl_->ids.audioReceiver(i);
                            auto video = nmos::make_receiver(us(videoId), us(impl_->ids.device), nmos::transports::mxl, {}, nmos::formats::video,
                                {nmos::media_types::video_v210, nmos::media_types::video_v210a}, nodeModel.settings);
                            video.data[U("label")] = value::string(us("Monitor " + std::to_string(i) + " Video"));
                            video.data[U("description")] = value::string(U("MXL video preview receiver"));
                            tagGroup(video, group, "Video");
                            web::json::value hd = web::json::value::object();
                            hd[U("urn:x-nmos:cap:format:media_type")] =
                                nmos::make_caps_string_constraint({nmos::media_types::video_v210.name, nmos::media_types::video_v210a.name});
                            hd[U("urn:x-nmos:cap:format:grain_rate")] = nmos::make_caps_rational_constraint(rates);
                            hd[U("urn:x-nmos:cap:format:frame_width")] = nmos::make_caps_integer_constraint({1920});
                            hd[U("urn:x-nmos:cap:format:frame_height")] = nmos::make_caps_integer_constraint({1080});
                            hd[U("urn:x-nmos:cap:format:interlace_mode")] =
                                nmos::make_caps_string_constraint({nmos::interlace_modes::progressive.name, nmos::interlace_modes::interlaced_tff.name});
                            hd[U("urn:x-nmos:cap:format:color_sampling")] = nmos::make_caps_string_constraint({sdp::samplings::YCbCr_4_2_2.name});
                            hd[U("urn:x-nmos:cap:format:component_depth")] = nmos::make_caps_integer_constraint({std::int64_t{10}});
                            web::json::value uhd = hd;
                            uhd[U("urn:x-nmos:cap:format:frame_width")] = nmos::make_caps_integer_constraint({3840});
                            uhd[U("urn:x-nmos:cap:format:frame_height")] = nmos::make_caps_integer_constraint({2160});
                            uhd[U("urn:x-nmos:cap:format:interlace_mode")] = nmos::make_caps_string_constraint({nmos::interlace_modes::progressive.name});
                            web::json::value sets = web::json::value::array();
                            web::json::push_back(sets, std::move(hd));
                            web::json::push_back(sets, std::move(uhd));
                            web::json::value mediaTypes = web::json::value::array();
                            web::json::push_back(mediaTypes, web::json::value::string(nmos::media_types::video_v210.name));
                            web::json::push_back(mediaTypes, web::json::value::string(nmos::media_types::video_v210a.name));
                            auto caps = capsWith(std::move(sets), {});
                            caps[U("media_types")] = std::move(mediaTypes);
                            video.data[U("caps")] = std::move(caps);
                            nmos::insert_resource(nodeModel.node_resources, std::move(video));

                            auto audio = nmos::make_receiver(us(audioId), us(impl_->ids.device), nmos::transports::mxl, {}, nmos::formats::audio,
                                {nmos::media_types::audio_float32}, nodeModel.settings);
                            audio.data[U("label")] = value::string(us("Monitor " + std::to_string(i) + " Audio"));
                            audio.data[U("description")] = value::string(U("MXL audio preview receiver"));
                            tagGroup(audio, group, "Audio");
                            web::json::value audioSet = web::json::value::object();
                            audioSet[U("urn:x-nmos:cap:format:media_type")] = nmos::make_caps_string_constraint({nmos::media_types::audio_float32.name});
                            audioSet[U("urn:x-nmos:cap:format:channel_count")] = nmos::make_caps_integer_constraint(std::vector<std::int64_t>{}, 1, 64);
                            audioSet[U("urn:x-nmos:cap:format:sample_rate")] = nmos::make_caps_rational_constraint({nmos::rational{48000, 1}});
                            audioSet[U("urn:x-nmos:cap:format:sample_depth")] = nmos::make_caps_integer_constraint({std::int64_t{32}});
                            web::json::value audioSets = web::json::value::array();
                            web::json::push_back(audioSets, std::move(audioSet));
                            auto audioCaps = capsWith(std::move(audioSets), {});
                            web::json::value audioTypes = web::json::value::array();
                            web::json::push_back(audioTypes, web::json::value::string(nmos::media_types::audio_float32.name));
                            audioCaps[U("media_types")] = std::move(audioTypes);
                            audio.data[U("caps")] = std::move(audioCaps);
                            nmos::insert_resource(nodeModel.node_resources, std::move(audio));

                            for (auto const& id : {videoId, audioId})
                            {
                                auto connection = nmos::make_connection_mxl_receiver(us(id), {});
                                connection.data[U("active")][U("master_enable")] = web::json::value::boolean(false);
                                connection.data[U("staged")][U("master_enable")] = web::json::value::boolean(false);
                                connection.data[U("active")][U("transport_params")][0][U("mxl_domain_id")] = web::json::value::null();
                                connection.data[U("active")][U("transport_params")][0][U("mxl_flow_id")] = web::json::value::null();
                                nmos::insert_resource(nodeModel.connection_resources, std::move(connection));
                            }
                        }
                        nodeModel.notify();
                    }
                    {
                        std::lock_guard const lock{impl_->readyMu};
                        impl_->ready = true;
                        impl_->running.store(true);
                        impl_->readyCv.notify_all();
                    }
                    auto lock = nodeModel.write_lock();
                    nodeModel.wait(lock, [&] { return nodeModel.shutdown; });
                }
                catch (std::exception const& ex)
                {
                    std::lock_guard const lock{impl_->readyMu};
                    impl_->error = ex.what();
                    impl_->ready = true;
                    impl_->readyCv.notify_all();
                }
            });
            nmos::server_guard guard(server);
            {
                std::unique_lock lock{impl_->readyMu};
                impl_->readyCv.wait(lock, [&] { return impl_->ready; });
            }
            if (!impl_->error.empty())
            {
                throw std::runtime_error(impl_->error);
            }
            log::info("nmos_node_ready", {{"port", std::to_string(impl_->cfg.nmos_port)}, {"node_id", impl_->ids.node}});
            while (impl_->running.load())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            {
                auto lock = nodeModel.write_lock();
                nodeModel.shutdown = true;
                nodeModel.notify();
            }
        }
        catch (std::exception const& ex)
        {
            std::lock_guard const lock{impl_->readyMu};
            if (impl_->error.empty())
            {
                impl_->error = ex.what();
            }
            impl_->ready = true;
            impl_->readyCv.notify_all();
            log::error("nmos_node_failed", {{"error", ex.what()}});
        }
    });

    std::unique_lock lock{impl_->readyMu};
    impl_->readyCv.wait_for(lock, std::chrono::seconds(20), [&] { return impl_->ready; });
    if (!impl_->error.empty())
    {
        auto const message = impl_->error;
        lock.unlock();
        stop();
        throw std::runtime_error(message);
    }
    if (!impl_->ready)
    {
        lock.unlock();
        stop();
        throw std::runtime_error("NMOS node did not become ready");
    }
}

void NmosNode::stop()
{
    impl_->running.store(false);
    if (impl_->thread.joinable())
    {
        impl_->thread.join();
    }
}
#endif
} // namespace mwm
