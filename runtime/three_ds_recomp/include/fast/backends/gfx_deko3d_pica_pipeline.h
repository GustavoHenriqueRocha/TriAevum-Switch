#pragma once

// Package 5 of the deko3d backend (docs/DEKO3D_PROGRESSO.md): render targets,
// pipeline state, display transfers and the scanout, as plain decisions free
// of deko3d so the host checks them (oot3d_deko3d_backend_tests). The console
// side (gfx_deko3d.cpp, gfx_deko3d_pica_targets.cpp) turns them into
// DkImage/Dk*State and command-buffer calls.
//
// Every rule mirrors the GL backend (gfx_opengl_pica.cpp): the target key,
// the PICA fill decode, the scaled viewport/scissor edges, the cull state,
// the display-transfer extents and the scanout placement. The enum values
// returned here are the deko3d ones (DkCompareOp, DkStencilOp, DkLogicOp,
// DkBlendOp, DkBlendFactor, DkPrimitive); gfx_deko3d.cpp static_asserts them.

#include "fast/renderer3ds/pica_native_state.h"
#include "fast/renderer3ds/pica_render_backend.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <utility>

namespace Fast::Deko3d {

// ---- enum values (deko3d.h) -------------------------------------------------

[[nodiscard]] constexpr uint8_t DekoCompareOp(Renderer3ds::PicaCompareFunction compare) noexcept {
    using C = Renderer3ds::PicaCompareFunction;
    switch (compare) {
        case C::Never: return 1U;          // DkCompareOp_Never
        case C::Less: return 2U;           // DkCompareOp_Less
        case C::Equal: return 3U;          // DkCompareOp_Equal
        case C::LessOrEqual: return 4U;    // DkCompareOp_Lequal
        case C::Greater: return 5U;        // DkCompareOp_Greater
        case C::NotEqual: return 6U;       // DkCompareOp_NotEqual
        case C::GreaterOrEqual: return 7U; // DkCompareOp_Gequal
        case C::Always: break;
    }
    return 8U; // DkCompareOp_Always
}

[[nodiscard]] constexpr uint8_t DekoStencilOp(Renderer3ds::PicaStencilAction action) noexcept {
    using A = Renderer3ds::PicaStencilAction;
    switch (action) {
        case A::Keep: return 1U;
        case A::Zero: return 2U;
        case A::Replace: return 3U;
        case A::Increment: return 4U;
        case A::Decrement: return 5U;
        case A::Invert: return 6U;
        case A::IncrementWrap: return 7U;
        case A::DecrementWrap: return 8U;
    }
    return 1U;
}

// DkLogicOp follows GL's order (Clear, And, AndReverse, Copy, AndInverted,
// NoOp, Xor, Or, Nor, Equivalent, Invert, OrReverse, CopyInverted,
// OrInverted, Nand, Set); the PICA enum does not.
[[nodiscard]] constexpr uint8_t DekoLogicOp(Renderer3ds::PicaLogicOperation operation) noexcept {
    using L = Renderer3ds::PicaLogicOperation;
    switch (operation) {
        case L::Clear: return 0U;
        case L::And: return 1U;
        case L::AndReverse: return 2U;
        case L::Copy: return 3U;
        case L::AndInverted: return 4U;
        case L::NoOp: return 5U;
        case L::Xor: return 6U;
        case L::Or: return 7U;
        case L::Nor: return 8U;
        case L::Equivalent: return 9U;
        case L::Invert: return 10U;
        case L::OrReverse: return 11U;
        case L::CopyInverted: return 12U;
        case L::OrInverted: return 13U;
        case L::Nand: return 14U;
        case L::Set: return 15U;
    }
    return 3U;
}
constexpr uint8_t kDekoLogicOpCopy = 3U;

[[nodiscard]] constexpr uint8_t DekoBlendOp(Renderer3ds::NativeBlendEquation equation) noexcept {
    using E = Renderer3ds::NativeBlendEquation;
    switch (equation) {
        case E::Add: return 1U;
        case E::Subtract: return 2U;
        case E::ReverseSubtract: return 3U;
        case E::Min: return 4U;
        case E::Max: return 5U;
    }
    return 1U;
}

[[nodiscard]] constexpr uint8_t DekoBlendFactor(Renderer3ds::NativeBlendFactor factor) noexcept {
    using F = Renderer3ds::NativeBlendFactor;
    switch (factor) {
        case F::Zero: return 1U;
        case F::One: return 2U;
        case F::SourceColor: return 3U;
        case F::OneMinusSourceColor: return 4U;
        case F::SourceAlpha: return 5U;
        case F::OneMinusSourceAlpha: return 6U;
        case F::DestAlpha: return 7U;
        case F::OneMinusDestAlpha: return 8U;
        case F::DestColor: return 9U;
        case F::OneMinusDestColor: return 10U;
        case F::SourceAlphaSaturate: return 11U;
        case F::ConstantColor: return 1U | 0x20U;
        case F::OneMinusConstantColor: return 2U | 0x20U;
        case F::ConstantAlpha: return 3U | 0x20U;
        case F::OneMinusConstantAlpha: return 4U | 0x20U;
    }
    return 2U;
}

[[nodiscard]] constexpr uint8_t DekoPrimitive(Renderer3ds::PicaTopology topology) noexcept {
    using T = Renderer3ds::PicaTopology;
    switch (topology) {
        case T::TriangleStrip: return 5U; // DkPrimitive_TriangleStrip
        case T::TriangleFan: return 6U;   // DkPrimitive_TriangleFan
        case T::TriangleList:
        case T::GeometryShader: break;
    }
    return 4U; // DkPrimitive_Triangles
}

// ---- cull -----------------------------------------------------------------

struct PicaCullPlan {
    bool Enabled = false;
    bool CullFront = false; // else back (DkFace_Front / DkFace_Back)
    bool FrontCcw = true;   // DkFrontFace_CCW, else CW
};

// GL: front face CCW for KeepCounterClockwise (else CW), cull FRONT on a
// flipped framebuffer (else BACK). deko3d with OriginUpperLeft rasterizes
// with window Y pointing down, which reverses the window-space winding of
// the GL convention (the Vulkan backend compensates the same way:
// ToPicaSceneFrontFace). `invertWinding` (config/deko3d_cull_invert) flips
// it back without a rebuild if the console shows inside-out geometry.
[[nodiscard]] constexpr PicaCullPlan PlanPicaCull(Renderer3ds::NativeCullMode mode, bool framebufferFlipped,
                                                  bool invertWinding) noexcept {
    PicaCullPlan plan;
    if (mode == Renderer3ds::NativeCullMode::KeepAll) {
        return plan;
    }
    plan.Enabled = true;
    plan.CullFront = framebufferFlipped;
    const bool glCcw = mode == Renderer3ds::NativeCullMode::KeepCounterClockwise;
    plan.FrontCcw = invertWinding ? glCcw : !glCcw;
    return plan;
}

// ---- render targets ---------------------------------------------------------

// Same identity as the GL backend's PicaGlTargetKey.
struct PicaTargetKey {
    uint64_t Namespace = 0;
    uint32_t ColorAddress = 0;
    uint32_t DepthAddress = 0;
    uint16_t Width = 0;
    uint16_t Height = 0;
    uint8_t ColorFormat = 0;
    uint8_t DepthFormat = 0;

