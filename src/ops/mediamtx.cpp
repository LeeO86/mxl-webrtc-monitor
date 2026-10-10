#include "ops/mediamtx.hpp"

#include "util/jsonutil.hpp"
#include "util/net.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace mwm
{
std::string renderMediamtxConfig(Config const& cfg)
{
    auto api = splitHostPort(cfg.mediamtx_api_url);
    auto rtsp = splitHostPort(cfg.publishUrl());
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
    // The channels publish over TCP. Without UDP, MediaMTX binds no RTP/RTCP ports (8000/8001 for every
    // instance), so two monitors on one host need only the ports in the README.
    yml += "rtspTransports: [tcp]\n";
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
        auto const text = renderMediamtxConfig(cfg);
        {
            std::ifstream current(path, std::ios::binary);
            std::string const existing((std::istreambuf_iterator<char>(current)), std::istreambuf_iterator<char>());
            if (current && existing == text)
            {
                return true;
            }
        }
        // MediaMTX reloads when the file changes: write a temporary file and rename it over the
        // old one, so it never reads a half-written file (it fell back to its defaults: no API,
        // no paths).
        auto const temporary = path.string() + ".tmp";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                if (error != nullptr)
                {
                    *error = "cannot write " + cfg.mediamtx_config_path;
                }
                return false;
            }
            out << text;
            if (!out.flush())
            {
                if (error != nullptr)
                {
                    *error = "cannot write " + temporary;
                }
                return false;
            }
        }
        std::filesystem::rename(temporary, path);
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

std::vector<MediamtxPath> mediamtxPathsFromList(std::string const& body, std::string const& prefix)
{
    auto const head = prefix + "/ch";
    std::vector<MediamtxPath> out;
    std::string err;
    auto const root = json::parse(body, &err);
    if (!err.empty() || !root.is<picojson::object>())
    {
        return out;
    }
    auto const& obj = root.get<picojson::object>();
    auto const items = obj.find("items");
    if (items == obj.end() || !items->second.is<picojson::array>())
    {
        return out;
    }
    for (auto const& item : items->second.get<picojson::array>())
    {
        auto const name = json::fieldString(item, "name");
        if (name.size() <= head.size() || name.size() > head.size() + 2 || name.compare(0, head.size(), head) != 0 ||
            name.find_first_not_of("0123456789", head.size()) != std::string::npos)
        {
            continue;
        }
        MediamtxPath path;
        path.channel = std::stoi(name.substr(head.size()));
        auto const& itemObj = item.get<picojson::object>();
        auto const ready = itemObj.find("ready");
        path.ready = ready != itemObj.end() && ready->second.is<bool>() && ready->second.get<bool>();
        auto const tracks = itemObj.find("tracks");
        if (tracks != itemObj.end() && tracks->second.is<picojson::array>())
        {
            for (auto const& track : tracks->second.get<picojson::array>())
            {
                if (track.is<std::string>())
                {
                    path.tracks.push_back(track.get<std::string>());
                }
            }
        }
        auto const readers = itemObj.find("readers");
        if (readers != itemObj.end() && readers->second.is<picojson::array>())
        {
            for (auto const& reader : readers->second.get<picojson::array>())
            {
                auto type = json::fieldString(reader, "type");
                for (auto& c : type)
                {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (type.find("webrtc") != std::string::npos)
                {
                    ++path.webrtc;
                }
                else if (type.find("hls") != std::string::npos)
                {
                    ++path.hls;
                }
            }
        }
        out.push_back(std::move(path));
    }
    return out;
}
} // namespace mwm
