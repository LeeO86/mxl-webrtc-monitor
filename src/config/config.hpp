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
    std::string monitor_whep_public_url;
    std::string monitor_hls_public_url;
    std::string mediamtx_rtsp_url = "rtsp://127.0.0.1:8554";
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
    std::string config_file;
    std::vector<ChannelSettings> channels;
};

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

// Empty is valid. Otherwise an absolute http or https URL with a host, an
// optional port, and no path other than "/". A trailing slash is removed.
std::string normalizePublicBaseUrl(std::string const& key, std::string const& value);

std::map<std::string, std::string> environmentValues(char const* const* envp);
std::map<std::string, std::string> loadConfigFile(std::string const& path, std::vector<ChannelSettings>* channels);

ChannelSettings defaultChannel(int index, Config const& cfg);
std::string channelKey(int index, std::string const& field);
bool isChannelKey(std::string const& key, int* index, std::string* field);
void applyChannelValue(ChannelSettings& channel, std::string const& field, std::string const& value);
} // namespace mwm
