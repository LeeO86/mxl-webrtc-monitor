#include "doctest/doctest.h"

#include "channel/book.hpp"
#include "config/config.hpp"
#include "config/store.hpp"
#include "ops/api.hpp"
#include "util/jsonutil.hpp"

#include <filesystem>
#include <memory>

namespace
{
picojson::value channelsJson(std::map<std::string, std::string> const& values)
{
    auto const cfg = mwm::parseConfig(values);
    auto store = std::make_shared<mwm::ConfigStore>(cfg, std::map<std::string, mwm::ValueOrigin>{}, std::map<std::string, std::string>{});
    auto book = std::make_shared<mwm::ChannelBook>(cfg);
    mwm::Api api(store, book);
    mwm::HttpRequest request;
    request.method = "GET";
    request.path = "/api/v1/channels";
    auto const response = api.handle(request);
    REQUIRE(response.status == 200);
    std::string err;
    auto const root = mwm::json::parse(response.body, &err);
    REQUIRE(err.empty());
    return root;
}

picojson::object const& playbackOf(picojson::value const& root)
{
    return root.get<picojson::object>().at("channels").get<picojson::array>().at(0).get<picojson::object>().at("playback").get<picojson::object>();
}
} // namespace

TEST_CASE("public playback base urls")
{
    auto const unset = mwm::parseConfig({{"HOST_ID", "test"}, {"MONITOR_PUBLIC_IP", "10.1.2.3"}});
    CHECK(unset.monitor_whep_public_url.empty());
    CHECK(unset.monitor_hls_public_url.empty());

    auto const cfg = mwm::parseConfig({
        {"HOST_ID", "test"},
        {"MONITOR_WHEP_PUBLIC_URL", "https://mon1-whep.small.mxl.ipla.media.int/"},
        {"MONITOR_HLS_PUBLIC_URL", "http://127.0.0.1:18888"},
    });
    CHECK(cfg.monitor_whep_public_url == "https://mon1-whep.small.mxl.ipla.media.int");
    CHECK(cfg.monitor_hls_public_url == "http://127.0.0.1:18888");
    CHECK(mwm::configToMap(cfg).at("MONITOR_WHEP_PUBLIC_URL") == cfg.monitor_whep_public_url);
    CHECK(mwm::configToEnv(cfg).find("MONITOR_HLS_PUBLIC_URL=http://127.0.0.1:18888\n") != std::string::npos);

    CHECK(mwm::normalizePublicBaseUrl("MONITOR_HLS_PUBLIC_URL", "HTTP://hls.example:8443/") == "http://hls.example:8443");
    CHECK_THROWS_AS(mwm::parseConfig({{"MONITOR_WHEP_PUBLIC_URL", "mon1-whep.example"}}), mwm::ConfigError);
    CHECK_THROWS_AS(mwm::parseConfig({{"MONITOR_WHEP_PUBLIC_URL", "https://mon1-whep.example/whep"}}), mwm::ConfigError);
    CHECK_THROWS_AS(mwm::parseConfig({{"MONITOR_HLS_PUBLIC_URL", "ws://hls.example"}}), mwm::ConfigError);
    CHECK_THROWS_AS(mwm::parseConfig({{"MONITOR_HLS_PUBLIC_URL", "http://"}}), mwm::ConfigError);
    CHECK_THROWS_AS(mwm::parseConfig({{"MONITOR_HLS_PUBLIC_URL", "http://hls.example:99999"}}), mwm::ConfigError);
}

TEST_CASE("channel playback json")
{
    auto const direct = channelsJson({{"HOST_ID", "test"}, {"MONITOR_PUBLIC_IP", "10.1.2.3"}});
    auto const& plain = playbackOf(direct);
    CHECK(plain.at("whep").get<std::string>() == "http://10.1.2.3:8889/ch1/whep");
    CHECK(plain.at("hls").get<std::string>() == "http://10.1.2.3:8888/ch1/index.m3u8");
    CHECK(plain.at("public").get<picojson::object>().at("whep").get<bool>() == false);
    CHECK(plain.at("public").get<picojson::object>().at("hls").get<bool>() == false);

    auto const proxied = channelsJson({
        {"HOST_ID", "test"},
        {"MONITOR_PUBLIC_IP", "10.1.2.3"},
        {"MONITOR_WHEP_PUBLIC_URL", "https://mon1-whep.small.mxl.ipla.media.int"},
        {"MONITOR_HLS_PUBLIC_URL", "https://mon1-hls.small.mxl.ipla.media.int/"},
    });
    auto const& playback = playbackOf(proxied);
    CHECK(playback.at("whep").get<std::string>() == "https://mon1-whep.small.mxl.ipla.media.int/ch1/whep");
    CHECK(playback.at("hls").get<std::string>() == "https://mon1-hls.small.mxl.ipla.media.int/ch1/index.m3u8");
    CHECK(playback.at("public").get<picojson::object>().at("whep").get<bool>() == true);
    CHECK(playback.at("public").get<picojson::object>().at("hls").get<bool>() == true);
}

