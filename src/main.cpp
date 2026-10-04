#include "channel/book.hpp"
#include "config/config.hpp"
#include "config/store.hpp"
#include "media/engine.hpp"
#include "nmos/connections.hpp"
#include "nmos/node.hpp"
#include "ops/api.hpp"
#include "ops/httpserver.hpp"
#include "ops/mediamtx.hpp"
#include "ops/metrics.hpp"
#include "util/httpclient.hpp"
#include "util/jsonutil.hpp"
#include "util/logging.hpp"
#include "version.hpp"

#include <mxl/mxl.h>

#include <gst/gst.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>
#include <unistd.h>

#if defined(MWM_HAS_UI)
#include "ops/webui_generated.hpp"
#endif

extern char** environ;

namespace
{
volatile std::sig_atomic_t gStop = 0;

void onSignal(int)
{
    gStop = 1;
}

void onAlarm(int)
{
    std::_Exit(143);
}

std::string mxlVersion()
{
    mxlVersionType version{};
    if (mxlGetVersion(&version) == MXL_STATUS_OK && version.full != nullptr)
    {
        return version.full;
    }
    return mwm::kMxlPin;
}

void refreshViewers(mwm::Config const& cfg, mwm::ChannelBook& book)
{
    auto const response = mwm::httpGet(cfg.mediamtx_api_url + "/v3/paths/list", 700);
    if (response.status != 200)
    {
        return;
    }
    std::string err;
    auto const root = mwm::json::parse(response.body, &err);
    if (!err.empty() || !root.is<picojson::object>())
    {
        return;
    }
    auto const& obj = root.get<picojson::object>();
    auto const items = obj.find("items");
    if (items == obj.end() || !items->second.is<picojson::array>())
    {
        return;
    }
    for (auto const& item : items->second.get<picojson::array>())
    {
        if (!item.is<picojson::object>())
        {
            continue;
        }
        auto const name = mwm::json::fieldString(item, "name");
        if (name.rfind("ch", 0) != 0)
        {
            continue;
        }
        int index = 0;
        try
        {
            index = std::stoi(name.substr(2));
        }
        catch (...)
        {
            continue;
        }
        int webrtc = 0;
        int hls = 0;
        auto const& itemObj = item.get<picojson::object>();
        auto const readers = itemObj.find("readers");
        if (readers != itemObj.end() && readers->second.is<picojson::array>())
        {
            for (auto const& reader : readers->second.get<picojson::array>())
            {
                auto type = mwm::json::fieldString(reader, "type");
                for (auto& c : type)
                {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (type.find("webrtc") != std::string::npos)
                {
                    ++webrtc;
                }
                else if (type.find("hls") != std::string::npos)
                {
                    ++hls;
                }
            }
        }
        book.setViewers(index, webrtc, hls);
    }
}

void refreshLabels(mwm::Config const& cfg, mwm::ChannelBook& book)
{
    if (cfg.nmos_registry_address.empty())
    {
        return;
    }
    auto const views = book.snapshot();
    for (auto const& view : views)
    {
        if (view.video.sender_id.empty())
        {
            continue;
        }
        auto const url = "http://" + cfg.queryHost() + ":" + std::to_string(cfg.queryPort()) + "/x-nmos/query/v1.3/senders/" + view.video.sender_id;
        auto const response = mwm::httpGet(url, 700);
        if (response.status != 200)
        {
            continue;
        }
        std::string err;
        auto const root = mwm::json::parse(response.body, &err);
        auto const label = mwm::json::fieldString(root, "label");
        if (!label.empty())
        {
            book.setSourceLabel(view.settings.index, label);
        }
    }
}
} // namespace

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    auto const env = mwm::environmentValues(environ);
    std::map<std::string, std::string> fileValues;
    auto const fileIt = env.find("MONITOR_CONFIG_FILE");
    try
    {
        if (fileIt != env.end() && !fileIt->second.empty())
        {
            fileValues = mwm::loadConfigFile(fileIt->second, nullptr);
            fileValues["MONITOR_CONFIG_FILE"] = fileIt->second;
        }
        std::map<std::string, mwm::ValueOrigin> origin;
        auto cfg = mwm::loadLayered(fileValues, env, {}, &origin);
        if (fileIt != env.end())
        {
            cfg.config_file = fileIt->second;
        }
        mwm::log::setLevel(cfg.log_level);
        std::error_code stateEc;
        std::filesystem::create_directories(cfg.state_dir, stateEc);
        if (stateEc)
        {
            mwm::log::error("state_dir_failed", {{"path", cfg.state_dir}, {"error", stateEc.message()}});
            return 75;
        }
        std::string configError;
        if (!mwm::writeMediamtxConfig(cfg, &configError))
        {
            mwm::log::error("mediamtx_config_failed", {{"error", configError}});
            return 75;
        }
        mwm::log::info("startup", {{"version", mwm::kVersion}, {"host_id", cfg.host_id}, {"channels", std::to_string(cfg.monitor_channels)}});
        auto store = std::make_shared<mwm::ConfigStore>(cfg, origin, fileValues);
        auto book = std::make_shared<mwm::ChannelBook>(cfg);
        mwm::loadConnections(cfg, *book);
        mwm::MediaHost media(cfg, *book);
        mwm::NmosNode nmos(cfg, *book);
        mwm::Api api(store, book);
        gst_init(nullptr, nullptr);
        mwm::ServiceInfo info;
        info.version = mwm::kVersion;
        info.mxl_version = mxlVersion();
        info.nmos_cpp = mwm::kNmosPin;
        info.gstreamer = gst_version_string();
        info.mediamtx = mwm::kMediamtxPin;
        info.encoder_available = media.encoderAvailable();
        api.setInfo(info);
#if defined(MWM_HAS_UI)
        api.setIndexPage(std::string(mwm::webui::indexHtml()));
#endif
        api.setMetrics([store, book, &media] {
            auto const current = store->get();
            return mwm::renderMetrics(current, book->snapshot(), mxlVersion(), gst_version_string(), media.encoderAvailable(), media.latency(), media.fallbacks());
        });
        api.setNmosRegistered([&] { return nmos.registered(); });
        api.setMediamtxReachable([cfg] {
            auto const response = mwm::httpGet(cfg.mediamtx_api_url + "/v3/paths/list", 500);
            return response.status == 200;
        });
        api.setMediamtxVersion([cfg] {
            auto const response = mwm::httpGet(cfg.mediamtx_api_url + "/v3/info", 500);
            return response.status == 200 ? mwm::mediamtxVersionFromInfo(response.body) : std::string{};
        });
        api.setNmosSummary([&] { return nmos.summary(); });
        mwm::HttpServer server;
        server.setHandler([&](mwm::HttpRequest const& request) { return api.handle(request); });
        if (!server.start(cfg.web_port))
        {
            mwm::log::error("http_bind_failed", {{"port", std::to_string(cfg.web_port)}});
            return 75;
        }
        media.start();
        try
        {
            nmos.start();
        }
        catch (std::exception const& ex)
        {
            mwm::log::error("nmos_start_failed", {{"error", ex.what()}});
            media.stop();
            server.stop();
            return 75;
        }
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);
        std::signal(SIGALRM, onAlarm);
        auto nextBroadcast = std::chrono::steady_clock::now();
        auto nextPoll = std::chrono::steady_clock::now();
        while (!gStop)
        {
            auto const now = std::chrono::steady_clock::now();
            if (now >= nextPoll)
            {
                auto const current = store->get();
                refreshViewers(current, *book);
                refreshLabels(current, *book);
                nextPoll = now + std::chrono::seconds(1);
            }
            if (now >= nextBroadcast)
            {
                server.broadcast(api.eventsJson());
                nextBroadcast = now + std::chrono::milliseconds(100);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ::alarm(static_cast<unsigned>(cfg.shutdown_timeout_s));
        media.stop();
        if (cfg.mxl_cleanup_on_exit)
        {
            mwm::log::info("mxl_cleanup_skipped", {{"reason", "monitor owns no output domain"}});
        }
        nmos.stop();
        server.stop();
        ::alarm(0);
        return 143;
    }
    catch (mwm::ConfigError const& ex)
    {
        std::cerr << "{\"level\":\"error\",\"event\":\"invalid_config\",\"error\":\"" << mwm::log::jsonEscape(ex.what()) << "\"}\n";
        return 78;
    }
    catch (std::exception const& ex)
    {
        std::cerr << "{\"level\":\"error\",\"event\":\"fatal\",\"error\":\"" << mwm::log::jsonEscape(ex.what()) << "\"}\n";
        return 75;
    }
}
