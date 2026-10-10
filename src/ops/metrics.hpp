#pragma once

#include "channel/book.hpp"
#include "config/config.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace mwm
{
class LatencyHistogram
{
public:
    void observe(double seconds);
    std::string render(std::string const& name, std::string const& labels) const;

private:
    mutable std::mutex mu_;
    static constexpr double kBounds[] = {0.001, 0.005, 0.01, 0.02, 0.05, 0.1, 0.25, 0.5, 1.0};
    std::uint64_t counts_[sizeof(kBounds) / sizeof(kBounds[0])]{};
    std::uint64_t count_ = 0;
    double sum_ = 0;
};

// previewMode: "own" or "shared" (the mode the process started in).
std::string renderMetrics(Config const& cfg, std::vector<ChannelView> const& channels, std::string const& mxlVersion, std::string const& gstVersion,
    std::string const& encoderAvailable, std::vector<LatencyHistogram const*> const& latency, std::vector<std::uint64_t> const& fallbacks,
    std::string const& previewMode);
} // namespace mwm
