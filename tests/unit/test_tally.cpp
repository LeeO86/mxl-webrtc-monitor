#include "doctest/doctest.h"

#include "channel/book.hpp"
#include "config/config.hpp"
#include "config/store.hpp"
#include "ops/api.hpp"
#include "tally/tsl.hpp"
#include "util/jsonutil.hpp"

#include <memory>

namespace
{
std::vector<std::uint8_t> hex(std::string const& text)
{
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2)
    {
        out.push_back(static_cast<std::uint8_t>(std::stoi(text.substr(i, 2), nullptr, 16)));
    }
    return out;
}

void put16(std::vector<std::uint8_t>& out, int value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

// PBC is the number of bytes after it.
void setPbc(std::vector<std::uint8_t>& body)
{
    body[0] = static_cast<std::uint8_t>((body.size() - 2) & 0xff);
    body[1] = static_cast<std::uint8_t>(((body.size() - 2) >> 8) & 0xff);
}

// Byte vectors from the platform's reference codec (mxl-poc-platform tsl5.py, a1883cb).
// Screen 0, UTF-16LE: display 0 LH red, text amber, RH green "Kamera Zürich"; display 1 RH red,
// brightness 1, U+1F3A5 " Cam 2"; display 254 LH and text green, brightness 2, "þ".
std::string const kUtf16 =
    "4200000100000000de001a004b0061006d0065007200610020005a00fc0072006900630068000100410010003cd8a5df2000430061006d0020003200fe00a8000200fe00";
// Screen 3, ASCII: display 2 LH amber "CAM 3"; display 3 text red, RH amber, brightness 0, "VT".
std::string const kAscii = "1700000003000200f000050043414d20330300070002005654";
// kUtf16 in DLE/STX ... DLE/ETX for TCP; its two 0xFE bytes are stuffed.
std::string const kUtf16Tcp =
    "fe024200000100000000de001a004b0061006d0065007200610020005a00fc0072006900630068000100410010003cd8a5df2000430061006d0020003200fefe00a8000200fefe00fe03";
// kAscii in DLE/STX without DLE/ETX.
std::string const kAsciiTcp = "fe021700000003000200f000050043414d20330300070002005654";
} // namespace

TEST_CASE("tsl 5.0 matches the reference codec: utf-16, several displays")
{
    auto const packet = hex(kUtf16);
    auto const message = mwm::parseTsl5(packet.data(), packet.size());
    CHECK(message.error.empty());
    REQUIRE(message.displays.size() == 3);
    auto const& first = message.displays[0];
    CHECK(first.screen == 0);
    CHECK(first.index == 0);
    CHECK(first.lh == 1);
    CHECK(first.text == 3);
    CHECK(first.rh == 2);
    CHECK(first.brightness == 3);
    CHECK(first.textValue == "Kamera Z\xC3\xBCrich");
    auto const& second = message.displays[1];
    CHECK(second.index == 1);
    CHECK((second.lh == 0 && second.text == 0 && second.rh == 1));
    CHECK(second.brightness == 1);
    CHECK(second.textValue == "\xF0\x9F\x8E\xA5 Cam 2");
    auto const& third = message.displays[2];
    CHECK(third.index == 254);
    CHECK((third.lh == 2 && third.text == 2 && third.rh == 0));
    CHECK(third.brightness == 2);
    CHECK(third.textValue == "\xC3\xBE");
}

TEST_CASE("tsl 5.0 matches the reference codec: ascii")
{
    auto const packet = hex(kAscii);
    auto const message = mwm::parseTsl5(packet.data(), packet.size());
    CHECK(message.error.empty());
    REQUIRE(message.displays.size() == 2);
    CHECK(message.displays[0].screen == 3);
    CHECK(message.displays[0].index == 2);
    CHECK((message.displays[0].lh == 3 && message.displays[0].text == 0 && message.displays[0].rh == 0));
    CHECK(message.displays[0].textValue == "CAM 3");
    CHECK(message.displays[1].index == 3);
    CHECK((message.displays[1].lh == 0 && message.displays[1].text == 1 && message.displays[1].rh == 3));
    CHECK(message.displays[1].brightness == 0);
    CHECK(message.displays[1].textValue == "VT");
}

TEST_CASE("tcp framing: dle stuffing, dle/etx optional, split reads")
{
    auto stream = hex(kUtf16Tcp);
    auto const ascii = hex(kAsciiTcp);
    stream.insert(stream.end(), ascii.begin(), ascii.end());
    stream.insert(stream.end(), ascii.begin(), ascii.end());
    // Every chunk size, so reads also split DLE pairs and the DLE/STX marks.
    for (std::size_t chunk = 1; chunk <= 8; ++chunk)
    {
        mwm::TslFramer framer;
        std::vector<std::vector<std::uint8_t>> packets;
        for (std::size_t i = 0; i < stream.size(); i += chunk)
        {
            for (auto& packet : framer.feed(stream.data() + i, std::min(chunk, stream.size() - i)))
            {
                packets.push_back(std::move(packet));
            }
        }
        REQUIRE(packets.size() == 3);
        CHECK(packets[0] == hex(kUtf16));
        CHECK(packets[1] == hex(kAscii));
        CHECK(packets[2] == hex(kAscii));
    }
    // Without DLE/ETX a packet is complete at PBC, not only when the next one starts.
    mwm::TslFramer framer;
    CHECK(framer.feed(ascii.data(), ascii.size()).size() == 1);

    // UDP: a bare packet, or one wrapped like TCP.
    auto const wrapped = hex(kUtf16Tcp);
    auto const fromWrapped = mwm::tslDatagram(wrapped.data(), wrapped.size());
    REQUIRE(fromWrapped.size() == 1);
    CHECK(fromWrapped[0] == hex(kUtf16));
    auto const bare = hex(kAscii);
    auto const fromBare = mwm::tslDatagram(bare.data(), bare.size());
    REQUIRE(fromBare.size() == 1);
    CHECK(fromBare[0] == bare);
}

