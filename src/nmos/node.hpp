#pragma once

#include "channel/book.hpp"
#include "config/config.hpp"

#include <memory>
#include <string>

namespace mwm
{
class NmosNode
{
public:
    NmosNode(Config cfg, ChannelBook& book);
    ~NmosNode();

    NmosNode(NmosNode const&) = delete;
    NmosNode& operator=(NmosNode const&) = delete;

    void start();
    void stop();
    bool running() const;
    bool registered() const;
    std::string nodeId() const;
    std::string summary() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace mwm
