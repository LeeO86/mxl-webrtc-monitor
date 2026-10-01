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
