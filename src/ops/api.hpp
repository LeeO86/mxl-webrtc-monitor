#pragma once

#include "channel/book.hpp"
#include "config/store.hpp"
#include "ops/httpserver.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace mwm
{
struct ServiceInfo
{
    std::string version;
    std::string mxl_version;
    std::string nmos_cpp;
    std::string gstreamer;
    std::string mediamtx;
    std::string encoder_available;
};

// The built-in MediaMTX (own mode).
struct ProcessState
{
    bool running = false;
    std::uint64_t restarts = 0;
};

class Api
{
public:
    Api(std::shared_ptr<ConfigStore> store, std::shared_ptr<ChannelBook> book);

    void setInfo(ServiceInfo info);
    void setIndexPage(std::string html);
    void setMetrics(std::function<std::string()> metrics);
    void setNmosRegistered(std::function<bool()> probe);
    void setMediamtxReachable(std::function<bool()> probe);
    void setMediamtxVersion(std::function<std::string()> probe);
    void setNmosSummary(std::function<std::string()> summary);
    void setMediamtxProcess(std::function<ProcessState()> probe);

    HttpResponse handle(HttpRequest const& request);
    std::string eventsJson() const;

private:
    std::string channelsJson() const;
    std::string infoJson() const;
    std::string statusJson() const;

    std::shared_ptr<ConfigStore> store_;
    std::shared_ptr<ChannelBook> book_;
    Config startup_; // the configuration the process started with (the preview mode, publish URL and prefix)
    ServiceInfo info_;
    std::string indexPage_;
    std::function<std::string()> metrics_;
    std::function<bool()> nmosRegistered_;
    std::function<bool()> mediamtx_;
    std::function<std::string()> mediamtxVersion_;
    std::function<std::string()> nmosSummary_;
    std::function<ProcessState()> mediamtxProcess_;
};
} // namespace mwm
