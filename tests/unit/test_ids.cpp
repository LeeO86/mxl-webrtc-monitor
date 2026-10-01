#include "doctest/doctest.h"

#include "nmos/ids.hpp"

TEST_CASE("deterministic uuid v5 ids")
{
    auto const a = mwm::makeNmosIds("host-a-monitor");
    auto const b = mwm::makeNmosIds("host-a-monitor");
    auto const c = mwm::makeNmosIds("host-b-monitor");
    CHECK(a.node == b.node);
    CHECK(a.device == b.device);
    CHECK(a.node != c.node);
    CHECK(a.videoReceiver(1) == b.videoReceiver(1));
    CHECK(a.videoReceiver(1) != a.videoReceiver(2));
    CHECK(a.videoReceiver(1) != a.audioReceiver(1));
    CHECK(a.node[14] == '5');
    CHECK(a.device[14] == '5');
    CHECK(a.videoReceiver(1)[14] == '5');
    CHECK(a.node.size() == 36);
}
