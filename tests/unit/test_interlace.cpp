// Interlaced input (1.4.1): MXL 218ddaa stores one field per grain (height / 2 lines) at twice the
// definition's grain_rate. Frames from field pairs, the preview picture, the per-grain size check and
// the `unsupported` state in the status and the metrics.
#include "doctest/doctest.h"

#include "channel/book.hpp"
#include "channel/state.hpp"
#include "config/config.hpp"
#include "config/store.hpp"
#include "media/interlace.hpp"
#include "media/v210.hpp"
#include "ops/api.hpp"
#include "ops/metrics.hpp"
#include "util/jsonutil.hpp"

#include <cstring>
#include <memory>
#include <vector>

namespace
{
// `rows` lines of v210 where every pixel has luma `y` (chroma 512).
std::vector<std::uint8_t> solidRows(int width, int rows, int y)
{
    std::vector<std::uint8_t> out(mwm::v210Stride(width) * static_cast<std::size_t>(rows), 0);
    auto const word = [](int a, int b, int c) { return static_cast<std::uint32_t>((a & 0x3ff) | ((b & 0x3ff) << 10) | ((c & 0x3ff) << 20)); };
    std::uint32_t const group[4] = {word(512, y, 512), word(y, 512, y), word(512, y, 512), word(y, 512, y)};
    for (int row = 0; row < rows; ++row)
    {
        for (int g = 0; g * 6 < width; ++g)
        {
            std::memcpy(out.data() + static_cast<std::size_t>(row) * mwm::v210Stride(width) + static_cast<std::size_t>(g) * 16, group, sizeof(group));
        }
    }
    return out;
}

// The luma of v210 pixel (0, row).
int lumaAt(std::vector<std::uint8_t> const& v210, int width, int row)
{
    std::uint32_t word = 0;
    std::memcpy(&word, v210.data() + static_cast<std::size_t>(row) * mwm::v210Stride(width), sizeof(word));
    return static_cast<int>((word >> 10) & 0x3ff);
}

// The mean luma of output row `row` of an I420 or NV12 picture.
double rowLuma(std::vector<std::uint8_t> const& picture, int width, int row)
{
    double sum = 0;
    for (int x = 0; x < width; ++x)
    {
        sum += picture[static_cast<std::size_t>(row) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
    }
    return sum / width;
}
} // namespace

TEST_CASE("a grain of an interlaced flow is one field")
{
    auto const field = mwm::videoGrainLayout(1920, 1080, true);
    CHECK(field.fields);
    CHECK(field.rows == 540);
    CHECK(field.bytes == mwm::v210Stride(1920) * 540);
    CHECK(field.bytes * 2 == mwm::v210FrameBytes(1920, 1080));

    auto const frame = mwm::videoGrainLayout(1920, 1080, false);
    CHECK_FALSE(frame.fields);
    CHECK(frame.rows == 1080);
    CHECK(frame.bytes == mwm::v210FrameBytes(1920, 1080));
}

TEST_CASE("every grain is checked against the definition")
{
    std::size_t const fieldBytes = mwm::v210Stride(1920) * 540;
    auto const interlaced = mwm::videoGrainLayout(1920, 1080, true);
    auto const progressive = mwm::videoGrainLayout(1920, 1080, false);
    // A field read as a field.
    CHECK(mwm::checkVideoGrain(interlaced, 1920, 1080, fieldBytes).empty());
    // 1.4.0: a field grain of a flow whose definition says progressive (or a reader that expects a
    // frame) is half a frame. It is unsupported, never read past its end.
    CHECK(mwm::checkVideoGrain(progressive, 1920, 1080, fieldBytes) == "grain_too_small");
    CHECK(mwm::checkVideoGrain(interlaced, 1920, 1080, fieldBytes - 1) == "grain_too_small");
    CHECK(mwm::checkVideoGrain(interlaced, 1920, 1080, 0) == "grain_too_small");
    // A larger grain (v210a: the key plane after the fill) is read.
    CHECK(mwm::checkVideoGrain(progressive, 1920, 1080, mwm::v210FrameBytes(1920, 1080) * 2).empty());
    CHECK(mwm::checkVideoGrain(mwm::videoGrainLayout(1920, 1081, true), 1920, 1081, mwm::v210FrameBytes(1920, 1081)) == "odd_interlaced_height");
    CHECK(mwm::checkVideoGrain(mwm::videoGrainLayout(0, 1080, false), 0, 1080, 1 << 20) == "no_raster");
    CHECK(mwm::checkVideoGrain(mwm::videoGrainLayout(1920, 1, true), 1920, 1, 1 << 20) == "odd_interlaced_height");
}

TEST_CASE("frames of two fields: grain 2k is the first field of frame k")
{
    // Progressive: a unit is a grain.
    CHECK(mwm::newestUnit(7, false) == 7);
    CHECK(mwm::firstGrainOfUnit(7, false) == 7);
    // Interlaced: frame k is complete once grain 2k + 1 is at or below the head.
    CHECK(mwm::newestUnit(1, true) == 0);
    CHECK(mwm::newestUnit(2, true) == 0); // grain 2 is frame 1's first field only
    CHECK(mwm::newestUnit(3, true) == 1);
    CHECK(mwm::newestUnit(4, true) == 1);
    CHECK(mwm::newestUnit(5, true) == 2);
    CHECK(mwm::firstGrainOfUnit(2, true) == 4);
    // A 1080i25 flow's head at the field index of TAI time t: one frame per two fields.
    std::uint64_t const fieldHead = 2'000'000'001ULL;
    CHECK(mwm::newestUnit(fieldHead, true) == 1'000'000'000ULL);
    CHECK(mwm::firstGrainOfUnit(mwm::newestUnit(fieldHead, true), true) + 1 == fieldHead);
}

TEST_CASE("weaving puts the top field on the even frame lines")
{
    int const width = 12;
    auto const top = solidRows(width, 4, 800);
    auto const bottom = solidRows(width, 4, 200);
    std::vector<std::uint8_t> frame(mwm::v210FrameBytes(width, 8));
    mwm::weaveFields(top.data(), bottom.data(), mwm::v210Stride(width), 4, frame.data());
    for (int row = 0; row < 8; ++row)
    {
        CHECK(lumaAt(frame, width, row) == (row % 2 == 0 ? 800 : 200));
    }
}

TEST_CASE("the preview takes the first field alone when it is not taller than a field")
{
    CHECK(mwm::fieldPreviewMode(1080, 540) == mwm::FieldPreview::FirstField);
    CHECK(mwm::fieldPreviewMode(1080, 270) == mwm::FieldPreview::FirstField);
    CHECK(mwm::fieldPreviewMode(1080, 542) == mwm::FieldPreview::Weave);
    CHECK(mwm::fieldPreviewMode(1080, 1080) == mwm::FieldPreview::Weave);
    CHECK(std::string(mwm::fieldPreviewName(mwm::FieldPreview::FirstField)) == "first_field");
    CHECK(std::string(mwm::fieldPreviewName(mwm::FieldPreview::Weave)) == "weave");
}

TEST_CASE("fields to the encoder's picture, tff and bff")
{
    int const width = 96;
    int const height = 64;
    // First field (grain 2k) 800, second (2k + 1) 560: 8-bit 200 and 140.
    auto const first = solidRows(width, height / 2, 800);
    auto const second = solidRows(width, height / 2, 560);
    std::vector<std::uint8_t> woven;
    for (bool bff : {false, true})
    {
        for (bool nv12 : {false, true})
        {
            CAPTURE(bff);
            CAPTURE(nv12);
            // A preview of half the height or less: the first field only, for tff and bff. All of the
            // picture (not its upper half: 1.4.0 squashed a field into the top) is the first field.
            std::vector<std::uint8_t> half(mwm::previewBytes(width / 2, height / 2));
            mwm::fieldsToPreview(first.data(), second.data(), width, height, bff, width / 2, height / 2, nv12, woven, half.data());
            for (int row = 0; row < height / 2; ++row)
            {
                CHECK(rowLuma(half, width / 2, row) == doctest::Approx(200));
            }
            // Full height: the woven frame. tff: the first field on the even lines; bff: on the odd ones.
            std::vector<std::uint8_t> full(mwm::previewBytes(width, height));
            mwm::fieldsToPreview(first.data(), second.data(), width, height, bff, width, height, nv12, woven, full.data());
            for (int row = 0; row < height; ++row)
            {
                bool const firstLine = (row % 2 == 0) != bff;
                CHECK(rowLuma(full, width, row) == doctest::Approx(firstLine ? 200 : 140));
            }
            // The same bytes as the frame woven by hand and converted as a progressive picture.
            auto const rowBytes = mwm::v210Stride(width);
            std::vector<std::uint8_t> frame(rowBytes * static_cast<std::size_t>(height));
            mwm::weaveFields(bff ? second.data() : first.data(), bff ? first.data() : second.data(), rowBytes, height / 2, frame.data());
            std::vector<std::uint8_t> expect(full.size());
            mwm::v210ToPreview(frame.data(), width, height, false, width, height, nv12, expect.data());
            CHECK(full == expect);
        }
    }
}

TEST_CASE("the first-field picture equals 1.4.0's first-field conversion of a whole frame")
{
    // For a tff frame the first field is the top field, which 1.4.0's interlaced conversion of a
    // whole frame used: the preview bytes do not change for a frame that MXL would have kept whole.
    int const width = 1920;
    int const height = 1080;
    auto const top = solidRows(width, height / 2, 700);
    auto const bottom = solidRows(width, height / 2, 300);
    auto const rowBytes = mwm::v210Stride(width);
    std::vector<std::uint8_t> frame(rowBytes * static_cast<std::size_t>(height));
    mwm::weaveFields(top.data(), bottom.data(), rowBytes, height / 2, frame.data());
    std::vector<std::uint8_t> before(mwm::previewBytes(960, 540));
    mwm::v210ToPreview(frame.data(), width, height, true, 960, 540, false, before.data());
    std::vector<std::uint8_t> now(before.size());
    std::vector<std::uint8_t> woven;
    mwm::fieldsToPreview(top.data(), bottom.data(), width, height, false, 960, 540, false, woven, now.data());
    CHECK(now == before);
    CHECK(woven.empty()); // the first field alone needs no woven frame
}

TEST_CASE("unsupported state")
{
    mwm::LegInput input;
    input.master_enable = true;
    input.domain_id = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
    input.flow_id = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
    input.domain_resolved = true;
    input.flow_open = true;
    input.unsupported = true;
    input.unsupported_reason = "grain_too_small";
    auto eval = mwm::evaluateLeg(input);
    CHECK(eval.state == mwm::RunState::Unsupported);
    CHECK(eval.reason == "grain_too_small");
    CHECK(std::string(mwm::stateName(eval.state)) == "unsupported");
    // Routing away wins.
    input.master_enable = false;
    CHECK(mwm::evaluateLeg(input).state == mwm::RunState::NotRouted);
}

TEST_CASE("status and metrics: unsupported, interlace mode and field preview")
{
    std::map<std::string, std::string> values{{"HOST_ID", "host"}, {"MONITOR_PUBLIC_IP", "10.1.2.3"}, {"MONITOR_CHANNELS", "2"}};
    auto const cfg = mwm::parseConfig(values);
    auto store = std::make_shared<mwm::ConfigStore>(cfg, std::map<std::string, mwm::ValueOrigin>{}, std::map<std::string, std::string>{});
    auto book = std::make_shared<mwm::ChannelBook>(cfg);
    mwm::Api api(store, book);
    book->setRoute(1, mwm::LegKind::Video, true, "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", "");
    book->setRoute(2, mwm::LegKind::Video, true, "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "cccccccc-cccc-4ccc-8ccc-cccccccccccc", "");
    book->setUnsupported(1, mwm::LegKind::Video, "grain_too_small", true);
    book->setUnsupported(1, mwm::LegKind::Video, "grain_too_small", false); // a retry: not counted again
    book->setScan(1, "progressive", "");
    book->setProbe(2, mwm::LegKind::Video, true, true, true, "");
    book->setFormat(2, 1920, 1080, 25, 1, true, mwm::formatString(1080, true, 25, 1), -1);
    book->setScan(2, "interlaced_bff", "first_field");

    mwm::HttpRequest request;
    request.method = "GET";
    request.path = "/api/v1/channels";
    auto const response = api.handle(request);
    REQUIRE(response.status == 200);
    std::string err;
    auto const root = mwm::json::parse(response.body, &err);
    REQUIRE(err.empty());
    auto const& channels = root.get<picojson::object>().at("channels").get<picojson::array>();
    auto const& one = channels.at(0).get<picojson::object>();
    CHECK(one.at("video").get<picojson::object>().at("state").get<std::string>() == "unsupported");
    CHECK(one.at("video").get<picojson::object>().at("reason").get<std::string>() == "grain_too_small");
    CHECK(one.at("interlace_mode").get<std::string>() == "progressive");
    CHECK(one.at("field_preview").is<picojson::null>());
    auto const& two = channels.at(1).get<picojson::object>();
    CHECK(two.at("video").get<picojson::object>().at("state").get<std::string>() == "running");
    CHECK(two.at("format").get<std::string>() == "1080i50");
    CHECK(two.at("interlace_mode").get<std::string>() == "interlaced_bff");
    CHECK(two.at("field_preview").get<std::string>() == "first_field");

    auto metrics = mwm::renderMetrics(cfg, book->snapshot(), "mxl", "gst", "x264", {}, {}, "own");
    CHECK(metrics.find("mxl_webrtc_monitor_channel_state{channel=\"1\",kind=\"video\",state=\"unsupported\"} 1\n") != std::string::npos);
    CHECK(metrics.find("mxl_webrtc_monitor_channel_state{channel=\"1\",kind=\"video\",state=\"running\"} 0\n") != std::string::npos);
    CHECK(metrics.find("mxl_webrtc_monitor_channel_state{channel=\"2\",kind=\"video\",state=\"unsupported\"} 0\n") != std::string::npos);
    CHECK(metrics.find("mxl_webrtc_monitor_video_unsupported_total{channel=\"1\",reason=\"grain_too_small\"} 1\n") != std::string::npos);
    CHECK(metrics.find("mxl_webrtc_monitor_video_unsupported_total{channel=\"2\",reason=\"grain_too_small\"} 0\n") != std::string::npos);

    // A good grain clears the state; the count stays.
    book->setProbe(1, mwm::LegKind::Video, true, true, true, "");
    auto const view = book->snapshot().at(0);
    CHECK(view.video.state == mwm::RunState::Running);
    CHECK(view.video.reason.empty());
    metrics = mwm::renderMetrics(cfg, book->snapshot(), "mxl", "gst", "x264", {}, {}, "own");
    CHECK(metrics.find("mxl_webrtc_monitor_video_unsupported_total{channel=\"1\",reason=\"grain_too_small\"} 1\n") != std::string::npos);

    // A new route clears it too.
    book->setUnsupported(1, mwm::LegKind::Video, "odd_interlaced_height", true);
    book->setRoute(1, mwm::LegKind::Video, true, "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "dddddddd-dddd-4ddd-8ddd-dddddddddddd", "");
    CHECK(book->snapshot().at(0).video.state == mwm::RunState::Waiting);
}
