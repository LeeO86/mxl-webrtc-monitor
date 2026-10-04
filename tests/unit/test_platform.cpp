#include "doctest/doctest.h"

#include "channel/book.hpp"
#include "config/config.hpp"
#include "config/store.hpp"
#include "nmos/connections.hpp"
#include "ops/api.hpp"
#include "ops/mediamtx.hpp"
#include "util/jsonutil.hpp"

#include <filesystem>
#include <fstream>
#include <memory>

TEST_CASE("mediamtx version comes from its info api")
{
    CHECK(mwm::mediamtxVersionFromInfo(R"({"version":"v1.21.1","started":"2026-10-04T08:08:39Z"})") == "1.21.1");
    CHECK(mwm::mediamtxVersionFromInfo(R"({"version":"1.20.1"})") == "1.20.1");
    CHECK(mwm::mediamtxVersionFromInfo(R"({"started":"x"})").empty());
    CHECK(mwm::mediamtxVersionFromInfo("not json").empty());
}

TEST_CASE("announce addresses reject names and loopback")
{
    CHECK_THROWS_AS(mwm::parseConfig({{"MONITOR_PUBLIC_IP", "127.0.0.1"}}), mwm::ConfigError);
    CHECK_THROWS_AS(mwm::parseConfig({{"MONITOR_PUBLIC_IP", "0.0.0.0"}}), mwm::ConfigError);
    CHECK_THROWS_AS(mwm::parseConfig({{"MONITOR_PUBLIC_IP", "monitor.example"}}), mwm::ConfigError);
    CHECK_THROWS_AS(mwm::parseConfig({{"MONITOR_PUBLIC_IP", "10.1.2.3"}, {"NMOS_HOST_ADDRESS", "::1"}}), mwm::ConfigError);
    CHECK_THROWS_AS(mwm::parseConfig({{"MONITOR_PUBLIC_IP", "10.1.2.3"}, {"NMOS_HOST_ADDRESS", "::"}}), mwm::ConfigError);

    auto const aliased = mwm::parseConfig({{"HOST_ID", "host"}, {"MONITOR_PUBLIC_IP", "10.1.2.3"}});
    CHECK(aliased.monitor_public_ip == "10.1.2.3");
    CHECK(aliased.nmos_host_address == "10.1.2.3");

    auto const split = mwm::parseConfig({{"HOST_ID", "host"}, {"MONITOR_PUBLIC_IP", "10.1.2.3"}, {"NMOS_HOST_ADDRESS", "10.9.9.9"}});
    CHECK(split.monitor_public_ip == "10.1.2.3");
    CHECK(split.nmos_host_address == "10.9.9.9");
}

TEST_CASE("platform settings and query defaults")
{
    auto const cfg = mwm::parseConfig({
        {"HOST_ID", "host"},
        {"MONITOR_PUBLIC_IP", "10.1.2.3"},
        {"NMOS_REGISTRY_ADDRESS", "10.0.0.8"},
        {"NMOS_REGISTRY_PORT", "4000"},
        {"NMOS_LABEL", "sport-sa"},
        {"NMOS_TAGS", R"({"urn:x-srf:production":["sport-sa"],"urn:x-srf:function":["mv1"]})"},
        {"STATE_DIR", "/tmp/mwm-state/"},
        {"SHUTDOWN_TIMEOUT_S", "12"},
        {"MXL_CLEANUP_ON_EXIT", "true"},
    });
    CHECK(cfg.queryHost() == "10.0.0.8");
    CHECK(cfg.queryPort() == 4001);
    CHECK(cfg.nmos_query_port == 0);
    CHECK(mwm::nodeLabel(cfg) == "sport-sa");
    CHECK(mwm::deviceLabel(cfg) == "sport-sa WebRTC Monitor");
    CHECK(cfg.nmos_tags.at("urn:x-srf:function").at(0) == "mv1");
    CHECK(cfg.state_dir == "/tmp/mwm-state");
    CHECK(cfg.shutdown_timeout_s == 12);
    CHECK(cfg.mxl_cleanup_on_exit);
    CHECK(cfg.mediamtx_config_path == "/tmp/mwm-state/mediamtx.yml");
    CHECK(cfg.mediamtx_metrics_port == 0);
    CHECK(mwm::configToMap(cfg).at("NMOS_QUERY_PORT") == "4001");
    CHECK(mwm::configToMap(cfg).at("MXL_CLEANUP_ON_EXIT") == "true");

    auto const explicitQuery = mwm::parseConfig({
        {"MONITOR_PUBLIC_IP", "10.1.2.3"},
        {"NMOS_QUERY_ADDRESS", "10.0.0.9"},
        {"NMOS_QUERY_PORT", "4500"},
        {"MEDIAMTX_METRICS_PORT", "9100"},
        {"MEDIAMTX_CONFIG_PATH", "/tmp/custom.yml"},
        {"STATE_DIR", "/tmp/other"},
    });
    CHECK(explicitQuery.queryHost() == "10.0.0.9");
    CHECK(explicitQuery.queryPort() == 4500);
    CHECK(explicitQuery.mediamtx_metrics_port == 9100);
    CHECK(explicitQuery.mediamtx_config_path == "/tmp/custom.yml");
    auto const yml = mwm::renderMediamtxConfig(explicitQuery);
    CHECK(yml.find("metricsAddress: 127.0.0.1:9100") != std::string::npos);
    CHECK(yml.find("10.1.2.3") != std::string::npos);

    CHECK_THROWS_AS(mwm::parseConfig({{"MONITOR_PUBLIC_IP", "10.1.2.3"}, {"NMOS_TAGS", "[]"}}), mwm::ConfigError);
    CHECK_THROWS_AS(mwm::parseConfig({{"MONITOR_PUBLIC_IP", "10.1.2.3"}, {"STATE_DIR", "relative"}}), mwm::ConfigError);
    CHECK_THROWS_AS(mwm::parseConfig({{"MONITOR_PUBLIC_IP", "10.1.2.3"}, {"SHUTDOWN_TIMEOUT_S", "0"}}), mwm::ConfigError);
}

