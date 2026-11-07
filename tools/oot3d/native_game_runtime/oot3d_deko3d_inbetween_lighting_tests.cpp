// fps60 in-between frames light like real frames under deko3d per-pixel
// lighting (SWITCH_PORT.md, "Per-pixel blink on fps60 in-between frames").
// Two real guest frames of a lit CMB draw go through the path the console
// takes: the prepared transition (exact step, carried unmatched draws), its
// sample at alpha 0 (the step frame), 1/2 (the in-between frame) and 1, the
// bridge's draw view (SubmitOot3dPicaVulkanDrawPlan with the sample's
// overrides) and the deko3d backend's per-pixel decision
// (Deko3d::BuildPerPixelSurface). Every frame kind must be admitted with the
// same rim and environment, and the in-between block must lie between the
// two real frames' blocks.
#include "oot3d_native_pica_visual_frame.h"
#include "oot3d_native_pica_vulkan_bridge.h"
#include "oot3d_native_pica_affine_layout.h"

#include "fast/backends/gfx_deko3d_lighting.h"
#include "fast/oot3d/pica_uniform_layout.h"
#include "fast/renderer3ds/pica_render_backend.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace Oot3dNativeGame;
namespace R = Fast::Renderer3ds;
using V4 = std::array<float, 4>;

void Require(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "oot3d_deko3d_inbetween_lighting_tests: FAIL: %s\n", message.c_str());
        std::exit(1);
    }
}

constexpr uint32_t kTopColor = 0x18000000U;
constexpr uint64_t kSunIdentity = 0x5000U;
constexpr uint64_t kNaviIdentity = 0x6000U;

// The deko3d backend's decision for every draw it receives.
class Deko3dLightingBackend final : public Oot3d::Renderer::PicaRenderBackend {
  public:
    struct Decision {
        Fast::Deko3d::PerPixelRefusal Refusal = Fast::Deko3d::PerPixelRefusal::None;
        R::PicaSurfaceLightingUniforms Block{};
    };

    R::PicaEnvironmentLighting Environment;
    std::optional<R::PicaSurfaceLightingView> View;
    std::vector<Decision> Decisions;

    bool PublishPicaCompositionSequence(const Oot3d::Renderer::PicaCompositionSequenceView&, std::string*) override {
        return true;
    }
    bool SubmitPicaDraw(const Oot3d::Renderer::PicaDrawView& draw, std::string*) override {
        Decision decision;
        decision.Refusal = Fast::Deko3d::BuildPerPixelSurface(draw, Environment, View, decision.Block);
        Decisions.push_back(decision);
        return true;
    }
    bool WantsPicaSequenceDrawPreparation(const Oot3d::Renderer::PicaCompositionDrawReference&) const override {
        return false;
    }
    bool PreparePicaSequenceDraw(const Oot3d::Renderer::PicaDrawView&, std::string*) override { return true; }
    bool SubmitPicaDisplayTransfer(const Oot3d::Renderer::PicaDisplayTransferView&, std::string*) override {
        return true;
    }
    bool ClearPicaRenderTarget(uint64_t, uint32_t, std::string*) override { return true; }
    bool SubmitPicaMemoryFill(const Oot3d::Renderer::PicaMemoryFillView&, std::string*) override { return true; }
};

// The title's CMB vertex program hooks (as the frontend recognizes them):
// projection 0-3, view 4-6, model (palette) 20-22, lights 80/83/86 with the
// enabled value in 93.y, material diffuse 8 and ambient 9, lighting boolean 9.
std::shared_ptr<const Oot3dPicaTemporalVertexProgram> CmbProgram() {
    auto program = std::make_shared<Oot3dPicaTemporalVertexProgram>();
    auto& hooks = program->Hooks;
    hooks.Transform = {R::PicaVertexTransformOperation::ModelViewProjection3x4, 0, 4, 4, 3, 20, 3, 0, 1};
    auto& s = hooks.Surface;
    s.ViewNormalTemporary = 14;
    s.LightingBooleanUniform = 9;
    s.DiffuseUniform = 8;
    s.AmbientUniform = 9;
    s.TintBooleanUniform = 5;
    s.TintInputRegister = 2;
    s.TintScale = {90, 2};
    s.LightEnabledValue = {93, 1};
    s.LightDirectionUniforms = {80, 83, 86};
    program->AffineLayout = kOot3dCmbAffineLayout;
    return program;
}

