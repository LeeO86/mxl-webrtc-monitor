#include "nmos/ids.hpp"

#include "util/uuid.hpp"

namespace mwm
{
std::string NmosIds::videoReceiver(int channel) const
{
    return uuidV5(kUuidNamespaceUrl, node + "/ch/" + std::to_string(channel) + "/video");
}

std::string NmosIds::audioReceiver(int channel) const
{
    return uuidV5(kUuidNamespaceUrl, node + "/ch/" + std::to_string(channel) + "/audio");
}

NmosIds makeNmosIds(std::string const& seed)
{
    NmosIds ids;
    ids.node = uuidV5(kUuidNamespaceUrl, "mxl-webrtc-monitor/" + seed + "/node");
    ids.device = uuidV5(kUuidNamespaceUrl, "mxl-webrtc-monitor/" + seed + "/device");
    return ids;
}
} // namespace mwm