    bool operator==(const PicaTargetKey&) const = default;
};

struct PicaTargetKeyHash {
    size_t operator()(const PicaTargetKey& key) const noexcept {
        uint64_t value = key.Namespace * 0x9E3779B97F4A7C15ULL;
        value ^= (static_cast<uint64_t>(key.ColorAddress) << 32U) | key.DepthAddress;
        value *= 0xBF58476D1CE4E5B9ULL;
        value ^= (static_cast<uint64_t>(key.Width) << 32U) | (static_cast<uint64_t>(key.Height) << 16U) |
                 (static_cast<uint64_t>(key.ColorFormat) << 8U) | key.DepthFormat;
        value *= 0x94D049BB133111EBULL;
        return static_cast<size_t>(value ^ (value >> 31U));
    }
};

[[nodiscard]] constexpr PicaTargetKey MakePicaTargetKey(const Renderer3ds::PicaDrawView& draw) noexcept {
    return {draw.RenderTargetNamespace,
            draw.FramebufferColorPhysicalAddress,
            draw.FramebufferDepthPhysicalAddress,
            static_cast<uint16_t>(draw.FramebufferWidth),
            static_cast<uint16_t>(draw.FramebufferHeight),
            draw.FramebufferColorFormat,
            draw.FramebufferDepthFormat};
}

// PicaGlScale::ScaledEdge / ScaledExtent (gfx_opengl_pica_scale.h).
[[nodiscard]] inline int32_t ScaledEdge(int64_t native, float scale) noexcept {
    return static_cast<int32_t>(std::llround(static_cast<double>(native) * static_cast<double>(scale)));
}

[[nodiscard]] inline uint32_t ScaledExtent(uint32_t native, float scale) noexcept {
    return static_cast<uint32_t>(std::max<int32_t>(native == 0U ? 0 : 1, ScaledEdge(native, scale)));
}

// Top LCD target (Oot3d::kNativeTopScreenPhysicalWidth/Height): 480x400
// stored rotated (its 400-pixel axis is the presented width); the only one
// widescreen widens.
constexpr uint32_t kTopScreenPhysicalWidth = 480U;
constexpr uint32_t kTopScreenPhysicalHeight = 400U;

struct PicaTargetExtent {
    uint32_t Width = 0;
    uint32_t Height = 0;
    float Scale = 1.0F;  // host pixels per native pixel along X
    float ScaleY = 1.0F; // along Y (wider on a widescreen top target)
    bool Wide = false;
    float WideFactor = 1.0F;
};

// GetPicaGlTarget's extent: the native extent times the scale; a top-screen
// target taller by the widescreen factor (> 1 only when enabled and the
// window is wider than 5:3).
[[nodiscard]] inline PicaTargetExtent PlanPicaTargetExtent(uint32_t width, uint32_t height, float scale,
                                                           float widescreenFactor, uint32_t topWidth,
                                                           uint32_t topHeight) noexcept {
    PicaTargetExtent extent;
    extent.Scale = scale;
    extent.Width = ScaledExtent(width, scale);
    extent.Height = ScaledExtent(height, scale);
    extent.ScaleY = scale;
    if (width == topWidth && height == topHeight && widescreenFactor > 1.0F) {
        extent.Height = static_cast<uint32_t>(std::lround(static_cast<float>(extent.Height) * widescreenFactor));
        extent.ScaleY = static_cast<float>(extent.Height) / static_cast<float>(height);
        extent.Wide = true;
        extent.WideFactor = widescreenFactor;
    }
    return extent;
}

struct PicaRect {
    int32_t X = 0;
    int32_t Y = 0;
    int32_t Width = 0;
    int32_t Height = 0;

