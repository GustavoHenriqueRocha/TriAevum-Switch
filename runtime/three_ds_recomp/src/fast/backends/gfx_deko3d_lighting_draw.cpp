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
