#include "ops/mediamtx.hpp"

#include "util/jsonutil.hpp"
#include "util/net.hpp"

#include <filesystem>
#include <fstream>

namespace mwm
{
std::string renderMediamtxConfig(Config const& cfg)
{
    auto api = splitHostPort(cfg.mediamtx_api_url);
    auto rtsp = splitHostPort(cfg.mediamtx_rtsp_url);
    std::string apiAddr = "127.0.0.1:9997";
    if (api && api->second > 0)
    {
        apiAddr = (api->first.empty() ? "127.0.0.1" : api->first) + ":" + std::to_string(api->second);
    }
    std::string rtspAddr = "127.0.0.1:8554";
    if (rtsp && rtsp->second > 0)
    {
        auto host = rtsp->first.empty() ? "127.0.0.1" : rtsp->first;
        if (host == "0.0.0.0")
        {
            host = "127.0.0.1";
        }
        rtspAddr = host + ":" + std::to_string(rtsp->second);
    }
    int metricsPort = cfg.mediamtx_metrics_port;
    if (metricsPort <= 0)
    {
        metricsPort = api && api->second > 0 ? api->second + 1 : 9998;
    }
    std::string yml;
    yml += "logLevel: info\n";
    yml += "api: true\n";
    yml += "apiAddress: " + apiAddr + "\n";
    yml += "apiAllowOrigins: [\"*\"]\n";
    yml += "metrics: true\n";
    yml += "metricsAddress: 127.0.0.1:" + std::to_string(metricsPort) + "\n";
    yml += "rtsp: true\n";
    yml += "rtspAddress: " + rtspAddr + "\n";
    yml += "rtmp: false\n";
    yml += "hls: true\n";
    yml += "hlsAddress: :" + std::to_string(cfg.mediamtx_hls_port) + "\n";
    yml += "hlsAllowOrigins: [\"*\"]\n";
    yml += "hlsVariant: lowLatency\n";
    yml += "hlsAlwaysRemux: true\n";
    yml += "hlsSegmentDuration: 1s\n";
    yml += "hlsPartDuration: 200ms\n";
    yml += "webrtc: true\n";
    yml += "webrtcAddress: :" + std::to_string(cfg.mediamtx_whep_port) + "\n";
    yml += "webrtcAllowOrigins: [\"*\"]\n";
    yml += "webrtcLocalUDPAddress: :" + std::to_string(cfg.mediamtx_ice_udp_port) + "\n";
    yml += "webrtcLocalTCPAddress: :" + std::to_string(cfg.mediamtx_ice_udp_port) + "\n";
    yml += "webrtcAdditionalHosts: [\"" + cfg.monitor_public_ip + "\"]\n";
    yml += "webrtcICEServers2: []\n";
    yml += "srt: false\n";
    yml += "moq: false\n";
    yml += "pathDefaults:\n";
    yml += "  source: publisher\n";
    yml += "paths:\n";
    yml += "  all_others:\n";
    return yml;
}

bool writeMediamtxConfig(Config const& cfg, std::string* error)
{
    try
    {
        std::filesystem::path const path(cfg.mediamtx_config_path);
        if (path.has_parent_path())
        {
            std::filesystem::create_directories(path.parent_path());
        }
        std::ofstream out(path, std::ios::trunc);
        if (!out)
        {
            if (error != nullptr)
            {
                *error = "cannot write " + cfg.mediamtx_config_path;
            }
            return false;
        }
        out << renderMediamtxConfig(cfg);
        return true;
    }
    catch (std::exception const& ex)
    {
        if (error != nullptr)
        {
            *error = ex.what();
        }
        return false;
    }
}

std::string mediamtxVersionFromInfo(std::string const& body)
{
    std::string err;
    auto const root = json::parse(body, &err);
    if (!err.empty() || !root.is<picojson::object>())
    {
        return {};
    }
    auto const& object = root.get<picojson::object>();
    auto const it = object.find("version");
    if (it == object.end() || !it->second.is<std::string>())
    {
        return {};
    }
    auto version = it->second.get<std::string>();
    if (!version.empty() && version.front() == 'v')
    {
        version.erase(0, 1);
    }
    return version;
}
} // namespace mwm
