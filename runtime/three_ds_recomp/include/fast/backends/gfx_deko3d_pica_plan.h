#pragma once

// Package 4 of the deko3d backend (docs/DEKO3D_PROGRESSO.md): what a PICA
// draw uploads, as plain decisions free of deko3d so the host checks them
// (oot3d_deko3d_backend_tests). The console side (gfx_deko3d_pica_resources.cpp)
// turns them into DkImage/DkSampler descriptors, stream memory and vertex
// state.
//
// Every rule mirrors the GL backend (gfx_opengl_pica.cpp) so both backends
// sample and fetch the same values: ToPicaGlWrap, PicaGlMinFilter, the
// sampler LOD range, the vertex validation and glVertexAttribPointer with
// normalized = false.

#include "fast/renderer3ds/pica_native_state.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Fast::Deko3d {

// ---- samplers -------------------------------------------------------------

enum class PicaWrap : uint8_t {
    ClampToEdge,
    Repeat,
    MirroredRepeat,
    Unsupported, // clamp-to-border, asymmetric and invalid modes (GL throws)
};

[[nodiscard]] constexpr PicaWrap MapPicaWrap(uint8_t wrap) noexcept {
    switch (wrap) {
        case 0U: return PicaWrap::ClampToEdge;
        case 2U:
        case 6U:
        case 7U: return PicaWrap::Repeat;
        case 3U: return PicaWrap::MirroredRepeat;
        default: return PicaWrap::Unsupported;
    }
}

enum class PicaMipFilter : uint8_t { None, Nearest, Linear };

struct PicaSamplerKey {
    bool MinLinear = false;
    bool MagLinear = false;
    PicaMipFilter Mip = PicaMipFilter::None;
    PicaWrap WrapS = PicaWrap::ClampToEdge;
    PicaWrap WrapT = PicaWrap::ClampToEdge;
    int16_t LodBiasRaw = 0; // 1/256 units, as the GL backend's GL_TEXTURE_LOD_BIAS
    uint8_t MinLevel = 0;   // GL_TEXTURE_BASE_LEVEL
    uint8_t MaxLevel = 0;   // GL_TEXTURE_MAX_LEVEL (clamped to the uploaded chain)

    bool operator==(const PicaSamplerKey&) const = default;
};

struct PicaSamplerKeyHash {
    size_t operator()(const PicaSamplerKey& key) const noexcept {
        uint64_t value = static_cast<uint64_t>(key.MinLinear) | (static_cast<uint64_t>(key.MagLinear) << 1U) |
                         (static_cast<uint64_t>(key.Mip) << 2U) | (static_cast<uint64_t>(key.WrapS) << 4U) |
                         (static_cast<uint64_t>(key.WrapT) << 6U) |
                         (static_cast<uint64_t>(static_cast<uint16_t>(key.LodBiasRaw)) << 8U) |
                         (static_cast<uint64_t>(key.MinLevel) << 24U) | (static_cast<uint64_t>(key.MaxLevel) << 32U);
        value *= 0x9E3779B97F4A7C15ULL;
        return static_cast<size_t>(value ^ (value >> 32U));
    }
};

// mipLevels: levels actually uploaded (PicaTextureMipCount).
[[nodiscard]] constexpr PicaSamplerKey MakePicaSamplerKey(const Renderer3ds::PicaTextureView& texture,
                                                          uint32_t mipLevels) noexcept {
    PicaSamplerKey key;
    key.MinLinear = texture.MinLinear;
    key.MagLinear = texture.MagLinear;
    key.Mip = mipLevels <= 1U ? PicaMipFilter::None
                              : (texture.MipLinear ? PicaMipFilter::Linear : PicaMipFilter::Nearest);
    key.WrapS = MapPicaWrap(texture.NativeWrapS);
    key.WrapT = MapPicaWrap(texture.NativeWrapT);
    key.LodBiasRaw = texture.LodBiasRaw;
    const uint32_t top = mipLevels == 0U ? 0U : mipLevels - 1U;
    key.MaxLevel = static_cast<uint8_t>(std::min<uint32_t>(texture.MaxMipLevel, top));
    key.MinLevel = static_cast<uint8_t>(std::min<uint32_t>(texture.MinMipLevel, key.MaxLevel));
    return key;
}

// ---- textures -------------------------------------------------------------

// Same identity as the GL backend's PicaGlTextureKey: content, placement,
// format and sampler state (the sampler is part of the GL texture object).
struct PicaTextureKey {
    uint64_t ContentHash = 0;
    uint32_t PhysicalAddress = 0;
    uint16_t Width = 0;
    uint16_t Height = 0;
    uint8_t Format = 0;
    uint8_t Type = 0;
    uint8_t WrapS = 0;
    uint8_t WrapT = 0;
    bool MinLinear = false;
    bool MagLinear = false;
    bool MipLinear = false;
    int16_t LodBiasRaw = 0;
    uint8_t MinMipLevel = 0;
    uint8_t MaxMipLevel = 0;

    bool operator==(const PicaTextureKey&) const = default;
};