TEST_CASE("tsl 5.0 skips control data and screen control, repairs text, rejects short packets")
{
    std::vector<std::uint8_t> body{0, 0, 0, 0x01};
    put16(body, 0);
    put16(body, 0);
    put16(body, 0x8000 | 1); // control data: skipped
    put16(body, 2);
    body.push_back('x');
    body.push_back('x');
    put16(body, 1);
    put16(body, 2);
    // "A", a lone high surrogate, "B", NUL.
    put16(body, 8);
    for (int unit : std::vector<int>{'A', 0xD800, 'B', 0})
    {
        put16(body, unit);
    }
    setPbc(body);
    auto message = mwm::parseTsl5(body.data(), body.size());
    CHECK(message.error.empty());
    REQUIRE(message.displays.size() == 1);
    CHECK(message.displays[0].index == 1);
    CHECK(message.displays[0].rh == 2);
    CHECK(message.displays[0].textValue == "A\xEF\xBF\xBD" "B");

    body[3] = 0x03; // screen control data
    CHECK(mwm::parseTsl5(body.data(), body.size()).displays.empty());

    // ASCII: a byte above 0x7F becomes U+FFFD, as the reference decodes it.
    std::vector<std::uint8_t> ascii{0, 0, 0, 0};
    put16(ascii, 0);
    put16(ascii, 0);
    put16(ascii, 1);
    put16(ascii, 3);
    ascii.push_back('C');
    ascii.push_back(0xFE);
    ascii.push_back(0);
    setPbc(ascii);
    message = mwm::parseTsl5(ascii.data(), ascii.size());
    REQUIRE(message.displays.size() == 1);
    CHECK(message.displays[0].textValue == "C\xEF\xBF\xBD");

    auto const truncated = hex(kAscii.substr(0, kAscii.size() - 4));
    CHECK(mwm::parseTsl5(truncated.data(), truncated.size()).error == "pbc");
    CHECK(mwm::parseTsl5(body.data(), 4).error == "short");
}

TEST_CASE("tally colours: border is text, else RH, else LH")
{
    CHECK(mwm::effectiveTally(0, 0, 0) == 0);
    CHECK(mwm::effectiveTally(1, 0, 0) == 1);
    CHECK(mwm::effectiveTally(1, 2, 0) == 2);
    CHECK(mwm::effectiveTally(1, 2, 3) == 3);
    CHECK(mwm::effectiveTally(0, 0, 3) == 3);
}

TEST_CASE("display index maps to channels; screen filter")
{
    CHECK(mwm::channelForDisplay("", 0) == 1);
    CHECK(mwm::channelForDisplay("", 3) == 4);
    CHECK(mwm::channelForDisplay("2:4,3:1", 3) == 1);
    CHECK(mwm::channelForDisplay("2:4,3:1", 0) == 0);

    // Empty map, every screen: display i is channel i + 1; display 254 has no channel.
    auto const cfg = mwm::parseConfig({{"MONITOR_PUBLIC_IP", "10.1.2.3"}, {"MONITOR_CHANNELS", "4"}});
    mwm::ChannelBook book(cfg);
    auto const utf16 = hex(kUtf16);
    mwm::applyTally(cfg, mwm::parseTsl5(utf16.data(), utf16.size()), book);
    auto views = book.snapshot();
    CHECK((views[0].tsl_lh == 1 && views[0].tsl_rh == 2 && views[0].tsl_text_tally == 3));
    CHECK(views[0].tsl_text == "Kamera Z\xC3\xBCrich");
    CHECK(views[1].tsl_rh == 1);
    CHECK(views[2].tsl_text.empty());

    // TSL_SCREEN 3 and a map: the screen 0 packet changes nothing, the screen 3 packet lands on channels 4 and 1.
    auto const mapped = mwm::parseConfig({{"MONITOR_PUBLIC_IP", "10.1.2.3"}, {"MONITOR_CHANNELS", "4"}, {"TSL_SCREEN", "3"}, {"TSL_MAP", "2:4,3:1"}});
    mwm::ChannelBook other(mapped);
    mwm::applyTally(mapped, mwm::parseTsl5(utf16.data(), utf16.size()), other);
    for (auto const& view : other.snapshot())
    {
        CHECK((view.tsl_lh == 0 && view.tsl_rh == 0 && view.tsl_text_tally == 0 && view.tsl_text.empty()));
    }
    auto const ascii = hex(kAscii);
    mwm::applyTally(mapped, mwm::parseTsl5(ascii.data(), ascii.size()), other);
    views = other.snapshot();
    CHECK((views[3].tsl_lh == 3 && views[3].tsl_text == "CAM 3"));
    CHECK((views[0].tsl_text_tally == 1 && views[0].tsl_rh == 3 && views[0].tsl_text == "VT"));
    CHECK(views[1].tsl_text.empty());
}

