#pragma once

#include "config/config.hpp"

#include <string>
#include <vector>

namespace mwm
{
std::string renderMediamtxConfig(Config const& cfg);
bool writeMediamtxConfig(Config const& cfg, std::string* error);
// The version in a MediaMTX `GET /v3/info` body (`"v1.21.1"` -> `"1.21.1"`), empty if absent.
std::string mediamtxVersionFromInfo(std::string const& body);

// The `ch<n>` paths of a MediaMTX `GET /v3/paths/list` body: whether the channel's stream is ready,
// its tracks, and its WebRTC and HLS readers. Other paths, and a body that is not a path list, give nothing.
struct MediamtxPath
{
    int channel = 0;
    bool ready = false;
    std::vector<std::string> tracks;
    int webrtc = 0;
    int hls = 0;
};
std::vector<MediamtxPath> mediamtxPathsFromList(std::string const& body);
} // namespace mwm