TEST_CASE("unknown environment variables are ignored")
{
    char const* envp[] = {"NMOS_LABEL=Cam", "MXL_OUTPUT_DOMAIN_DIR=/Volumes/mxl/out", "MXL_OUTPUT_DOMAIN_ID=abc", "NOT_OURS=1", nullptr};
    auto const values = mwm::environmentValues(envp);
    CHECK(values.at("NMOS_LABEL") == "Cam");
    CHECK(values.count("MXL_OUTPUT_DOMAIN_DIR") == 0);
    CHECK(values.count("MXL_OUTPUT_DOMAIN_ID") == 0);
    CHECK(values.count("NOT_OURS") == 0);
    auto const cfg = mwm::parseConfig(values);
    CHECK(cfg.nmos_label == "Cam");
}

TEST_CASE("config export and import")
{
    auto const path = std::string("/tmp/mwm-platform-import.json");
    std::filesystem::remove(path);
    auto cfg = mwm::parseConfig({{"HOST_ID", "host"}, {"MONITOR_PUBLIC_IP", "10.1.2.3"}, {"MONITOR_CONFIG_FILE", path}, {"MONITOR_CHANNELS", "1"},
        {"CH1_VIDEO_LABEL", "PGM"}});
    std::string err;
    auto const exported = mwm::json::parse(mwm::exportConfigDocument(cfg), &err);
    REQUIRE(err.empty());
    auto const& root = exported.get<picojson::object>();
    CHECK(root.at("secrets_included").get<bool>() == false);
    CHECK(static_cast<int>(root.at("version").get<double>()) == 1);
    CHECK(root.at("settings").get<picojson::object>().at("CH1_VIDEO_LABEL").get<std::string>() == "PGM");
    CHECK(root.at("settings").get<picojson::object>().at("NMOS_HOST_ADDRESS").get<std::string>() == "10.1.2.3");

    std::map<std::string, mwm::ValueOrigin> origin;
    origin["WEB_PORT"] = mwm::ValueOrigin::Env;
    auto store = std::make_shared<mwm::ConfigStore>(cfg, origin, std::map<std::string, std::string>{});
    auto book = std::make_shared<mwm::ChannelBook>(cfg);
    mwm::Api api(store, book);
    mwm::HttpRequest request;
    request.method = "POST";
    request.path = "/api/v1/config/import";
    request.body = R"({"version":1,"secrets_included":false,"settings":{"WEB_PORT":"9000","CH1_VIDEO_LABEL":"Imported"},"channels":[{"index":1,"audio_label":"Desk"}]})";
    auto const response = api.handle(request);
    CHECK(response.status == 200);
    auto const updated = store->get();
    CHECK(updated.web_port == 8100);
    CHECK(updated.channels[0].video_label == "Imported");
    CHECK(updated.channels[0].audio_label == "Desk");
    CHECK(std::filesystem::is_regular_file(path));

    request.body = R"({"version":2,"settings":{}})";
    CHECK(api.handle(request).status == 400);

    auto const bare = mwm::parseConfig({{"HOST_ID", "host"}, {"MONITOR_PUBLIC_IP", "10.1.2.3"}});
    mwm::Api bareApi(std::make_shared<mwm::ConfigStore>(bare, std::map<std::string, mwm::ValueOrigin>{}, std::map<std::string, std::string>{}),
        std::make_shared<mwm::ChannelBook>(bare));
    request.body = R"({"version":1,"settings":{"LOG_LEVEL":"debug"}})";
    CHECK(bareApi.handle(request).status == 409);

    mwm::HttpRequest get;
    get.method = "GET";
    get.path = "/api/v1/config/export";
    auto const exportedResponse = api.handle(get);
    CHECK(exportedResponse.status == 200);
    CHECK(exportedResponse.body.find("\"secrets_included\":false") != std::string::npos);
}

TEST_CASE("is05 connection state survives a restart")
{
    auto const cfg = mwm::parseConfig(
        {{"HOST_ID", "host"}, {"MONITOR_PUBLIC_IP", "10.1.2.3"}, {"STATE_DIR", "/tmp/mwm-is05-state"}, {"MONITOR_CHANNELS", "1"}});
    std::filesystem::remove_all(cfg.state_dir);
    mwm::ChannelBook book(cfg);
    book.setRoute(1, mwm::LegKind::Video, true, "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", "sender-1");
    book.setRoute(1, mwm::LegKind::Audio, false, "", "", "");
    mwm::saveConnections(cfg, book);

    mwm::ChannelBook loaded(cfg);
    mwm::loadConnections(cfg, loaded);
    auto const video = loaded.route(1, mwm::LegKind::Video);
    CHECK(video.master_enable);
    CHECK(video.domain_id == "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    CHECK(video.flow_id == "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    CHECK(video.sender_id == "sender-1");
    CHECK(loaded.route(1, mwm::LegKind::Audio).master_enable == false);

    {
        std::ofstream out(cfg.state_dir + "/is05.json", std::ios::trunc);
        out << "{not json";
    }
    mwm::ChannelBook ignored(cfg);
    mwm::loadConnections(cfg, ignored);
    CHECK(ignored.route(1, mwm::LegKind::Video).master_enable == false);
}