// A camera that turns by `yaw` (radians) about the vertical axis and stands
// at `x` along it: the world-to-view rows.
std::array<V4, 3> Camera(float yaw, float x) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    return {{{c, 0, -s, -x * c}, {0, 1, 0, -40}, {s, 0, c, -300 - x * s}}};
}

struct GuestFrame {
    float Yaw = 0;
    float X = 0;
    std::array<float, 3> Navi{};
};

Oot3dPicaVulkanDrawPlan LitDraw(const GuestFrame& frame, const std::shared_ptr<const Oot3dPicaTemporalVertexProgram>& program,
                                const std::shared_ptr<const R::PicaSceneDirectionalLights>& sun) {
    Oot3dPicaVulkanDrawPlan plan;
    plan.SubmissionId = 10;
    plan.CompositionDomain = Oot3dPicaCompositionDomain::Scene;
    plan.Composition.Layer = Oot3dPicaCompositionLayer::OpaqueWorld;
    plan.Composition.Provenance = Oot3dPicaCompositionProvenance::NativeCmbDrawPass;
    plan.Composition.NativeActorGeometry = true;
    plan.Composition.TemporalIdentity = {1, 2, 3, 4, 5};
    plan.VertexShader.StateKey = 0x51;
    plan.VertexShader.TemporalProgram = program;
    plan.FragmentShader.StateKey = 0x52;
    plan.State.Framebuffer.ColorPhysicalAddress = kTopColor;
    plan.State.Framebuffer.Width = 240;
    plan.State.Framebuffer.Height = 400;
    plan.State.OutputMerger.Depth.TestEnabled = true;
    plan.State.OutputMerger.Depth.WriteEnabled = true;
    plan.State.Viewport.HalfWidth = 120;
    plan.State.Viewport.HalfHeight = 200;
    plan.VertexCount = 3;
    plan.VertexAttributes.push_back({0, 0, Oot3dPicaVertexFormat::Float, 3, 0});
    Oot3dPicaVulkanVertexBinding binding;
    binding.ByteStride = 12;
    binding.Bytes.resize(36);
    plan.VertexBindings.push_back(binding);

    auto& uniforms = plan.VertexShader.Uniforms;
    uniforms.BooleanMask = 1U << 9U;
    // Perspective projection: the homogeneous row is (0, 0, -1, 0).
    uniforms.Floats[0] = {1.2F, 0, 0, 0};
    uniforms.Floats[1] = {0, 2.0F, 0, 0};
    uniforms.Floats[2] = {0, 0, -1.0F, -2.0F};
    uniforms.Floats[3] = {0, 0, -1.0F, 0};
    const auto view = Camera(frame.Yaw, frame.X);
    for (size_t row = 0; row < 3; ++row) uniforms.Floats[4 + row] = view[row];
    uniforms.Floats[20] = {1, 0, 0, 0};
    uniforms.Floats[21] = {0, 1, 0, 0};
    uniforms.Floats[22] = {0, 0, 1, 0};
    uniforms.Floats[8] = {0.8F, 0.8F, 0.8F, 1};
    uniforms.Floats[9] = {0.6F, 0.6F, 0.6F, 1};
    // Light 0: the sun, in view space (the title rotates it with the camera).
    const std::array<float, 3> sunWorld{0.6F, 0.8F, 0};
    V4 sunView{0, 0, 0, 1};
    for (size_t row = 0; row < 3; ++row)
        for (size_t axis = 0; axis < 3; ++axis) sunView[row] += view[row][axis] * sunWorld[axis];
    sunView[3] = 1;
    uniforms.Floats[80] = sunView;
    uniforms.Floats[81] = {0.9F, 0.75F, 0.55F, 0};
    uniforms.Floats[82] = {0.3F, 0.3F, 0.35F, 0};
    uniforms.Floats[83] = {0, -1, 0, 0}; // slot 1 off
    uniforms.Floats[86] = {0, -1, 0, 0}; // slot 2 off
    uniforms.Floats[90] = {1, 1, 1, 1};
    uniforms.Floats[93] = {0, 1, 0, 0};

    auto& light = plan.LightBinding;
    std::array<float, 16> worldToView{};
    for (size_t row = 0; row < 3; ++row)
        for (size_t column = 0; column < 4; ++column) worldToView[column * 4 + row] = view[row][column];
    worldToView[15] = 1;
    light.WorldToView = worldToView;
    light.Sources[0].Kind = R::PicaNativeLightSource::Type::Directional;
    light.Sources[0].SourceIdentity = kSunIdentity;
    light.Sources[0].UnattenuatedColor = {0.9F, 0.75F, 0.55F};
    light.Sources[0].ReferenceColor = {0.9F, 0.75F, 0.55F};
    light.SceneDirectionals = sun;
    // Navi moves every guest frame: a new snapshot each frame (the binding
    // of every draw then differs from its partner's).
    auto points = std::make_shared<R::PicaScenePointLights>();
    points->Count = 1;
    points->Lights[0] = {kNaviIdentity, frame.Navi, {0.6F, 0.8F, 1.0F}, 120.0F, 0, true};
    light.ScenePoints = points;
    return plan;
}

