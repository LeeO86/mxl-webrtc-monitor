#pragma once

#include "config/config.hpp"

#include <string>

namespace mwm
{
std::string renderMediamtxConfig(Config const& cfg);
bool writeMediamtxConfig(Config const& cfg, std::string* error);
// The version in a MediaMTX `GET /v3/info` body (`"v1.21.1"` -> `"1.21.1"`), empty if absent.
std::string mediamtxVersionFromInfo(std::string const& body);
} // namespace mwm
