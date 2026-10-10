// Preview contract (PREVIEW_*, own or shared MediaMTX), widgets and the built-in MediaMTX's supervisor.
#include "doctest/doctest.h"

#include "channel/book.hpp"
#include "config/config.hpp"
#include "config/store.hpp"
#include "ops/api.hpp"
#include "ops/child.hpp"
#include "ops/mediamtx.hpp"
#include "ops/metrics.hpp"
#include "util/jsonutil.hpp"

#include <chrono>
#include <memory>
#include <thread>

namespace
{
std::map<std::string, std::string> base(std::map<std::string, std::string> values)
{
    values.emplace("HOST_ID", "host");
    values.emplace("MONITOR_PUBLIC_IP", "10.1.2.3");
    return values;
}

struct Fixture
{
    explicit Fixture(std::map<std::string, std::string> const& values)
        : cfg(mwm::parseConfig(base(values)))
        , store(std::make_shared<mwm::ConfigStore>(cfg, std::map<std::string, mwm::ValueOrigin>{}, std::map<std::string, std::string>{}))
        , book(std::make_shared<mwm::ChannelBook>(cfg))
        , api(store, book)
    {
    }

    mwm::HttpResponse get(std::string const& path, std::string const& query = {})
    {
        mwm::HttpRequest request;
        request.method = "GET";
        request.path = path;
        request.query = query;
        return api.handle(request);
    }

    picojson::value json(std::string const& path)
    {
        auto const response = get(path);
        REQUIRE(response.status == 200);
        std::string err;
        auto value = mwm::json::parse(response.body, &err);
        REQUIRE(err.empty());
        return value;
    }

    mwm::Config cfg;
    std::shared_ptr<mwm::ConfigStore> store;
    std::shared_ptr<mwm::ChannelBook> book;
    mwm::Api api;
};

std::string header(mwm::HttpResponse const& response, std::string const& name)
{
    for (auto const& [key, value] : response.headers)
    {
        if (key == name)
        {
            return value;
        }
    }
    return {};
}

template <typename Pred>
bool waitFor(Pred pred, int ms)
{
    auto const end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end)
    {
        if (pred())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}
} // namespace

TEST_CASE("own mode is the default: the built-in MediaMTX on localhost")
{
    auto const cfg = mwm::parseConfig(base({}));
    CHECK_FALSE(cfg.previewShared());
    CHECK(cfg.publishUrl() == "rtsp://127.0.0.1:8554");
    CHECK(cfg.preview_path_prefix == "mxl-webrtc-monitor");
    CHECK(cfg.streamPath(2) == "mxl-webrtc-monitor/ch2");
    CHECK(cfg.mediamtx_api_url == "http://127.0.0.1:9997");
    CHECK(cfg.widget_frame_ancestors == "'self'");

    auto const ports = mwm::parseConfig(base({{"MEDIAMTX_RTSP_PORT", "18554"}, {"MEDIAMTX_API_URL", ""}}));
    CHECK(ports.publishUrl() == "rtsp://127.0.0.1:18554");
    CHECK(ports.mediamtx_api_url == "http://127.0.0.1:9997");
    CHECK(mwm::renderMediamtxConfig(ports).find("rtspAddress: 127.0.0.1:18554\n") != std::string::npos);
    CHECK_THROWS_AS(mwm::parseConfig(base({{"MEDIAMTX_RTSP_PORT", "0"}})), mwm::ConfigError);
}

