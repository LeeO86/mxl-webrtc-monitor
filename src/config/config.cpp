#include "config/config.hpp"

#include "util/jsonutil.hpp"
#include "util/net.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace mwm
{
namespace
{
std::string lower(std::string text)
{
    for (auto& c : text)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

bool parseBool(std::string const& text, bool* out)
{
    auto const v = lower(text);
    if (v == "true" || v == "1" || v == "yes" || v == "on")
    {
        *out = true;
        return true;
    }
    if (v == "false" || v == "0" || v == "no" || v == "off")
    {
        *out = false;
        return true;
    }
    return false;
}

int parseInt(std::string const& key, std::string const& text, int min, int max)
{
    try
    {
        std::size_t used = 0;
        int const value = std::stoi(text, &used);
        if (used != text.size())
        {
            throw ConfigError(key + " is not an integer");
        }
        if (value < min || value > max)
        {
            throw ConfigError(key + " is outside " + std::to_string(min) + ".." + std::to_string(max));
        }
        return value;
    }
    catch (ConfigError const&)
    {
        throw;
    }
    catch (...)
    {
        throw ConfigError(key + " is not an integer");
    }
}

std::string valueOr(std::map<std::string, std::string> const& values, std::string const& key, std::string const& fallback)
{
    auto const it = values.find(key);
    if (it == values.end())
    {
        return fallback;
    }
    return it->second;
}

void requireKnown(std::map<std::string, std::string> const& values)
{
    auto const known = configKeys();
    for (auto const& [key, value] : values)
    {
        (void)value;
        if (key == "channels")
        {
            continue;
        }
        int index = 0;
        std::string field;
        if (isChannelKey(key, &index, &field))
        {
            continue;
        }
        if (std::find(known.begin(), known.end(), key) == known.end())
        {
            throw ConfigError("unknown configuration key " + key);
        }
    }
}
} // namespace

std::vector<std::string> configKeys()
{
    return {"HOST_ID", "MXL_DOMAIN_SCAN_PATH", "MONITOR_CHANNELS", "MONITOR_PREVIEW_HEIGHT", "MONITOR_MAX_FPS", "MONITOR_VIDEO_BITRATE_KBPS",
        "MONITOR_AUDIO_BITRATE_KBPS", "READ_OFFSET_GRAINS", "ENCODER", "MONITOR_PUBLIC_IP", "NMOS_HOST_ADDRESS", "MONITOR_WHEP_PUBLIC_URL",
        "MONITOR_HLS_PUBLIC_URL", "STATE_DIR", "SHUTDOWN_TIMEOUT_S", "MXL_CLEANUP_ON_EXIT", "NMOS_LABEL", "NMOS_TAGS", "NMOS_QUERY_ADDRESS",
        "NMOS_QUERY_PORT", "MEDIAMTX_METRICS_PORT", "MEDIAMTX_RTSP_URL", "MEDIAMTX_API_URL", "MEDIAMTX_CONFIG_PATH", "MEDIAMTX_WHEP_PORT",
        "MEDIAMTX_HLS_PORT", "MEDIAMTX_ICE_UDP_PORT", "NMOS_ENABLE", "NMOS_REGISTRY_ADDRESS", "NMOS_REGISTRY_PORT", "NMOS_DNS_SD", "NMOS_PORT",
        "NMOS_SEED", "WEB_PORT", "LOG_LEVEL", "METRICS_AUDIO_PEAK", "MONITOR_CONFIG_FILE"};
}

std::string normalizePublicBaseUrl(std::string const& key, std::string const& value)
{
    if (value.empty())
    {
        return {};
    }
    auto const schemeSep = value.find("://");
    if (schemeSep == std::string::npos || schemeSep == 0)
    {
        throw ConfigError(key + " must be an absolute http or https URL");
    }
    auto scheme = lower(value.substr(0, schemeSep));
    if (scheme != "http" && scheme != "https")
    {
        throw ConfigError(key + " must be an absolute http or https URL");
    }
    auto rest = value.substr(schemeSep + 3);
    if (rest.empty() || rest.find('@') != std::string::npos || rest.find(' ') != std::string::npos || rest.find('#') != std::string::npos ||
        rest.find('?') != std::string::npos)
    {
        throw ConfigError(key + " must be an absolute http or https URL with a host and no path");
    }
    auto const slash = rest.find('/');
    auto authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    auto const path = slash == std::string::npos ? std::string{} : rest.substr(slash);
    if (path != "" && path != "/")
    {
        throw ConfigError(key + " must not include a path");
    }
    if (authority.empty())
    {
        throw ConfigError(key + " must include a host");
    }
    std::string host;
    std::string port;
    if (authority.front() == '[')
    {
        auto const end = authority.find(']');
        if (end == std::string::npos || end == 1)
        {
            throw ConfigError(key + " must include a host");
        }
        host = authority.substr(0, end + 1);
        if (end + 1 < authority.size())
        {
            if (authority[end + 1] != ':' || end + 2 >= authority.size())
            {
                throw ConfigError(key + " has an invalid port");
            }
            port = authority.substr(end + 2);
        }
    }
    else
    {
        auto const colon = authority.rfind(':');
        if (colon != std::string::npos)
        {
            if (authority.find(':') != colon || colon == 0 || colon + 1 >= authority.size())
            {
                throw ConfigError(key + " has an invalid port");
            }
            host = authority.substr(0, colon);
            port = authority.substr(colon + 1);
        }
        else
        {
            host = authority;
        }
    }
    if (host.empty() || host == "[" || host.back() == '.')
    {
        throw ConfigError(key + " must include a host");
    }
    if (!port.empty())
    {
        if (port.find_first_not_of("0123456789") != std::string::npos)
        {
            throw ConfigError(key + " has an invalid port");
        }
        try
        {
            auto const number = std::stoi(port);
            if (number < 1 || number > 65535)
            {
                throw ConfigError(key + " has an invalid port");
            }
        }
        catch (ConfigError const&)
        {
            throw;
        }
        catch (...)
        {
            throw ConfigError(key + " has an invalid port");
        }
    }
    std::string normalized = scheme + "://" + host;
    if (!port.empty())
    {
        normalized += ":";
        normalized += port;
    }
    return normalized;
}

namespace
{
bool ipv4LoopbackOrUnspecified(in_addr const& address)
{
    auto const value = ntohl(address.s_addr);
    if (value == 0)
    {
        return true;
    }
    return (value & 0xff000000u) == 0x7f000000u;
}

bool ipv6LoopbackOrUnspecified(in6_addr const& address)
{
    bool zero = true;
    for (int i = 0; i < 16; ++i)
    {
        if (address.s6_addr[i] != 0)
        {
            zero = false;
            break;
        }
    }
    if (zero)
    {
        return true;
    }
    bool loopback = address.s6_addr[15] == 1;
    for (int i = 0; i < 15; ++i)
    {
        if (address.s6_addr[i] != 0)
        {
            loopback = false;
            break;
        }
    }
    return loopback;
}

std::map<std::string, std::vector<std::string>> parseTags(std::string const& text)
{
    if (text.empty() || text == "{}")
    {
        return {};
    }
    std::string err;
    auto const root = json::parse(text, &err);
    if (!err.empty() || !root.is<picojson::object>())
    {
        throw ConfigError("NMOS_TAGS must be a JSON object of string arrays");
    }
    std::map<std::string, std::vector<std::string>> tags;
    for (auto const& [name, value] : root.get<picojson::object>())
    {
        if (name.empty() || !value.is<picojson::array>())
        {
            throw ConfigError("NMOS_TAGS must be a JSON object of string arrays");
        }
        std::vector<std::string> values;
        for (auto const& item : value.get<picojson::array>())
        {
            if (!item.is<std::string>())
            {
                throw ConfigError("NMOS_TAGS values must be strings");
            }
            values.push_back(item.get<std::string>());
        }
        tags[name] = std::move(values);
    }
    return tags;
}

std::string settingText(std::string const& key, picojson::value const& value)
{
    if (value.is<std::string>())
    {
        return value.get<std::string>();
    }
    if (value.is<bool>())
    {
        return value.get<bool>() ? "true" : "false";
    }
    if (value.is<double>())
    {
        return std::to_string(static_cast<long long>(value.get<double>()));
    }
    if (key == "NMOS_TAGS" && value.is<picojson::object>())
    {
        return value.serialize();
    }
    throw ConfigError("unsupported config value for " + key);
}

void appendChannelObject(picojson::object const& obj, std::map<std::string, std::string>& out)
{
    auto const indexIt = obj.find("index");
    if (indexIt == obj.end() || !indexIt->second.is<double>())
    {
        throw ConfigError("channel index is required");
    }
    int const index = static_cast<int>(indexIt->second.get<double>());
    if (index < 1 || index > 16)
    {
        throw ConfigError("channel index is outside 1..16");
    }
    ChannelSettings probe = defaultChannel(index, Config{});
    for (auto const& [field, fieldValue] : obj)
    {
        if (field == "index")
        {
            continue;
        }
        std::string mapped = field;
        for (auto& c : mapped)
        {
            if (c == '-')
            {
                c = '_';
            }
            else
            {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
        }
        auto const text = settingText(channelKey(index, mapped), fieldValue);
        applyChannelValue(probe, mapped, text);
        out[channelKey(index, mapped)] = text;
    }
}
} // namespace

void validateAnnounceAddress(std::string const& key, std::string const& value)
{
    if (value.empty())
    {
        throw ConfigError(key + " is empty");
    }
    in_addr ipv4{};
    in6_addr ipv6{};
    bool const v4 = ::inet_pton(AF_INET, value.c_str(), &ipv4) == 1;
    bool const v6 = !v4 && ::inet_pton(AF_INET6, value.c_str(), &ipv6) == 1;
    if (!v4 && !v6)
    {
        throw ConfigError(key + " must be an IP address literal");
    }
    if ((v4 && ipv4LoopbackOrUnspecified(ipv4)) || (v6 && ipv6LoopbackOrUnspecified(ipv6)))
    {
        throw ConfigError(key + " must not be a loopback or unspecified address");
    }
}

std::string nodeLabel(Config const& cfg)
{
    return cfg.nmos_label.empty() ? cfg.host_id : cfg.nmos_label;
}

std::string deviceLabel(Config const& cfg)
{
    return cfg.nmos_label.empty() ? std::string("MXL WebRTC Monitor") : cfg.nmos_label + " WebRTC Monitor";
}

std::string tagsToJson(std::map<std::string, std::vector<std::string>> const& tags)
{
    picojson::object root;
    for (auto const& [name, values] : tags)
    {
        picojson::array items;
        for (auto const& value : values)
        {
            items.push_back(picojson::value(value));
        }
        root[name] = picojson::value(items);
    }
    return picojson::value(root).serialize();
}

std::string exportConfigDocument(Config const& cfg)
{
    picojson::object settings;
    for (auto const& [key, value] : configToMap(cfg))
    {
        settings[key] = picojson::value(value);
    }
    picojson::object root;
    root["version"] = picojson::value(1.0);
    root["settings"] = picojson::value(settings);
    root["secrets_included"] = picojson::value(false);
    return picojson::value(root).serialize();
}

std::map<std::string, std::string> settingsFromImport(std::string const& document)
{
    std::string err;
    auto const root = json::parse(document, &err);
    if (!err.empty() || !root.is<picojson::object>())
    {
        throw ConfigError("config import is not a JSON object");
    }
    auto const& obj = root.get<picojson::object>();
    auto const version = obj.find("version");
    if (version != obj.end())
    {
        if (!version->second.is<double>() || static_cast<int>(version->second.get<double>()) != 1)
        {
            throw ConfigError("config import version must be 1");
        }
    }
    auto const settingsIt = obj.find("settings");
    if (settingsIt == obj.end() || !settingsIt->second.is<picojson::object>())
    {
        throw ConfigError("config import requires a settings object");
    }
    std::map<std::string, std::string> out;
    for (auto const& [key, value] : settingsIt->second.get<picojson::object>())
    {
        out[key] = settingText(key, value);
    }
    auto const channels = obj.find("channels");
    if (channels != obj.end())
    {
        if (!channels->second.is<picojson::array>())
        {
            throw ConfigError("channels entries must be objects");
        }
        for (auto const& item : channels->second.get<picojson::array>())
        {
            if (!item.is<picojson::object>())
            {
                throw ConfigError("channels entries must be objects");
            }
            appendChannelObject(item.get<picojson::object>(), out);
        }
    }
    return out;
}

bool isRuntimeKey(std::string const& key)
{
    int index = 0;
    std::string field;
    return isChannelKey(key, &index, &field);
}

ChannelSettings defaultChannel(int index, Config const& cfg)
{
    ChannelSettings channel;
    channel.index = index;
    channel.video_label = "Monitor " + std::to_string(index) + " Video";
    channel.audio_label = "Monitor " + std::to_string(index) + " Audio";
    channel.preview_height = cfg.monitor_preview_height;
    channel.video_bitrate_kbps = cfg.monitor_video_bitrate_kbps;
    channel.audio_bitrate_kbps = cfg.monitor_audio_bitrate_kbps;
    channel.max_fps = cfg.monitor_max_fps;
    return channel;
}

std::string channelKey(int index, std::string const& field)
{
    return "CH" + std::to_string(index) + "_" + field;
}

bool isChannelKey(std::string const& key, int* index, std::string* field)
{
    if (key.size() < 5 || key.rfind("CH", 0) != 0)
    {
        return false;
    }
    auto const underscore = key.find('_');
    if (underscore == std::string::npos || underscore < 3)
    {
        return false;
    }
    try
    {
        std::size_t used = 0;
        int const n = std::stoi(key.substr(2, underscore - 2), &used);
        if (used != underscore - 2 || n < 1 || n > 16)
        {
            return false;
        }
        auto const rest = key.substr(underscore + 1);
        static std::vector<std::string> const fields = {"VIDEO_LABEL", "AUDIO_LABEL", "PREVIEW_HEIGHT", "VIDEO_BITRATE_KBPS", "AUDIO_BITRATE_KBPS",
            "MAX_FPS", "AUDIO_PAIR", "DOWNMIX", "OVERLAY", "OVERLAY_LABEL", "OVERLAY_SOURCE", "OVERLAY_FORMAT"};
        if (std::find(fields.begin(), fields.end(), rest) == fields.end())
        {
            return false;
        }
        if (index != nullptr)
        {
            *index = n;
        }
        if (field != nullptr)
        {
            *field = rest;
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void applyChannelValue(ChannelSettings& channel, std::string const& field, std::string const& value)
{
    auto const key = channelKey(channel.index, field);
    if (field == "VIDEO_LABEL")
    {
        if (value.empty())
        {
            throw ConfigError(key + " is empty");
        }
        channel.video_label = value;
    }
    else if (field == "AUDIO_LABEL")
    {
        if (value.empty())
        {
            throw ConfigError(key + " is empty");
        }
        channel.audio_label = value;
    }
    else if (field == "PREVIEW_HEIGHT")
    {
        channel.preview_height = parseInt(key, value, 64, 2160);
    }
    else if (field == "VIDEO_BITRATE_KBPS")
    {
        channel.video_bitrate_kbps = parseInt(key, value, 100, 50000);
    }
    else if (field == "AUDIO_BITRATE_KBPS")
    {
        channel.audio_bitrate_kbps = parseInt(key, value, 16, 512);
    }
    else if (field == "MAX_FPS")
    {
        channel.max_fps = parseInt(key, value, 0, 120);
    }
    else if (field == "AUDIO_PAIR")
    {
        channel.audio_pair = parseInt(key, value, 1, 32);
    }
    else if (field == "DOWNMIX")
    {
        auto const mode = lower(value);
        if (mode != "stereo" && mode != "mono")
        {
            throw ConfigError(key + " must be stereo or mono");
        }
        channel.downmix = mode;
    }
    else if (field == "OVERLAY" || field == "OVERLAY_LABEL" || field == "OVERLAY_SOURCE" || field == "OVERLAY_FORMAT")
    {
        bool flag = false;
        if (!parseBool(value, &flag))
        {
            throw ConfigError(key + " must be a boolean");
        }
        if (field == "OVERLAY")
        {
            channel.overlay = flag;
        }
        else if (field == "OVERLAY_LABEL")
        {
            channel.overlay_label = flag;
        }
        else if (field == "OVERLAY_SOURCE")
        {
            channel.overlay_source = flag;
        }
        else
        {
            channel.overlay_format = flag;
        }
    }
    else
    {
        throw ConfigError("unknown channel field " + field);
    }
}

Config parseConfig(std::map<std::string, std::string> const& values, std::vector<ChannelSettings> const& channelOverrides)
{
    requireKnown(values);
    Config cfg;
    cfg.host_id = valueOr(values, "HOST_ID", hostname());
    if (cfg.host_id.empty())
    {
        throw ConfigError("HOST_ID is empty");
    }
    cfg.mxl_domain_scan_path = valueOr(values, "MXL_DOMAIN_SCAN_PATH", cfg.mxl_domain_scan_path);
    if (cfg.mxl_domain_scan_path.empty() || cfg.mxl_domain_scan_path.front() != '/')
    {
        throw ConfigError("MXL_DOMAIN_SCAN_PATH must be an absolute path");
    }
    cfg.monitor_channels = parseInt("MONITOR_CHANNELS", valueOr(values, "MONITOR_CHANNELS", "4"), 1, 16);
    cfg.monitor_preview_height = parseInt("MONITOR_PREVIEW_HEIGHT", valueOr(values, "MONITOR_PREVIEW_HEIGHT", "540"), 64, 2160);
    cfg.monitor_max_fps = parseInt("MONITOR_MAX_FPS", valueOr(values, "MONITOR_MAX_FPS", "0"), 0, 120);
    cfg.monitor_video_bitrate_kbps = parseInt("MONITOR_VIDEO_BITRATE_KBPS", valueOr(values, "MONITOR_VIDEO_BITRATE_KBPS", "2000"), 100, 50000);
    cfg.monitor_audio_bitrate_kbps = parseInt("MONITOR_AUDIO_BITRATE_KBPS", valueOr(values, "MONITOR_AUDIO_BITRATE_KBPS", "128"), 16, 512);
    cfg.read_offset_grains = parseInt("READ_OFFSET_GRAINS", valueOr(values, "READ_OFFSET_GRAINS", "2"), 0, 30);
    cfg.encoder = lower(valueOr(values, "ENCODER", "auto"));
    if (cfg.encoder != "auto" && cfg.encoder != "nvenc" && cfg.encoder != "x264")
    {
        throw ConfigError("ENCODER must be auto, nvenc, or x264");
    }
    cfg.monitor_public_ip = valueOr(values, "MONITOR_PUBLIC_IP", "");
    if (cfg.monitor_public_ip.empty())
    {
        cfg.monitor_public_ip = firstNonLoopbackIpv4();
    }
    if (cfg.monitor_public_ip.empty())
    {
        throw ConfigError("MONITOR_PUBLIC_IP is unset and no non-loopback IPv4 address was found");
    }
    validateAnnounceAddress("MONITOR_PUBLIC_IP", cfg.monitor_public_ip);
    cfg.nmos_host_address = valueOr(values, "NMOS_HOST_ADDRESS", "");
    if (cfg.nmos_host_address.empty())
    {
        cfg.nmos_host_address = cfg.monitor_public_ip;
    }
    validateAnnounceAddress("NMOS_HOST_ADDRESS", cfg.nmos_host_address);
    cfg.monitor_whep_public_url = normalizePublicBaseUrl("MONITOR_WHEP_PUBLIC_URL", valueOr(values, "MONITOR_WHEP_PUBLIC_URL", ""));
    cfg.monitor_hls_public_url = normalizePublicBaseUrl("MONITOR_HLS_PUBLIC_URL", valueOr(values, "MONITOR_HLS_PUBLIC_URL", ""));
    cfg.state_dir = valueOr(values, "STATE_DIR", cfg.state_dir);
    while (cfg.state_dir.size() > 1 && cfg.state_dir.back() == '/')
    {
        cfg.state_dir.pop_back();
    }
    if (cfg.state_dir.empty() || cfg.state_dir.front() != '/')
    {
        throw ConfigError("STATE_DIR must be an absolute path");
    }
    cfg.shutdown_timeout_s = parseInt("SHUTDOWN_TIMEOUT_S", valueOr(values, "SHUTDOWN_TIMEOUT_S", "10"), 1, 120);
    if (!parseBool(valueOr(values, "MXL_CLEANUP_ON_EXIT", "false"), &cfg.mxl_cleanup_on_exit))
    {
        throw ConfigError("MXL_CLEANUP_ON_EXIT must be a boolean");
    }
    cfg.nmos_label = valueOr(values, "NMOS_LABEL", "");
    cfg.nmos_tags = parseTags(valueOr(values, "NMOS_TAGS", ""));
    cfg.mediamtx_rtsp_url = valueOr(values, "MEDIAMTX_RTSP_URL", cfg.mediamtx_rtsp_url);
    cfg.mediamtx_api_url = valueOr(values, "MEDIAMTX_API_URL", cfg.mediamtx_api_url);
    if (values.find("MEDIAMTX_CONFIG_PATH") == values.end())
    {
        cfg.mediamtx_config_path = cfg.state_dir + "/mediamtx.yml";
    }
    else
    {
        cfg.mediamtx_config_path = values.at("MEDIAMTX_CONFIG_PATH");
    }
    if (values.find("MEDIAMTX_METRICS_PORT") == values.end() || values.at("MEDIAMTX_METRICS_PORT").empty() || values.at("MEDIAMTX_METRICS_PORT") == "0")
    {
        cfg.mediamtx_metrics_port = 0;
    }
    else
    {
        cfg.mediamtx_metrics_port = parseInt("MEDIAMTX_METRICS_PORT", values.at("MEDIAMTX_METRICS_PORT"), 1, 65535);
    }
    cfg.mediamtx_whep_port = parseInt("MEDIAMTX_WHEP_PORT", valueOr(values, "MEDIAMTX_WHEP_PORT", "8889"), 1, 65535);
    cfg.mediamtx_hls_port = parseInt("MEDIAMTX_HLS_PORT", valueOr(values, "MEDIAMTX_HLS_PORT", "8888"), 1, 65535);
    cfg.mediamtx_ice_udp_port = parseInt("MEDIAMTX_ICE_UDP_PORT", valueOr(values, "MEDIAMTX_ICE_UDP_PORT", "8189"), 1, 65535);
    if (!parseBool(valueOr(values, "NMOS_ENABLE", "true"), &cfg.nmos_enable))
    {
        throw ConfigError("NMOS_ENABLE must be a boolean");
    }
    cfg.nmos_registry_address = valueOr(values, "NMOS_REGISTRY_ADDRESS", "");
    cfg.nmos_registry_port = parseInt("NMOS_REGISTRY_PORT", valueOr(values, "NMOS_REGISTRY_PORT", "3210"), 1, 65535);
    cfg.nmos_query_address = valueOr(values, "NMOS_QUERY_ADDRESS", "");
    if (values.find("NMOS_QUERY_PORT") == values.end() || values.at("NMOS_QUERY_PORT").empty())
    {
        cfg.nmos_query_port = 0;
    }
    else
    {
        cfg.nmos_query_port = parseInt("NMOS_QUERY_PORT", values.at("NMOS_QUERY_PORT"), 1, 65535);
    }
    if (!parseBool(valueOr(values, "NMOS_DNS_SD", "false"), &cfg.nmos_dns_sd))
    {
        throw ConfigError("NMOS_DNS_SD must be a boolean");
    }
    cfg.nmos_port = parseInt("NMOS_PORT", valueOr(values, "NMOS_PORT", "3242"), 1, 65534);
    cfg.nmos_seed = valueOr(values, "NMOS_SEED", cfg.host_id + "-monitor");
    if (cfg.nmos_seed.empty())
    {
        throw ConfigError("NMOS_SEED is empty");
    }
    cfg.web_port = parseInt("WEB_PORT", valueOr(values, "WEB_PORT", "8100"), 1, 65535);
    cfg.log_level = lower(valueOr(values, "LOG_LEVEL", "info"));
    if (cfg.log_level != "error" && cfg.log_level != "warn" && cfg.log_level != "info" && cfg.log_level != "debug")
    {
        throw ConfigError("LOG_LEVEL must be error, warn, info, or debug");
    }
    if (!parseBool(valueOr(values, "METRICS_AUDIO_PEAK", "false"), &cfg.metrics_audio_peak))
    {
        throw ConfigError("METRICS_AUDIO_PEAK must be a boolean");
    }
    cfg.config_file = valueOr(values, "MONITOR_CONFIG_FILE", "");

    cfg.channels.clear();
    for (int i = 1; i <= cfg.monitor_channels; ++i)
    {
        cfg.channels.push_back(defaultChannel(i, cfg));
    }
    auto apply = [&](int index, std::string const& field, std::string const& value) {
        if (index < 1 || index > cfg.monitor_channels)
        {
            return;
        }
        applyChannelValue(cfg.channels[static_cast<std::size_t>(index - 1)], field, value);
    };
    for (auto const& overrideChannel : channelOverrides)
    {
        if (overrideChannel.index < 1 || overrideChannel.index > cfg.monitor_channels)
        {
            continue;
        }
        cfg.channels[static_cast<std::size_t>(overrideChannel.index - 1)] = overrideChannel;
    }
    for (auto const& [key, value] : values)
    {
        int index = 0;
        std::string field;
        if (isChannelKey(key, &index, &field))
        {
            apply(index, field, value);
        }
    }
    return cfg;
}

Config loadLayered(std::map<std::string, std::string> const& fileValues, std::map<std::string, std::string> const& envValues,
    std::vector<ChannelSettings> const& channelOverrides, std::map<std::string, ValueOrigin>* origin)
{
    std::map<std::string, std::string> merged;
    if (origin != nullptr)
    {
        origin->clear();
    }
    auto take = [&](std::map<std::string, std::string> const& layer, ValueOrigin where) {
        for (auto const& [key, value] : layer)
        {
            merged[key] = value;
            if (origin != nullptr)
            {
                (*origin)[key] = where;
            }
        }
    };
    take(fileValues, ValueOrigin::File);
    take(envValues, ValueOrigin::Env);
    auto cfg = parseConfig(merged, channelOverrides);
    if (origin != nullptr)
    {
        for (auto const& key : configKeys())
        {
            origin->emplace(key, ValueOrigin::Default);
        }
    }
    return cfg;
}

std::map<std::string, std::string> configToMap(Config const& cfg)
{
    std::map<std::string, std::string> out;
    out["HOST_ID"] = cfg.host_id;
    out["MXL_DOMAIN_SCAN_PATH"] = cfg.mxl_domain_scan_path;
    out["MONITOR_CHANNELS"] = std::to_string(cfg.monitor_channels);
    out["MONITOR_PREVIEW_HEIGHT"] = std::to_string(cfg.monitor_preview_height);
    out["MONITOR_MAX_FPS"] = std::to_string(cfg.monitor_max_fps);
    out["MONITOR_VIDEO_BITRATE_KBPS"] = std::to_string(cfg.monitor_video_bitrate_kbps);
    out["MONITOR_AUDIO_BITRATE_KBPS"] = std::to_string(cfg.monitor_audio_bitrate_kbps);
    out["READ_OFFSET_GRAINS"] = std::to_string(cfg.read_offset_grains);
    out["ENCODER"] = cfg.encoder;
    out["MONITOR_PUBLIC_IP"] = cfg.monitor_public_ip;
    out["NMOS_HOST_ADDRESS"] = cfg.nmos_host_address;
    out["MONITOR_WHEP_PUBLIC_URL"] = cfg.monitor_whep_public_url;
    out["MONITOR_HLS_PUBLIC_URL"] = cfg.monitor_hls_public_url;
    out["STATE_DIR"] = cfg.state_dir;
    out["SHUTDOWN_TIMEOUT_S"] = std::to_string(cfg.shutdown_timeout_s);
    out["MXL_CLEANUP_ON_EXIT"] = cfg.mxl_cleanup_on_exit ? "true" : "false";
    out["NMOS_LABEL"] = cfg.nmos_label;
    out["NMOS_TAGS"] = tagsToJson(cfg.nmos_tags);
    out["NMOS_QUERY_ADDRESS"] = cfg.queryHost();
    out["NMOS_QUERY_PORT"] = std::to_string(cfg.queryPort());
    out["MEDIAMTX_METRICS_PORT"] = std::to_string(cfg.mediamtx_metrics_port);
    out["MEDIAMTX_RTSP_URL"] = cfg.mediamtx_rtsp_url;
    out["MEDIAMTX_API_URL"] = cfg.mediamtx_api_url;
    out["MEDIAMTX_CONFIG_PATH"] = cfg.mediamtx_config_path;
    out["MEDIAMTX_WHEP_PORT"] = std::to_string(cfg.mediamtx_whep_port);
    out["MEDIAMTX_HLS_PORT"] = std::to_string(cfg.mediamtx_hls_port);
    out["MEDIAMTX_ICE_UDP_PORT"] = std::to_string(cfg.mediamtx_ice_udp_port);
    out["NMOS_ENABLE"] = cfg.nmos_enable ? "true" : "false";
    out["NMOS_REGISTRY_ADDRESS"] = cfg.nmos_registry_address;
    out["NMOS_REGISTRY_PORT"] = std::to_string(cfg.nmos_registry_port);
    out["NMOS_DNS_SD"] = cfg.nmos_dns_sd ? "true" : "false";
    out["NMOS_PORT"] = std::to_string(cfg.nmos_port);
    out["NMOS_SEED"] = cfg.nmos_seed;
    out["WEB_PORT"] = std::to_string(cfg.web_port);
    out["LOG_LEVEL"] = cfg.log_level;
    out["METRICS_AUDIO_PEAK"] = cfg.metrics_audio_peak ? "true" : "false";
    out["MONITOR_CONFIG_FILE"] = cfg.config_file;
    for (auto const& channel : cfg.channels)
    {
        out[channelKey(channel.index, "VIDEO_LABEL")] = channel.video_label;
        out[channelKey(channel.index, "AUDIO_LABEL")] = channel.audio_label;
        out[channelKey(channel.index, "PREVIEW_HEIGHT")] = std::to_string(channel.preview_height);
        out[channelKey(channel.index, "VIDEO_BITRATE_KBPS")] = std::to_string(channel.video_bitrate_kbps);
        out[channelKey(channel.index, "AUDIO_BITRATE_KBPS")] = std::to_string(channel.audio_bitrate_kbps);
        out[channelKey(channel.index, "MAX_FPS")] = std::to_string(channel.max_fps);
        out[channelKey(channel.index, "AUDIO_PAIR")] = std::to_string(channel.audio_pair);
        out[channelKey(channel.index, "DOWNMIX")] = channel.downmix;
        out[channelKey(channel.index, "OVERLAY")] = channel.overlay ? "true" : "false";
        out[channelKey(channel.index, "OVERLAY_LABEL")] = channel.overlay_label ? "true" : "false";
        out[channelKey(channel.index, "OVERLAY_SOURCE")] = channel.overlay_source ? "true" : "false";
        out[channelKey(channel.index, "OVERLAY_FORMAT")] = channel.overlay_format ? "true" : "false";
    }
    return out;
}

std::string configToEnv(Config const& cfg)
{
    std::string out;
    for (auto const& [key, value] : configToMap(cfg))
    {
        out += key;
        out += "=";
        out += value;
        out += "\n";
    }
    return out;
}

std::string configToJson(Config const& cfg, std::map<std::string, ValueOrigin> const& origin, bool restartRequired)
{
    picojson::object root;
    picojson::object values;
    picojson::object origins;
    for (auto const& [key, value] : configToMap(cfg))
    {
        values[key] = picojson::value(value);
        auto const it = origin.find(key);
        char const* name = "default";
        if (it != origin.end())
        {
            name = it->second == ValueOrigin::Env ? "env" : it->second == ValueOrigin::File ? "file" : "default";
        }
        origins[key] = picojson::value(std::string(name));
    }
    root["values"] = picojson::value(values);
    root["origin"] = picojson::value(origins);
    root["restart_required"] = picojson::value(restartRequired);
    picojson::array channels;
    for (auto const& channel : cfg.channels)
    {
        picojson::object item;
        item["index"] = picojson::value(static_cast<double>(channel.index));
        item["video_label"] = picojson::value(channel.video_label);
        item["audio_label"] = picojson::value(channel.audio_label);
        item["preview_height"] = picojson::value(static_cast<double>(channel.preview_height));
        item["video_bitrate_kbps"] = picojson::value(static_cast<double>(channel.video_bitrate_kbps));
        item["audio_bitrate_kbps"] = picojson::value(static_cast<double>(channel.audio_bitrate_kbps));
        item["max_fps"] = picojson::value(static_cast<double>(channel.max_fps));
        item["audio_pair"] = picojson::value(static_cast<double>(channel.audio_pair));
        item["downmix"] = picojson::value(channel.downmix);
        item["overlay"] = picojson::value(channel.overlay);
        item["overlay_label"] = picojson::value(channel.overlay_label);
        item["overlay_source"] = picojson::value(channel.overlay_source);
        item["overlay_format"] = picojson::value(channel.overlay_format);
        channels.push_back(picojson::value(item));
    }
    root["channels"] = picojson::value(channels);
    return picojson::value(root).serialize();
}

std::map<std::string, std::string> environmentValues(char const* const* envp)
{
    std::map<std::string, std::string> out;
    if (envp == nullptr)
    {
        return out;
    }
    auto const known = configKeys();
    for (char const* const* it = envp; *it != nullptr; ++it)
    {
        std::string const entry = *it;
        auto const eq = entry.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }
        auto const key = entry.substr(0, eq);
        int index = 0;
        std::string field;
        bool const relevant = std::find(known.begin(), known.end(), key) != known.end() || isChannelKey(key, &index, &field);
        if (relevant)
        {
            out[key] = entry.substr(eq + 1);
        }
    }
    return out;
}

std::map<std::string, std::string> loadConfigFile(std::string const& path, std::vector<ChannelSettings>* channels)
{
    std::ifstream in(path);
    if (!in)
    {
        throw ConfigError("cannot read config file " + path);
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string err;
    auto const root = json::parse(buffer.str(), &err);
    if (!err.empty() || !root.is<picojson::object>())
    {
        throw ConfigError("config file is not a JSON object");
    }
    std::map<std::string, std::string> out;
    for (auto const& [key, value] : root.get<picojson::object>())
    {
        if (key == "channels" && value.is<picojson::array>())
        {
            if (channels == nullptr)
            {
                continue;
            }
            for (auto const& item : value.get<picojson::array>())
            {
                if (!item.is<picojson::object>())
                {
                    throw ConfigError("channels entries must be objects");
                }
                auto const& obj = item.get<picojson::object>();
                auto const indexIt = obj.find("index");
                if (indexIt == obj.end() || !indexIt->second.is<double>())
                {
                    throw ConfigError("channel index is required");
                }
                int const index = static_cast<int>(indexIt->second.get<double>());
                if (index < 1 || index > 16)
                {
                    throw ConfigError("channel index is outside 1..16");
                }
                ChannelSettings probe = defaultChannel(index, Config{});
                for (auto const& [field, fieldValue] : obj)
                {
                    if (field == "index")
                    {
                        continue;
                    }
                    std::string text;
                    if (fieldValue.is<std::string>())
                    {
                        text = fieldValue.get<std::string>();
                    }
                    else if (fieldValue.is<bool>())
                    {
                        text = fieldValue.get<bool>() ? "true" : "false";
                    }
                    else if (fieldValue.is<double>())
                    {
                        text = std::to_string(static_cast<long long>(fieldValue.get<double>()));
                    }
                    else
                    {
                        throw ConfigError("unsupported channel field " + field);
                    }
                    std::string mapped = field;
                    for (auto& c : mapped)
                    {
                        if (c == '-')
                        {
                            c = '_';
                        }
                        else
                        {
                            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                        }
                    }
                    applyChannelValue(probe, mapped, text);
                    out[channelKey(index, mapped)] = text;
                }
                if (channels != nullptr)
                {
                    channels->push_back(probe);
                }
            }
            continue;
        }
        out[key] = settingText(key, value);
    }
    return out;
}
} // namespace mwm