TEST_CASE("tsl settings")
{
    auto const defaults = mwm::parseConfig({{"MONITOR_PUBLIC_IP", "10.1.2.3"}});
    CHECK(defaults.tsl_enable == false);
    CHECK(defaults.tsl_udp_port == 8912);
    CHECK(defaults.tsl_tcp_port == 8913);
    CHECK(defaults.tsl_screen == -1);
    CHECK(defaults.tsl_map.empty());
    CHECK(defaults.channels[0].tally_text == false);
    CHECK(mwm::configToMap(defaults).at("TSL_UDP_PORT") == "8912");
    CHECK(mwm::configToMap(defaults).at("CH1_TALLY_TEXT") == "false");

    auto const set = mwm::parseConfig({{"MONITOR_PUBLIC_IP", "10.1.2.3"}, {"TSL_ENABLE", "true"}, {"TSL_SCREEN", "0"}, {"TSL_MAP", "0:2,7:1"},
        {"CH2_TALLY_TEXT", "true"}});
    CHECK(set.tsl_enable);
    CHECK(set.tsl_screen == 0);
    CHECK(set.tsl_map == "0:2,7:1");
    CHECK(set.channels[1].tally_text);

    for (auto const& map : {"0:5", "0:0", "-1:1", "a:1", "0-1", "0:1,,1:2"})
    {
        CHECK_THROWS_AS(mwm::parseConfig({{"MONITOR_PUBLIC_IP", "10.1.2.3"}, {"TSL_MAP", map}}), mwm::ConfigError);
    }
    CHECK_THROWS_AS(mwm::parseConfig({{"MONITOR_PUBLIC_IP", "10.1.2.3"}, {"TSL_SCREEN", "-2"}}), mwm::ConfigError);
    CHECK_THROWS_AS(mwm::parseConfig({{"MONITOR_PUBLIC_IP", "10.1.2.3"}, {"TSL_UDP_PORT", "0"}}), mwm::ConfigError);
    CHECK_THROWS_AS(mwm::parseConfig({{"MONITOR_PUBLIC_IP", "10.1.2.3"}, {"TSL_ENABLE", "maybe"}}), mwm::ConfigError);
}

TEST_CASE("channel status carries the tally fields and tally_text")
{
    auto const cfg = mwm::parseConfig({{"HOST_ID", "host"}, {"MONITOR_PUBLIC_IP", "10.1.2.3"}, {"MONITOR_CHANNELS", "2"}});
    auto store = std::make_shared<mwm::ConfigStore>(cfg, std::map<std::string, mwm::ValueOrigin>{}, std::map<std::string, std::string>{});
    auto book = std::make_shared<mwm::ChannelBook>(cfg);
    book->setTally(1, 1, 2, 0, "Kamera Z\xC3\xBCrich");
    mwm::Api api(store, book);
    mwm::HttpRequest request;
    request.method = "GET";
    request.path = "/api/v1/channels";
    std::string err;
    auto root = mwm::json::parse(api.handle(request).body, &err);
    REQUIRE(err.empty());
    auto channel = root.get<picojson::object>().at("channels").get<picojson::array>().at(0).get<picojson::object>();
    CHECK(channel.at("tsl_lh").get<double>() == 1);
    CHECK(channel.at("tsl_rh").get<double>() == 2);
    CHECK(channel.at("tsl_text_tally").get<double>() == 0);
    CHECK(channel.at("tally").get<double>() == 2);
    CHECK(channel.at("tsl_text").get<std::string>() == "Kamera Z\xC3\xBCrich");
    CHECK(channel.at("tally_text").get<bool>() == false);
    CHECK(api.eventsJson().find("\"tally\":2,\"tsl_text\":\"Kamera Z\xC3\xBCrich\",\"tsl_lh\":1,\"tsl_rh\":2,\"tsl_text_tally\":0") != std::string::npos);

    mwm::HttpRequest patch;
    patch.method = "PATCH";
    patch.path = "/api/v1/channels/1";
    patch.body = R"({"tally_text":true})";
    root = mwm::json::parse(api.handle(patch).body, &err);
    REQUIRE(err.empty());
    channel = root.get<picojson::object>().at("channels").get<picojson::array>().at(0).get<picojson::object>();
    CHECK(channel.at("tally_text").get<bool>() == true);
    CHECK(store->get().channels[0].tally_text);
    CHECK_FALSE(store->restartRequired());

    // A configuration reload keeps the tally the controller sent.
    book->reset(store->get());
    CHECK(book->snapshot()[0].tsl_rh == 2);
}
