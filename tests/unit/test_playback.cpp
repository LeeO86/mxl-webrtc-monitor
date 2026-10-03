#include "doctest/doctest.h"

#include "channel/book.hpp"
#include "config/config.hpp"
#include "config/store.hpp"
#include "ops/api.hpp"
#include "util/jsonutil.hpp"

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
