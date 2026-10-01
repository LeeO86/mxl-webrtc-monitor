#pragma once

#include <optional>
#include <string>
#include <vector>

namespace mwm
{
struct DomainInfo
{
    std::string path;
    std::string id;
    bool mirror = false;
};

// Direct children of root that contain domain_def.json. Identity is the id
// field. Mirror domains (x-mxl-fabrics-agent) are included like any other domain.
std::vector<DomainInfo> scanDomains(std::string const& root);
std::optional<DomainInfo> resolveDomain(std::string const& root, std::string const& id);
} // namespace mwm