Oot3dPicaVisualFrame Frame(uint64_t sequence, Oot3dPicaVulkanDrawPlan draw) {
    Oot3dPicaVisualFrame frame;
    frame.Sequence = sequence;
    frame.TopTransfer.InputPhysicalAddress = kTopColor;
    frame.Draws.push_back(std::move(draw));
    frame.StrictDrawIdentities.push_back(ComputeOot3dPicaVisualDrawIdentity(frame.Draws.back()));
    return frame;
}

R::PicaEnvironmentLighting SceneEnvironment() {
    // The release default (oot3d_scene_environment.cpp, BaseSceneEnvironmentProfile).
    R::PicaEnvironmentLighting environment;
    environment.Enabled = true;
    environment.PreserveSceneBrightness = false;
    environment.NativeDirectStrength = 1.0F;
    environment.NativeLocalStrength = 1.0F;
    environment.NativeAmbientStrength = 0.6F;
    environment.AmbientIntensity = 0.0F;
    environment.KeyDirection = R::PicaEnvironmentKeyDirection::SceneMain;
    environment.Lights[0] = {{0.5F, 0.7F, 0.5F}, {1, 1, 1}, 1.0F};
    environment.PointLights[0].Enabled = true;
    environment.PointLights[0].Intensity = 1.0F;
    environment.Rim.Enabled = true;
    environment.Rim.Intensity = 0.5F;
    return environment;
}

std::vector<Deko3dLightingBackend::Decision> Draw(Deko3dLightingBackend& backend,
                                                   const Oot3dPicaVisualFrameViewSample& sample) {
    backend.Decisions.clear();
    for (const auto& view : sample.Draws) {
        const auto overrides = view.Overrides();
        std::string error;
        Require(SubmitOot3dPicaVulkanDrawPlan(backend, *view.BasePlan, &error, 2U, &overrides), error);
    }
    return backend.Decisions;
}

template <typename Array>
bool Between(const Array& value, const Array& a, const Array& b, const char* what) {
    const auto* v = reinterpret_cast<const float*>(&value);
    const auto* x = reinterpret_cast<const float*>(&a);
    const auto* y = reinterpret_cast<const float*>(&b);
    for (size_t i = 0; i < sizeof(Array) / sizeof(float); ++i) {
        const float low = std::min(x[i], y[i]) - 1.0e-4F, high = std::max(x[i], y[i]) + 1.0e-4F;
        if (!(v[i] >= low && v[i] <= high)) {
            std::fprintf(stderr, "  %s[%zu]: in-between %g, real frames %g and %g\n", what, i, v[i], x[i], y[i]);
            return false;
        }
    }
    return true;
}

