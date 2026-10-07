#include "doctest/doctest.h"

#include "channel/audio.hpp"

TEST_CASE("audio pair selection and downmix")
{
    auto const first = mwm::selectAudioPair(8, 1, "stereo");
    CHECK(first.valid);
    CHECK(first.left == 0);
    CHECK(first.right == 1);
    CHECK_FALSE(first.mono);
    auto const third = mwm::selectAudioPair(8, 3, "mono");
    CHECK(third.left == 4);
    CHECK(third.right == 5);
    CHECK(third.mono);
    auto const missing = mwm::selectAudioPair(0, 1, "stereo");
    CHECK_FALSE(missing.valid);

    float ch0[] = {1.f, 1.f};
    float ch1[] = {0.f, 0.f};
    float ch4[] = {0.5f, -0.5f};
    float ch5[] = {0.5f, 0.5f};
    float const* ptrs[] = {ch0, ch1, nullptr, nullptr, ch4, ch5};
    float dst[4] = {};
    mwm::renderAudioPair(ptrs, 6, 2, first, dst);
    CHECK(dst[0] == doctest::Approx(1.f));
    CHECK(dst[1] == doctest::Approx(0.f));
    mwm::renderAudioPair(ptrs, 6, 2, third, dst);
    CHECK(dst[0] == doctest::Approx(0.5f));
    CHECK(dst[1] == doctest::Approx(0.5f));
    CHECK(dst[2] == doctest::Approx(0.f));
    CHECK(dst[3] == doctest::Approx(0.f));
}

TEST_CASE("audio follows the pushed video timeline")
{
    // 50p: 960 samples per frame; 59.94: 800.8, floored over the whole count.
    CHECK(mwm::audioSamplesDue(1, 50, 1, 960) == 0);
    CHECK(mwm::audioSamplesDue(3, 50, 1, 960) == 1920);
    CHECK(mwm::audioSamplesDue(3, 50, 1, 5000) == 0); // never negative
    CHECK(mwm::audioSamplesDue(5, 60000, 1001, 0) == 4004);
    CHECK(mwm::audioSamplesDue(10, 0, 0, 0) == 480000); // bad rate: 1/1
}

TEST_CASE("audio reads are placed at the target and then follow the cursor")
{
    std::uint64_t const slack = 3 * 960;
    // First read: ends at the target.
    auto plan = mwm::planAudioRead(std::nullopt, 100000, 100000, 960, slack, false);
    CHECK(plan.step == mwm::AudioStep::Read);
    CHECK(plan.start == 99040);
    CHECK_FALSE(plan.resync);
    // A mirror whose head moves 480 samples at a time: the next frame waits for the head, then reads
    // exactly the samples after the last push (no overlap, no gap).
    plan = mwm::planAudioRead(100000, 100480, 100480, 960, slack, false);
    CHECK(plan.step == mwm::AudioStep::Wait);
    plan = mwm::planAudioRead(100000, 100960, 100960, 960, slack, false);
    CHECK(plan.step == mwm::AudioStep::Read);
    CHECK(plan.start == 100000);
    // The head does not arrive in time: silence for that frame.
    plan = mwm::planAudioRead(100000, 100480, 100480, 960, slack, true);
    CHECK(plan.step == mwm::AudioStep::Silence);
    CHECK(plan.start == 100000);
    // Far from the target (video resynced, source restarted): placed again.
    plan = mwm::planAudioRead(100000, 200000, 200000, 960, slack, false);
    CHECK(plan.resync);
    CHECK(plan.start == 199040);
    CHECK(plan.step == mwm::AudioStep::Read);
    plan = mwm::planAudioRead(300000, 200000, 400000, 960, slack, false);
    CHECK(plan.resync);
    CHECK(plan.start == 199040);
}
