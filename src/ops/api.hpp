#pragma once

#include "channel/book.hpp"
#include "config/store.hpp"
#include "ops/httpserver.hpp"

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

class Api
{
public:
    Api(std::shared_ptr<ConfigStore> store, std::shared_ptr<ChannelBook> book);

    void setInfo(ServiceInfo info);
    void setIndexPage(std::string html);
    void setMetrics(std::function<std::string()> metrics);
    void setNmosRegistered(std::function<bool()> probe);
    void setMediamtxReachable(std::function<bool()> probe);
    void setNmosSummary(std::function<std::string()> summary);

    HttpResponse handle(HttpRequest const& request);
    std::string eventsJson() const;

private:
    std::string channelsJson() const;
    std::string infoJson() const;

    std::shared_ptr<ConfigStore> store_;
    std::shared_ptr<ChannelBook> book_;
    ServiceInfo info_;
    std::string indexPage_;
    std::function<std::string()> metrics_;
    std::function<bool()> nmosRegistered_;
    std::function<bool()> mediamtx_;
    std::function<std::string()> nmosSummary_;
};
} // namespace mwm
