// The deko3d backend's per-pixel decision for one PICA draw (which draws are
// relit and the surface block they read). See gfx_deko3d_lighting.h. Pure
// code: the backend (gfx_deko3d.cpp) and the host tests
// (oot3d_deko3d_inbetween_lighting_tests) run the same function.
#include "fast/backends/gfx_deko3d_lighting.h"

#include "fast/oot3d/directional_shadows.h"
#include "fast/oot3d/pica_rim_light.h"
#include "fast/oot3d/pica_uniform_layout.h"
#include "fast/renderer3ds/pica_composition.h"
#include "fast/renderer3ds/pica_render_backend.h"
#include "fast/renderer3ds/pica_surface_appearance.h"

#include <cstdio>
#include <cstring>

namespace Fast::Deko3d {

const char* PerPixelRefusalName(PerPixelRefusal refusal) noexcept {
    switch (refusal) {
        case PerPixelRefusal::None: return "none";
        case PerPixelRefusal::NotSceneWorld: return "not_scene_world";
        case PerPixelRefusal::NoSurfaceProgram: return "no_surface_program";
        case PerPixelRefusal::NoDepthWrite: return "no_depth_write";
        case PerPixelRefusal::FragmentOperation: return "fragment_operation";
        case PerPixelRefusal::NoUniforms: return "no_uniforms";
        case PerPixelRefusal::NotPerspective: return "not_perspective";
        case PerPixelRefusal::NotRelightable: return "not_relightable";
        case PerPixelRefusal::NativeLights: return "native_lights";
        case PerPixelRefusal::NoCamera: return "no_camera";
        case PerPixelRefusal::Block: return "block";
        case PerPixelRefusal::Count: break;
    }
    return "?";
}

std::string FormatFrameKindLighting(const FrameKindLighting& kind) {
    std::string text = "frames=" + std::to_string(kind.Samples) + " lit=" + std::to_string(kind.Lit) +
                       " (no env " + std::to_string(kind.LitWithoutEnvironment) + ", no rim " +
                       std::to_string(kind.LitWithoutRim) + ") native=" + std::to_string(kind.Native) +
                       " ineligible=" + std::to_string(kind.Ineligible) +
                       " first=" + PerPixelRefusalName(kind.FirstRefusal);
    bool open = false;
    for (size_t index = 1; index < kind.Refusals.size(); ++index) {
        if (kind.Refusals[index] == 0U) continue;
        text += open ? " " : " (";
        text += PerPixelRefusalName(static_cast<PerPixelRefusal>(index));
        text += '=' + std::to_string(kind.Refusals[index]);
        open = true;
    }
    if (open) text += ')';
    uint64_t lost = 0;
    for (const uint64_t count : kind.LostWorld) lost += count;
    text += " lost world=" + std::to_string(lost);
    open = false;
    for (size_t index = 0; index < kind.LostWorld.size(); ++index) {
        if (kind.LostWorld[index] == 0U) continue;
        text += open ? " " : " (";
        text += LostWorldCompositionName(static_cast<LostWorldComposition>(index));
        text += '=' + std::to_string(kind.LostWorld[index]);
        open = true;
    }
    if (open) text += ')';
    text += " transients=" + std::to_string(kind.Transients);
    if (kind.FirstLit) {
        const auto& block = *kind.FirstLit;
        char summary[96];
        std::snprintf(summary, sizeof(summary), " block control=%u/%u/%u/%u env=%g rim=%g baked=%g",
                      block.Control[0], block.Control[1], block.Control[2], block.Control[3],
                      static_cast<double>(block.Environment), static_cast<double>(block.Rim),
                      static_cast<double>(block.Baked));
        text += summary;
    }
    return text;
}

const char* LostWorldCompositionName(LostWorldComposition lost) noexcept {
    switch (lost) {
        case LostWorldComposition::UiList: return "ui_list";
        case LostWorldComposition::UiSpan: return "ui_span";
        case LostWorldComposition::NoSpan: return "no_span";
        case LostWorldComposition::ControlFlow: return "control_flow";
        case LostWorldComposition::Atmosphere: return "atmosphere";
        case LostWorldComposition::Other: return "other";
        case LostWorldComposition::Count: break;
    }
    return "?";
}

std::optional<LostWorldComposition> ClassifyLostWorldComposition(const Renderer3ds::PicaDrawView& draw,
                                                                PerPixelRefusal refusal) noexcept {
    using Renderer3ds::PicaCompositionDomain;
    using Renderer3ds::PicaCompositionLayer;
    using Renderer3ds::PicaCompositionProvenance;
    const auto& hooks = draw.TemporalVertexProgram.Hooks;
    if (refusal != PerPixelRefusal::NotSceneWorld || hooks.SurfaceProgram() == nullptr ||
        hooks.TransformProgram() == nullptr || !draw.DepthWriteEnabled)
        return std::nullopt;
    const auto& composition = draw.Composition;
    if (draw.CompositionDomain == PicaCompositionDomain::Ui || composition.Layer == PicaCompositionLayer::Ui) {
        // The frontend's attribution of a list submitted in the HUD phase:
        // UI lifecycle, no source PC (oot3d_native_pica_frontend.cpp).
        return composition.Provenance == PicaCompositionProvenance::NativeUiLifecycle && composition.SourcePc == 0U
                   ? LostWorldComposition::UiList
                   : LostWorldComposition::UiSpan;
    }
    if (composition.Layer == PicaCompositionLayer::Unknown) {
        return composition.Provenance == PicaCompositionProvenance::NativeControlFlow
                   ? LostWorldComposition::ControlFlow
                   : LostWorldComposition::NoSpan;
    }
    if (composition.Layer == PicaCompositionLayer::Atmosphere) return LostWorldComposition::Atmosphere;
    return LostWorldComposition::Other;
}

uint32_t LightingSample::LostTotal() const noexcept {
    uint32_t total = 0;
    for (const uint32_t count : Lost) total += count;
    return total;
}

uint32_t LightingSample::LostToComposition() const noexcept {
    return Lost[static_cast<size_t>(LostWorldComposition::UiList)] +
           Lost[static_cast<size_t>(LostWorldComposition::UiSpan)] +
           Lost[static_cast<size_t>(LostWorldComposition::NoSpan)] +
           Lost[static_cast<size_t>(LostWorldComposition::ControlFlow)];
}

bool IsLightingTransient(const LightingSample& before, const LightingSample& sample,
                         const LightingSample& after) noexcept {
    if (before.Lit < 4U || after.Lit < 4U) return false;
    const auto more = [](uint32_t value, uint32_t a, uint32_t b) { return value > std::max(a, b); };
    return uint64_t{sample.Lit} * 4U < uint64_t{std::min(before.Lit, after.Lit)} * 3U ||
           more(sample.LitWithoutEnvironment, before.LitWithoutEnvironment, after.LitWithoutEnvironment) ||
           more(sample.LitWithoutRim, before.LitWithoutRim, after.LitWithoutRim) ||
           more(sample.Native, before.Native, after.Native) ||
           more(sample.LostToComposition(), before.LostToComposition(), after.LostToComposition());
}

std::string FormatLightingTransient(const LightingTransient& transient) {
    const auto& b = transient.Before;
    const auto& s = transient.Sample;
    const auto& a = transient.After;
    char text[384];
    int length = std::snprintf(
        text, sizeof(text),
        "frame %llu (%s) lit %u/%u/%u, no env %u/%u/%u, no rim %u/%u/%u, native %u/%u/%u, lost world %u/%u/%u",
        static_cast<unsigned long long>(s.DeviceFrame), s.InBetween ? "in-between" : "real", b.Lit, s.Lit, a.Lit,
        b.LitWithoutEnvironment, s.LitWithoutEnvironment, a.LitWithoutEnvironment, b.LitWithoutRim,
        s.LitWithoutRim, a.LitWithoutRim, b.Native, s.Native, a.Native, b.LostTotal(), s.LostTotal(),
        a.LostTotal());
    std::string result(text, static_cast<size_t>(std::clamp(length, 0, static_cast<int>(sizeof(text)) - 1)));
    if (s.LostTotal() == 0U) return result;
    bool open = false;
    for (size_t index = 0; index < s.Lost.size(); ++index) {
        if (s.Lost[index] == 0U) continue;
        result += open ? " " : " (";
        result += LostWorldCompositionName(static_cast<LostWorldComposition>(index));
        result += '=' + std::to_string(s.Lost[index]);
        open = true;
    }
    length = std::snprintf(text, sizeof(text),
                           "; first domain=%u layer=%u provenance=%u pc=%08x actor=%llx list=%08x+%u)",
                           s.LostDomain, s.LostLayer, s.LostProvenance, s.LostSourcePc,
                           static_cast<unsigned long long>(s.LostActor), s.LostCommandList, s.LostOffsetWords);
    result.append(text, static_cast<size_t>(std::clamp(length, 0, static_cast<int>(sizeof(text)) - 1)));
    return result;
}

std::optional<LightingTransient> LightingTransientWatch::Begin(uint64_t deviceFrame, bool inBetween) {
    std::optional<LightingTransient> transient;
    if (mOpened) {
        if (mClosedCount == 2U && IsLightingTransient(mClosed[0], mClosed[1], mOpen))
            transient = LightingTransient{mClosed[0], mClosed[1], mOpen};
        mClosed[0] = mClosed[1];
        mClosed[1] = mOpen;
        mClosedCount = std::min<size_t>(mClosedCount + 1U, 2U);
    }
    mOpen = {};
    mOpen.DeviceFrame = deviceFrame;
    mOpen.InBetween = inBetween;
    mOpened = true;
    mNamespace.reset();
    return transient;
}

bool LightingTransientWatch::Counts(const Renderer3ds::PicaDrawView& draw) noexcept {
    if (!mOpened) return false;
    if (!mNamespace) mNamespace = draw.RenderTargetNamespace;
    return *mNamespace == draw.RenderTargetNamespace;
}

void LightingTransientWatch::Lit(const Renderer3ds::PicaDrawView& draw,
                                 const Renderer3ds::PicaSurfaceLightingUniforms& block) {
    if (!Counts(draw)) return;
    ++mOpen.Lit;
    if (block.EnvironmentParameters[3] == 0.0F) ++mOpen.LitWithoutEnvironment;
    if (block.Rim[0] <= 0.0F) ++mOpen.LitWithoutRim;
}

void LightingTransientWatch::Native(const Renderer3ds::PicaDrawView& draw) {
    if (Counts(draw)) ++mOpen.Native;
}

void LightingTransientWatch::Refused(const Renderer3ds::PicaDrawView& draw, PerPixelRefusal refusal) {
    const auto lost = ClassifyLostWorldComposition(draw, refusal);
    if (!lost || !Counts(draw)) return;
    if (mOpen.LostTotal() == 0U) {
        mOpen.LostDomain = static_cast<uint8_t>(draw.CompositionDomain);
        mOpen.LostLayer = static_cast<uint8_t>(draw.Composition.Layer);
        mOpen.LostProvenance = static_cast<uint8_t>(draw.Composition.Provenance);
        mOpen.LostSourcePc = draw.Composition.SourcePc;
        mOpen.LostActor = draw.Composition.NativeActorIdentity;
        mOpen.LostCommandList = draw.CommandListAddress;
        mOpen.LostOffsetWords = draw.CommandListOffsetWords;
    }
    ++mOpen.Lost[static_cast<size_t>(*lost)];
}

PerPixelRefusal AdmitPerPixelDraw(const Renderer3ds::PicaDrawView& draw) noexcept {
    const auto& hooks = draw.TemporalVertexProgram.Hooks;
    if (draw.CompositionDomain != Renderer3ds::PicaCompositionDomain::Scene ||
        !Renderer3ds::PicaCompositionReceivesSurfaceLighting(draw.Composition.Layer))
        return PerPixelRefusal::NotSceneWorld;
    if (hooks.SurfaceProgram() == nullptr || hooks.TransformProgram() == nullptr)
        return PerPixelRefusal::NoSurfaceProgram;
    if (!PerPixelCostAdmits(draw.DepthWriteEnabled)) return PerPixelRefusal::NoDepthWrite;
    return PerPixelRefusal::None;
}

// Upstream's per-pixel admission (gfx_vulkan_pica_draw.cpp, surfaceCandidate
// and the authored-appearance test), then its block, trimmed.
PerPixelRefusal BuildPerPixelSurface(const Renderer3ds::PicaDrawView& draw,
                                     const Renderer3ds::PicaEnvironmentLighting& environment,
                                     std::optional<Renderer3ds::PicaSurfaceLightingView>& view,
                                     Renderer3ds::PicaSurfaceLightingUniforms& block) {
    if (const auto refusal = AdmitPerPixelDraw(draw); refusal != PerPixelRefusal::None) return refusal;
    if (draw.FragmentOperationMode != 0U) return PerPixelRefusal::FragmentOperation;
    if (draw.VertexUniformBytes.size() < Oot3d::kPicaPackedVertexFloatOffset) return PerPixelRefusal::NoUniforms;
    if (!Oot3d::UsesPicaPerspectiveProjection(draw.VertexUniformBytes)) return PerPixelRefusal::NotPerspective;
    const auto& hooks = draw.TemporalVertexProgram.Hooks;
    const auto* surface = hooks.SurfaceProgram();
    uint32_t booleans = 0U;
    std::memcpy(&booleans, draw.VertexUniformBytes.data() + Oot3d::kPicaPackedVertexBooleanMaskOffset,
                sizeof(booleans));
    const bool vertexLit = ((booleans >> surface->LightingBooleanUniform) & 1U) != 0U;
    const auto appearance =
        Renderer3ds::ClassifyPicaSurfaceAppearance(draw.Blend, vertexLit, draw.FragmentFeatures.FragmentLightingEnabled);
    if (appearance != Renderer3ds::PicaSurfaceAppearance::Relightable) return PerPixelRefusal::NotRelightable;
    const auto* worldToView = draw.LightBinding.WorldToView ? &*draw.LightBinding.WorldToView : nullptr;
    if (worldToView != nullptr && (!view || view->WorldToView != *worldToView)) {
        view = Renderer3ds::BuildPicaSurfaceLightingView(*worldToView);
    }
    const auto* cameraView = worldToView != nullptr && view ? &*view : nullptr;
    const auto floats = draw.VertexUniformBytes.subspan(Oot3d::kPicaPackedVertexFloatOffset);
    // A draw the title lights on its own keeps its native environment.
    auto isolated = environment;
    isolated.Enabled = false;
    const auto uniforms = Renderer3ds::BuildPicaSurfaceLightingUniforms(
        hooks, booleans, floats, draw.LightBinding, draw.Composition.IsolatedLighting ? isolated : environment,
        cameraView);
    if (!uniforms) {
        if (!Renderer3ds::ReadPicaNativeSurfaceLighting(hooks, booleans, floats)) return PerPixelRefusal::NativeLights;
        return cameraView == nullptr ? PerPixelRefusal::NoCamera : PerPixelRefusal::Block;
    }
    block = *uniforms;
    if (draw.Composition.IsolatedLighting || draw.FragmentFeatures.FragmentLightingEnabled) {
        block.BakedReferenceUp = {};
    }
    TrimSurfaceLighting(block);
    // Scene lights > Rim light, on the draws upstream gives it to.
    if (Oot3d::PicaDrawReceivesRimLight(environment, draw.CompositionDomain, draw.Composition, draw.Blend, appearance,
                                        draw.DepthTestEnabled, draw.DepthWriteEnabled, draw.FragmentOperationMode)) {
        Renderer3ds::AssignPicaSurfaceRim(block, environment.Rim);
    }
    return PerPixelRefusal::None;
}

} // namespace Fast::Deko3d