void RequireLitLikeRealFrames(const Deko3dLightingBackend::Decision& step, const Deko3dLightingBackend::Decision& inBetween,
                              const Deko3dLightingBackend::Decision& next, const char* what) {
    using Fast::Deko3d::PerPixelRefusal;
    using Fast::Deko3d::PerPixelRefusalName;
    Require(step.Refusal == PerPixelRefusal::None,
            std::string(what) + ": step frame lit (" + PerPixelRefusalName(step.Refusal) + ")");
    Require(next.Refusal == PerPixelRefusal::None,
            std::string(what) + ": next real frame lit (" + PerPixelRefusalName(next.Refusal) + ")");
    Require(inBetween.Refusal == PerPixelRefusal::None,
            std::string(what) + ": in-between frame lit like the real ones (" +
                PerPixelRefusalName(inBetween.Refusal) + ")");
    const auto& a = step.Block;
    const auto& b = next.Block;
    const auto& m = inBetween.Block;
    Require(a.Rim[0] > 0.0F && m.Rim == a.Rim && m.RimColor == a.RimColor && m.RimDirection == a.RimDirection,
            std::string(what) + ": the in-between frame keeps the rim");
    Require(m.Control == a.Control && m.Control == b.Control, std::string(what) + ": same lights and branches");
    Require(m.EnvironmentParameters == a.EnvironmentParameters, std::string(what) + ": same environment");
    Require(Between(m.Lights, a.Lights, b.Lights, "Lights") &&
                Between(m.ResolvedAmbient, a.ResolvedAmbient, b.ResolvedAmbient, "ResolvedAmbient") &&
                Between(m.MaterialDiffuse, a.MaterialDiffuse, b.MaterialDiffuse, "MaterialDiffuse") &&
                Between(m.SceneDirections, a.SceneDirections, b.SceneDirections, "SceneDirections") &&
                Between(m.SceneDirectionalColors, a.SceneDirectionalColors, b.SceneDirectionalColors,
                        "SceneDirectionalColors") &&
                Between(m.ScenePoints, a.ScenePoints, b.ScenePoints, "ScenePoints") &&
                Between(m.EnvironmentDirections, a.EnvironmentDirections, b.EnvironmentDirections,
                        "EnvironmentDirections") &&
                Between(m.EnvironmentColors, a.EnvironmentColors, b.EnvironmentColors, "EnvironmentColors") &&
                Between(m.ViewToWorldLinear, a.ViewToWorldLinear, b.ViewToWorldLinear, "ViewToWorldLinear"),
            std::string(what) + ": the in-between block lies between the real frames' blocks");
}

