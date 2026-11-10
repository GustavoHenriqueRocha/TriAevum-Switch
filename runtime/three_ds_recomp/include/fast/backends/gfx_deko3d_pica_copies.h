#pragma once

// deko3d: raw PICA TextureCopies out of a GPU render target. The frontend
// defers such a copy to the backend and never reads guest memory where it
// lands: a texture over those bytes arrives with zeros there and a
// PicaTextureCopyReference to the copy (pica_native_state.h). Vulkan keeps a
// GPU image per copy version; deko3d reads the source target back once,
// encodes it as the guest's framebuffer bytes (EncodePicaFramebuffer) and keeps
// each version's destination bytes: a texture takes them in place of the
// zeros, and the guest gets them when the copy completes. Pure parts.

#include "fast/backends/gfx_deko3d_pica_pipeline.h"
#include "fast/renderer3ds/pica_framebuffer_encode.h"
#include "fast/renderer3ds/pica_native_state.h"
#include "fast/renderer3ds/pica_render_backend.h"
#include "fast/renderer3ds/pica_texture_copy_plan.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Fast::Deko3d {

// The copy's byte walk (Vulkan's SubmitNativePicaTextureCopy). A TextureCopy
// view carries line widths in InputWidth/OutputWidth and the gaps after each
// line in InputHeight/OutputHeight, both in 16-byte units; no gap is one line.
struct PicaRawCopyWalk {
    std::vector<Renderer3ds::PicaRawCopySpan> Spans;
    uint64_t InputEnd = 0;
    uint64_t OutputEnd = 0;
};

// nullptr, or why the copy has no walk.
[[nodiscard]] inline const char* WalkPicaRawCopy(const Renderer3ds::PicaDisplayTransferView& transfer,
                                                 PicaRawCopyWalk& walk) {
    walk.Spans.clear();
    const uint32_t size = transfer.TextureCopyBytes & ~15U;
    const uint32_t inputGap = uint32_t{transfer.InputHeight} * 16U;
    const uint32_t outputGap = uint32_t{transfer.OutputHeight} * 16U;
    const uint32_t inputWidth = inputGap == 0U ? size : uint32_t{transfer.InputWidth} * 16U;
    const uint32_t outputWidth = outputGap == 0U ? size : uint32_t{transfer.OutputWidth} * 16U;
    if (size == 0U || inputWidth == 0U || outputWidth == 0U) {
        return "raw PICA copy has no size or line width";
    }
    walk.InputEnd = uint64_t{transfer.InputPhysicalAddress} + size + uint64_t{(size - 1U) / inputWidth} * inputGap;
    walk.OutputEnd =
        uint64_t{transfer.OutputPhysicalAddress} + size + uint64_t{(size - 1U) / outputWidth} * outputGap;
    if (walk.InputEnd > (uint64_t{1} << 32U) || walk.OutputEnd > (uint64_t{1} << 32U)) {
        return "raw PICA copy wraps physical addresses";
    }
    uint64_t input = transfer.InputPhysicalAddress;
    uint64_t output = transfer.OutputPhysicalAddress;
    uint32_t left = size;
    uint32_t inputLeft = inputWidth;
    uint32_t outputLeft = outputWidth;
    while (left != 0U) {
        const uint32_t count = std::min({left, inputLeft, outputLeft});
        walk.Spans.push_back({static_cast<uint32_t>(input), static_cast<uint32_t>(output), count});
        left -= count;
        inputLeft -= count;
        outputLeft -= count;
        input += count;
        output += count;
        if (inputLeft == 0U) {
            input += inputGap;
            inputLeft = inputWidth;
        }
        if (outputLeft == 0U) {
            output += outputGap;
            outputLeft = outputWidth;
        }
    }
    return nullptr;
}

// Whether `key` is a colour target of the namespace holding every input byte.
[[nodiscard]] inline bool PicaTargetHoldsCopyInput(const PicaTargetKey& key, uint64_t targetNamespace,
                                                   uint32_t inputAddress, uint64_t inputEnd) noexcept {
    const uint64_t end =
        uint64_t{key.ColorAddress} + uint64_t{key.Width} * key.Height * PicaColorBytesPerPixel(key.ColorFormat);
    return key.Namespace == targetNamespace && inputAddress >= key.ColorAddress && inputEnd <= end;
}

