// Plan build cost per draw (config/plan_full_identity, config/plan_copy_build,
// config/plan_one_lock; SWITCH_PORT.md "Plan build cost per draw") against a
// frozen copy of the builder as it was before them
// (oot3d_native_pica_vulkan_plan_reference.inc), on random draw streams:
//  1. every option set builds, field for field, the plan the old builder
//     built (the canonical identity is empty without it), with the same
//     result and error, across shader-cache hits and misses, TEV modes,
//     fog-table changes and plans reused from earlier draws;
//  2. parallel builds: the plan worker and resolves on several threads build
//     different slots of the same batches at once and every frame resolves
//     to the old builder's plans (run with TSan too);
// --bench: the build cost per draw of each option set, and the time a render
// thread spends resolving a freshly published batch with the worker running.
#include "oot3d_native_pica_deferred_plan.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace Reference {
using namespace Oot3dNativeGame;
inline void SetError(std::string* error, const char* message) {
    if (error != nullptr) *error = message;
}
#include "oot3d_native_pica_vulkan_plan_reference.inc"
} // namespace Reference

namespace {

using namespace Oot3dNativeGame;
using Clock = std::chrono::steady_clock;

[[noreturn]] void Fail(std::string_view message) {
    std::cerr << "oot3d_native_pica_plan_build_tests: " << message << '\n';
    std::exit(1);
}

void Require(bool condition, std::string_view message) {
    if (!condition) Fail(message);
}

// ------------------------------------------------------------- comparison

template <typename T> bool SameBits(const T& a, const T& b) {
    return std::memcmp(&a, &b, sizeof(T)) == 0;
}

// Field by field into bytes (padding never enters), as the visual savestate
// writes a draw state.
class FieldBytes {
  public:
    template <typename T> FieldBytes& operator<<(const T& value) {
        static_assert(std::is_trivially_copyable_v<T> && !std::is_class_v<T>);
        const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
        mBytes.insert(mBytes.end(), bytes, bytes + sizeof(T));
        return *this;
    }
    const std::vector<uint8_t>& Bytes() const { return mBytes; }

