#pragma once

#include <optional>
#include <string>
#include <utility>

namespace mwm
{
std::string hostname();
std::string firstNonLoopbackIpv4();
std::optional<std::pair<std::string, int>> splitHostPort(std::string text);
} // namespace mwm
