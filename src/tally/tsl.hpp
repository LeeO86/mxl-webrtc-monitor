#pragma once

#include "channel/book.hpp"
#include "config/config.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace mwm
{
// One TSL 5.0 display message. Tally values: 0 off, 1 red, 2 green, 3 amber.
struct TallyUpdate
{
    int screen = 0;
    int index = 0;
    int rh = 0;
    int text = 0;
    int lh = 0;
    int brightness = 0;
    std::string textValue; // UTF-8
};

struct TslMessage
{
    std::vector<TallyUpdate> displays;
    std::string error;
};

// Splits a byte stream into TSL packets: DLE/STX starts a packet, DLE DLE is one 0xFE byte, and a
// packet ends at DLE/ETX, at the next DLE/STX, or when it holds PBC + 2 bytes.
class TslFramer
{
public:
    std::vector<std::vector<std::uint8_t>> feed(std::uint8_t const* data, std::size_t size);

private:
    std::vector<std::uint8_t> buffer_;
    bool inPacket_ = false;
    bool dle_ = false;
};

// The packets of one UDP datagram: the bare packet, or the packets of a DLE/STX wrapped datagram.
std::vector<std::vector<std::uint8_t>> tslDatagram(std::uint8_t const* data, std::size_t size);
TslMessage parseTsl5(std::uint8_t const* body, std::size_t size);
// The border colour: text tally, else RH, else LH. The lamps show LH and RH themselves.
int effectiveTally(int lh, int rh, int text);
// TSL_MAP ("display:channel,..."); empty means display i is channel i + 1. 0 when the display is not mapped.
int channelForDisplay(std::string const& map, int display);
// Sets the tally of the channels the messages address (TSL_SCREEN, TSL_MAP).
void applyTally(Config const& cfg, TslMessage const& message, ChannelBook& book);

// TSL UMD 5.0 on TSL_UDP_PORT and TSL_TCP_PORT (DLE/STX framing).
class TslListener
{
public:
    ~TslListener();

    // Binds both ports and starts the receive thread. False when a port cannot be bound.
    bool start(Config const& cfg, ChannelBook& book);
    void stop();

private:
    void run();

    Config cfg_;
    ChannelBook* book_ = nullptr;
    int udp_ = -1;
    int tcp_ = -1;
    std::atomic<bool> running_{false};
    std::thread thread_;
};
} // namespace mwm