  private:
    std::vector<uint8_t> mBytes;
};

void AppendTexture(FieldBytes& out, const Oot3dPicaTextureState& t) {
    out << t.Enabled << t.Width << t.Height << t.PhysicalAddress << t.Format << t.Type << t.WrapS << t.WrapT
        << t.MinLinear << t.MagLinear << t.MipLinear << t.LodBiasRaw << t.MinMipLevel << t.MaxMipLevel;
}

std::vector<uint8_t> StateBytes(const Oot3dPicaDecodedDrawState& s) {
    FieldBytes out;
    const auto& v = s.VertexInput;
    out << v.PhysicalBaseAddress << v.AttributeCount;
    for (const auto& a : v.Attributes) out << a.Format << a.ComponentCount << a.Default;
    for (const auto& l : v.Loaders) {
        out << l.DataOffset << l.PhysicalAddress << l.ByteStride << l.ComponentCount;
        for (const auto c : l.Components) out << c;
    }
    out << v.Indexed << v.IndicesAre16Bit << v.IndexPhysicalAddress << v.VertexCount << v.VertexOffset;
    for (const auto& t : s.Textures) AppendTexture(out, t);
    out << s.Texture2UsesCoordinate1;
    const auto& f = s.Framebuffer;
    out << f.ColorPhysicalAddress << f.DepthPhysicalAddress << f.Width << f.Height << f.ColorFormat
        << f.DepthFormat << f.Flipped << f.ColorWriteEnabled << f.DepthStencilWriteEnabled;
    const auto& vp = s.Viewport;
    out << vp.HalfWidth << vp.HalfHeight << vp.DepthRange << vp.NearPlane << vp.CornerX << vp.CornerY
        << vp.ZBuffering;
    out << s.Scissor.Mode << s.Scissor.X1 << s.Scissor.Y1 << s.Scissor.X2 << s.Scissor.Y2 << s.CullMode;
    const auto& o = s.OutputMerger;
    out << o.FragmentOperationMode << o.ColorWriteMask << o.LogicOperation << o.Depth.TestEnabled
        << o.Depth.WriteEnabled << o.Depth.Compare << o.Stencil.Enabled << o.Stencil.Compare
        << o.Stencil.Reference << o.Stencil.CompareMask << o.Stencil.WriteMask << o.Stencil.Fail
        << o.Stencil.DepthFail << o.Stencil.Pass << o.Blend.Enabled << o.Blend.ColorEquation
        << o.Blend.AlphaEquation << o.Blend.SourceColor << o.Blend.DestinationColor << o.Blend.SourceAlpha
        << o.Blend.DestinationAlpha;
    for (const float c : o.Blend.ConstantColor) out << c;
    const auto& sh = s.ShaderInterface;
    out << sh.GeometryShaderEnabled << sh.VertexMainOffset << sh.MaximumInputAttribute;
    for (const auto input : sh.InputRegisterByAttribute) out << input;
    out << sh.OutputMask << s.Topology;
    return out.Bytes();
}

bool SameText(const std::shared_ptr<const std::string>& a, const std::shared_ptr<const std::string>& b) {
    return (a == nullptr) == (b == nullptr) && (a == nullptr || *a == *b);
}

bool SameFragmentUniforms(const Oot3dPicaFragmentUniformState& a, const Oot3dPicaFragmentUniformState& b) {
    return SameBits(a.TevConstants, b.TevConstants) && SameBits(a.CombinerBufferColor, b.CombinerBufferColor) &&
           a.AlphaReference == b.AlphaReference && SameBits(a.FogColor, b.FogColor) &&
           SameBits(a.FogLut, b.FogLut) && SameBits(a.TextureLodBias, b.TextureLodBias) &&
           SameBits(a.Lighting, b.Lighting) && a.ShadowTextureBias == b.ShadowTextureBias &&
           a.ShadowOrthographic == b.ShadowOrthographic && SameBits(a.ShadowBiasConstant, b.ShadowBiasConstant) &&
           SameBits(a.ShadowBiasLinear, b.ShadowBiasLinear) && SameBits(a.TevProgram, b.TevProgram) &&
           a.LightingProgram == b.LightingProgram && a.FragmentControl == b.FragmentControl &&
           SameBits(a.ProcTexProgram, b.ProcTexProgram);
}

// The first field of `actual` that differs from `expected`, or nothing.
// identity: the canonical identity is expected; otherwise it must be empty.
std::optional<std::string> PlanDifference(const Oot3dPicaVulkanDrawPlan& e, const Oot3dPicaVulkanDrawPlan& a,
                                          bool identity) {
    const auto differs = [](const char* field) { return std::optional<std::string>(field); };
    if (a.Deferred != nullptr || e.Deferred != nullptr) return differs("deferred");
    if (e.SubmissionId != a.SubmissionId || e.CommandListAddress != a.CommandListAddress ||
        e.CommandListOffsetWords != a.CommandListOffsetWords)
        return differs("submission/command list");
    if (e.CompositionDomain != a.CompositionDomain || !(e.Composition == a.Composition))
        return differs("composition");
    if (!(e.LightBinding == a.LightBinding)) return differs("light binding");
    if (identity ? !(e.CanonicalIdentity == a.CanonicalIdentity)
                 : !(a.CanonicalIdentity == Oot3dPicaCanonicalDrawIdentity{}))
        return differs("canonical identity");
    if (!(e.FragmentFeatures == a.FragmentFeatures)) return differs("fragment features");
    const auto& ev = e.VertexShader;
    const auto& av = a.VertexShader;
    if (ev.StateKey != av.StateKey || ev.Source != av.Source || !(ev.SourceIdentity == av.SourceIdentity))
        return differs("vertex shader");
    if ((ev.TemporalProgram == nullptr) != (av.TemporalProgram == nullptr) ||
        (ev.TemporalProgram != nullptr &&
         (ev.TemporalProgram->PreviousRegisterState != av.TemporalProgram->PreviousRegisterState ||
          ev.TemporalProgram->PreviousMainBody != av.TemporalProgram->PreviousMainBody ||
          !(ev.TemporalProgram->Hooks == av.TemporalProgram->Hooks) ||
          !(ev.TemporalProgram->AffineLayout == av.TemporalProgram->AffineLayout))))
        return differs("vertex temporal program");
    if (ev.Uniforms.BooleanMask != av.Uniforms.BooleanMask || ev.Uniforms.Integers != av.Uniforms.Integers ||
        !SameBits(ev.Uniforms.Floats, av.Uniforms.Floats))
        return differs("vertex uniforms");
    const auto& ef = e.FragmentShader;
    const auto& af = a.FragmentShader;
    if (ef.StateKey != af.StateKey) return differs("fragment shader key");
    if (ef.Source != af.Source) return differs("fragment shader source");
    if (!(ef.SourceIdentity == af.SourceIdentity)) return differs("fragment shader identity");
    if (!(ef.Hooks == af.Hooks)) return differs("fragment shader hooks");
    if (!SameFragmentUniforms(ef.Uniforms, af.Uniforms)) return differs("fragment uniforms");
    if (!SameText(e.SharedVertexShaderSource, a.SharedVertexShaderSource) ||
        !SameText(e.SharedFragmentShaderSource, a.SharedFragmentShaderSource))
        return differs("shared shader sources");
    if (e.UberFragmentShader.StateKey != a.UberFragmentShader.StateKey ||
        !(e.UberFragmentShader.SourceIdentity == a.UberFragmentShader.SourceIdentity) ||
        !(e.UberFragmentShader.Hooks == a.UberFragmentShader.Hooks) ||
        !SameText(e.SharedUberFragmentShaderSource, a.SharedUberFragmentShaderSource))
        return differs("uber fragment shader");
    if (StateBytes(e.State) != StateBytes(a.State)) return differs("decoded state");
    if (e.VertexBindings.size() != a.VertexBindings.size()) return differs("vertex binding count");
    for (size_t i = 0; i < e.VertexBindings.size(); ++i) {
        const auto& x = e.VertexBindings[i];
        const auto& y = a.VertexBindings[i];
        if (x.Binding != y.Binding || x.ByteStride != y.ByteStride || x.InputRate != y.InputRate ||
            x.Bytes != y.Bytes || (x.SharedBytes == nullptr) != (y.SharedBytes == nullptr) ||
            !std::ranges::equal(x.ResolvedBytes(), y.ResolvedBytes()) ||
            x.SourcePhysicalAddress != y.SourcePhysicalAddress || x.ContentVersion != y.ContentVersion ||
            x.ContentVersionAvailable != y.ContentVersionAvailable)
            return differs("vertex binding");
    }
    if (e.VertexAttributes.size() != a.VertexAttributes.size()) return differs("vertex attribute count");
    for (size_t i = 0; i < e.VertexAttributes.size(); ++i) {
        const auto& x = e.VertexAttributes[i];
        const auto& y = a.VertexAttributes[i];
        if (x.Location != y.Location || x.Binding != y.Binding || x.Format != y.Format ||
            x.ComponentCount != y.ComponentCount || x.ByteOffset != y.ByteOffset)
            return differs("vertex attribute");
    }
    if (e.IndexBytes != a.IndexBytes || (e.SharedIndexBytes == nullptr) != (a.SharedIndexBytes == nullptr) ||
        !std::ranges::equal(e.ResolvedIndexBytes(), a.ResolvedIndexBytes()) ||
        e.IndexPhysicalAddress != a.IndexPhysicalAddress || e.IndexContentVersion != a.IndexContentVersion ||
        e.IndexContentVersionAvailable != a.IndexContentVersionAvailable)
        return differs("index buffer");
    if (e.Indexed != a.Indexed || e.IndicesAre16Bit != a.IndicesAre16Bit || e.BaseVertex != a.BaseVertex ||
        e.VertexCount != a.VertexCount)
        return differs("draw range");
    if (e.Textures.size() != a.Textures.size()) return differs("texture count");
    for (size_t i = 0; i < e.Textures.size(); ++i) {
        const auto& x = e.Textures[i];
        const auto& y = a.Textures[i];
        FieldBytes xs, ys;
        AppendTexture(xs, x.State);
        AppendTexture(ys, y.State);
        if (x.Slot != y.Slot || xs.Bytes() != ys.Bytes() || x.NativeBytes != y.NativeBytes ||
            (x.SharedNativeBytes == nullptr) != (y.SharedNativeBytes == nullptr) ||
            !std::ranges::equal(x.ResolvedNativeBytes(), y.ResolvedNativeBytes()) ||
            x.NativeContentHash != y.NativeContentHash ||
            x.NativeContentHashAvailable != y.NativeContentHashAvailable ||
            x.NativeBaseLevelContentHash != y.NativeBaseLevelContentHash ||
            x.NativeBaseLevelContentHashAvailable != y.NativeBaseLevelContentHashAvailable ||
            x.CopyReferences.size() != y.CopyReferences.size())
            return differs("texture");
        for (size_t r = 0; r < x.CopyReferences.size(); ++r) {
            const auto& p = x.CopyReferences[r];
            const auto& q = y.CopyReferences[r];
            if (p.CompletionId != q.CompletionId || p.AfterDrawSubmissionId != q.AfterDrawSubmissionId ||
                p.PhysicalAddress != q.PhysicalAddress || p.ByteCount != q.ByteCount)
                return differs("texture copy reference");
        }
    }
    if ((e.LightingLuts == nullptr) != (a.LightingLuts == nullptr) ||
        (e.LightingLuts != nullptr && (e.LightingLuts->ContentHash != a.LightingLuts->ContentHash ||
                                       e.LightingLuts->PackedEntries != a.LightingLuts->PackedEntries)))
        return differs("lighting LUTs");
    if (e.GeometryIdentity != a.GeometryIdentity || e.GeometryContentVersion != a.GeometryContentVersion ||
        e.GeometryIdentityAvailable != a.GeometryIdentityAvailable)
        return differs("geometry contract");
    if (ComputeOot3dPicaVisualDrawIdentity(e) != ComputeOot3dPicaVisualDrawIdentity(a))
        return differs("visual identity");
    return std::nullopt;
}

// ------------------------------------------------------------- draw stream

// A frontend's live state drifting draw by draw: uniforms, TEV, fog,
// lighting, textures and vertex layouts change at random, shader programs
// are re-uploaded now and then (a new mutation identity: a structural-cache
// miss). Two streams with the same seed submit the same draws.
class DrawStream {
  public:
    // allowInvalid: now and then draws the generators refuse (any TEV words,
    // gas fog, procedural textures with random tables). steady: a scene
    // between loads, every structural choice from a small pool (the shader
    // caches end up hitting), only uniforms, colours and buffers drifting.
    explicit DrawStream(uint64_t seed, bool allowInvalid = true, bool steady = false)
        : mRandom(seed), mAllowInvalid(allowInvalid && !steady), mSteady(steady) {
        mLive.Indexed = true;
        mLive.CompositionDomain = Oot3dPicaCompositionDomain::Scene;
        mLive.VertexShader.Program[0] = 0x88000000U;
        mLive.VertexShader.ProgramWordCount = 1U;
        mLive.VertexShader.SwizzleWordCount = 1U;
        mLive.Registers[0x04F] = 1U;
        mLive.Registers[0x050] = 0x03020100U;
        mLive.Registers[0x2BD] = 1U;
        for (auto& row : mLive.VertexShader.FloatUniforms)
            for (auto& value : row) value = Float();
        for (size_t i = 0; i < mLive.FogLut.size(); ++i) mLive.FogLut[i] = Word();
        for (uint32_t r = 0x140U; r <= 0x1D9U; ++r) mLive.Registers[r] = Word();
        mLive.Registers[0x0E0U] = 5U;
        for (uint32_t i = 0; i < 6U; ++i) {
            mVertexBytes.push_back(std::make_shared<const std::vector<uint8_t>>(Bytes(256U + i * 96U)));
            mTextureBytes.push_back(std::make_shared<const std::vector<uint8_t>>(Bytes(16U * 16U * 4U)));
        }
        NewLuts();
        // Few distinct TEV and lighting configurations, as a scene has: the
        // fragment cache mostly hits, with a miss for each new combination.
        for (auto& program : mTevPool)
            for (auto& stage : program) stage = TevStage();
        // Lighting setups (config0, config1, light permutation, light count):
        // shapes the generators accept and, in a stream allowing invalid
        // draws, one setup of any words.
        mLightingPool[0] = {0x10U, 0xFFFFFFFEU & ~(1U << 16U), 0x76543210U, 0U};
        mLightingPool[1] = {0x0U, 0xFFFFFFFEU & ~(1U << 16U), 0x76543210U, 1U};
        mLightingPool[2] = {0x10U, 0xFFFFFFFEU, 0x01234567U, 2U};
        mLightingPool[3] = mAllowInvalid ? std::array<uint32_t, 4>{Word(), Word(), Word(), 1U}
                                         : std::array<uint32_t, 4>{0x0U, 0xFFFFFFFEU, 0x76543210U, 0U};
    }