// The logs15 Temple of Time case: Link close up, the camera turning a
// little, Navi moving; the sun's snapshot shared by both frames (unchanged).
void TestInBetweenLightsLikeRealFrames(float alpha, const char* what) {
    const auto program = CmbProgram();
    auto sunSnapshot = std::make_shared<R::PicaSceneDirectionalLights>();
    sunSnapshot->Count = 1;
    sunSnapshot->Lights[0] = {kSunIdentity, {0.6F, 0.8F, 0}, {0.9F, 0.75F, 0.55F}, false};
    sunSnapshot->ContinuityEpoch = 7;
    std::shared_ptr<const R::PicaSceneDirectionalLights> sun = sunSnapshot;
    const auto previous = Frame(1, LitDraw({0.00F, 0.0F, {10, 50, 0}}, program, sun));
    const auto current = Frame(2, LitDraw({0.02F, 1.5F, {14, 52, 3}}, program, sun));

    Oot3dPicaPreparedVisualTransition transition;
    transition.SetOptions({true, true});
    Require(transition.Prepare(previous, current), "transition prepared");
    Require(transition.Stats().MatchedDraws == 1U, "the draw is matched");

    Deko3dLightingBackend backend;
    backend.Environment = SceneEnvironment();
    Oot3dPicaVisualFrameViewSample sample;
    Require(transition.Sample(previous, current, 0.0F, sample), "step sample");
    const auto step = Draw(backend, sample);
    Require(transition.Sample(previous, current, alpha, sample), "in-between sample");
    Require(sample.Draws.size() == 1U && sample.Draws[0].LightBinding.has_value(),
            "the in-between draw carries an interpolated light binding (Navi moved)");
    const auto inBetween = Draw(backend, sample);
    Require(ViewOot3dPicaVisualFrame(current, sample), "next step view");
    const auto next = Draw(backend, sample);
    Require(step.size() == 1U && inBetween.size() == 1U && next.size() == 1U, "one draw each");
    RequireLitLikeRealFrames(step[0], inBetween[0], next[0], what);
}

// logs/deko3d.txt "lighting real/in-between": counts, first refusal, refusals by
// reason and the first lit draw's block, per frame kind.
void TestFrameKindLog() {
    using namespace Fast::Deko3d;
    FrameKindLighting kind;
    kind.Samples = 2;
    R::PicaSurfaceLightingUniforms block;
    block.Control = {1, 0, 1, 0};
    block.EnvironmentParameters[3] = 1.0F;
    block.Rim[0] = 0.5F;
    kind.Light(block);
    block.Rim[0] = 0.0F;
    kind.Light(block);
    kind.Refuse(PerPixelRefusal::NoCamera);
    kind.Refuse(PerPixelRefusal::NoDepthWrite);
    kind.Refuse(PerPixelRefusal::NoCamera);
    ++kind.Native;
    kind.LostWorld[static_cast<size_t>(LostWorldComposition::UiList)] = 3;
    kind.Transients = 1;
    const std::string text = FormatFrameKindLighting(kind);
    Require(text == "frames=2 lit=2 (no env 0, no rim 1) native=1 ineligible=3 first=no_camera "
                    "(no_depth_write=1 no_camera=2) lost world=3 (ui_list=3) transients=1 "
                    "block control=1/0/1/0 env=1 rim=0.5 baked=0",
            "frame kind log: " + text);
    Require(FormatFrameKindLighting({}) ==
                "frames=0 lit=0 (no env 0, no rim 0) native=0 ineligible=0 first=none lost world=0 transients=0",
            "empty frame kind log");
}

// The per-pixel decisions of one presented frame, as ApplyPicaLighting feeds
// them to the transient watch.
class WatchingBackend final : public Oot3d::Renderer::PicaRenderBackend {
  public:
    R::PicaEnvironmentLighting Environment;
    std::optional<R::PicaSurfaceLightingView> View;
    Fast::Deko3d::LightingTransientWatch Watch;
    std::vector<Fast::Deko3d::PerPixelRefusal> Refusals;

    bool PublishPicaCompositionSequence(const Oot3d::Renderer::PicaCompositionSequenceView&, std::string*) override {
        return true;
    }
    bool SubmitPicaDraw(const Oot3d::Renderer::PicaDrawView& draw, std::string*) override {
        R::PicaSurfaceLightingUniforms block{};
        const auto refusal = Fast::Deko3d::BuildPerPixelSurface(draw, Environment, View, block);
        Refusals.push_back(refusal);
        if (refusal != Fast::Deko3d::PerPixelRefusal::None) Watch.Refused(draw, refusal);
        else Watch.Lit(draw, block);
        return true;
    }
    bool WantsPicaSequenceDrawPreparation(const Oot3d::Renderer::PicaCompositionDrawReference&) const override {
        return false;
    }
    bool PreparePicaSequenceDraw(const Oot3d::Renderer::PicaDrawView&, std::string*) override { return true; }
    bool SubmitPicaDisplayTransfer(const Oot3d::Renderer::PicaDisplayTransferView&, std::string*) override {
        return true;
    }
    bool ClearPicaRenderTarget(uint64_t, uint32_t, std::string*) override { return true; }
    bool SubmitPicaMemoryFill(const Oot3d::Renderer::PicaMemoryFillView&, std::string*) override { return true; }
};

