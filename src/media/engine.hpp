#pragma once

#include "channel/book.hpp"
#include "config/config.hpp"
#include "ops/metrics.hpp"

#include <memory>
#include <string>
#include <vector>

namespace mwm
{
class MediaHost
{
public:
    MediaHost(Config cfg, ChannelBook& book);
    ~MediaHost();

    MediaHost(MediaHost const&) = delete;
    MediaHost& operator=(MediaHost const&) = delete;

    void start();
    void stop();

    std::string encoderAvailable() const;
    std::vector<std::uint64_t> fallbacks() const;
    std::vector<LatencyHistogram const*> latency() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace mwm
