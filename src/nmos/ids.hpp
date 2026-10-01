#pragma once

#include <string>

namespace mwm
{
inline constexpr char kUuidNamespaceUrl[] = "6ba7b811-9dad-11d1-80b4-00c04fd430c8";

struct NmosIds
{
    std::string node;
    std::string device;
    std::string videoReceiver(int channel) const;
    std::string audioReceiver(int channel) const;
};

NmosIds makeNmosIds(std::string const& seed);
} // namespace mwm
