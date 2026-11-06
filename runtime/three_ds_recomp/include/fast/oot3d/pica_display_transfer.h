#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>

namespace Fast::Oot3d {

struct PicaDisplayTransferPlanInput {
    uint32_t NativeInputWidth = 0;
    uint32_t NativeInputHeight = 0;
    uint32_t NativeOutputWidth = 0;
    uint32_t NativeOutputHeight = 0;
    uint32_t SourceWidth = 0;
    uint32_t SourceHeight = 0;
    uint32_t Flags = 0;
    // The host source already has one texel per presented pixel along the
    // filtered axes (the top screen, see render_resolution_policy.h): the
    // native box filter becomes a copy at the source's density.
    bool SourceAtPresentedDensity = false;
};

struct PicaDisplayTransferPlan {
    uint32_t SourceWidth = 0;
    uint32_t SourceHeight = 0;
    uint32_t DestinationWidth = 0;
    uint32_t DestinationHeight = 0;
    uint32_t HorizontalSamples = 1;
    uint32_t VerticalSamples = 1;
    uint32_t ScalingMode = 0;

    [[nodiscard]] bool SamplingFitsSource() const {
        return SourceWidth && SourceHeight && DestinationWidth && DestinationHeight &&
            (HorizontalSamples == 1 || HorizontalSamples == 2) &&
            (VerticalSamples == 1 || VerticalSamples == 2) &&
            static_cast<uint64_t>(DestinationWidth) * HorizontalSamples <=
                static_cast<uint64_t>(SourceWidth) + HorizontalSamples - 1 &&
            static_cast<uint64_t>(DestinationHeight) * VerticalSamples <=
                static_cast<uint64_t>(SourceHeight) + VerticalSamples - 1;
    }
    [[nodiscard]] uint32_t BlitSourceWidth() const {
        return static_cast<uint32_t>(std::min(
            static_cast<uint64_t>(SourceWidth),
            static_cast<uint64_t>(DestinationWidth) * HorizontalSamples));
    }
    [[nodiscard]] uint32_t BlitSourceHeight() const {
        return static_cast<uint32_t>(std::min(
            static_cast<uint64_t>(SourceHeight),
            static_cast<uint64_t>(DestinationHeight) * VerticalSamples));
    }
};

struct PicaDisplayTransferSourceRows {
    uint32_t First = 0;
    uint32_t Count = 0;
};

// Native attachment rows a transfer reads from a target targetHeight lines
// tall, starting lineOffset lines into it (a sub-rectangle source). Memory
// line y is attachment row targetHeight - 1 - y (pica_framebuffer_encode.h).
// No offset keeps the whole target, as transfers always did.
[[nodiscard]] inline std::optional<PicaDisplayTransferSourceRows>
PicaDisplayTransferSourceRowsFor(uint32_t targetHeight, uint32_t inputHeight,
                                 uint32_t lineOffset) {
    if (lineOffset == 0U) return PicaDisplayTransferSourceRows{0U, targetHeight};
    if (lineOffset >= targetHeight || inputHeight == 0U) return std::nullopt;
    const uint32_t count = std::min(inputHeight, targetHeight - lineOffset);
    return PicaDisplayTransferSourceRows{targetHeight - lineOffset - count, count};
}

// Builds the GPU-image equivalent of the native PICA display transfer. The
// output dimensions preserve renderer supersampling while crop and 2:1 box
// filtering continue to operate in native PICA coordinates; a source at the
// presented density is copied instead of filtered.
[[nodiscard]] std::optional<PicaDisplayTransferPlan>
BuildPicaDisplayTransferPlan(const PicaDisplayTransferPlanInput& input,
                             std::string* error = nullptr);

[[nodiscard]] std::string BuildPicaDisplayTransferComputeShader();

} // namespace Fast::Oot3d