TEST_CASE("a publish URL selects shared mode")
{
    auto const cfg = mwm::parseConfig(base({{"PREVIEW_PUBLISH_URL", "rtsp://mxl-mediamtx.mxl-platform.svc:8554/"}, {"PREVIEW_PATH_PREFIX", "/test-all/mon/"}}));
    CHECK(cfg.previewShared());
    CHECK(cfg.publishUrl() == "rtsp://mxl-mediamtx.mxl-platform.svc:8554");
    CHECK(cfg.streamPath(1) == "test-all/mon/ch1");
    // No API unless one is set: the shared MediaMTX is not this process's.
    CHECK(cfg.mediamtx_api_url.empty());
    auto const withApi = mwm::parseConfig(base({{"PREVIEW_PUBLISH_URL", "rtsps://mtx.example"}, {"MEDIAMTX_API_URL", "http://mtx.example:9997"}}));
    CHECK(withApi.mediamtx_api_url == "http://mtx.example:9997");

    CHECK_THROWS_AS(mwm::parseConfig(base({{"PREVIEW_PUBLISH_URL", "http://mtx.example:8554"}})), mwm::ConfigError);
    CHECK_THROWS_AS(mwm::parseConfig(base({{"PREVIEW_PUBLISH_URL", "rtsp://mtx.example:8554/live"}})), mwm::ConfigError);
    CHECK_THROWS_AS(mwm::parseConfig(base({{"PREVIEW_PUBLISH_URL", "rtsp://user:pw@mtx.example"}})), mwm::ConfigError);
    for (auto const* bad : {"a//b", "a/../b", "a b", "mon/ch?1"})
    {
        CHECK_THROWS_AS(mwm::parseConfig(base({{"PREVIEW_PATH_PREFIX", bad}})), mwm::ConfigError);
    }
    CHECK(mwm::parseConfig(base({{"PREVIEW_PATH_PREFIX", ""}})).preview_path_prefix == "mxl-webrtc-monitor");
}

TEST_CASE("the 1.2.0 names are aliases; the new name wins")
{
    auto const old = mwm::parseConfig(base({{"MEDIAMTX_RTSP_URL", "rtsp://127.0.0.1:18554"}, {"MONITOR_WHEP_PUBLIC_URL", "https://whep.example"},
        {"MONITOR_HLS_PUBLIC_URL", "https://hls.example/"}}));
    CHECK(old.previewShared());
    CHECK(old.publishUrl() == "rtsp://127.0.0.1:18554");
    CHECK(old.preview_whep_url == "https://whep.example");
    CHECK(old.preview_hls_url == "https://hls.example");
    // The 1.2.0 sidecar still gets its config, on the address it was given.
    CHECK(mwm::renderMediamtxConfig(old).find("rtspAddress: 127.0.0.1:18554\n") != std::string::npos);

    auto const both = mwm::parseConfig(base({{"MEDIAMTX_RTSP_URL", "rtsp://old.example"}, {"PREVIEW_PUBLISH_URL", "rtsp://new.example"},
        {"MONITOR_WHEP_PUBLIC_URL", "https://old.example"}, {"PREVIEW_WHEP_URL", "https://new.example"}}));
    CHECK(both.publishUrl() == "rtsp://new.example");
    CHECK(both.preview_whep_url == "https://new.example");
    // An empty new name counts as unset.
    CHECK(mwm::parseConfig(base({{"MONITOR_HLS_PUBLIC_URL", "https://old.example"}, {"PREVIEW_HLS_URL", ""}})).preview_hls_url == "https://old.example");

    // Layers first resolve their aliases: the environment's old name beats the file's new one.
    std::map<std::string, mwm::ValueOrigin> origin;
    auto const layered = mwm::loadLayered({{"PREVIEW_WHEP_URL", "https://file.example"}}, base({{"MONITOR_WHEP_PUBLIC_URL", "https://env.example"}}), {}, &origin);
    CHECK(layered.preview_whep_url == "https://env.example");
    CHECK(origin.at("PREVIEW_WHEP_URL") == mwm::ValueOrigin::Env);

    // Settings, export and KEY=value carry only the new names.
    auto const map = mwm::configToMap(old);
    CHECK(map.at("PREVIEW_PUBLISH_URL") == "rtsp://127.0.0.1:18554");
    CHECK(map.count("MEDIAMTX_RTSP_URL") == 0);
    CHECK(map.count("MONITOR_WHEP_PUBLIC_URL") == 0);
    CHECK(map.count("MONITOR_HLS_PUBLIC_URL") == 0);
    // Own mode keeps PREVIEW_PUBLISH_URL empty in an export, so an import stays in own mode.
    CHECK(mwm::configToMap(mwm::parseConfig(base({}))).at("PREVIEW_PUBLISH_URL").empty());

    char const* envp[] = {"MEDIAMTX_RTSP_URL=rtsp://127.0.0.1:8554", "PREVIEW_PATH_PREFIX=a/b", nullptr};
    auto const env = mwm::environmentValues(envp);
    CHECK(env.at("MEDIAMTX_RTSP_URL") == "rtsp://127.0.0.1:8554");
    CHECK(env.at("PREVIEW_PATH_PREFIX") == "a/b");
}

