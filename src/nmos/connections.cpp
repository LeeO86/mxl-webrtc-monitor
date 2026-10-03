#include "nmos/connections.hpp"

#include "util/jsonutil.hpp"
#include "util/logging.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace mwm
{
namespace
{
std::filesystem::path connectionsPath(Config const& cfg)
{
    return std::filesystem::path(cfg.state_dir) / "is05.json";
}

std::string fieldOrEmpty(picojson::value const& object, std::string const& key)
{
    if (!object.is<picojson::object>())
    {
        return {};
    }
    auto const& obj = object.get<picojson::object>();
    auto const it = obj.find(key);
    if (it == obj.end() || it->second.is<picojson::null>() || !it->second.is<std::string>())
    {
        return {};
    }
    return it->second.get<std::string>();
}

picojson::value legJson(LegRoute const& route)
{
    picojson::object out;
    out["master_enable"] = picojson::value(route.master_enable);
    out["mxl_domain_id"] = route.domain_id.empty() ? picojson::value() : picojson::value(route.domain_id);
    out["mxl_flow_id"] = route.flow_id.empty() ? picojson::value() : picojson::value(route.flow_id);
    out["sender_id"] = route.sender_id.empty() ? picojson::value() : picojson::value(route.sender_id);
    return picojson::value(out);
}

void applyLeg(ChannelBook& book, int index, LegKind kind, picojson::value const& value)
{
    if (!value.is<picojson::object>())
    {
        return;
    }
    auto const& obj = value.get<picojson::object>();
    auto const enableIt = obj.find("master_enable");
    bool enable = false;
    if (enableIt != obj.end() && enableIt->second.is<bool>())
    {
        enable = enableIt->second.get<bool>();
    }
    book.setRoute(index, kind, enable, fieldOrEmpty(value, "mxl_domain_id"), fieldOrEmpty(value, "mxl_flow_id"), fieldOrEmpty(value, "sender_id"));
}
} // namespace

void loadConnections(Config const& cfg, ChannelBook& book)
{
    auto const path = connectionsPath(cfg);
    std::ifstream in(path);
    if (!in)
    {
        return;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string err;
    auto const root = json::parse(buffer.str(), &err);
    if (!err.empty() || !root.is<picojson::object>())
    {
        log::warn("is05_state_ignored", {{"path", path.string()}, {"error", err.empty() ? "not a JSON object" : err}});
        return;
    }
    auto const& obj = root.get<picojson::object>();
    auto const channels = obj.find("channels");
    if (channels == obj.end() || !channels->second.is<picojson::array>())
    {
        log::warn("is05_state_ignored", {{"path", path.string()}, {"error", "channels array missing"}});
        return;
    }
    for (auto const& item : channels->second.get<picojson::array>())
    {
        if (!item.is<picojson::object>())
        {
            continue;
        }
        auto const& channel = item.get<picojson::object>();
        auto const indexIt = channel.find("index");
        if (indexIt == channel.end() || !indexIt->second.is<double>())
        {
            continue;
        }
        int const index = static_cast<int>(indexIt->second.get<double>());
        if (index < 1 || index > cfg.monitor_channels)
        {
            continue;
        }
        auto const video = channel.find("video");
        auto const audio = channel.find("audio");
        if (video != channel.end())
        {
            applyLeg(book, index, LegKind::Video, video->second);
        }
        if (audio != channel.end())
        {
            applyLeg(book, index, LegKind::Audio, audio->second);
        }
    }
    log::info("is05_state_loaded", {{"path", path.string()}});
}

void saveConnections(Config const& cfg, ChannelBook const& book)
{
    std::error_code ec;
    std::filesystem::create_directories(cfg.state_dir, ec);
    picojson::object root;
    root["version"] = picojson::value(1.0);
    picojson::array channels;
    for (auto const& view : book.snapshot())
    {
        picojson::object item;
        item["index"] = picojson::value(static_cast<double>(view.settings.index));
        item["video"] = legJson(view.video);
        item["audio"] = legJson(view.audio);
        channels.push_back(picojson::value(item));
    }
    root["channels"] = picojson::value(channels);
    auto const path = connectionsPath(cfg);
    auto const tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out)
        {
            throw ConfigError("cannot write " + tmp);
        }
        out << picojson::value(root).serialize();
        if (!out)
        {
            throw ConfigError("cannot write " + tmp);
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec)
    {
        throw ConfigError("cannot replace " + path.string() + ": " + ec.message());
    }
}
} // namespace mwm