    Oot3dPicaDrawSubmission Next() {
        Drift();
        Oot3dPicaDrawSubmission submission(mNextId++, mLive);
        submission.Packet.CommandListAddress = Word();
        submission.Packet.CommandListOffsetWords = Below(4096U);
        submission.Packet.Composition.SourcePc = Word();
        submission.Packet.Composition.NativeActorIdentity = Below(3U) == 0U ? Word() : 0U;
        auto& state = submission.State;
        state.Framebuffer.ColorPhysicalAddress = Below(4U) == 0U ? 0x1F200000U : 0x1F000000U;
        state.Framebuffer.Width = 240U;
        state.Framebuffer.Height = 400U;
        state.Viewport.HalfWidth = Below(2U) ? 200.0F : 160.0F;
        state.Viewport.HalfHeight = 120.0F;
        state.Viewport.DepthRange = mSteady || Below(2U) ? -1.0F : 1.0F;
        state.Viewport.ZBuffering = mSteady || Below(2U) != 0U;
        // Decoded from the registers, as the frontend does (the shader keys
        // read the register, the generators the decoded state).
        state.OutputMerger.FragmentOperationMode = static_cast<uint8_t>(mLive.Registers[0x100U] & 3U);
        state.OutputMerger.Blend.Enabled = Below(2U) != 0U;
        state.OutputMerger.Blend.ConstantColor = {Float(), Float(), Float(), Float()};
        state.CullMode = static_cast<decltype(state.CullMode)>(Below(3U));

        // Vertex layout: one or two loaders over float/short/byte attributes,
        // some attributes left to their defaults (the fixed binding).
        auto& vertex = state.VertexInput;
        vertex.AttributeCount = static_cast<uint8_t>(2U + Below(5U));
        static constexpr std::array<Oot3dPicaVertexFormat, 4> kFormats{
            Oot3dPicaVertexFormat::Float, Oot3dPicaVertexFormat::SignedShort,
            Oot3dPicaVertexFormat::SignedByte, Oot3dPicaVertexFormat::UnsignedByte};
        for (uint8_t i = 0; i < vertex.AttributeCount; ++i) {
            vertex.Attributes[i] = {i == 0U ? Oot3dPicaVertexFormat::Float : kFormats[Below(4U)],
                                    static_cast<uint8_t>(i == 0U ? 3U : 1U + Below(4U)),
                                    i != 0U && Below(3U) == 0U};
            state.ShaderInterface.InputRegisterByAttribute[i] = static_cast<uint8_t>(
                (i + Below(2U) * 4U) % 16U);
        }
        state.ShaderInterface.OutputMask = 1U;
        const size_t loaders = 1U + Below(2U);
        size_t attribute = 0U;
        for (size_t loader = 0; loader < loaders; ++loader) {
            auto& l = vertex.Loaders[loader];
            size_t bytes = 0;
            while (attribute < vertex.AttributeCount && l.ComponentCount < 3U) {
                if (vertex.Attributes[attribute].Default) {
                    ++attribute;
                    continue;
                }
                const size_t size = vertex.Attributes[attribute].Format == Oot3dPicaVertexFormat::Float ? 4U
                                    : vertex.Attributes[attribute].Format ==
                                            Oot3dPicaVertexFormat::SignedShort
                                        ? 2U
                                        : 1U;
                bytes = (bytes + size - 1U) / size * size + size * vertex.Attributes[attribute].ComponentCount;
                l.Components[l.ComponentCount++] = static_cast<uint8_t>(attribute++);
                if (Below(3U) == 0U) break;
            }
            if (l.ComponentCount == 0U) break;
            l.ByteStride = static_cast<uint8_t>((bytes + 3U) / 4U * 4U + Below(2U) * 4U);
            Oot3dPicaResourceSnapshot snapshot;
            snapshot.Kind = Oot3dPicaResourceKind::VertexLoader;
            snapshot.Slot = static_cast<uint8_t>(loader);
            snapshot.PhysicalAddress = 0x20000000U + Below(64U) * 0x100U;
            if (Below(2U) != 0U) {
                snapshot.SharedBytes = mVertexBytes[Below(mVertexBytes.size())];
            } else {
                snapshot.Bytes = Bytes(l.ByteStride * (8U + Below(16U)));
            }
            snapshot.ContentVersion = Word();
            snapshot.ContentVersionAvailable = Below(4U) != 0U;
            submission.Resources.push_back(std::move(snapshot));
        }
        for (; attribute < vertex.AttributeCount; ++attribute) vertex.Attributes[attribute].Default = true;
        vertex.Indexed = Below(5U) != 0U;
        vertex.IndicesAre16Bit = Below(2U) != 0U;
        vertex.VertexCount = 3U + Below(300U);
        submission.Packet.Indexed = vertex.Indexed;
        if (vertex.Indexed) {
            submission.MinimumVertexIndex = Below(8U);
            submission.MaximumVertexIndex = submission.MinimumVertexIndex + 8U;
            Oot3dPicaResourceSnapshot indices;
            indices.Kind = Oot3dPicaResourceKind::IndexBuffer;
            indices.PhysicalAddress = 0x20100000U + Below(64U) * 0x40U;
            auto bytes = Bytes(vertex.VertexCount * (vertex.IndicesAre16Bit ? 2U : 1U));
            if (Below(2U) != 0U) {
                indices.SharedBytes = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
            } else {
                indices.Bytes = std::move(bytes);
            }
            indices.ContentVersion = Word();
            indices.ContentVersionAvailable = Below(4U) != 0U;
            submission.Resources.push_back(std::move(indices));
        }

        // Textures as the TEV enables them: RGBA8 16x16 with one mip level
        // when the registers say so, with or without capture hashes and GPU
        // copy producers.
        for (uint8_t unit = 0; unit < 3U; ++unit) {
            if ((mLive.Registers[0x080U] & (1U << unit)) == 0U) continue;
            auto& texture = state.Textures[unit];
            texture.Enabled = true;
            texture.Width = 16U;
            texture.Height = 16U;
            texture.Format = 0U;
            texture.Type = unit == 0U && !mSteady && Below(16U) == 0U ? 2U : 0U;
            texture.WrapS = static_cast<uint8_t>(Below(4U));
            texture.MinLinear = Below(2U) != 0U;
            texture.PhysicalAddress = 0x18000000U + Below(16U) * 0x1000U;
            Oot3dPicaResourceSnapshot snapshot;
            snapshot.Kind = Oot3dPicaResourceKind::Texture;
            snapshot.Slot = unit;
            snapshot.PhysicalAddress = texture.PhysicalAddress;
            if (Below(3U) != 0U) {
                snapshot.SharedBytes = mTextureBytes[Below(mTextureBytes.size())];
            } else {
                snapshot.Bytes = Bytes(16U * 16U * 4U);
            }
            if (Below(4U) != 0U) {
                snapshot.ContentHash = Word();
                snapshot.ContentHashAvailable = true;
                if (Below(2U) != 0U) {
                    snapshot.BaseLevelContentHash = snapshot.ContentHash;
                    snapshot.BaseLevelContentHashAvailable = true;
                }
            }
            if (Below(8U) == 0U)
                snapshot.CopyReferences.push_back({Word(), mNextId - 2U, texture.PhysicalAddress, 1024U});
            submission.Resources.push_back(std::move(snapshot));
        }
        return submission;
    }