TEST_CASE("channel status has the overlay parts and the MediaMTX path, info the node label")
{
    auto const cfg = mwm::parseConfig({{"HOST_ID", "host"}, {"MONITOR_PUBLIC_IP", "10.1.2.3"}, {"NMOS_LABEL", "Desk"}, {"MONITOR_CHANNELS", "2"},
        {"CH1_OVERLAY_SOURCE", "false"}});
    auto store = std::make_shared<mwm::ConfigStore>(cfg, std::map<std::string, mwm::ValueOrigin>{}, std::map<std::string, std::string>{});
    auto book = std::make_shared<mwm::ChannelBook>(cfg);
    book->setMediamtxPath(1, true, {"Opus", "H264"});
    mwm::Api api(store, book);
    mwm::HttpRequest request;
    request.method = "GET";
    request.path = "/api/v1/channels";
    std::string err;
    auto const root = mwm::json::parse(api.handle(request).body, &err);
    REQUIRE(err.empty());
    auto const& channels = root.get<picojson::object>().at("channels").get<picojson::array>();
    auto const& first = channels.at(0).get<picojson::object>();
    CHECK(first.at("overlay_label").get<bool>() == true);
    CHECK(first.at("overlay_source").get<bool>() == false);
    CHECK(first.at("overlay_format").get<bool>() == true);
    auto const& mediamtx = first.at("mediamtx").get<picojson::object>();
    CHECK(mediamtx.at("ready").get<bool>() == true);
    REQUIRE(mediamtx.at("tracks").get<picojson::array>().size() == 2);
    CHECK(mediamtx.at("tracks").get<picojson::array>()[1].get<std::string>() == "H264");
    auto const& second = channels.at(1).get<picojson::object>().at("mediamtx").get<picojson::object>();
    CHECK(second.at("ready").get<bool>() == false);
    CHECK(second.at("tracks").get<picojson::array>().empty());

    request.path = "/api/v1/info";
    auto const info = mwm::json::parse(api.handle(request).body, &err);
    REQUIRE(err.empty());
    CHECK(info.get<picojson::object>().at("label").get<std::string>() == "Desk");
}

TEST_CASE("a rejected change leaves the settings as they were")
{
    auto const path = std::string("/tmp/mwm-rejected-change.json");
    std::filesystem::remove(path);
    auto const cfg = mwm::parseConfig({{"HOST_ID", "host"}, {"MONITOR_PUBLIC_IP", "10.1.2.3"}, {"MONITOR_CONFIG_FILE", path}, {"MONITOR_CHANNELS", "1"}});
    // As in a deployment: the environment names the configuration file and the channel count.
    std::map<std::string, mwm::ValueOrigin> origin{{"HOST_ID", mwm::ValueOrigin::Env}, {"MONITOR_PUBLIC_IP", mwm::ValueOrigin::Env},
        {"MONITOR_CONFIG_FILE", mwm::ValueOrigin::Env}, {"MONITOR_CHANNELS", mwm::ValueOrigin::Env}};
    auto store = std::make_shared<mwm::ConfigStore>(cfg, origin, std::map<std::string, std::string>{});
    auto book = std::make_shared<mwm::ChannelBook>(cfg);
    mwm::Api api(store, book);
    mwm::HttpRequest patch;
    patch.method = "PATCH";
    patch.path = "/api/v1/channels/1";
    patch.body = R"({"preview_height":10})";
    CHECK(api.handle(patch).status == 400);
    // 1.0.5 kept the invalid value in the file layer: every later change failed with the same error.
    patch.body = R"({"video_label":"PGM","overlay_format":false})";
    CHECK(api.handle(patch).status == 200);
    CHECK(store->get().channels[0].video_label == "PGM");
    CHECK(store->get().channels[0].overlay_format == false);
    CHECK(store->get().channels[0].preview_height == 540);
    CHECK_FALSE(store->restartRequired());

    mwm::HttpRequest put;
    put.method = "PUT";
    put.path = "/api/v1/config";
    put.body = R"({"ENCODER":"vp9","CH1_VIDEO_LABEL":"PGM"})";
    CHECK(api.handle(put).status == 400);
    CHECK_FALSE(store->restartRequired());
    CHECK(store->get().encoder == "auto");
    put.body = R"({"ENCODER":"x264","CH1_VIDEO_LABEL":"PGM"})";
    CHECK(api.handle(put).status == 200);
    CHECK(store->restartRequired());
    CHECK(store->origins().at("ENCODER") == mwm::ValueOrigin::File);
    CHECK(store->get().monitor_channels == 1);
    std::filesystem::remove(path);
}
