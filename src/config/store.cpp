#include "config/store.hpp"

#include "util/jsonutil.hpp"
#include "util/logging.hpp"

#include <filesystem>
#include <fstream>

namespace mwm
{
ConfigStore::ConfigStore(Config cfg, std::map<std::string, ValueOrigin> origin, std::map<std::string, std::string> fileLayer)
    : cfg_(std::move(cfg))
    , origin_(std::move(origin))
    , file_(std::move(fileLayer))
{
    for (auto const& [key, where] : origin_)
    {
        if (where == ValueOrigin::Env)
        {
            auto const map = configToMap(cfg_);
            auto const it = map.find(key);
            if (it != map.end())
            {
                env_[key] = it->second;
            }
        }
    }
}

Config ConfigStore::get() const
{
    std::lock_guard const lock{mu_};
    return cfg_;
}

std::map<std::string, ValueOrigin> ConfigStore::origins() const
{
    std::lock_guard const lock{mu_};
    return origin_;
}

bool ConfigStore::restartRequired() const
{
    std::lock_guard const lock{mu_};
    return restart_;
}

std::string ConfigStore::configFile() const
{
    std::lock_guard const lock{mu_};
    return cfg_.config_file;
}

Config ConfigStore::updateFile(std::map<std::string, std::string> const& patch, bool* restart)
{
    std::lock_guard const lock{mu_};
    for (auto const& [key, value] : patch)
    {
        auto const it = origin_.find(key);
        if (it != origin_.end() && it->second == ValueOrigin::Env)
        {
            throw ConfigError(key + " is set by the environment and is read-only");
        }
        file_[key] = value;
        if (!isRuntimeKey(key))
        {
            restart_ = true;
        }
    }
    cfg_ = loadLayered(file_, env_, {}, &origin_);
    persistUnlocked();
    if (restart != nullptr)
    {
        *restart = restart_;
    }
    return cfg_;
}

void ConfigStore::replaceFile(std::map<std::string, std::string> const& fileLayer)
{
    std::lock_guard const lock{mu_};
    for (auto const& [key, value] : fileLayer)
    {
        (void)value;
        auto const it = origin_.find(key);
        if (it != origin_.end() && it->second == ValueOrigin::Env)
        {
            throw ConfigError(key + " is set by the environment and is read-only");
        }
        if (!isRuntimeKey(key))
        {
            restart_ = true;
        }
    }
    file_ = fileLayer;
    cfg_ = loadLayered(file_, env_, {}, &origin_);
    persistUnlocked();
}

void ConfigStore::persistUnlocked()
{
    if (cfg_.config_file.empty())
    {
        return;
    }
    std::filesystem::path const path(cfg_.config_file);
    if (path.has_parent_path())
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    picojson::object root;
    for (auto const& [key, value] : file_)
    {
        int index = 0;
        std::string field;
        if (isChannelKey(key, &index, &field))
        {
            continue;
        }
        root[key] = picojson::value(value);
    }
    picojson::array channels;
    for (auto const& channel : cfg_.channels)
    {
        bool any = false;
        for (auto const& [key, value] : file_)
        {
            (void)value;
            int index = 0;
            std::string field;
            if (isChannelKey(key, &index, &field) && index == channel.index)
            {
                any = true;
            }
        }
        if (!any)
        {
            continue;
        }
        picojson::object item;
        item["index"] = picojson::value(static_cast<double>(channel.index));
        item["video_label"] = picojson::value(channel.video_label);
        item["audio_label"] = picojson::value(channel.audio_label);
        item["preview_height"] = picojson::value(static_cast<double>(channel.preview_height));
        item["video_bitrate_kbps"] = picojson::value(static_cast<double>(channel.video_bitrate_kbps));
        item["audio_bitrate_kbps"] = picojson::value(static_cast<double>(channel.audio_bitrate_kbps));
        item["max_fps"] = picojson::value(static_cast<double>(channel.max_fps));
        item["audio_pair"] = picojson::value(static_cast<double>(channel.audio_pair));
        item["downmix"] = picojson::value(channel.downmix);
        item["overlay"] = picojson::value(channel.overlay);
        item["overlay_label"] = picojson::value(channel.overlay_label);
        item["overlay_source"] = picojson::value(channel.overlay_source);
        item["overlay_format"] = picojson::value(channel.overlay_format);
        channels.push_back(picojson::value(item));
    }
    if (!channels.empty())
    {
        root["channels"] = picojson::value(channels);
    }
    std::ofstream out(path, std::ios::trunc);
    if (!out)
    {
        throw ConfigError("cannot write config file " + cfg_.config_file);
    }
    out << picojson::value(root).serialize(true);
    log::info("config_saved", {{"path", cfg_.config_file}});
}
} // namespace mwm