// How the frontend attributes a draw (oot3d_native_pica_frontend.cpp): its
// mesh span, a list submitted in the HUD phase (every draw UI), no span.
enum class Attribution { World, UiList, UiSpan, NoSpan, ControlFlow, Atmosphere };

Oot3dPicaVulkanDrawPlan Attributed(Oot3dPicaVulkanDrawPlan plan, Attribution attribution) {
    switch (attribution) {
        case Attribution::World: break;
        case Attribution::UiList:
            plan.CompositionDomain = Oot3dPicaCompositionDomain::Ui;
            plan.Composition = {Oot3dPicaCompositionLayer::Ui, Oot3dPicaCompositionProvenance::NativeUiLifecycle, 0U,
                                0U};
            break;
        case Attribution::UiSpan:
            plan.CompositionDomain = Oot3dPicaCompositionDomain::Ui;
            plan.Composition = {Oot3dPicaCompositionLayer::Ui, Oot3dPicaCompositionProvenance::NativeUiLifecycle,
                                0x004228E4U, 0U};
            break;
        case Attribution::NoSpan:
            plan.Composition = {};
            break;
        case Attribution::ControlFlow:
            plan.Composition = {Oot3dPicaCompositionLayer::Unknown, Oot3dPicaCompositionProvenance::NativeControlFlow,
                                0x00313444U, 0x00300524U};
            break;
        case Attribution::Atmosphere:
            plan.Composition.Layer = Oot3dPicaCompositionLayer::Atmosphere;
            break;
    }
    return plan;
}

// One presented frame of `count` copies of Link's lit draw, `lost` of them
// attributed otherwise, plus `flushed` draws of a dependency flush (render
// target namespace 1, never presented).
void PresentFrame(WatchingBackend& backend, const Oot3dPicaVulkanDrawPlan& draw, size_t count, size_t lost,
                  Attribution attribution, size_t flushed = 0) {
    std::string error;
    for (size_t index = 0; index < count; ++index) {
        const auto plan = index < lost ? Attributed(draw, attribution) : draw;
        Require(SubmitOot3dPicaVulkanDrawPlan(backend, plan, &error, 2U, nullptr), error);
    }
    for (size_t index = 0; index < flushed; ++index)
        Require(SubmitOot3dPicaVulkanDrawPlan(backend, Attributed(draw, Attribution::NoSpan), &error, 1U, nullptr),
                error);
}

std::shared_ptr<const R::PicaSceneDirectionalLights> Sun() {
    auto sun = std::make_shared<R::PicaSceneDirectionalLights>();
    sun->Count = 1;
    sun->Lights[0] = {kSunIdentity, {0.6F, 0.8F, 0}, {0.9F, 0.75F, 0.55F}, false};
    sun->ContinuityEpoch = 7;
    return sun;
}