  private:
    uint32_t Word() { return static_cast<uint32_t>(mRandom()); }
    size_t Below(size_t bound) { return static_cast<size_t>(mRandom() % bound); }
    float Float() { return static_cast<float>(static_cast<int32_t>(Word() % 20001U) - 10000) / 64.0F; }
    std::vector<uint8_t> Bytes(size_t size) {
        std::vector<uint8_t> bytes(size);
        for (auto& byte : bytes) byte = static_cast<uint8_t>(mRandom());
        return bytes;
    }

    void NewLuts() {
        auto luts = std::make_shared<Oot3dPicaLightingLutState>();
        for (size_t i = 0; i < 64U; ++i) luts->PackedEntries[Below(luts->PackedEntries.size())] = Word();
        luts->ContentHash = ComputeOot3dPicaLightingLutContentHash(*luts);
        luts->ContentHashAvailable = true;
        mLive.LightingLuts = std::move(luts);
    }

    // A TEV stage's source, operand and operation words over what the
    // generators accept.
    std::array<uint32_t, 3> TevStage() {
        static constexpr std::array<uint32_t, 8> kSources{0U, 1U, 2U, 3U, 4U, 5U, 0xEU, 0xFU};
        uint32_t sources = 0U;
        for (uint32_t i = 0; i < 6U; ++i) sources |= kSources[Below(kSources.size())] << (i * 4U + (i >= 3U ? 4U : 0U));
        static constexpr std::array<uint32_t, 6> kOperands{0U, 1U, 2U, 3U, 4U, 5U};
        uint32_t operands = 0U;
        for (uint32_t i = 0; i < 9U; ++i) operands |= (i < 3U ? kOperands[Below(6U)] : Below(4U)) << (i * 4U);
        const uint32_t color = static_cast<uint32_t>(Below(10U));
        const uint32_t alpha = static_cast<uint32_t>(std::array<uint32_t, 8>{0U, 1U, 2U, 3U, 4U, 5U, 8U, 9U}[Below(8U)]);
        return {sources, operands, color | (alpha << 16U)};
    }