    bool operator==(const PicaRect&) const = default;
};

// Viewport of a draw on a target that is not widened: both edges of the
// guest rectangle (truncated to integers first) scaled, as the GL backend.
[[nodiscard]] inline PicaRect PlanPicaViewport(float x, float y, float width, float height, float scale) noexcept {
    const auto vx = static_cast<int64_t>(static_cast<int32_t>(x));
    const auto vy = static_cast<int64_t>(static_cast<int32_t>(y));
    const auto vw = static_cast<int64_t>(static_cast<int32_t>(width));
    const auto vh = static_cast<int64_t>(static_cast<int32_t>(height));
    return {ScaledEdge(vx, scale), ScaledEdge(vy, scale), ScaledEdge(vx + vw, scale) - ScaledEdge(vx, scale),
            ScaledEdge(vy + vh, scale) - ScaledEdge(vy, scale)};
}

// Scissor (mode 3 = inclusive rectangle; anything else = the whole target).
[[nodiscard]] inline PicaRect PlanPicaScissor(uint8_t mode, uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2,
                                              float scale, uint32_t targetWidth, uint32_t targetHeight) noexcept {
    if (mode != 3U) {
        return {0, 0, static_cast<int32_t>(targetWidth), static_cast<int32_t>(targetHeight)};
    }
    const int64_t width = x2 >= x1 ? static_cast<int64_t>(x2) - x1 + 1 : 0;
    const int64_t height = y2 >= y1 ? static_cast<int64_t>(y2) - y1 + 1 : 0;
    const auto sx = static_cast<int64_t>(x1);
    const auto sy = static_cast<int64_t>(y1);
    return {ScaledEdge(sx, scale), ScaledEdge(sy, scale), ScaledEdge(sx + width, scale) - ScaledEdge(sx, scale),
            ScaledEdge(sy + height, scale) - ScaledEdge(sy, scale)};
}

// A deko3d scissor is unsigned: clip a rectangle to the target (an empty
// one stays empty, at the origin).
[[nodiscard]] inline PicaRect ClipPicaRect(const PicaRect& rect, uint32_t width, uint32_t height) noexcept {
    const int64_t x0 = std::clamp<int64_t>(rect.X, 0, width);
    const int64_t y0 = std::clamp<int64_t>(rect.Y, 0, height);
    const int64_t x1 = std::clamp<int64_t>(static_cast<int64_t>(rect.X) + std::max(0, rect.Width), 0, width);
    const int64_t y1 = std::clamp<int64_t>(static_cast<int64_t>(rect.Y) + std::max(0, rect.Height), 0, height);
    if (x1 <= x0 || y1 <= y0) {
        return {};
    }
    return {static_cast<int32_t>(x0), static_cast<int32_t>(y0), static_cast<int32_t>(x1 - x0),
            static_cast<int32_t>(y1 - y0)};
}

// ---- fills (ApplyPicaFill / DecodePicaClearColor of the GL backend) --------

[[nodiscard]] constexpr uint32_t PicaColorBytesPerPixel(uint8_t format) noexcept {
    return format == 0U ? 4U : (format == 1U ? 3U : 2U);
}

[[nodiscard]] constexpr uint32_t PicaDepthBytesPerPixel(uint8_t format) noexcept {
    return format == 0U ? 2U : (format == 2U ? 3U : 4U);
}

[[nodiscard]] constexpr uint32_t PicaFillElementSize(uint16_t control) noexcept {
    return (control & (1U << 9U)) != 0U ? 4U : ((control & (1U << 8U)) != 0U ? 3U : 2U);
}

[[nodiscard]] constexpr uint8_t PicaFillByte(const Renderer3ds::PicaMemoryFillView& fill, uint32_t index) noexcept {
    const uint32_t shift = (index % PicaFillElementSize(fill.Control)) * 8U;
    return static_cast<uint8_t>(fill.Value >> shift);
}

struct PicaFillCoverage {
    bool Color = false;
    bool Depth = false;
};

[[nodiscard]] constexpr PicaFillCoverage PicaFillCovers(const Renderer3ds::PicaMemoryFillView& fill,
                                                        const PicaTargetKey& key) noexcept {
    const uint64_t pixels = static_cast<uint64_t>(key.Width) * key.Height;
    const uint64_t colorEnd = static_cast<uint64_t>(key.ColorAddress) + pixels * PicaColorBytesPerPixel(key.ColorFormat);
    const uint64_t depthEnd = static_cast<uint64_t>(key.DepthAddress) + pixels * PicaDepthBytesPerPixel(key.DepthFormat);
    PicaFillCoverage coverage;
    coverage.Color = fill.StartPhysicalAddress <= key.ColorAddress && fill.EndPhysicalAddress >= colorEnd;
    coverage.Depth = key.DepthAddress != 0U && fill.StartPhysicalAddress <= key.DepthAddress &&
                     fill.EndPhysicalAddress >= depthEnd;
    return coverage;
}

[[nodiscard]] inline float PicaUnorm(uint32_t value, uint32_t maximum) noexcept {
    return static_cast<float>(value) / static_cast<float>(maximum);
}

[[nodiscard]] inline std::array<float, 4> DecodePicaClearColor(const Renderer3ds::PicaMemoryFillView& fill,
                                                               uint8_t format) noexcept {
    const uint8_t b0 = PicaFillByte(fill, 0U);
    const uint8_t b1 = PicaFillByte(fill, 1U);
    const uint8_t b2 = PicaFillByte(fill, 2U);
    const uint8_t b3 = PicaFillByte(fill, 3U);
    if (format == 0U) {
        return {PicaUnorm(b3, 0xFFU), PicaUnorm(b2, 0xFFU), PicaUnorm(b1, 0xFFU), PicaUnorm(b0, 0xFFU)};
    }
    if (format == 1U) {
        return {PicaUnorm(b2, 0xFFU), PicaUnorm(b1, 0xFFU), PicaUnorm(b0, 0xFFU), 1.0F};
    }
    const uint16_t packed = static_cast<uint16_t>(b0 | (b1 << 8U));
    if (format == 2U) {
        return {PicaUnorm((packed >> 11U) & 0x1FU, 0x1FU), PicaUnorm((packed >> 5U) & 0x3FU, 0x3FU),
                PicaUnorm(packed & 0x1FU, 0x1FU), 1.0F};
    }
    if (format == 3U) {
        return {PicaUnorm((packed >> 11U) & 0x1FU, 0x1FU), PicaUnorm((packed >> 6U) & 0x1FU, 0x1FU),
                PicaUnorm((packed >> 1U) & 0x1FU, 0x1FU), static_cast<float>(packed & 1U)};
    }
    return {PicaUnorm((packed >> 12U) & 0xFU, 0xFU), PicaUnorm((packed >> 8U) & 0xFU, 0xFU),
            PicaUnorm((packed >> 4U) & 0xFU, 0xFU), PicaUnorm(packed & 0xFU, 0xFU)};
}

[[nodiscard]] inline std::pair<float, uint8_t> DecodePicaClearDepthStencil(const Renderer3ds::PicaMemoryFillView& fill,
                                                                           uint8_t format) noexcept {
    const uint32_t b0 = PicaFillByte(fill, 0U);
    const uint32_t b1 = PicaFillByte(fill, 1U);
    const uint32_t b2 = PicaFillByte(fill, 2U);
    const uint32_t depth = format == 0U ? b0 | (b1 << 8U) : b0 | (b1 << 8U) | (b2 << 16U);
    return {PicaUnorm(depth, format == 0U ? 0xFFFFU : 0xFFFFFFU),
            format == 3U ? PicaFillByte(fill, 3U) : static_cast<uint8_t>(0U)};
}

// GL backend's DepthFillClearsStencil with GPU lean on (its default): a D24S8
// fill clears the stencil plane; without one nothing reads the stencil, so
// clearing it too (to the decoded 0) makes it a full depth/stencil clear.
[[nodiscard]] constexpr bool PicaDepthFillClearsStencil(uint8_t depthFormat) noexcept {
    (void)depthFormat;
    return true;
}

// ---- display transfer -------------------------------------------------------

struct PicaTransferPlan {
    bool Valid = false;
    const char* Error = nullptr;
    uint32_t OutputWidth = 0;  // native, in source pixels' terms
    uint32_t OutputHeight = 0;
    bool Linear = false;       // 2:1 box filter (scaling 1 or 2)
    uint32_t SourceWidth = 0;  // host pixels read from the target
    uint32_t SourceHeight = 0;
    uint32_t AllocWidth = 0;   // display image
    uint32_t AllocHeight = 0;
    uint32_t ContentWidth = 0; // host pixels written into it
    uint32_t ContentHeight = 0;
    bool Flip = false;         // scanout reads the image upside down
};

// SubmitPicaDisplayTransfer of the GL backend, minus dynamic resolution
// (scale == allocation scale): the extents of the copy into the display
// image (keyed by namespace and output address) and the scanout flip.
[[nodiscard]] inline PicaTransferPlan PlanPicaTransfer(const Renderer3ds::PicaDisplayTransferView& transfer,
                                                       uint32_t sourceNativeWidth, uint32_t sourceNativeHeight,
                                                       float scale, float scaleY, uint32_t targetWidth,
                                                       uint32_t targetHeight, float allocScale = 0.0F,
                                                       float allocScaleY = 0.0F) noexcept {
    PicaTransferPlan plan;
    const auto fail = [&](const char* why) {
        plan.Error = why;
        return plan;
    };
    if (transfer.InputPhysicalAddress == 0U || transfer.OutputPhysicalAddress == 0U || transfer.InputWidth == 0U ||
        transfer.InputHeight == 0U || transfer.OutputWidth == 0U || transfer.OutputHeight == 0U) {
        return fail("PICA display transfer is incomplete");
    }
    const uint32_t scaling = (transfer.Flags >> 24U) & 3U;
    if ((transfer.Flags & 8U) != 0U || (transfer.Flags & 0x10000U) != 0U || scaling > 2U) {
        return fail("PICA display transfer mode is unsupported");
    }
    if (transfer.InputWidth > sourceNativeWidth || transfer.InputHeight > sourceNativeHeight) {
        return fail("PICA display transfer exceeds its source");
    }
    plan.OutputWidth = std::max<uint32_t>(
        1U, static_cast<uint32_t>((static_cast<uint64_t>(transfer.OutputWidth) * sourceNativeWidth +
                                   transfer.InputWidth / 2U) /
                                  transfer.InputWidth));
    plan.OutputHeight = std::max<uint32_t>(
        1U, static_cast<uint32_t>((static_cast<uint64_t>(transfer.OutputHeight) * sourceNativeHeight +
                                   transfer.InputHeight / 2U) /
                                  transfer.InputHeight));
    const uint32_t horizontalSamples = scaling != 0U ? 2U : 1U;
    const uint32_t verticalSamples = scaling == 2U ? 2U : 1U;
    if (static_cast<uint64_t>(plan.OutputWidth) * horizontalSamples > sourceNativeWidth ||
        static_cast<uint64_t>(plan.OutputHeight) * verticalSamples > sourceNativeHeight) {
        return fail("PICA display transfer samples out of bounds");
    }
    const auto scaledY = [&](uint32_t native) {
        return static_cast<uint32_t>(std::lround(static_cast<float>(native) * scaleY));
    };
    plan.Linear = scaling != 0U;
    // Dynamic resolution (gl_pica_dynamic_scale.h): the source may be drawn
    // below its allocation scale. The display image keeps the allocation's
    // extent (never reallocated as the scale moves) and the copy fills its
    // bottom-left part, the content the scanout reads.
    if (!(allocScale > 0.0F)) {
        allocScale = scale;
    }
    if (!(allocScaleY > 0.0F)) {
        allocScaleY = scaleY;
    }
    plan.AllocWidth = ScaledExtent(plan.OutputWidth, allocScale);
    plan.AllocHeight = std::max<uint32_t>(
        1U, static_cast<uint32_t>(std::lround(static_cast<float>(plan.OutputHeight) * allocScaleY)));
    plan.ContentWidth = std::min(plan.AllocWidth, ScaledExtent(plan.OutputWidth, scale));
    plan.ContentHeight = std::min(plan.AllocHeight, std::max<uint32_t>(1U, scaledY(plan.OutputHeight)));
    plan.SourceWidth = std::min<uint32_t>(ScaledExtent(plan.OutputWidth * horizontalSamples, scale), targetWidth);
    plan.SourceHeight = std::min<uint32_t>(scaledY(plan.OutputHeight * verticalSamples), targetHeight);
    const bool inputLinear = (transfer.Flags & (1U << 1U)) != 0U;
    const bool dontSwizzle = (transfer.Flags & (1U << 5U)) != 0U;
    plan.Flip = (!inputLinear != (inputLinear != dontSwizzle)) ^ ((transfer.Flags & 1U) != 0U);
    plan.Valid = true;
    return plan;
}

struct PicaScanoutPlacement {
    PicaRect Viewport;
    bool CoversWindow = false;
};

// Presentation of a portrait display image on the window (GL scanout): the
// transposed logical extent, widened like its target, fitted and centred.
// `allocScale` is the source target's allocation scale.
[[nodiscard]] inline PicaScanoutPlacement PlanPicaScanout(uint32_t outputWidth, uint32_t outputHeight,
                                                          uint32_t displayAllocHeight, float allocScale,
                                                          uint32_t windowWidth, uint32_t windowHeight) noexcept {
    PicaScanoutPlacement placement;
    const float presentedWidth = static_cast<float>(outputHeight) * static_cast<float>(displayAllocHeight) /
                                 (static_cast<float>(outputHeight) * allocScale);
    const float presentedHeight = static_cast<float>(outputWidth);
    const float scale = std::min(static_cast<float>(windowWidth) / presentedWidth,
                                 static_cast<float>(windowHeight) / presentedHeight);
    int width = std::max(1, static_cast<int>(std::lround(presentedWidth * scale)));
    int height = std::max(1, static_cast<int>(std::lround(presentedHeight * scale)));
    const int ww = static_cast<int>(windowWidth);
    const int wh = static_cast<int>(windowHeight);
    if (std::abs(width - ww) <= 2) {
        width = ww;
    }
    if (std::abs(height - wh) <= 2) {
        height = wh;
    }
    placement.Viewport = {(ww - width) / 2, (wh - height) / 2, width, height};
    placement.CoversWindow = placement.Viewport.X <= 0 && placement.Viewport.Y <= 0 &&
                             placement.Viewport.X + width >= ww && placement.Viewport.Y + height >= wh;
    return placement;
}

} // namespace Fast::Deko3d