struct PicaTextureKeyHash {
    size_t operator()(const PicaTextureKey& key) const noexcept {
        uint64_t value = key.ContentHash;
        const auto mix = [&](uint64_t part) {
            value ^= part + 0x9E3779B97F4A7C15ULL + (value << 6U) + (value >> 2U);
        };
        mix(key.PhysicalAddress);
        mix((static_cast<uint64_t>(key.Width) << 16U) | key.Height);
        mix((static_cast<uint64_t>(key.Format) << 24U) | (static_cast<uint64_t>(key.Type) << 16U) |
            (static_cast<uint64_t>(key.WrapS) << 8U) | key.WrapT);
        mix((static_cast<uint64_t>(key.MinLinear) << 2U) | (static_cast<uint64_t>(key.MagLinear) << 1U) |
            static_cast<uint64_t>(key.MipLinear));
        mix((static_cast<uint64_t>(static_cast<uint16_t>(key.LodBiasRaw)) << 16U) |
            (static_cast<uint64_t>(key.MinMipLevel) << 8U) | key.MaxMipLevel);
        return static_cast<size_t>(value);
    }
};

[[nodiscard]] constexpr PicaTextureKey MakePicaTextureKey(const Renderer3ds::PicaTextureView& texture,
                                                          uint64_t contentHash) noexcept {
    return {contentHash,         texture.PhysicalAddress, texture.Width,     texture.Height,
            texture.NativeFormat, texture.NativeType,     texture.NativeWrapS, texture.NativeWrapT,
            texture.MinLinear,   texture.MagLinear,       texture.MipLinear, texture.LodBiasRaw,
            texture.MinMipLevel, texture.MaxMipLevel};
}

enum class PicaTextureUse : uint8_t {
    Color,       // RGBA8 image from the decoded mips
    Shadow2D,    // integer Shadow2D image: not in the first version (fallback)
    Unsupported, // a type the GL backend rejects too
};

[[nodiscard]] constexpr PicaTextureUse ClassifyPicaTexture(const Renderer3ds::PicaTextureView& texture) noexcept {
    if (texture.NativeType == 0U) {
        return PicaTextureUse::Color;
    }
    if (texture.Slot == 0U && (texture.NativeType == 2U || texture.NativeType == 3U)) {
        return texture.NativeType == 2U ? PicaTextureUse::Shadow2D : PicaTextureUse::Color;
    }
    return PicaTextureUse::Unsupported;
}

constexpr uint32_t kMaxPicaMipLevels = 16U;

struct PicaMipPlan {
    uint32_t Width = 0;
    uint32_t Height = 0;
    uint32_t Offset = 0; // in the RGBA8 staging block
    uint32_t Bytes = 0;
};

// RGBA8 staging layout of a mip chain, each level `alignment`-aligned.
// Returns the total bytes (0 for an empty or oversized chain).
[[nodiscard]] inline uint32_t PlanPicaRgba8Mips(uint32_t width, uint32_t height, uint32_t levels,
                                                uint32_t alignment, std::array<PicaMipPlan, kMaxPicaMipLevels>& out) {
    if (width == 0U || height == 0U || levels == 0U || levels > kMaxPicaMipLevels || alignment == 0U) {
        return 0U;
    }
    uint64_t offset = 0U;
    for (uint32_t level = 0U; level < levels; ++level) {
        auto& mip = out[level];
        mip.Width = std::max(1U, width >> level);
        mip.Height = std::max(1U, height >> level);
        offset = (offset + alignment - 1U) / alignment * alignment;
        mip.Offset = static_cast<uint32_t>(offset);
        mip.Bytes = mip.Width * mip.Height * 4U;
        offset += mip.Bytes;
        if (offset > 0xFFFFFFFFULL) {
            return 0U;
        }
    }
    return static_cast<uint32_t>(offset);
}

// ---- descriptor slots -----------------------------------------------------

// Indices of a descriptor array. Slots below `reserved` are fixed (fallback
// images); freed slots are reused LIFO. A slot is freed only once nothing in
// flight can still read it (the caller's eviction keeps two frames).
template <uint32_t Capacity> class DescriptorSlots {
  public:
    explicit constexpr DescriptorSlots(uint32_t reserved = 0U) noexcept : mNext(std::min(reserved, Capacity)) {}

    // Capacity when full.
    [[nodiscard]] uint32_t Allocate() noexcept {
        if (mFreeCount != 0U) {
            return mFree[--mFreeCount];
        }
        return mNext < Capacity ? mNext++ : Capacity;
    }

    void Free(uint32_t slot) noexcept {
        if (slot < mNext && mFreeCount < Capacity) {
            mFree[mFreeCount++] = slot;
        }
    }

    [[nodiscard]] uint32_t Live() const noexcept {
        return mNext - mFreeCount;
    }

  private:
    std::array<uint32_t, Capacity> mFree{};
    uint32_t mFreeCount = 0;
    uint32_t mNext = 0;
};

// ---- vertices -------------------------------------------------------------

