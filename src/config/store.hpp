#pragma once

#include "config/config.hpp"

#include <mutex>
#include <string>

namespace mwm
{
class ConfigStore
{
public:
    ConfigStore(Config cfg, std::map<std::string, ValueOrigin> origin, std::map<std::string, std::string> fileLayer);

    Config get() const;
    std::map<std::string, ValueOrigin> origins() const;
    bool restartRequired() const;
    std::string configFile() const;

    // Rejects keys whose origin is the environment. Global keys set restart_required.
    Config updateFile(std::map<std::string, std::string> const& patch, bool* restart);
    void replaceFile(std::map<std::string, std::string> const& fileLayer);
    // Replaces the file layer. Keys whose origin is the environment are skipped.
    Config importDocument(std::map<std::string, std::string> const& settings);

private:
    void persistUnlocked();

    mutable std::mutex mu_;
    Config cfg_;
    std::map<std::string, ValueOrigin> origin_;
    std::map<std::string, std::string> file_;
    std::map<std::string, std::string> env_;
    bool restart_ = false;
};
} // namespace mwm