TEST_CASE("status and metrics show the mode and each stream's publish state")
{
    Fixture shared({{"PREVIEW_PUBLISH_URL", "rtsp://mtx.example:8554"}, {"PREVIEW_PATH_PREFIX", "test-all/mon"}, {"MONITOR_CHANNELS", "2"},
        {"PREVIEW_WHEP_URL", "https://preview.example"}});
    shared.book->setPublish(1, "publishing", "");
    shared.book->setPublish(2, "error", "Could not open resource for reading and writing.");
    auto const status = shared.json("/statusz").get<picojson::object>().at("preview").get<picojson::object>();
    CHECK(status.at("mode").get<std::string>() == "shared");
    CHECK(status.at("publish_url").get<std::string>() == "rtsp://mtx.example:8554");
    CHECK(status.at("path_prefix").get<std::string>() == "test-all/mon");
    CHECK(status.count("mediamtx") == 0);
    auto const& streams = status.at("streams").get<picojson::array>();
    REQUIRE(streams.size() == 2);
    CHECK(streams[0].get<picojson::object>().at("path").get<std::string>() == "test-all/mon/ch1");
    CHECK(streams[0].get<picojson::object>().at("state").get<std::string>() == "publishing");
    CHECK(streams[1].get<picojson::object>().at("state").get<std::string>() == "error");
    CHECK(streams[1].get<picojson::object>().at("error").get<std::string>() == "Could not open resource for reading and writing.");

    auto const channel = shared.json("/api/v1/channels").get<picojson::object>().at("channels").get<picojson::array>().at(0).get<picojson::object>();
    CHECK(channel.at("preview").get<picojson::object>().at("state").get<std::string>() == "publishing");
    CHECK(channel.at("playback").get<picojson::object>().at("whep").get<std::string>() == "https://preview.example/test-all/mon/ch1/whep");

    auto const metrics = mwm::renderMetrics(shared.cfg, shared.book->snapshot(), "mxl", "gst", "x264", {}, {}, "shared");
    CHECK(metrics.find("mxl_webrtc_monitor_preview_mode{mode=\"shared\"} 1\n") != std::string::npos);
    CHECK(metrics.find("mxl_webrtc_monitor_preview_mode{mode=\"own\"} 0\n") != std::string::npos);
    CHECK(metrics.find("mxl_webrtc_monitor_preview_publish_state{channel=\"1\",state=\"publishing\"} 1\n") != std::string::npos);
    CHECK(metrics.find("mxl_webrtc_monitor_preview_publish_state{channel=\"2\",state=\"error\"} 1\n") != std::string::npos);
    CHECK(metrics.find("mxl_webrtc_monitor_preview_publish_state{channel=\"2\",state=\"connecting\"} 0\n") != std::string::npos);

    Fixture own({});
    own.api.setMediamtxProcess([] { return mwm::ProcessState{true, 2}; });
    auto const ownStatus = own.json("/statusz").get<picojson::object>().at("preview").get<picojson::object>();
    CHECK(ownStatus.at("mode").get<std::string>() == "own");
    CHECK(ownStatus.at("publish_url").get<std::string>() == "rtsp://127.0.0.1:8554");
    CHECK(ownStatus.at("mediamtx").get<picojson::object>().at("running").get<bool>());
    CHECK(ownStatus.at("mediamtx").get<picojson::object>().at("restarts").get<double>() == 2);
    CHECK(ownStatus.at("streams").get<picojson::array>().at(0).get<picojson::object>().at("state").get<std::string>() == "connecting");
}