// A world model the per-pixel admission refuses as not_scene_world is
// classified by what its composition says; anything else is not "lost".
void TestLostWorldComposition() {
    using namespace Fast::Deko3d;
    const auto program = CmbProgram();
    const auto link = LitDraw({0.0F, 0.0F, {10, 50, 0}}, program, Sun());
    WatchingBackend backend;
    backend.Environment = SceneEnvironment();
    const std::array<std::pair<Attribution, LostWorldComposition>, 5> cases{{
        {Attribution::UiList, LostWorldComposition::UiList},
        {Attribution::UiSpan, LostWorldComposition::UiSpan},
        {Attribution::NoSpan, LostWorldComposition::NoSpan},
        {Attribution::ControlFlow, LostWorldComposition::ControlFlow},
        {Attribution::Atmosphere, LostWorldComposition::Atmosphere},
    }};
    for (const auto& [attribution, expected] : cases) {
        backend.Watch.Begin(1, false);
        PresentFrame(backend, link, 1, 1, attribution);
        Require(backend.Refusals.back() == PerPixelRefusal::NotSceneWorld, "the lost draw is refused");
        const auto closed = backend.Watch.Begin(2, false);
        (void)closed;
    }
    // The classification itself, on the bridge's view of each attribution.
    struct Classifier final : Oot3d::Renderer::PicaRenderBackend {
        std::vector<std::optional<LostWorldComposition>> Classes;
        bool PublishPicaCompositionSequence(const Oot3d::Renderer::PicaCompositionSequenceView&, std::string*) override {
            return true;
        }
        bool SubmitPicaDraw(const Oot3d::Renderer::PicaDrawView& draw, std::string*) override {
            Classes.push_back(ClassifyLostWorldComposition(draw, AdmitPerPixelDraw(draw)));
            return true;
        }
        bool WantsPicaSequenceDrawPreparation(const Oot3d::Renderer::PicaCompositionDrawReference&) const override {
            return false;
        }
        bool PreparePicaSequenceDraw(const Oot3d::Renderer::PicaDrawView&, std::string*) override { return true; }
        bool SubmitPicaDisplayTransfer(const Oot3d::Renderer::PicaDisplayTransferView&, std::string*) override {
            return true;
        }
        bool ClearPicaRenderTarget(uint64_t, uint32_t, std::string*) override { return true; }
        bool SubmitPicaMemoryFill(const Oot3d::Renderer::PicaMemoryFillView&, std::string*) override { return true; }
    } classifier;
    std::string error;
    for (const auto& [attribution, expected] : cases) {
        Require(SubmitOot3dPicaVulkanDrawPlan(classifier, Attributed(link, attribution), &error, 2U, nullptr), error);
        Require(classifier.Classes.back() == expected,
                std::string("lost world class ") + LostWorldCompositionName(expected));
    }
    Require(SubmitOot3dPicaVulkanDrawPlan(classifier, link, &error, 2U, nullptr), error);
    Require(!classifier.Classes.back(), "an admitted world draw is not lost");
    auto depthless = Attributed(link, Attribution::UiList);
    depthless.State.OutputMerger.Depth.WriteEnabled = false;
    Require(SubmitOot3dPicaVulkanDrawPlan(classifier, depthless, &error, 2U, nullptr), error);
    Require(!classifier.Classes.back(), "a UI draw that writes no depth is not a lost world model");
}