    // A material's TEV program from the scene's pool; (rarely) one stage of
    // any words at all, until the next material.
    void SetTevProgram() {
        static constexpr std::array<uint16_t, 6> kStages{0x0C0U, 0x0C8U, 0x0D0U, 0x0D8U, 0x0F0U, 0x0F8U};
        const size_t index = Below(mTevPool.size());
        for (size_t stage = 0; stage < kStages.size(); ++stage) {
            const uint16_t base = kStages[stage];
            mLive.Registers[base] = mTevPool[index][stage][0];
            mLive.Registers[base + 1U] = mTevPool[index][stage][1];
            mLive.Registers[base + 2U] = mTevPool[index][stage][2];
            mLive.Registers[base + 3U] = Word();
            mLive.Registers[base + 4U] = index % 3U == 0U ? 0x00010001U : 0U;
        }
        if (mAllowInvalid && Below(64U) == 0U) {
            for (uint16_t r = 0; r < 3U; ++r) mLive.Registers[kStages[Below(6U)] + r] = Word();
        }
    }

    void Drift() {
        // Per-draw: a model matrix and a constant colour.
        for (size_t row = 0; row < 4U; ++row) {
            const size_t r = Below(96U);
            mLive.VertexShader.FloatUniforms[r] = {Float(), Float(), Float(), Float()};
        }
        mLive.VertexShader.BooleanUniforms[Below(16U)] = Below(2U) != 0U;
        mLive.VertexShader.IntegerUniforms[Below(4U)][Below(4U)] = static_cast<uint8_t>(Word());
        static constexpr std::array<uint16_t, 6> kStages{0x0C0U, 0x0C8U, 0x0D0U, 0x0D8U, 0x0F0U, 0x0F8U};
        mLive.Registers[kStages[Below(6U)] + 3U] = Word();
        if (Below(4U) == 0U) SetTevProgram();
        if (Below(12U) == 0U) {
            mLive.Registers[0x080U] = static_cast<uint32_t>(std::array<uint32_t, 4>{0U, 1U, 1U, 3U}[Below(4U)]) |
                                      (mAllowInvalid && Below(16U) == 0U ? (1U << 10U) : 0U) |
                                      (Below(4U) == 0U ? (1U << 13U) : 0U);
            mLive.Registers[0x083U] = Word();
            mLive.Registers[0x084U] = Word() & 0x1FFFU;
            mLive.Registers[0x094U] = Word() & 0x1FFFU;
        }
        if (Below(6U) == 0U) {
            const auto& lighting = mLightingPool[Below(mLightingPool.size())];
            mLive.Registers[0x08FU] = Below(2U);
            mLive.Registers[0x1C2U] = lighting[3];
            mLive.Registers[0x1C3U] = lighting[0];
            mLive.Registers[0x1C4U] = lighting[1];
            mLive.Registers[0x1D9U] = lighting[2];
        }
        if (Below(4U) == 0U) {
            // Light colours, positions and directions (not the per-light
            // configuration word, which is structural).
            for (int i = 0; i < 8; ++i) mLive.Registers[0x140U + Below(8U) * 0x10U + Below(8U)] = Word();
        }
        if (!mSteady && Below(40U) == 0U) {
            static constexpr std::array<uint32_t, 3> kFog{0U, 5U, 5U};
            const uint32_t mode = mAllowInvalid && Below(12U) == 0U ? 7U : kFog[Below(3U)];
            mLive.Registers[0x0E0U] = mode | (Below(2U) << 16U);
            mLive.Registers[0x0E1U] = Word();
        }
        // The fog table changes with the scene's fog only.
        if (!mSteady && Below(40U) == 0U) {
            for (size_t i = 0; i < mLive.FogLut.size(); ++i) mLive.FogLut[i] = Word();
        }
        if (!mSteady && Below(40U) == 0U) {
            mLive.Registers[0x100U + Below(8U)] = Word();
            mLive.Registers[0x100U] = (mLive.Registers[0x100U] & ~3U) | (Below(6U) == 0U ? 3U : 0U);
            mLive.Registers[0x0FDU] = Word();
            mLive.Registers[0x104U] = Word() & 0xFF71U;
            mLive.Registers[0x08BU] = Word();
            mLive.Registers[0x130U] = Word();
        }
        if (Below(10U) == 0U) mLive.DefaultAttributes[Below(16U)] = {Float(), Float(), Float(), 1.0F};
        if (mSteady) return;
        if (Below(30U) == 0U) NewLuts();
        if (Below(25U) == 0U) {
            // A program upload: the same words under a new mutation identity.
            mLive.VertexShader.Program[0] = 0x88000000U;
        }
        if (Below(50U) == 0U) mLive.Registers[0x229U] = Below(2U) ? 0U : 0x00000100U;
    }

