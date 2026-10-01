#include "domain/scan.hpp"

#include "util/jsonutil.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace mwm
{
std::vector<DomainInfo> scanDomains(std::string const& root)
{
    std::vector<DomainInfo> found;
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec))
    {
        return found;
    }
    for (auto const& entry : std::filesystem::directory_iterator(root))
    {
        std::error_code entryEc;
        if (!entry.is_directory(entryEc))
        {
            continue;
        }
        auto const def = entry.path() / "domain_def.json";
        if (!std::filesystem::is_regular_file(def, entryEc))
        {
            continue;
        }
        std::ifstream in(def);
        if (!in)
        {
            continue;
        }
        std::stringstream buffer;
        buffer << in.rdbuf();
        std::string err;
        auto const rootJson = json::parse(buffer.str(), &err);
        if (!err.empty() || !rootJson.is<picojson::object>())
        {
            continue;
        }
        auto const id = json::fieldString(rootJson, "id");
        if (id.empty())
        {
            continue;
        }
        DomainInfo info;
        info.path = entry.path().string();
        info.id = id;
        auto const& obj = rootJson.get<picojson::object>();
        auto const mirror = obj.find("x-mxl-fabrics-agent");
        info.mirror = mirror != obj.end() && mirror->second.is<picojson::object>();
        found.push_back(std::move(info));
    }
    return found;
}

std::optional<DomainInfo> resolveDomain(std::string const& root, std::string const& id)
{
    for (auto const& domain : scanDomains(root))
    {
        if (domain.id == id)
        {
            return domain;
        }
    }
    return std::nullopt;
}
} // namespace mwm