// The destination bytes of `spans`, concatenated in span order, from the
// source target read back at its drawn extent: `rgba` rows of imageWidth x
// imageHeight with row 0 at NDC y -1, the native extent and format of `key`.
// nullptr, or why the copy cannot be encoded.
[[nodiscard]] inline const char* EncodePicaRawCopy(std::span<const uint8_t> rgba, uint32_t imageWidth,
                                                   uint32_t imageHeight, const PicaTargetKey& key,
                                                   std::span<const Renderer3ds::PicaRawCopySpan> spans,
                                                   std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> native;
    try {
        native = Renderer3ds::EncodePicaFramebuffer(rgba, imageWidth, imageHeight, key.Width, key.Height,
                                                    key.ColorFormat);
    } catch (const std::exception&) {
        return "raw PICA copy source target cannot be encoded";
    }
    bytes.clear();
    for (const auto& span : spans) {
        if (span.SourceAddress < key.ColorAddress ||
            uint64_t{span.SourceAddress} - key.ColorAddress + span.ByteCount > native.size()) {
            return "raw PICA copy source span exceeds its target";
        }
        const auto from = native.begin() + (span.SourceAddress - key.ColorAddress);
        bytes.insert(bytes.end(), from, from + span.ByteCount);
    }
    return nullptr;
}

// Each captured copy version's destination bytes, by CompletionId.
class PicaCopyStore {
  public:
    struct Version {
        std::vector<Renderer3ds::PicaRawCopySpan> Spans;
        std::vector<uint8_t> Bytes; // the spans' destination bytes, concatenated
        bool Materialized = false;  // the guest got them and the copy's completion
    };

    [[nodiscard]] const Version* Find(uint64_t completionId) const {
        const auto found = mVersions.find(completionId);
        return found == mVersions.end() ? nullptr : &found->second;
    }
    const Version& Put(uint64_t completionId, Version version) {
        return mVersions.insert_or_assign(completionId, std::move(version)).first->second;
    }
    void MarkMaterialized(uint64_t completionId) {
        if (const auto found = mVersions.find(completionId); found != mVersions.end()) {
            found->second.Materialized = true;
        }
    }
    // SetPinnedPicaTextureCopies: a version no pinned reference names can no
    // longer be sampled (an upload copies the bytes it reads).
    void Retain(std::span<const Renderer3ds::PicaTextureCopyReference> pinned) {
        std::unordered_set<uint64_t> kept;
        for (const auto& reference : pinned) {
            kept.insert(reference.CompletionId);
        }
        std::erase_if(mVersions, [&](const auto& entry) { return !kept.contains(entry.first); });
    }
    // `view` with the captured bytes where its copy references point, held in
    // `scratch`; false when no reference has a captured version (the view keeps
    // the frontend's zeros there).
    [[nodiscard]] bool Fill(const Renderer3ds::PicaTextureView& view, std::vector<uint8_t>& scratch,
                            Renderer3ds::PicaTextureView& filled) const {
        bool any = false;
        for (const auto& reference : view.CopyReferences) {
            const Version* version = Find(reference.CompletionId);
            if (version == nullptr) {
                continue;
            }
            if (!any) {
                scratch.assign(view.NativeBytes.begin(), view.NativeBytes.end());
                any = true;
            }
            const uint64_t low = std::max<uint64_t>(reference.PhysicalAddress, view.PhysicalAddress);
            const uint64_t high = std::min<uint64_t>(uint64_t{reference.PhysicalAddress} + reference.ByteCount,
                                                     uint64_t{view.PhysicalAddress} + scratch.size());
            size_t at = 0U;
            for (const auto& span : version->Spans) {
                const uint64_t spanLow = span.DestinationAddress;
                const uint64_t from = std::max(low, spanLow);
                const uint64_t to = std::min(high, spanLow + span.ByteCount);
                if (from < to) {
                    std::memcpy(scratch.data() + (from - view.PhysicalAddress),
                                version->Bytes.data() + at + (from - spanLow), static_cast<size_t>(to - from));
                }
                at += span.ByteCount;
            }
        }
        if (!any) {
            return false;
        }
        filled = view;
        filled.NativeBytes = scratch;
        filled.NativeContentHashAvailable = false;
        filled.NativeBaseLevelContentHashAvailable = false;
        return true;
    }
    [[nodiscard]] size_t Count() const noexcept { return mVersions.size(); }

  private:
    std::unordered_map<uint64_t, Version> mVersions;
};

} // namespace Fast::Deko3d