TEST_CASE("widgets list")
{
    Fixture f({{"MONITOR_CHANNELS", "6"}});
    auto const list = f.json("/widgets").get<picojson::array>();
    REQUIRE(list.size() == 1);
    auto const& widget = list[0].get<picojson::object>();
    CHECK(widget.at("id").get<std::string>() == "channel");
    CHECK(!widget.at("title").get<std::string>().empty());
    CHECK(widget.at("version").get<std::string>() == "1.3.0");
    CHECK(widget.at("min_size").get<picojson::object>().at("w").get<double>() == 320);
    CHECK(widget.at("min_size").get<picojson::object>().at("h").get<double>() == 200);
    auto const& params = widget.at("params").get<picojson::object>();
    CHECK(params.at("type").get<std::string>() == "object");
    CHECK(params.at("required").get<picojson::array>().at(0).get<std::string>() == "ch");
    auto const& props = params.at("properties").get<picojson::object>();
    CHECK(props.at("ch").get<picojson::object>().at("type").get<std::string>() == "integer");
    CHECK(props.at("ch").get<picojson::object>().at("minimum").get<double>() == 1);
    CHECK(props.at("ch").get<picojson::object>().at("maximum").get<double>() == 6);
    CHECK(props.at("labels").get<picojson::object>().at("type").get<std::string>() == "boolean");
    CHECK(props.at("meters").get<picojson::object>().at("type").get<std::string>() == "boolean");
}

TEST_CASE("the widget page may be framed by WIDGET_FRAME_ANCESTORS only")
{
    Fixture f({{"MONITOR_CHANNELS", "4"}});
    f.api.setIndexPage("<!doctype html><div id=\"app\"></div>");
    auto const page = f.get("/widget/channel", "ch=2&labels=false&meters=1&theme=transparent");
    CHECK(page.status == 200);
    CHECK(page.contentType.rfind("text/html", 0) == 0);
    CHECK(page.body.find("<div id=\"app\">") != std::string::npos);
    CHECK(header(page, "Content-Security-Policy") == "frame-ancestors 'self'");
    CHECK(header(page, "X-Frame-Options").empty());
    // The rest of the app is unchanged: no frame policy there.
    CHECK(header(f.get("/"), "Content-Security-Policy").empty());

    CHECK(f.get("/widget/channel", "").status == 400);
    CHECK(f.get("/widget/channel", "ch=0").status == 400);
    CHECK(f.get("/widget/channel", "ch=5").status == 400);
    CHECK(f.get("/widget/channel", "ch=x").status == 400);
    CHECK(f.get("/widget/channel", "ch=1&meters=maybe").status == 400);
    CHECK(f.get("/widget/channel", "ch=1&theme=blue").status == 400);
    CHECK(f.get("/widget/channel", "ch=%34").status == 200);
    CHECK(f.get("/widget/other", "ch=1").status == 404);

    Fixture designer({{"WIDGET_FRAME_ANCESTORS", "'self' https://designer.small.mxl.ipla.media.int"}});
    CHECK(header(designer.get("/widget/channel", "ch=1"), "Content-Security-Policy") == "frame-ancestors 'self' https://designer.small.mxl.ipla.media.int");
    CHECK_THROWS_AS(mwm::parseConfig(base({{"WIDGET_FRAME_ANCESTORS", "'self'; script-src *"}})), mwm::ConfigError);
    CHECK_THROWS_AS(mwm::parseConfig(base({{"WIDGET_FRAME_ANCESTORS", "a\r\nX-Injected: 1"}})), mwm::ConfigError);
}

TEST_CASE("a supervised child is started again after it exits, and stopped")
{
    mwm::ChildProcess failing("test");
    failing.start({"/bin/sh", "-c", "exit 3"});
    CHECK(waitFor([&] { return failing.restarts() >= 1; }, 3000));
    failing.stop();
    CHECK_FALSE(failing.running());

    mwm::ChildProcess sleeper("test");
    sleeper.start({"sleep", "60"});
    REQUIRE(waitFor([&] { return sleeper.running(); }, 2000));
    auto const before = std::chrono::steady_clock::now();
    sleeper.stop();
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(2));
    CHECK_FALSE(sleeper.running());
    CHECK(sleeper.restarts() == 0);
}
