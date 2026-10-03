#pragma once

#include "channel/book.hpp"
#include "config/config.hpp"

namespace mwm
{
// Active IS-05 routes live in STATE_DIR/is05.json. A missing or unreadable file
// is ignored. A corrupt file is logged and ignored.
void loadConnections(Config const& cfg, ChannelBook& book);
void saveConnections(Config const& cfg, ChannelBook const& book);
} // namespace mwm
