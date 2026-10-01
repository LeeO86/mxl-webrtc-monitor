#include "doctest/doctest.h"

#include "config/config.hpp"

#include <fstream>

TEST_CASE("config defaults and precedence")
{
    auto const defaults = mwm::parseConfig({});
    CHECK(defaults.monitor_channels == 4);
    CHECK(defaults.web_port == 8100);
    CHECK(defaults.nmos_port == 3242);
    CHECK(defaults.encoder == "auto");
    CHECK(defaults.nmos_dns_sd == false);
    CHECK(defaults.channels.size() == 4);
    CHECK(defaults.channels[0].video_label == "Monitor 1 Video");

    std::map<std::string, std::string> file{{"WEB_PORT", "9000"}, {"MONITOR_CHANNELS", "2"}, {"ENCODER", "x264"}};
    std::map<std::string, std::string> env{{"WEB_PORT", "9001"}};
    std::map<std::string, mwm::ValueOrigin> origin;
    auto const layered = mwm::loadLayered(file, env, {}, &origin);
    CHECK(layered.web_port == 9001);
    CHECK(layered.monitor_channels == 2);
    CHECK(layered.encoder == "x264");
    CHECK(origin.at("WEB_PORT") == mwm::ValueOrigin::Env);
    CHECK(origin.at("ENCODER") == mwm::ValueOrigin::File);
    CHECK(origin.at("LOG_LEVEL") == mwm::ValueOrigin::Default);
    CHECK(layered.channels.size() == 2);
}

TEST_CASE("invalid configuration")
{
    CHECK_THROWS_AS(mwm::parseConfig({{"MONITOR_CHANNELS", "0"}}), mwm::ConfigError);
    CHECK_THROWS_AS(mwm::parseConfig({{"ENCODER", "vp9"}}), mwm::ConfigError);
    CHECK_THROWS_AS(mwm::parseConfig({{"NO_SUCH", "1"}}), mwm::ConfigError);
    CHECK_THROWS_AS(mwm::parseConfig({{"CH1_DOWNMIX", "surround"}}), mwm::ConfigError);
}

TEST_CASE("channel settings override globals")
{
    auto cfg = mwm::parseConfig({{"MONITOR_PREVIEW_HEIGHT", "360"}, {"CH2_PREVIEW_HEIGHT", "720"}, {"CH2_AUDIO_PAIR", "3"}, {"CH2_DOWNMIX", "mono"}});
    CHECK(cfg.channels[0].preview_height == 360);
    CHECK(cfg.channels[1].preview_height == 720);
    CHECK(cfg.channels[1].audio_pair == 3);
    CHECK(cfg.channels[1].downmix == "mono");
}

TEST_CASE("config file channels array")
{
    auto const path = std::string("/tmp/mwm-config-test.json");
    {
        std::ofstream out(path);
        out << R"({"MONITOR_CHANNELS":2,"channels":[{"index":1,"video_label":"PGM","audio_pair":2}]})";
    }
    std::vector<mwm::ChannelSettings> channels;
    auto const values = mwm::loadConfigFile(path, &channels);
    auto cfg = mwm::parseConfig(values);
    CHECK(cfg.monitor_channels == 2);
    CHECK(cfg.channels[0].video_label == "PGM");
    CHECK(cfg.channels[0].audio_pair == 2);
    CHECK(cfg.channels[0].preview_height == 540);
}