constexpr uint32_t kPicaVertexAttributeCount = 16U;
// Vertex buffer slot of the constant (0, 0, 0, 1) every absent attribute
// reads, like a disabled GL attribute array; PICA bindings stay below it.
constexpr uint32_t kPicaConstantVertexBinding = 15U;
constexpr uint32_t kPicaVertexStreamAlignment = 16U;

enum class PicaAttributeKind : uint8_t {
    Constant, // absent: the constant buffer
    SignedByte,
    UnsignedByte,
    SignedShort,
    Float,
};

struct PicaAttributePlan {
    PicaAttributeKind Kind = PicaAttributeKind::Constant;
    uint8_t Components = 4;
    uint8_t Binding = kPicaConstantVertexBinding;
    uint16_t Offset = 0;
};

struct PicaVertexPlan {
    std::array<PicaAttributePlan, kPicaVertexAttributeCount> Attributes{};
    // Offset of each binding's bytes in the draw's stream block.
    std::array<uint32_t, kPicaVertexAttributeCount> BindingOffset{};
    std::array<uint16_t, kPicaVertexAttributeCount> BindingStride{};
    std::array<bool, kPicaVertexAttributeCount> BindingPerInstance{};
    uint16_t BindingMask = 0;
    uint16_t AttributeMask = 0;
    uint32_t StreamBytes = 0;
    const char* Error = nullptr;
};

[[nodiscard]] constexpr PicaAttributeKind ToPicaAttributeKind(Renderer3ds::PicaVertexFormat format) noexcept {
    switch (format) {
        case Renderer3ds::PicaVertexFormat::SignedByte: return PicaAttributeKind::SignedByte;
        case Renderer3ds::PicaVertexFormat::UnsignedByte: return PicaAttributeKind::UnsignedByte;
        case Renderer3ds::PicaVertexFormat::SignedShort: return PicaAttributeKind::SignedShort;
        case Renderer3ds::PicaVertexFormat::Float: return PicaAttributeKind::Float;
    }
    return PicaAttributeKind::Constant;
}

// False (with Error) on what the GL backend rejects as invalid.
[[nodiscard]] inline bool PlanPicaVertices(std::span<const Renderer3ds::PicaVertexBindingView> bindings,
                                           std::span<const Renderer3ds::PicaVertexAttributeView> attributes,
                                           PicaVertexPlan& plan) {
    plan = {};
    uint64_t offset = 0U;
    for (const auto& binding : bindings) {
        if (binding.Binding >= kPicaConstantVertexBinding || binding.ByteStride == 0U || binding.Bytes.empty()) {
            plan.Error = "PICA vertex binding is invalid";
            return false;
        }
        // A repeated binding keeps its last bytes, as in the GL backend.
        plan.BindingMask |= static_cast<uint16_t>(1U << binding.Binding);
        offset = (offset + kPicaVertexStreamAlignment - 1U) / kPicaVertexStreamAlignment * kPicaVertexStreamAlignment;
        plan.BindingOffset[binding.Binding] = static_cast<uint32_t>(offset);
        plan.BindingStride[binding.Binding] = binding.ByteStride;
        plan.BindingPerInstance[binding.Binding] = binding.PerInstance;
        offset += binding.Bytes.size();
        if (offset > 0x7FFFFFFFULL) {
            plan.Error = "PICA vertex streams too large";
            return false;
        }
    }
    plan.StreamBytes = static_cast<uint32_t>(offset);
    for (const auto& attribute : attributes) {
        if (attribute.Location >= kPicaVertexAttributeCount || attribute.Binding >= kPicaVertexAttributeCount ||
            attribute.ComponentCount == 0U || attribute.ComponentCount > 4U) {
            plan.Error = "PICA vertex attribute is invalid";
            return false;
        }
        if ((plan.BindingMask & (1U << attribute.Binding)) == 0U) {
            plan.Error = "PICA vertex attribute has no binding";
            return false;
        }
        // DkVtxAttribState::offset has 14 bits.
        if (attribute.ByteOffset >= (1U << 14U)) {
            plan.Error = "PICA vertex attribute offset too large";
            return false;
        }
        auto& slot = plan.Attributes[attribute.Location];
        slot.Kind = ToPicaAttributeKind(attribute.Format);
        slot.Components = attribute.ComponentCount;
        slot.Binding = attribute.Binding;
        slot.Offset = attribute.ByteOffset;
        plan.AttributeMask |= static_cast<uint16_t>(1U << attribute.Location);
    }
    return true;
}

// ---- uniforms -------------------------------------------------------------

// deko3d uniform buffers: 256-byte aligned addresses, at most 64 KiB bound.
constexpr uint32_t kDekoUniformAlignment = 0x100U;
constexpr uint32_t kDekoUniformMaxBytes = 0x10000U;

[[nodiscard]] constexpr uint32_t PicaUniformBindBytes(size_t bytes) noexcept {
    const uint64_t aligned = (static_cast<uint64_t>(bytes) + kDekoUniformAlignment - 1U) / kDekoUniformAlignment *
                             kDekoUniformAlignment;
    return static_cast<uint32_t>(std::min<uint64_t>(aligned, kDekoUniformMaxBytes));
}

} // namespace Fast::Deko3d