    std::mt19937_64 mRandom;
    bool mAllowInvalid = true;
    bool mSteady = false;
    Oot3dPicaDrawPacket mLive;
    std::vector<std::shared_ptr<const std::vector<uint8_t>>> mVertexBytes;
    std::vector<std::shared_ptr<const std::vector<uint8_t>>> mTextureBytes;
    std::array<std::array<std::array<uint32_t, 3>, 6>, 6> mTevPool{};
    std::array<std::array<uint32_t, 4>, 4> mLightingPool{};
    uint64_t mNextId = 1U;
};

struct CacheSetup {
    Oot3dPicaTevMode Mode = Oot3dPicaTevMode::Specialized;
    bool Uber = false;
    const char* Name = "";
};

// Parametric vertex programs need the title's translated programs, which a
// synthetic stream does not have; the parametric fragment programs come in
// through the uber pair.
constexpr std::array<CacheSetup, 2> kCacheSetups{{
    {Oot3dPicaTevMode::Specialized, true, "specialized+uber"},
    {Oot3dPicaTevMode::Specialized, false, "specialized"},
}};

void Configure(Oot3dPicaVulkanShaderSourceCache& cache, const CacheSetup& setup) {
    cache.TevMode = setup.Mode;
    cache.UberFragments = setup.Uber;
}

// ------------------------------------------------------------ 1. option sets

void TestOptionSets(uint64_t seed, size_t draws) {
    struct Variant {
        Oot3dPicaPlanBuildOptions Options;
        std::mutex Mutex;
        Oot3dPicaVulkanShaderSourceCache Cache;
        std::optional<DrawStream> Stream;
        Oot3dPicaVulkanDrawPlan Plan;  // reused draw after draw, as in a batch
    };
    for (const auto& setup : kCacheSetups) {
        Oot3dPicaVulkanShaderSourceCache referenceCache;
        Configure(referenceCache, setup);
        DrawStream referenceStream(seed);
        Oot3dPicaVulkanDrawPlan referencePlan;
        std::vector<std::unique_ptr<Variant>> variants;
        for (const bool identity : {true, false})
            for (const bool inPlace : {false, true})
                for (const bool locked : {false, true}) {
                    auto variant = std::make_unique<Variant>();
                    variant->Options.CanonicalIdentity = identity;
                    variant->Options.InPlace = inPlace;
                    variant->Options.CacheMutex = locked ? &variant->Mutex : nullptr;
                    Configure(variant->Cache, setup);
                    variant->Stream.emplace(seed);
                    variants.push_back(std::move(variant));
                }
        size_t built = 0U;
        size_t failed = 0U;
        for (size_t draw = 0; draw < draws; ++draw) {
            auto submission = referenceStream.Next();
            std::string referenceError;
            const bool referenceBuilt = Reference::BuildOot3dPicaVulkanDrawPlanImpl<true>(
                submission, referencePlan, &referenceError, &referenceCache);
            (referenceBuilt ? built : failed) += 1U;
            for (size_t v = 0; v < variants.size(); ++v) {
                auto& variant = *variants[v];
                auto mine = variant.Stream->Next();
                std::string error;
                const bool ok = BuildOot3dPicaVulkanDrawPlanAndConsumeResources(mine, variant.Plan, &error,
                                                                               &variant.Cache, variant.Options);
                const std::string where = std::string(setup.Name) + " seed " + std::to_string(seed) +
                                          " draw " + std::to_string(draw) + " identity=" +
                                          std::to_string(variant.Options.CanonicalIdentity) +
                                          " in_place=" + std::to_string(variant.Options.InPlace) +
                                          " locked=" + std::to_string(variant.Options.CacheMutex != nullptr);
                Require(ok == referenceBuilt && error == referenceError,
                        "build result differs (" + where + "): " + error + " vs " + referenceError);
                // The resources the build consumed (or left) are the same.
                Require(mine.Resources.size() == submission.Resources.size(), "consumed resources differ " + where);
                for (size_t r = 0; r < mine.Resources.size(); ++r) {
                    Require(mine.Resources[r].Bytes == submission.Resources[r].Bytes &&
                                (mine.Resources[r].SharedBytes == nullptr) ==
                                    (submission.Resources[r].SharedBytes == nullptr) &&
                                mine.Resources[r].CopyReferences.size() ==
                                    submission.Resources[r].CopyReferences.size(),
                            "consumed resources differ " + where);
                }
                if (!referenceBuilt) continue;
                if (const auto difference =
                        PlanDifference(referencePlan, variant.Plan, variant.Options.CanonicalIdentity))
                    Fail("plan differs (" + where + "): " + *difference);
            }
        }
        for (const auto& variant : variants) {
            Require(variant->Cache.VertexHits == referenceCache.VertexHits &&
                        variant->Cache.VertexMisses == referenceCache.VertexMisses &&
                        variant->Cache.FragmentHits == referenceCache.FragmentHits &&
                        variant->Cache.FragmentMisses == referenceCache.FragmentMisses &&
                        variant->Cache.VertexStructuralStates.size() == referenceCache.VertexStructuralStates.size() &&
                        variant->Cache.UberFragmentsBySpecializedKey.size() ==
                            referenceCache.UberFragmentsBySpecializedKey.size() &&
                        variant->Cache.FixedBindingBytes.size() == referenceCache.FixedBindingBytes.size(),
                    std::string("shader cache learned differently (") + setup.Name + ")");
        }
        Require(built > draws / 2U && referenceCache.VertexMisses > 1U && referenceCache.FragmentMisses > 4U &&
                    referenceCache.FragmentHits > draws / 4U,
                std::string("stream too narrow to compare (") + setup.Name + "): built " + std::to_string(built) +
                    " failed " + std::to_string(failed) + " vertex misses " +
                    std::to_string(referenceCache.VertexMisses) + " fragment misses " +
                    std::to_string(referenceCache.FragmentMisses) + " hits " +
                    std::to_string(referenceCache.FragmentHits));
        std::printf("option sets %-17s seed=%llu draws=%zu built=%zu failed=%zu vertex misses=%llu fragment "
                    "misses=%llu\n",
                    setup.Name, static_cast<unsigned long long>(seed), draws, built, failed,
                    static_cast<unsigned long long>(referenceCache.VertexMisses),
                    static_cast<unsigned long long>(referenceCache.FragmentMisses));
    }
}

// ------------------------------------------- shader keys determine the source

// Parallel builds fill the shader cache in the order builds reach it, not in
// draw order: a key must determine everything its generator reads, or which
// draw comes first would pick the program. Every draw of the stream whose key
// was seen before must generate the same program as the first one.
void TestKeysDetermineSources(uint64_t seed, size_t draws) {
    using Identity = Oot3d::Renderer::PicaShaderSourceIdentity;
    std::map<uint64_t, Identity> vertex;
    std::map<uint64_t, Identity> fragment;
    std::map<uint64_t, std::optional<Identity>> uber;
    DrawStream stream(seed);
    size_t compared = 0U;
    for (size_t draw = 0; draw < draws; ++draw) {
        const auto submission = stream.Next();
        const auto& packet = submission.Packet;
        const auto& state = submission.State;
        std::string error;
        Oot3dPicaGeneratedVertexShader vertexShader;
        if (GenerateOot3dPicaVertexShader(packet, state, vertexShader, &error)) {
            const auto [entry, inserted] = vertex.emplace(vertexShader.StateKey, vertexShader.SourceIdentity);
            Require(inserted || entry->second == vertexShader.SourceIdentity,
                    "a vertex shader key does not determine its program (draw " + std::to_string(draw) + ")");
            compared += inserted ? 0U : 1U;
        }
        const uint64_t key = ComputeOot3dPicaFragmentShaderStateKey(packet, state, Oot3dPicaTevMode::Specialized);
        Oot3dPicaGeneratedFragmentShader specialized;
        if (GenerateOot3dPicaFragmentShader(packet, state, specialized, &error,
                                            Oot3dPicaShaderBuildPurpose::RuntimeDraw,
                                            Oot3dPicaTevMode::Specialized)) {
            Require(specialized.StateKey == key, "fragment key and generated key differ");
            const auto [entry, inserted] = fragment.emplace(key, specialized.SourceIdentity);
            Require(inserted || entry->second == specialized.SourceIdentity,
                    "a fragment shader key does not determine its program (draw " + std::to_string(draw) + ")");
            compared += inserted ? 0U : 1U;
            Oot3dPicaGeneratedFragmentShader parametric;
            std::optional<Identity> pair;
            if (GenerateOot3dPicaFragmentShader(packet, state, parametric, &error,
                                                Oot3dPicaShaderBuildPurpose::RuntimeDraw,
                                                Oot3dPicaTevMode::Parametric))
                pair = parametric.SourceIdentity;
            const auto [uberEntry, uberInserted] = uber.emplace(key, pair);
            Require(uberInserted || uberEntry->second == pair,
                    "a fragment key does not determine its parametric pair (draw " + std::to_string(draw) + ")");
        }
    }
    Require(compared > draws / 2U, "too few repeated keys to compare");
    std::printf("shader keys seed=%llu draws=%zu: %zu vertex, %zu fragment programs, %zu repeats compared\n",
                static_cast<unsigned long long>(seed), draws, vertex.size(), fragment.size(), compared);
}

// ------------------------------------------------- 2. parallel deferred builds

// Batches published one after another while the worker builds and `resolvers`
// threads resolve each published frame at once (several frames in flight on
// different threads, as the render thread and the dependency flush would);
// every resolved plan equals the old builder's.
void TestParallelResolve(uint64_t seed, size_t frames, size_t drawsPerFrame, size_t resolvers, bool worker,
                         bool parallel) {
    // The expected plans, serially with the old builder.
    std::vector<Oot3dPicaVulkanDrawPlan> expected;
    std::vector<std::string> expectedErrors;
    {
        Oot3dPicaVulkanShaderSourceCache cache;
        Configure(cache, kCacheSetups[0]);
        DrawStream stream(seed, false);
        for (size_t i = 0; i < frames * drawsPerFrame; ++i) {
            auto submission = stream.Next();
            Oot3dPicaVulkanDrawPlan plan;
            std::string error;
            if (!Reference::BuildOot3dPicaVulkanDrawPlanImpl<true>(submission, plan, &error, &cache)) {
                plan = {};
            }
            expected.push_back(std::move(plan));
            expectedErrors.push_back(error);
        }
    }
    Oot3dPicaVulkanShaderSourceCache cache;
    Configure(cache, kCacheSetups[0]);
    Oot3dPicaDeferredPlanBuilder builder(cache);
    builder.SetBuildOptions(false, true, parallel);
    builder.Configure(true, worker);
    DrawStream stream(seed, false);
    std::vector<Oot3dPicaVisualFrame> published(frames);
    std::atomic<size_t> ready{0};
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    std::string failure;
    std::mutex failureMutex;
    std::vector<std::thread> threads;
    for (size_t t = 0; t < resolvers; ++t) {
        threads.emplace_back([&] {
            for (;;) {
                const size_t frame = next.fetch_add(1U);
                if (frame >= frames) return;
                while (ready.load(std::memory_order_acquire) <= frame) std::this_thread::yield();
                std::string error;
                auto& draws = published[frame];
                const bool ok = ResolveOot3dPicaDeferredVisualFrame(draws, &error);
                // A frame holding a draw the builder refuses fails to resolve
                // (fatal in the session); the others resolve completely.
                const bool frameFails = std::any_of(
                    expectedErrors.begin() + static_cast<std::ptrdiff_t>(frame * drawsPerFrame),
                    expectedErrors.begin() + static_cast<std::ptrdiff_t>((frame + 1U) * drawsPerFrame),
                    [](const std::string& e) { return !e.empty(); });
                for (size_t i = 0; i < draws.Draws.size(); ++i) {
                    const size_t global = frame * drawsPerFrame + i;
                    std::optional<std::string> difference;
                    if (frameFails) {
                        if (ok) difference = "a frame with a failing build resolved";
                    } else if (!ok) {
                        difference = "resolve failed: " + error;
                    } else {
                        difference = PlanDifference(expected[global], draws.Draws[i], false);
                    }
                    if (difference) {
                        std::lock_guard lock(failureMutex);
                        failure = "frame " + std::to_string(frame) + " draw " + std::to_string(i) + ": " +
                                  *difference;
                        failed = true;
                        return;
                    }
                    if (frameFails) break;
                }
            }
        });
    }
    for (size_t frame = 0; frame < frames; ++frame) {
        std::vector<Oot3dPicaDrawSubmission> drained;
        for (size_t i = 0; i < drawsPerFrame; ++i) drained.push_back(stream.Next());
        auto batch = builder.BeginBatch(drained);
        auto& draws = published[frame];
        draws.Draws.resize(drawsPerFrame);
        for (size_t i = 0; i < drawsPerFrame; ++i) builder.FillDeferredPlan(batch, i, draws.Draws[i]);
        builder.Publish(std::move(batch));
        ready.store(frame + 1U, std::memory_order_release);
    }
    for (auto& thread : threads) thread.join();
    Require(!failed, "parallel resolve: " + failure);
    builder.StopWorker();
    const auto stats = builder.Stats();
    std::printf("parallel=%d worker=%d resolvers=%zu seed=%llu frames=%zu: worker builds %llu, late builds %llu, "
                "slot waits %llu\n",
                parallel ? 1 : 0, worker ? 1 : 0, resolvers, static_cast<unsigned long long>(seed), frames,
                static_cast<unsigned long long>(stats.WorkerBuilds),
                static_cast<unsigned long long>(stats.ConsumerBuilds),
                static_cast<unsigned long long>(stats.ConsumerSlotWaits));
    const bool anyFails = std::any_of(expectedErrors.begin(), expectedErrors.end(),
                                      [](const std::string& e) { return !e.empty(); });
    const uint64_t builds = stats.WorkerBuilds + stats.ConsumerBuilds;
    Require(anyFails ? builds <= frames * drawsPerFrame : builds == frames * drawsPerFrame,
            "every slot is built exactly once");
}

// ------------------------------------------------------------------ bench

// A lit, fogged, textured CMB-like draw stream with steady caches, as on the
// console between scene changes.
double BenchBuild(const Oot3dPicaPlanBuildOptions* options, size_t draws) {
    Oot3dPicaVulkanShaderSourceCache cache;
    cache.UberFragments = true;
    DrawStream stream(99U, false, true);
    std::vector<Oot3dPicaDrawSubmission> warm;
    for (size_t i = 0; i < 6000U; ++i) warm.push_back(stream.Next());
    std::vector<Oot3dPicaDrawSubmission> timed;
    for (size_t i = 0; i < draws; ++i) timed.push_back(stream.Next());
    std::vector<Oot3dPicaVulkanDrawPlan> plans(64U);
    std::string error;
    for (size_t i = 0; i < warm.size(); ++i) {
        if (options == nullptr)
            (void)Reference::BuildOot3dPicaVulkanDrawPlanImpl<true>(warm[i], plans[i % plans.size()], &error, &cache);
        else
            (void)BuildOot3dPicaVulkanDrawPlanAndConsumeResources(warm[i], plans[i % plans.size()], &error, &cache,
                                                                  *options);
    }
    const auto started = Clock::now();
    for (size_t i = 0; i < timed.size(); ++i) {
        if (options == nullptr)
            (void)Reference::BuildOot3dPicaVulkanDrawPlanImpl<true>(timed[i], plans[i % plans.size()], &error,
                                                                   &cache);
        else
            (void)BuildOot3dPicaVulkanDrawPlanAndConsumeResources(timed[i], plans[i % plans.size()], &error, &cache,
                                                                  *options);
    }
    const double ns =
        std::chrono::duration<double, std::nano>(Clock::now() - started).count() / static_cast<double>(draws);
    static bool reported = false;
    if (!reported) {
        reported = true;
        std::printf("bench stream: fragment hits %llu misses %llu, vertex hits %llu misses %llu\n",
                    static_cast<unsigned long long>(cache.FragmentHits),
                    static_cast<unsigned long long>(cache.FragmentMisses),
                    static_cast<unsigned long long>(cache.VertexHits),
                    static_cast<unsigned long long>(cache.VertexMisses));
    }
    return ns;
}

// One frame of `draws` published at once (the bush case: the whole step's
// draws arrive with the frame), resolved by this thread while the worker
// builds: the render thread's resolve time.
double BenchResolve(bool parallel, bool identity, bool inPlace, size_t draws, size_t frames) {
    Oot3dPicaVulkanShaderSourceCache cache;
    cache.UberFragments = true;
    Oot3dPicaDeferredPlanBuilder builder(cache);
    builder.SetBuildOptions(identity, inPlace, parallel);
    builder.Configure(true, true);
    DrawStream stream(7U, false, true);
    double total = 0.0;
    constexpr size_t kWarmFrames = 60U;
    for (size_t frame = 0; frame < frames + kWarmFrames; ++frame) {
        std::vector<Oot3dPicaDrawSubmission> drained;
        for (size_t i = 0; i < draws; ++i) drained.push_back(stream.Next());
        auto batch = builder.BeginBatch(drained);
        Oot3dPicaVisualFrame visual;
        visual.Draws.resize(draws);
        for (size_t i = 0; i < draws; ++i) builder.FillDeferredPlan(batch, i, visual.Draws[i]);
        builder.Publish(std::move(batch));
        const auto started = Clock::now();
        std::string error;
        (void)ResolveOot3dPicaDeferredVisualFrame(visual, &error);
        if (frame >= kWarmFrames) total += std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    builder.StopWorker();
    return total / static_cast<double>(frames);
}

void Bench() {
    constexpr size_t kDraws = 20000U;
    Oot3dPicaPlanBuildOptions current;
    Oot3dPicaPlanBuildOptions lean;
    lean.CanonicalIdentity = false;
    Oot3dPicaPlanBuildOptions inPlace;
    inPlace.InPlace = true;
    Oot3dPicaPlanBuildOptions both;
    both.CanonicalIdentity = false;
    both.InPlace = true;
    std::mutex mutex;
    Oot3dPicaPlanBuildOptions bothLocked = both;
    bothLocked.CacheMutex = &mutex;
    const double reference = BenchBuild(nullptr, kDraws);
    std::printf("build ns/draw: reference %.0f, defaults %.0f, no identity %.0f, in place %.0f, both %.0f, "
                "both + cache mutex %.0f\n",
                reference, BenchBuild(&current, kDraws), BenchBuild(&lean, kDraws), BenchBuild(&inPlace, kDraws),
                BenchBuild(&both, kDraws), BenchBuild(&bothLocked, kDraws));
    for (const size_t draws : {85U, 133U}) {
        std::printf("resolve of a %zu-draw frame published at once, ms: one lock %.3f, parallel %.3f, "
                    "parallel + lean %.3f\n",
                    draws, BenchResolve(false, true, false, draws, 60U), BenchResolve(true, true, false, draws, 60U),
                    BenchResolve(true, false, true, draws, 60U));
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--bench") {
        Bench();
        return 0;
    }
    for (const uint64_t seed : {1U, 2U, 3U, 4U}) TestOptionSets(seed, 1500U);
    for (const uint64_t seed : {5U, 6U}) TestKeysDetermineSources(seed, 3000U);
    for (const uint64_t seed : {11U, 12U}) {
        TestParallelResolve(seed, 40U, 40U, 1U, true, true);
        TestParallelResolve(seed, 40U, 40U, 3U, true, true);
        TestParallelResolve(seed, 40U, 40U, 2U, false, true);
        TestParallelResolve(seed, 20U, 40U, 1U, true, false);
    }
    std::printf("oot3d_native_pica_plan_build_tests: ok\n");
    return 0;
}
