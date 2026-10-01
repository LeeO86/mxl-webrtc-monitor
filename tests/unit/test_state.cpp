#include "doctest/doctest.h"

#include "channel/state.hpp"

TEST_CASE("channel state machine")
{
    mwm::LegInput input;
    auto eval = mwm::evaluateLeg(input);
    CHECK(eval.state == mwm::RunState::NotRouted);

    input.master_enable = true;
    input.domain_id = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
    input.flow_id = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
    eval = mwm::evaluateLeg(input);
    CHECK(eval.state == mwm::RunState::Waiting);
    CHECK(eval.reason == "domain_not_found");

    input.domain_resolved = true;
    eval = mwm::evaluateLeg(input);
    CHECK(eval.state == mwm::RunState::Waiting);
    CHECK(eval.reason == "flow_not_found");

    input.flow_open = true;
    eval = mwm::evaluateLeg(input);
    CHECK(eval.state == mwm::RunState::NoSignal);

    input.grains_flowing = true;
    eval = mwm::evaluateLeg(input);
    CHECK(eval.state == mwm::RunState::Running);

    input.flow_open = false;
    input.grains_flowing = false;
    eval = mwm::evaluateLeg(input);
    CHECK(eval.state == mwm::RunState::Waiting);
    CHECK(eval.reason == "flow_not_found");

    input.master_enable = false;
    eval = mwm::evaluateLeg(input);
    CHECK(eval.state == mwm::RunState::NotRouted);
}

TEST_CASE("reader backoff")
{
    CHECK(mwm::backoffMs(0) == 250);
    CHECK(mwm::backoffMs(1) == 500);
    CHECK(mwm::backoffMs(2) == 1000);
    CHECK(mwm::backoffMs(4) == 4000);
    CHECK(mwm::backoffMs(5) == 5000);
    CHECK(mwm::backoffMs(8) == 5000);
}

TEST_CASE("format strings")
{
    CHECK(mwm::formatString(1080, false, 50, 1) == "1080p50");
    CHECK(mwm::formatString(1080, true, 25, 1) == "1080i50");
    CHECK(mwm::formatString(2160, false, 60000, 1001) == "2160p59.94");
}
