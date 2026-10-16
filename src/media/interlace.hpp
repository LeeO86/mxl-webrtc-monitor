#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mwm
{
// What one MXL video grain holds and how the reader turns grains into frames (1.4.1).
//
// MXL (218ddaa) stores an interlaced flow as fields: each grain is ONE field of frame_height / 2
// lines, and the grains come at twice the flow definition's grain_rate (a 1080i flow declared at
// 25/1 has 50 grains per second; mxlFlowConfigInfo.common.grainRate says 50/1). Grain 2k is the
// first field of frame k in time (a writer requirement, as in mxl-multiviewer 1.4.4): the top
// field for interlaced_tff, the bottom field for interlaced_bff.
struct VideoGrainLayout
{
    bool fields = false;   // interlaced: a grain is one field, a frame is two grains
    int rows = 0;          // lines in one grain
    std::size_t bytes = 0; // v210 bytes of those lines (a v210a key plane may follow)
};

VideoGrainLayout videoGrainLayout(int width, int height, bool interlaced);

// Empty when a grain of `grainSize` bytes can be read with `layout`, else the reason the input is
// unsupported: "no_raster", "odd_interlaced_height" or "grain_too_small". A larger grain (v210a: the
// key plane after the fill) is read.
std::string checkVideoGrain(VideoGrainLayout const& layout, int width, int height, std::size_t grainSize);

// The reader counts in units: a grain of a progressive flow, a frame (two grains) of an interlaced one.
// The newest unit whose grains are all at or below `head` (head >= 1 for an interlaced flow).
std::uint64_t newestUnit(std::uint64_t head, bool fields);

// The first grain of unit `unit`: the grain itself, or the first field (grain 2k) of frame k.
std::uint64_t firstGrainOfUnit(std::uint64_t unit, bool fields);

// Interleaves two fields of `fieldRows` lines (`rowBytes` each) into a frame of 2 × fieldRows lines:
// `top` on the even frame lines, `bottom` on the odd ones.
void weaveFields(std::uint8_t const* top, std::uint8_t const* bottom, std::size_t rowBytes, int fieldRows, std::uint8_t* dst);

// How the encoder's picture is made from one interlaced frame.
enum class FieldPreview
{
    FirstField, // the preview is not taller than a field: the first field alone, no line of the other
    Weave,      // the preview is taller: both fields woven into the frame, then scaled
};

FieldPreview fieldPreviewMode(int height, int previewHeight);

// How many grains of a unit (from its first) must be readable and valid to show it: the grain of a
// progressive flow, the first field alone (FirstField: grain 2k + 1 may be missing or invalid) or
// both fields (Weave).
std::uint64_t requiredGrains(bool fields, FieldPreview mode);

// MXL's own grain rate (mxlFlowConfigInfo.common.grainRate) against the definition: an interlaced
// definition needs twice its grain_rate (one grain per field). Empty when it fits or MXL gives no rate,
// else "format_mismatch" (e.g. whole-frame grains of a flow whose flow_def.json now says interlaced).
std::string checkGrainRate(bool interlaced, std::int64_t defNum, std::int64_t defDen, std::int64_t mxlNum, std::int64_t mxlDen);

char const* fieldPreviewName(FieldPreview mode);

// The encoder's picture (8-bit 4:2:0, outWidth × outHeight) of frame k of an interlaced flow:
// `first` is grain 2k, `second` grain 2k + 1 (each height / 2 lines of v210). FirstField scales the
// first field as a progressive picture (`second` may be null); Weave weaves the fields into `woven`
// (top field on the even lines: `first` for tff, `second` for bff) and scales the frame.
void fieldsToPreview(std::uint8_t const* first, std::uint8_t const* second, int width, int height, bool bottomFieldFirst, int outWidth, int outHeight,
    bool nv12, std::vector<std::uint8_t>& woven, std::uint8_t* dst);
} // namespace mwm
