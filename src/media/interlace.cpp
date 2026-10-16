#include "media/interlace.hpp"

#include "media/v210.hpp"

#include <cstring>

namespace mwm
{
VideoGrainLayout videoGrainLayout(int width, int height, bool interlaced)
{
    VideoGrainLayout layout;
    layout.fields = interlaced;
    layout.rows = interlaced ? height / 2 : height;
    if (width > 0 && layout.rows > 0)
    {
        layout.bytes = v210Stride(width) * static_cast<std::size_t>(layout.rows);
    }
    return layout;
}

std::string checkVideoGrain(VideoGrainLayout const& layout, int width, int height, std::size_t grainSize)
{
    if (width < 2 || height < 1)
    {
        return "no_raster";
    }
    if (layout.fields && height % 2 != 0)
    {
        return "odd_interlaced_height";
    }
    if (layout.rows < 1)
    {
        return "no_raster";
    }
    if (grainSize < layout.bytes)
    {
        return "grain_too_small";
    }
    return {};
}

std::uint64_t newestUnit(std::uint64_t head, bool fields)
{
    if (!fields)
    {
        return head;
    }
    // Frame k is complete once its second field (grain 2k + 1) is at or below the head.
    return head >= 1 ? (head + 1) / 2 - 1 : 0;
}

std::uint64_t firstGrainOfUnit(std::uint64_t unit, bool fields)
{
    return fields ? unit * 2 : unit;
}

void weaveFields(std::uint8_t const* top, std::uint8_t const* bottom, std::size_t rowBytes, int fieldRows, std::uint8_t* dst)
{
    for (int row = 0; row < fieldRows; ++row)
    {
        auto const offset = rowBytes * static_cast<std::size_t>(row);
        std::memcpy(dst + offset * 2, top + offset, rowBytes);
        std::memcpy(dst + offset * 2 + rowBytes, bottom + offset, rowBytes);
    }
}

FieldPreview fieldPreviewMode(int height, int previewHeight)
{
    return previewHeight <= height / 2 ? FieldPreview::FirstField : FieldPreview::Weave;
}

char const* fieldPreviewName(FieldPreview mode)
{
    return mode == FieldPreview::FirstField ? "first_field" : "weave";
}

void fieldsToPreview(std::uint8_t const* first, std::uint8_t const* second, int width, int height, bool bottomFieldFirst, int outWidth, int outHeight,
    bool nv12, std::vector<std::uint8_t>& woven, std::uint8_t* dst)
{
    int const fieldRows = height / 2;
    if (first == nullptr || second == nullptr || fieldRows < 1)
    {
        return;
    }
    if (fieldPreviewMode(height, outHeight) == FieldPreview::FirstField)
    {
        v210ToPreview(first, width, fieldRows, false, outWidth, outHeight, nv12, dst);
        return;
    }
    auto const rowBytes = v210Stride(width);
    woven.resize(rowBytes * static_cast<std::size_t>(fieldRows) * 2);
    weaveFields(bottomFieldFirst ? second : first, bottomFieldFirst ? first : second, rowBytes, fieldRows, woven.data());
    v210ToPreview(woven.data(), width, fieldRows * 2, false, outWidth, outHeight, nv12, dst);
}
} // namespace mwm
