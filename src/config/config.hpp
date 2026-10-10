#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace mwm
{
struct ConfigError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct ChannelSettings
{
    int index = 1;
    std::string video_label;
    std::string audio_label;
    int preview_height = 540;
    int video_bitrate_kbps = 2000;
    int audio_bitrate_kbps = 128;
    int max_fps = 0;
    int audio_pair = 1;
    std::string downmix = "stereo";
    bool overlay = true;
    bool overlay_label = true;
    bool overlay_source = true;
    bool overlay_format = true;
    bool tally_text = false;
};

struct Config
{
    std::string host_id;
    std::string mxl_domain_scan_path = "/Volumes/mxl";
    int monitor_channels = 4;
    int monitor_preview_height = 540;
    int monitor_max_fps = 0;
    int monitor_video_bitrate_kbps = 2000;
    int monitor_audio_bitrate_kbps = 128;
    int read_offset_grains = 2;
    std::string encoder = "auto";
    std::string monitor_public_ip;
    std::string nmos_host_address;
    // Preview contract (PREVIEW_*): an empty publish URL runs the built-in MediaMTX ("own").
    std::string preview_publish_url;
    std::string preview_path_prefix = "mxl-webrtc-monitor";
    std::string preview_whep_url;
    std::string preview_hls_url;
    std::string widget_frame_ancestors = "'self'";
    std::string state_dir = "/config";
    int shutdown_timeout_s = 10;
    bool mxl_cleanup_on_exit = false;
    std::string nmos_label;
    std::map<std::string, std::vector<std::string>> nmos_tags;
    std::string nmos_query_address;
    int nmos_query_port = 0;
    int mediamtx_metrics_port = 0;
    int mediamtx_rtsp_port = 8554;
    // Default in own mode; empty in shared mode unless set (no API: no viewer counts).
    std::string mediamtx_api_url = "http://127.0.0.1:9997";
    std::string mediamtx_config_path = "/config/mediamtx.yml";
    int mediamtx_whep_port = 8889;
    int mediamtx_hls_port = 8888;
    int mediamtx_ice_udp_port = 8189;
    bool nmos_enable = true;
    std::string nmos_registry_address;
    int nmos_registry_port = 3210;
    bool nmos_dns_sd = false;
    int nmos_port = 3242;
    std::string nmos_seed;
    int web_port = 8100;
    std::string log_level = "info";
    bool metrics_audio_peak = false;
    bool tsl_enable = false;
    int tsl_udp_port = 8912;
    int tsl_tcp_port = 8913;
    int tsl_screen = -1;
    std::string tsl_map;
    std::string config_file;
    std::vector<ChannelSettings> channels;

    std::string queryHost() const
    {
        return nmos_query_address.empty() ? nmos_registry_address : nmos_query_address;
    }

    int queryPort() const
    {
        return nmos_query_port > 0 ? nmos_query_port : nmos_registry_port + 1;
    }

    // Shared mode: the channels publish to a MediaMTX this process does not start.
    bool previewShared() const
    {
        return !preview_publish_url.empty();
    }

    std::string publishUrl() const
    {
        return previewShared() ? preview_publish_url : "rtsp://127.0.0.1:" + std::to_string(mediamtx_rtsp_port);
    }

    // The MediaMTX path of a channel's stream: <prefix>/ch<n>.
    std::string streamPath(int channel) const
    {
        return preview_path_prefix + "/ch" + std::to_string(channel);
    }
};

std::string nodeLabel(Config const& cfg);
std::string deviceLabel(Config const& cfg);
std::string tagsToJson(std::map<std::string, std::vector<std::string>> const& tags);
std::string exportConfigDocument(Config const& cfg);
// Settings map from a config export document. Throws ConfigError on a bad document.
std::map<std::string, std::string> settingsFromImport(std::string const& document);

enum class ValueOrigin
{
    Default,
    File,
    Env,
};

Config parseConfig(std::map<std::string, std::string> const& values, std::vector<ChannelSettings> const& channelOverrides = {});
Config loadLayered(std::map<std::string, std::string> const& fileValues, std::map<std::string, std::string> const& envValues,
    std::vector<ChannelSettings> const& channelOverrides, std::map<std::string, ValueOrigin>* origin = nullptr);

bool isRuntimeKey(std::string const& key);
std::vector<std::string> configKeys();
std::map<std::string, std::string> configToMap(Config const& cfg);
std::string configToJson(Config const& cfg, std::map<std::string, ValueOrigin> const& origin, bool restartRequired);
std::string configToEnv(Config const& cfg);

// Empty is valid. Otherwise an absolute URL of one of the schemes with a host, an
// optional port, and no path other than "/". A trailing slash is removed.
std::string normalizeBaseUrl(std::string const& key, std::string const& value, std::vector<std::string> const& schemes);
// normalizeBaseUrl for http and https.
std::string normalizePublicBaseUrl(std::string const& key, std::string const& value);
// The 1.2.0 names (MEDIAMTX_RTSP_URL, MONITOR_WHEP_PUBLIC_URL, MONITOR_HLS_PUBLIC_URL) renamed to
// PREVIEW_*. In one layer, a set new name wins over its alias.
std::map<std::string, std::string> applyAliases(std::map<std::string, std::string> values);
void validateAnnounceAddress(std::string const& key, std::string const& value);

std::map<std::string, std::string> environmentValues(char const* const* envp);
std::map<std::string, std::string> loadConfigFile(std::string const& path, std::vector<ChannelSettings>* channels);

ChannelSettings defaultChannel(int index, Config const& cfg);
std::string channelKey(int index, std::string const& field);
bool isChannelKey(std::string const& key, int* index, std::string* field);
void applyChannelValue(ChannelSettings& channel, std::string const& field, std::string const& value);
} // namespace mwm
