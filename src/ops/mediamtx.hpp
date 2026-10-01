#pragma once

#include "config/config.hpp"

#include <string>

namespace mwm
{
std::string renderMediamtxConfig(Config const& cfg);
bool writeMediamtxConfig(Config const& cfg, std::string* error);
} // namespace mwm