// The one-frame blink as the CPU sees it: a presented frame whose world
// models lose their composition (a list submitted in the HUD phase), or
// whose lit draws lose the environment, between two frames that agree. The
// watch reports it (with the first lost draw) once its next frame is known;
// steady frames, dependency flushes and two-frame changes are not transients.
void TestLightingTransients() {
    using namespace Fast::Deko3d;
    const auto program = CmbProgram();
    const auto link = LitDraw({0.0F, 0.0F, {10, 50, 0}}, program, Sun());
    WatchingBackend backend;
    backend.Environment = SceneEnvironment();
    auto& watch = backend.Watch;

    // Steady frames (with a dependency flush of lost-looking draws).
    for (uint64_t frame = 10; frame < 14; ++frame) {
        Require(!watch.Begin(frame, frame % 2U != 0U), "steady frames are not transients");
        PresentFrame(backend, link, 8, 0, Attribution::World, 3);
    }
    // Frame 14: 6 of Link's 8 draws in a list submitted in the HUD phase.
    Require(!watch.Begin(14, false), "frame 12 agrees with its neighbours");
    PresentFrame(backend, link, 8, 6, Attribution::UiList);
    Require(!watch.Begin(15, true), "frame 14 is reported once frame 15 is known");
    PresentFrame(backend, link, 8, 0, Attribution::World);
    const auto lost = watch.Begin(16, false);
    Require(lost.has_value() && lost->Sample.DeviceFrame == 14U && !lost->Sample.InBetween &&
                lost->Sample.Lit == 2U && lost->Before.Lit == 8U && lost->After.Lit == 8U &&
                lost->Sample.Lost[static_cast<size_t>(LostWorldComposition::UiList)] == 6U,
            "a frame whose world models lost their composition is a transient");
    const std::string text = FormatLightingTransient(*lost);
    Require(text == "frame 14 (real) lit 8/2/8, no env 0/0/0, no rim 0/0/0, native 0/0/0, lost world 0/6/0 "
                    "(ui_list=6; first domain=2 layer=4 provenance=3 pc=00000000 actor=0 list=00000000+0)",
            "transient log: " + text);
    PresentFrame(backend, link, 8, 0, Attribution::World);

    // Frame 17 (in-between): the scene environment off for one frame.
    Require(!watch.Begin(17, true), "frame 15 follows the transient frame 14");
    backend.Environment.Enabled = false;
    PresentFrame(backend, link, 8, 0, Attribution::World);
    backend.Environment = SceneEnvironment();
    Require(!watch.Begin(18, false), "frame 17 is reported once frame 18 is known");
    PresentFrame(backend, link, 8, 0, Attribution::World);
    const auto dark = watch.Begin(19, true);
    Require(dark.has_value() && dark->Sample.DeviceFrame == 17U && dark->Sample.InBetween &&
                dark->Sample.Lit == 8U && dark->Sample.LitWithoutEnvironment == 8U &&
                dark->Sample.LitWithoutRim == 8U,
            "a frame lit without the scene environment is a transient");
    PresentFrame(backend, link, 8, 0, Attribution::World);

    // Two frames without the environment (an area change) and a scene that
    // lights fewer than 4 draws are not one-frame transients.
    Require(!watch.Begin(20, false), "frame 18 agrees with its neighbours");
    backend.Environment.Enabled = false;
    PresentFrame(backend, link, 8, 0, Attribution::World);
    Require(!watch.Begin(21, false), "frame 19 precedes the environment going off");
    PresentFrame(backend, link, 8, 0, Attribution::World);
    backend.Environment = SceneEnvironment();
    Require(!watch.Begin(22, false), "the first of two frames without the environment");
    PresentFrame(backend, link, 3, 0, Attribution::World);
    Require(!watch.Begin(23, false), "the second of two frames without the environment");
    PresentFrame(backend, link, 1, 1, Attribution::UiList);
    Require(!watch.Begin(24, false), "frame 22 lights fewer than 4 draws");
    PresentFrame(backend, link, 3, 0, Attribution::World);
    Require(!watch.Begin(25, false), "frame 23's neighbours light fewer than 4 draws");

    Require(IsLightingTransient({.Lit = 8}, {.Lit = 5}, {.Lit = 8}), "5 of 8 lit draws is a dip");
    Require(!IsLightingTransient({.Lit = 8}, {.Lit = 6}, {.Lit = 8}), "6 of 8 lit draws is not a dip");
    Require(IsLightingTransient({.Lit = 8}, {.Lit = 8, .Native = 1}, {.Lit = 8}), "a native draw is a transient");
    LightingSample atmosphere{.Lit = 8};
    atmosphere.Lost[static_cast<size_t>(LostWorldComposition::Atmosphere)] = 2;
    Require(!IsLightingTransient({.Lit = 8}, atmosphere, {.Lit = 8}),
            "an atmosphere draw with a surface program is not a lost world model");
}

} // namespace

int main() {
    TestInBetweenLightsLikeRealFrames(0.5F, "alpha 1/2");
    TestInBetweenLightsLikeRealFrames(0.25F, "alpha 1/4");
    TestFrameKindLog();
    TestLostWorldComposition();
    TestLightingTransients();
    std::puts("oot3d_deko3d_inbetween_lighting_tests: ok");
    return 0;
}
