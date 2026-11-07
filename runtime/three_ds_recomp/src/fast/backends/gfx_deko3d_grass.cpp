// config/grass on the deko3d backend (SWITCH_PORT.md, "Grass on deko3d").
//
// Off (no file): nothing here runs; every hook returns at its first test.
// "placements" (or an empty file): opaque world draws of the scene whose
// texture has a rule of the product preset become grass sources
// (pica_grass_source.h), their placements are built by one worker, and every
// 600 frames deko3d.txt gets a `grass:` line. Nothing is drawn.
// "draw": the same, plus the tufts are drawn into the scene target before its
// first non-opaque scene draw (or before its display transfer).
//
// Who does what (none of it on the game thread, core 0):
// - render thread: publishes sources (as before), once per frame matches the
//   placements to this frame's source draws (their model-to-clip, fog and
//   lights), records the instanced draws and hands the next cull to the
//   worker. An fps60 in-between frame publishes no sources: it draws the last
//   real frame's placements with its own camera (GrassInBetweenCameras);
// - grass cull worker (service thread, cores 1|2): culls per cluster and
//   writes the 32-byte instances straight into GPU-visible memory
//   (grass_instance_stream.h); its result is drawn from the next frame on;
// - placement builder worker (service thread): builds placements, thinned
//   evenly to the profile's cap (ThinGrassAnchorsToLimit).
// The game thread only reads Link's and Epona's colliders when drawing is on
// (CollectNativeGrassColliders, a few guest reads per step).

#include "fast/backends/gfx_deko3d.h"

#if defined(__SWITCH__)
#include "fast/backends/gfx_deko3d_pica_pipeline.h"
#include "fast/oot3d/grass_draw_preparation.h"
#include "fast/oot3d/grass_instance_stream.h"
#include "fast/oot3d/grass_interaction_bridge.h"
#include "fast/oot3d/grass_scene_bridge.h"
#include "fast/oot3d/grass_stream_handoff.h"
#include "fast/oot3d/grass_switch_profile.h"
#include "fast/oot3d/grass_texture_source_cache.h"
#include "fast/oot3d/grass_visibility.h"
#include "fast/oot3d/graphics_settings.h"
#include "fast/oot3d/graphics_settings_persistence.h"
#include "fast/oot3d/pica_grass_source.h"
#include "gfx_deko3d_api.h"
#include "gfx_deko3d_device.h"
#include "gfx_deko3d_grass_dksh.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

namespace Fast {
namespace {

constexpr uint64_t kLogEveryFrames = 600U;
// Instance memory slots (GrassStreamSlots): two read by frames in flight,
// the newest result and the one the worker writes.
constexpr uint32_t kStreamSlots = Oot3d::GrassStreamSlots::kSlots;
constexpr uint32_t kMaxColliders = 4U;
constexpr uint32_t kTimestampBytes = 16U; // DkCounter report: value, timestamp
// Per device slot: before the grass, after its blades, after its tufts.
constexpr uint32_t kTimestampsPerSlot = 3U;
// Keys of the grass stages in the ShaderStore (not text hashes).
constexpr uint64_t kGrassVertexKey = 0xDE0C3D56A5500001ULL;
constexpr uint64_t kGrassFragmentKey = 0xDE0C3D56A5500002ULL;
constexpr uint64_t kGrassFragmentDepthKey = 0xDE0C3D56A5500003ULL;

// std140 GrassPlacement of grass_vsh.glsl / grass_fsh.glsl.
struct GrassPlacementUniforms {
    std::array<float, 16> PositionToClip{};
    std::array<float, 4> Depth{};    // scale, offset, w-buffer, explicit depth
    std::array<float, 4> FogColor{}; // rgb, mode
    std::array<std::array<float, 4>, 64> FogLut{};
    std::array<std::array<float, 4>, 3> LightDirection{};
    std::array<std::array<float, 4>, 3> LightDiffuse{};
    std::array<std::array<float, 4>, 3> LightAmbient{};
    std::array<float, 4> Eye{}; // xyz, active light count
};
static_assert(sizeof(GrassPlacementUniforms) == 1280U);

// std140 GrassFrame of grass_vsh.glsl.
struct GrassFrameUniforms {
    std::array<float, 4> Wind{};
    std::array<float, 4> WindDetail{};
    std::array<float, 4> Shape{};
    std::array<float, 4> Distance{};
    std::array<float, 4> RootColor{};
    std::array<float, 4> TipColor{};
    std::array<float, 4> Brightness{};
    std::array<std::array<float, 4>, kMaxColliders> Collider{};
    std::array<std::array<float, 4>, kMaxColliders> ColliderExtent{};
};
static_assert(sizeof(GrassFrameUniforms) == 240U);

double MillisecondsSince(std::chrono::steady_clock::time_point begin) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
}

// Culls on its own service thread: one job at a time, the newest result kept.
class GrassCullWorker {
  public:
    struct Job {
        std::vector<Oot3d::GrassStreamPlacement> Placements;
        std::vector<uint64_t> Identities;
        Oot3d::GrassStreamSettings Settings;
        Oot3d::GrassStreamInstance* Output = nullptr;
        size_t Capacity = 0;
        uint32_t Slot = 0;
        uint64_t Frame = 0; // whose cameras
        uint32_t Area = 0;  // GrassArea of that frame
    };
    struct Result {
        bool Valid = false;
        uint32_t Slot = 0;
        uint64_t Frame = 0;
        uint32_t Area = 0;
        std::vector<uint64_t> Identities;
        std::vector<Oot3d::GrassStreamRange> Ranges;
        Oot3d::GrassStreamStats Stats;
        double Milliseconds = 0.0;
    };

    GrassCullWorker() : mThread([this] { Run(); }) {}
    ~GrassCullWorker() {
        {
            std::scoped_lock lock(mMutex);
            mStop = true;
        }
        mWake.notify_one();
        mThread.join();
    }
    [[nodiscard]] bool Busy() {
        std::scoped_lock lock(mMutex);
        return mJob.has_value() || mRunning;
    }
    // The slot the worker may still be writing (when busy).
    [[nodiscard]] std::optional<uint32_t> WritingSlot() {
        std::scoped_lock lock(mMutex);
        if (mJob) return mJob->Slot;
        if (mRunning) return mRunningSlot;
        return std::nullopt;
    }
    void Post(Job job) {
        {
            std::scoped_lock lock(mMutex);
            mJob = std::move(job);
        }
        mWake.notify_one();
    }
    bool Take(Result& result) {
        std::scoped_lock lock(mMutex);
        if (!mResult.Valid) return false;
        result = std::move(mResult);
        mResult = {};
        return true;
    }

  private:
    void Run() {
        Oot3d::GrassInstanceStreamBuilder builder;
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mMutex);
                mWake.wait(lock, [this] { return mStop || mJob.has_value(); });
                if (mStop) return;
                job = std::move(*mJob);
                mJob.reset();
                mRunning = true;
                mRunningSlot = job.Slot;
            }
            Result result;
            const auto begin = std::chrono::steady_clock::now();
            result.Stats = builder.Build(job.Placements, job.Settings,
                                         std::span<Oot3d::GrassStreamInstance>(job.Output, job.Capacity),
                                         result.Ranges);
            result.Milliseconds = MillisecondsSince(begin);
            result.Slot = job.Slot;
            result.Frame = job.Frame;
            result.Area = job.Area;
            result.Identities = std::move(job.Identities);
            result.Valid = true;
            std::scoped_lock lock(mMutex);
            mResult = std::move(result);
            mRunning = false;
        }
    }

    std::mutex mMutex;
    std::condition_variable mWake;
    std::optional<Job> mJob;
    Result mResult;
    bool mRunning = false;
    uint32_t mRunningSlot = 0;
    bool mStop = false;
    std::thread mThread; // last: starts after the members it uses
};

// This frame's draw data of one placement.
struct FramePlacement {
    std::shared_ptr<const Oot3d::GrassWorldPlacement> World;
    uint64_t Identity = 0;
    uint64_t Namespace = 0;
    uint32_t ColorAddress = 0;
    GrassPlacementUniforms Uniforms;
    bool ExplicitDepth = false;
    DkGpuAddr UniformsGpu = DK_GPU_ADDR_INVALID; // written at its first draw this frame
};

struct GrassState {
    Oot3d::GrassSwitchMode Mode = Oot3d::GrassSwitchMode::Off;
    bool Fps60 = false;
    Oot3d::GrassSwitchDistance Distance = Oot3d::GrassSwitchDistance::Near;
    Oot3d::GrassSwitchProfile Profile;
    Oot3d::GrassStreamSettings Stream;
    Oot3d::InteractiveGrassSettings Settings;
    // Reset on an area change (its memo and geometry registry are not movable).
    std::unique_ptr<Oot3d::PicaGrassSourceState> Sources = std::make_unique<Oot3d::PicaGrassSourceState>();
    std::unique_ptr<Oot3d::PicaGrassPlacements> Placements;
    std::chrono::steady_clock::time_point Start = std::chrono::steady_clock::now();
    uint64_t DrawOrder = 0;
    uint64_t DrawFrame = ~0ULL;

    // Drawing (mode Draw): created at the first draw.
    std::unique_ptr<GrassCullWorker> Worker;
    bool DrawReady = false;
    bool DrawFailed = false;
    DkMemBlock StreamMemory = nullptr;
    uint32_t SlotInstances = 0;
    Oot3d::GrassStreamSlots Slots{Deko3d::kFramesInFlight};
    GrassCullWorker::Result Current; // the cull in Slots.Current()
    DkMemBlock Timestamps = nullptr;
    std::array<bool, Deko3d::kFramesInFlight> TimestampPending{};
    const DkShader* Vertex = nullptr;
    const DkShader* Fragment = nullptr;
    const DkShader* FragmentDepth = nullptr;
    // This frame (render thread).
    uint64_t PreparedFrame = ~0ULL;
    // One placement per render target it was drawn into, also found by
    // placement identity (GrassFramePlacements).
    Oot3d::GrassFramePlacements<FramePlacement> Frame;
    // fps60 in-between frames (GrassNoteSample): the device frame presenting
    // a synthetic sample, the last real frame's placements (what an
    // in-between frame draws) and the in-between frame's camera per target.
    uint64_t InBetweenFrame = ~0ULL;
    Oot3d::GrassFramePlacements<FramePlacement> RealFrame;
    Oot3d::GrassInBetweenCameras InBetween;
    // The target this frame's sources were published into.
    uint64_t SourceFrame = ~0ULL;
    uint64_t SourceNamespace = 0;
    uint32_t SourceColorAddress = 0;
    // The target grass was last drawn into (its camera feeds the cull).
    uint64_t DrawnNamespace = 0;
    uint32_t DrawnColorAddress = 0;
    std::array<DkGpuAddr, 2> FrameUniforms{DK_GPU_ADDR_INVALID, DK_GPU_ADDR_INVALID}; // tufts, blades
    bool Posted = false;

    // Accumulated between logs.
    uint64_t Frames = 0;
    uint64_t Published = 0;
    double ObserveMs = 0.0, ObserveMaxMs = 0.0;
    double UpdateMs = 0.0, UpdateMaxMs = 0.0;
    double DrawMs = 0.0, DrawMaxMs = 0.0;
    uint64_t AnchorsMax = 0;
    uint64_t DrawnFrames = 0;
    uint64_t Visible = 0, VisibleMax = 0, Tufts = 0, Blades = 0, Draws = 0;
    uint64_t ClustersVisible = 0, ClustersTested = 0, OverBudgetFrames = 0;
    uint64_t CullJobs = 0;
    double CullMs = 0.0, CullMaxMs = 0.0;
    uint64_t GpuSamples = 0;
    double GpuMs = 0.0, GpuMaxMs = 0.0, GpuBladesMs = 0.0, GpuTuftsMs = 0.0;
    uint64_t RasterDraws = 0, DepthDraws = 0;
    uint64_t CullInstances = 0, CullInstancesMax = 0, CullThinned = 0;
    uint64_t EmptyFrames = 0;   // a draw point with a cull result but nothing of it drawn
    uint64_t NoResultFrames = 0; // a draw point before the first cull finished
    uint64_t FreshBase = 0, StaleBase = 0;
    // The game's area (GrassArea) the placements of this frame belong to.
    uint32_t Area = 0xffffffffU;
    uint32_t DrawnArea = 0xffffffffU;
    // Culls dropped before drawing (GrassCullFitsFrame): of another area, or
    // with no placement in common with this frame's; and cull ranges drawn
    // with their placement as drawn into another target this frame.
    uint64_t AreaChanges = 0, ForeignOtherArea = 0, ForeignNoPlacement = 0, OtherTargetRanges = 0;
    // In-between frames, and those that had the real frame's placements but
    // none of its source draws to take their camera from.
    uint64_t InBetweenFrames = 0, InBetweenWithoutCamera = 0;
};

// Set once before the first frame (Deko3dConfigureGrass), read by the render
// thread afterwards.
std::atomic<GrassState*> gGrass{nullptr};
// Written by the game thread (Deko3dSetGrassArea), read by the render thread.
std::atomic<uint32_t> gGrassArea{0xffffffffU};
// Read by the game thread once per step: true only with config/grass = draw.
bool gGrassWantsColliders = false;

GrassFrameUniforms BuildFrameUniforms(const GrassState& state, bool blades) {
    const auto& settings = state.Settings;
    GrassFrameUniforms frame;
    const float direction = settings.WindDirectionDegrees * 3.14159265F / 180.0F;
    const float seconds = static_cast<float>(
        std::fmod(std::chrono::duration<double>(std::chrono::steady_clock::now() - state.Start).count(), 3600.0));
    frame.Wind = {std::cos(direction), std::sin(direction), settings.WindStrength, seconds};
    frame.WindDetail = {settings.WindSpeed, settings.WindSpatialScale, settings.WindGustStrength,
                        settings.WindGustFrequency};
    frame.Shape = {static_cast<float>(state.Profile.BladesPerTuft), 1.0F, settings.MaximumBend,
                   settings.Appearance.ReceiveLighting ? 1.0F : 0.0F};
    frame.Distance = {state.Stream.DrawDistance, state.Stream.FadeStart, state.Stream.SingleBladeDistance,
                      blades ? 1.0F : 0.0F};
    const auto& look = settings.Appearance;
    frame.RootColor = {look.RootColor[0], look.RootColor[1], look.RootColor[2], look.TextureColorInfluence};
    frame.TipColor = {look.TipColor[0], look.TipColor[1], look.TipColor[2], 0.0F};
    uint32_t colliders = 0U;
    for (const auto& actor : Oot3d::GrassInteractionBridge::Instance().LatestActors()) {
        if (colliders == kMaxColliders) break;
        if (actor.Type != Oot3d::GrassInteractor::Kind::PlayerLink &&
            actor.Type != Oot3d::GrassInteractor::Kind::Epona)
            continue;
        frame.Collider[colliders] = {actor.Position[0], actor.Position[1], actor.Position[2],
                                     actor.Radius * settings.ColliderRadiusMultiplier};
        frame.ColliderExtent[colliders] = {actor.HalfHeight * settings.ColliderHeightMultiplier, 0.0F, 0.0F, 0.0F};
        ++colliders;
    }
    frame.Brightness = {look.TextureRootBrightness, look.TextureTipBrightness, 0.0F, static_cast<float>(colliders)};
    return frame;
}

FramePlacement BuildFramePlacement(const Oot3d::PicaGrassDrawable& drawable, const GrassState& state) {
    FramePlacement result;
    result.World = drawable.Placement;
    result.Identity = drawable.Placement->Identity;
    const auto& mesh = drawable.Mesh;
    result.Namespace = mesh.RenderTargetNamespace;
    result.ColorAddress = mesh.FramebufferColorPhysicalAddress;
    auto push = Oot3d::ProjectionForMesh(mesh, {});
    const auto& depth = mesh.Shading.Depth;
    // Without a decoded PICA depth mapping the raster depth stands in
    // (scale -1, offset 0 is the identity on -gl_FragCoord.z).
    push.DepthState = {depth.Valid ? depth.Scale : -1.0F, depth.Valid ? depth.Offset : 0.0F,
                       depth.Valid && depth.WBuffering ? 1.0F : 0.0F, depth.Valid ? 1.0F : 0.0F};
    // The vertex program puts a linear PICA mapping into clip z; only a
    // w-buffered one needs gl_FragDepth (and loses early depth testing).
    result.ExplicitDepth = push.DepthState[3] > 0.5F && push.DepthState[2] > 0.5F;
    auto& u = result.Uniforms;
    u.PositionToClip = push.PositionToClip;
    u.Depth = {push.DepthState[0], push.DepthState[1], push.DepthState[2], result.ExplicitDepth ? 1.0F : 0.0F};
    (void)Oot3d::GrassDepthIsRasterDepth; // the PC's test; deko3d folds the mapping into clip z
    const auto& fog = mesh.Shading.Fog;
    if (state.Settings.Appearance.ReceiveFog && fog.Enabled) {
        u.FogColor = {fog.Color[0], fog.Color[1], fog.Color[2], fog.Flip ? 2.0F : 1.0F};
        for (size_t entry = 0; entry < fog.Lut.size(); ++entry) {
            u.FogLut[entry / 2U][(entry % 2U) * 2U] = fog.Lut[entry][0];
            u.FogLut[entry / 2U][(entry % 2U) * 2U + 1U] = fog.Lut[entry][1];
        }
    }
    const auto& shading = mesh.Shading;
    for (size_t index = 0; index < shading.ActiveLightCount && index < 3U; ++index) {
        const auto& light = shading.Lights[index];
        u.LightDirection[index] = {light.DirectionWorldTowardSource[0], light.DirectionWorldTowardSource[1],
                                   light.DirectionWorldTowardSource[2], light.Valid ? 1.0F : 0.0F};
        u.LightDiffuse[index] = {light.Diffuse[0], light.Diffuse[1], light.Diffuse[2], 0.0F};
        u.LightAmbient[index] = {light.Ambient[0], light.Ambient[1], light.Ambient[2], 0.0F};
    }
    const auto eye = Oot3d::GrassCameraFromProjection(push.PositionToClip).value_or(std::array<float, 3>{});
    u.Eye = {eye[0], eye[1], eye[2], static_cast<float>(std::min<size_t>(shading.ActiveLightCount, 3U))};
    return result;
}

// A source draw (its plan's geometry identity, which fps60 interpolation
// keeps) and its render target, as GrassInBetweenCameras keys them.
uint64_t SourceDrawKey(uint64_t geometryIdentity, uint64_t renderTargetNamespace, uint32_t colorAddress) {
    return Oot3d::GrassFramePlacementKey(geometryIdentity, renderTargetNamespace, colorAddress);
}
uint64_t TargetKey(uint64_t renderTargetNamespace, uint32_t colorAddress) {
    return Oot3d::GrassFramePlacementKey(0U, renderTargetNamespace, colorAddress);
}

// A real frame's placement in in-between frame `frame`: the same placement
// (anchors, fog, lights, depth mapping) with that frame's camera of its
// target; nullopt when none of the real frame's sources was drawn there.
std::optional<FramePlacement> MoveToInBetween(const GrassState& state, const FramePlacement& real, uint64_t frame) {
    const auto* change = state.InBetween.Change(frame, TargetKey(real.Namespace, real.ColorAddress));
    if (change == nullptr) return std::nullopt;
    FramePlacement moved = real;
    moved.UniformsGpu = DK_GPU_ADDR_INVALID;
    auto& u = moved.Uniforms;
    u.PositionToClip = Oot3d::GrassMultiplyClip(*change, real.Uniforms.PositionToClip);
    const auto eye = Oot3d::GrassCameraFromProjection(u.PositionToClip);
    if (eye) u.Eye = {(*eye)[0], (*eye)[1], (*eye)[2], real.Uniforms.Eye[3]};
    return moved;
}

// The budgets of one frame rate and grass distance on top of the preset (at
// start, and when the FPS or the distance changes while the game runs): the
// same code every way, so a switch leaves what a start with those choices
// would. The placement fields (density, per-placement cap, retained bytes)
// are the same for all of them.
void ApplyProfile(GrassState& state, bool fps60, Oot3d::GrassSwitchDistance distance) {
    state.Fps60 = fps60;
    state.Distance = distance;
    state.Profile = Oot3d::SelectGrassSwitchProfile(fps60, distance);
    state.Stream = Oot3d::GrassStreamSettingsFromProfile(state.Profile);
    Oot3d::ApplyGrassSwitchProfile(state.Profile, state.Settings);
}

void LogProfile(const GrassState& state) {
    const auto distance = Oot3d::GrassSwitchDistanceWord(state.Distance);
    if (state.Distance == Oot3d::GrassSwitchDistance::Area) {
        Deko3d::Log("grass: %s distance=area: every tuft of the area, at most %u visible (nearest clusters first)",
                    state.Fps60 ? "60 fps" : "30 fps", state.Stream.MaxInstances);
        return;
    }
    Deko3d::Log("grass: %s distance=%.*s: full density to %.0f, thinning to %.0f (shrinking from %.0f), single "
                "blades within %.0f, at most %u visible",
                state.Fps60 ? "60 fps" : "30 fps", static_cast<int>(distance.size()), distance.data(),
                static_cast<double>(state.Stream.FullDensityDistance), static_cast<double>(state.Stream.DrawDistance),
                static_cast<double>(state.Stream.FadeStart), static_cast<double>(state.Stream.SingleBladeDistance),
                state.Stream.MaxInstances);
}

} // namespace

void Deko3dConfigureGrass(std::string_view graphicsJson, bool fps60) {
    if (gGrass.load(std::memory_order_acquire) != nullptr) {
        return;
    }
    const std::filesystem::path config = "sdmc:/switch/oot3dre/config";
    const auto mode = Oot3d::ReadGrassSwitchMode(config);
    if (mode == Oot3d::GrassSwitchMode::Off) {
        return;
    }
    auto state = std::make_unique<GrassState>();
    state->Mode = mode;
    try {
        auto document = Oot3d::SerializeGraphicsSettings(Oot3d::GraphicsSettings{});
        document.merge_patch(nlohmann::json::parse(graphicsJson));
        state->Settings = Oot3d::DeserializeGraphicsSettings(document).Value.Grass;
    } catch (const std::exception& exception) {
        Deko3d::Log("grass: preset unreadable (%s); off", exception.what());
        return;
    }
    if (state->Settings.Rules.empty()) {
        Deko3d::Log("grass: the preset has no source rules; off");
        return;
    }
    // Rule textures' signatures are kept across sessions (as on PC, beside the
    // shader pack): a ground that takes its rule from a twin drawn in another
    // area then grows grass in a session that starts there (a save in the
    // Forest Temple had none until Sacred Forest Meadow was visited).
    const size_t kept = Oot3d::GrassTextureSourceCache::Instance().SetSignatureStore(
        "sdmc:/switch/oot3dre/shadercache/grass_texture_signatures.bin");
    Deko3d::Log("grass: %zu rule texture signatures kept from earlier sessions "
                "(shadercache/grass_texture_signatures.bin)",
                kept);
    ApplyProfile(*state, fps60, Oot3d::ReadGrassSwitchDistance(config));
    state->Placements = std::make_unique<Oot3d::PicaGrassPlacements>(state->Profile.RetainedBytes);
    Deko3d::Log("grass: on (config/grass = %s), %zu rules, %.0f tufts/m2 of %u blades, at most %u per placement, "
                "%.1f MiB retained",
                mode == Oot3d::GrassSwitchMode::Draw ? "draw" : "placements", state->Settings.Rules.size(),
                static_cast<double>(state->Profile.TuftsPerSquareMeter), state->Profile.BladesPerTuft,
                state->Profile.MaxAnchorsPerPlacement,
                static_cast<double>(state->Profile.RetainedBytes) / (1024.0 * 1024.0));
    LogProfile(*state);
    gGrassWantsColliders = mode == Oot3d::GrassSwitchMode::Draw;
    gGrass.store(state.release(), std::memory_order_release);
}

void Deko3dSetGrassFrameRate(bool fps60) {
    GrassState* state = gGrass.load(std::memory_order_acquire);
    if (state == nullptr || state->Fps60 == fps60) {
        return;
    }
    // Stream settings are copied into each cull job: the next job culls with
    // the new distances and cap (the stream memory holds the largest cap).
    ApplyProfile(*state, fps60, state->Distance);
    LogProfile(*state);
}

void Deko3dSetGrassArea(int scene, int room) {
    gGrassArea.store(Oot3d::GrassArea(scene, room), std::memory_order_relaxed);
}

bool Deko3dGrassWantsColliders() {
    return gGrassWantsColliders;
}

namespace Deko3d {
void CheckGrassArea(GrassState& state);

bool GrassDrawEnabled() {
    const GrassState* state = gGrass.load(std::memory_order_acquire);
    return state != nullptr && state->Mode == Oot3d::GrassSwitchMode::Draw;
}

void GrassObserveDraw(const GfxNativePicaDrawView& draw, uint64_t frame) {
    GrassState* state = gGrass.load(std::memory_order_acquire);
    if (state == nullptr || draw.CompositionDomain != decltype(draw.CompositionDomain)::Scene ||
        draw.Composition.Layer != decltype(draw.Composition.Layer)::OpaqueWorld) {
        return;
    }
    if (state->DrawFrame != frame) {
        state->DrawFrame = frame;
        state->DrawOrder = 0;
        CheckGrassArea(*state);
    }
    const auto begin = std::chrono::steady_clock::now();
    if (state->InBetweenFrame == frame) {
        // fps60 in-between frame: no sources of its own. A draw that was a
        // source of the last real frame gives its target's camera.
        if (state->InBetween.InBetweenDraw(
                frame, SourceDrawKey(draw.GeometryIdentity, draw.RenderTargetNamespace,
                                     draw.FramebufferColorPhysicalAddress),
                TargetKey(draw.RenderTargetNamespace, draw.FramebufferColorPhysicalAddress), draw.VertexUniformBytes,
                draw.FramebufferFlipped)) {
            state->SourceFrame = frame;
            state->SourceNamespace = draw.RenderTargetNamespace;
            state->SourceColorAddress = draw.FramebufferColorPhysicalAddress;
        }
    } else {
        try {
            if (Oot3d::PublishPicaGrassSource(draw, frame, state->Settings, *state->Sources, frame,
                                              state->DrawOrder++)) {
                ++state->Published;
                state->SourceFrame = frame;
                state->SourceNamespace = draw.RenderTargetNamespace;
                state->SourceColorAddress = draw.FramebufferColorPhysicalAddress;
                if (const auto clip = Oot3d::GrassRigidModelToClip(draw.VertexUniformBytes, draw.FramebufferFlipped))
                    state->InBetween.RealSource(SourceDrawKey(draw.GeometryIdentity, draw.RenderTargetNamespace,
                                                              draw.FramebufferColorPhysicalAddress),
                                                *clip);
            }
        } catch (const std::exception& exception) {
            static bool reported = false;
            if (!reported) {
                reported = true;
                Log("grass: source failed (%s)", exception.what());
            }
        }
    }
    const double ms = MillisecondsSince(begin);
    state->ObserveMs += ms;
    state->ObserveMaxMs = std::max(state->ObserveMaxMs, ms);
}

void GrassNoteSample(bool synthetic, uint64_t frame) {
    GrassState* state = gGrass.load(std::memory_order_acquire);
    if (state == nullptr) return;
    if (synthetic) state->InBetweenFrame = frame;
    else if (state->InBetweenFrame == frame) state->InBetweenFrame = ~0ULL;
}

// A scene or room change: the drawn cull, the sources' memo (whose geometry
// identities the new area may reuse) and this frame's placements are
// dropped, so no grass of the previous area is ever drawn in the new one.
void CheckGrassArea(GrassState& state) {
    const uint32_t area = gGrassArea.load(std::memory_order_relaxed);
    if (area == state.Area) return;
    state.Area = area;
    ++state.AreaChanges;
    state.Slots.Reset();
    state.Current = {};
    state.Sources = std::make_unique<Oot3d::PicaGrassSourceState>();
    state.Frame.Clear();
    state.RealFrame.Clear();
    state.InBetween.Clear();
    state.SourceFrame = ~0ULL;
}

namespace {

// Placements of this frame's sources, once per frame (at the first draw
// point, else at the end of the frame); in an fps60 in-between frame, the
// last real frame's placements with this frame's camera.
void UpdatePlacements(GrassState& state, uint64_t frame) {
    CheckGrassArea(state);
    if (state.PreparedFrame == frame) return;
    state.PreparedFrame = frame;
    state.Frame.Clear();
    state.FrameUniforms = {DK_GPU_ADDR_INVALID, DK_GPU_ADDR_INVALID};
    state.Posted = false;
    const auto begin = std::chrono::steady_clock::now();
    try {
        if (state.InBetweenFrame == frame) {
            ++state.InBetweenFrames;
            const uint64_t reference = state.InBetween.ReferenceFrame();
            if (reference < frame && frame - reference <= Oot3d::kGrassInBetweenReach)
                state.Frame.AssignMoved(state.RealFrame, [&](const FramePlacement& real) {
                    return MoveToInBetween(state, real, frame);
                });
            if (state.Frame.Empty() && !state.RealFrame.Empty()) ++state.InBetweenWithoutCamera;
        } else {
            const auto& drawables = state.Placements->Update(frame, state.Settings);
            if (state.Mode == Oot3d::GrassSwitchMode::Draw) {
                for (const auto& drawable : drawables)
                    state.Frame.Add(drawable.Placement->Identity, drawable.Mesh.RenderTargetNamespace,
                                    drawable.Mesh.FramebufferColorPhysicalAddress, BuildFramePlacement(drawable, state));
            }
        }
    } catch (const std::exception& exception) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            Log("grass: placement failed (%s)", exception.what());
        }
    }
    const double ms = MillisecondsSince(begin);
    state.UpdateMs += ms;
    state.UpdateMaxMs = std::max(state.UpdateMaxMs, ms);
}

// The GPU time of the grass draws of the frame that last used this slot
// (end-of-pipe timestamps: before the grass, after its blades, after its tufts).
void CollectGpuTime(GrassState& state, uint32_t slot) {
    if (state.Timestamps == nullptr || !state.TimestampPending[slot]) return;
    state.TimestampPending[slot] = false;
    const auto* reports = static_cast<const uint8_t*>(dkMemBlockGetCpuAddr(state.Timestamps)) +
                          slot * kTimestampsPerSlot * kTimestampBytes;
    std::array<uint64_t, kTimestampsPerSlot> t{};
    for (uint32_t i = 0; i < kTimestampsPerSlot; ++i) std::memcpy(&t[i], reports + i * kTimestampBytes + 8U, 8U);
    if (t[2] <= t[0] || t[1] < t[0] || t[2] < t[1]) return;
    // Report timestamps count GPU ticks (614.4 MHz on the Tegra X1).
    const auto ms = [](uint64_t ticks) { return static_cast<double>(ticks * 625U / 384U) / 1.0e6; };
    ++state.GpuSamples;
    state.GpuMs += ms(t[2] - t[0]);
    state.GpuMaxMs = std::max(state.GpuMaxMs, ms(t[2] - t[0]));
    state.GpuBladesMs += ms(t[1] - t[0]);
    state.GpuTuftsMs += ms(t[2] - t[1]);
}

// The newest cull becomes the one drawn; the slot it replaces stays read
// by the frames in flight until they finish (GrassStreamSlots).
void TakeCull(GrassState& state) {
    GrassCullWorker::Result result;
    if (state.Worker == nullptr || !state.Worker->Take(result)) return;
    ++state.CullJobs;
    state.CullMs += result.Milliseconds;
    state.CullMaxMs = std::max(state.CullMaxMs, result.Milliseconds);
    state.ClustersVisible += result.Stats.ClustersVisible;
    state.ClustersTested += result.Stats.ClustersTested;
    state.OverBudgetFrames += result.Stats.ClustersOverBudget != 0U ? 1U : 0U;
    const uint64_t instances = result.Stats.Tufts + result.Stats.Blades;
    state.CullInstances += instances;
    state.CullInstancesMax = std::max(state.CullInstancesMax, instances);
    state.CullThinned += result.Stats.AnchorsThinned;
    state.Slots.FinishWrite(result.Slot, result.Frame);
    state.Current = std::move(result);
}

// Hands this frame's placements to the worker, into a slot no frame in
// flight reads.
void PostCull(GrassState& state, uint64_t frame) {
    if (state.Posted || state.Worker == nullptr || state.Worker->Busy()) return;
    state.Posted = true;
    TakeCull(state); // its slot must not be handed out again
    // One entry per placement: the camera of the target grass was last drawn
    // into, else of any target it was drawn into this frame.
    std::unordered_map<uint64_t, const FramePlacement*> unique;
    for (const auto& [key, placement] : state.Frame) {
        auto& chosen = unique[placement.Identity];
        if (chosen == nullptr || (placement.Namespace == state.DrawnNamespace &&
                                  placement.ColorAddress == state.DrawnColorAddress))
            chosen = &placement;
    }
    if (unique.empty()) return;
    const auto slot = state.Slots.BeginWrite(frame);
    if (!slot) return;
    GrassCullWorker::Job job;
    job.Settings = state.Stream;
    job.Capacity = state.SlotInstances;
    job.Slot = *slot;
    job.Frame = frame;
    job.Area = state.Area;
    job.Output = reinterpret_cast<Oot3d::GrassStreamInstance*>(
        static_cast<uint8_t*>(dkMemBlockGetCpuAddr(state.StreamMemory)) +
        size_t{*slot} * state.SlotInstances * sizeof(Oot3d::GrassStreamInstance));
    for (const auto& [identity, placement] : unique) {
        Oot3d::GrassStreamPlacement input;
        input.World = placement->World;
        input.PositionToClip = placement->Uniforms.PositionToClip;
        input.Eye = {placement->Uniforms.Eye[0], placement->Uniforms.Eye[1], placement->Uniforms.Eye[2]};
        job.Placements.push_back(std::move(input));
        job.Identities.push_back(identity);
    }
    state.Worker->Post(std::move(job));
}

} // namespace

void GrassEndFrame(uint64_t frame) {
    GrassState* state = gGrass.load(std::memory_order_acquire);
    if (state == nullptr) {
        return;
    }
    // L3+R3 > Video > "Grass distance" while the game runs: the next cull
    // job reads the new distances and cap (the stream memory holds any cap).
    if (const auto distance = Oot3d::PublishedGrassSwitchDistance(); distance && *distance != state->Distance) {
        ApplyProfile(*state, state->Fps60, *distance);
        LogProfile(*state);
    }
    UpdatePlacements(*state, frame);
    if (state->Mode == Oot3d::GrassSwitchMode::Draw && state->DrawReady) {
        // A frame without a draw point (no scene target this frame) still
        // keeps the cull following the camera.
        PostCull(*state, frame);
    }
    if (state->InBetweenFrame != frame) {
        // A real frame: its placements and source draws are what the fps60
        // in-between frames after it draw, with their own camera.
        state->RealFrame = std::move(state->Frame);
        state->Frame.Clear();
        state->InBetween.EndRealFrame(frame);
    }
    if (frame > 3U) {
        Oot3d::GrassSceneBridge::Instance().PruneBeforeFrame(frame - 2U);
    }
    const auto& stats = state->Placements->Stats();
    state->AnchorsMax = std::max(state->AnchorsMax, stats.Anchors);
    if (++state->Frames < kLogEveryFrames) {
        return;
    }
    const double frames = static_cast<double>(state->Frames);
    const double drawn = std::max<double>(1.0, static_cast<double>(state->DrawnFrames));
    Log("grass: frame %llu %s %s | rules_seen %u meshes %u placements %u queued %u tufts %llu (max %llu) "
        "retained %.1f MiB | sources %.3f ms/frame (max %.2f, %llu published) placements %.3f ms/frame (max %.2f)",
        static_cast<unsigned long long>(frame), state->Mode == Oot3d::GrassSwitchMode::Draw ? "draw" : "placements",
        state->Fps60 ? "fps60" : "fps30", stats.Rules, stats.Meshes, stats.Ready, stats.Queued,
        static_cast<unsigned long long>(stats.Anchors), static_cast<unsigned long long>(state->AnchorsMax),
        static_cast<double>(stats.ResidentBytes) / (1024.0 * 1024.0), state->ObserveMs / frames,
        state->ObserveMaxMs, static_cast<unsigned long long>(state->Published), state->UpdateMs / frames,
        state->UpdateMaxMs);
    if (state->Mode == Oot3d::GrassSwitchMode::Draw) {
        const double jobs = std::max<double>(1.0, static_cast<double>(state->CullJobs));
        const double gpu = std::max<double>(1.0, static_cast<double>(state->GpuSamples));
        Log("grass: frame %llu drawn %llu/%.0f frames | visible %.0f (max %llu, budget %u) tufts %.0f blades %.0f "
            "draws %.1f clusters %.0f/%.0f over_budget %llu | worker %.3f ms/cull (max %.2f, %llu culls) | "
            "render draw %.3f ms/frame (max %.2f) | gpu %.3f ms/frame (max %.2f, %llu samples; blades %.3f tufts %.3f)%s",
            static_cast<unsigned long long>(frame), static_cast<unsigned long long>(state->DrawnFrames), frames,
            static_cast<double>(state->Visible) / drawn, static_cast<unsigned long long>(state->VisibleMax),
            state->Stream.MaxInstances, static_cast<double>(state->Tufts) / drawn,
            static_cast<double>(state->Blades) / drawn, static_cast<double>(state->Draws) / drawn,
            static_cast<double>(state->ClustersVisible) / jobs, static_cast<double>(state->ClustersTested) / jobs,
            static_cast<unsigned long long>(state->OverBudgetFrames), state->CullMs / jobs, state->CullMaxMs,
            static_cast<unsigned long long>(state->CullJobs), state->DrawMs / frames, state->DrawMaxMs,
            state->GpuMs / gpu, state->GpuMaxMs, static_cast<unsigned long long>(state->GpuSamples),
            state->GpuBladesMs / gpu, state->GpuTuftsMs / gpu,
            state->DrawFailed ? " | DRAW OFF (setup failed, see above)" : "");
        // Which fragment program drew (gl_FragDepth turns early depth
        // testing off), how many draw points reused an older cull, and what
        // the culls selected.
        Log("grass: frame %llu fragment raster %llu draws, depth-writing %llu | culls fresh %llu stale %llu "
            "(frames that redrew an older cull), no result %llu, nothing drawn %llu | cull instances %.0f "
            "(max %llu) thinned %.0f | distance=%s full %.0f max %.0f | area %s scene %d room %d, changes %llu, "
            "foreign culls dropped %llu (other area %llu, no shared placement %llu), ranges drawn with another "
            "target's placement %llu | empty placements %u | in-between frames %llu (without a camera %llu)",
            static_cast<unsigned long long>(frame), static_cast<unsigned long long>(state->RasterDraws),
            static_cast<unsigned long long>(state->DepthDraws),
            static_cast<unsigned long long>(state->Slots.FreshFrames - state->FreshBase),
            static_cast<unsigned long long>(state->Slots.StaleFrames - state->StaleBase),
            static_cast<unsigned long long>(state->NoResultFrames), static_cast<unsigned long long>(state->EmptyFrames),
            static_cast<double>(state->CullInstances) / jobs, static_cast<unsigned long long>(state->CullInstancesMax),
            static_cast<double>(state->CullThinned) / jobs,
            Oot3d::GrassSwitchDistanceWord(state->Distance).data(),
            static_cast<double>(state->Stream.FullDensityDistance),
            static_cast<double>(state->Stream.DrawDistance), state->DrawnArea == 0xffffffffU ? "none" : "drawn",
            state->DrawnArea == 0xffffffffU ? -1 : static_cast<int>(state->DrawnArea >> 8U),
            state->DrawnArea == 0xffffffffU ? -1 : static_cast<int>(static_cast<int8_t>(state->DrawnArea & 0xffU)),
            static_cast<unsigned long long>(state->AreaChanges),
            static_cast<unsigned long long>(state->ForeignOtherArea + state->ForeignNoPlacement),
            static_cast<unsigned long long>(state->ForeignOtherArea),
            static_cast<unsigned long long>(state->ForeignNoPlacement),
            static_cast<unsigned long long>(state->OtherTargetRanges), state->Placements->Stats().Empty,
            static_cast<unsigned long long>(state->InBetweenFrames),
            static_cast<unsigned long long>(state->InBetweenWithoutCamera));
    }
    state->Frames = 0;
    state->Published = 0;
    state->ObserveMs = state->ObserveMaxMs = 0.0;
    state->UpdateMs = state->UpdateMaxMs = 0.0;
    state->DrawMs = state->DrawMaxMs = 0.0;
    state->AnchorsMax = 0;
    state->DrawnFrames = state->Visible = state->VisibleMax = state->Tufts = state->Blades = state->Draws = 0;
    state->ClustersVisible = state->ClustersTested = state->OverBudgetFrames = state->CullJobs = 0;
    state->CullMs = state->CullMaxMs = 0.0;
    state->GpuSamples = 0;
    state->GpuMs = state->GpuMaxMs = state->GpuBladesMs = state->GpuTuftsMs = 0.0;
    state->RasterDraws = state->DepthDraws = 0;
    state->CullInstances = state->CullInstancesMax = state->CullThinned = 0;
    state->EmptyFrames = state->NoResultFrames = 0;
    state->FreshBase = state->Slots.FreshFrames;
    state->AreaChanges = state->ForeignOtherArea = state->ForeignNoPlacement = state->OtherTargetRanges = 0;
    state->InBetweenFrames = state->InBetweenWithoutCamera = 0;
    state->StaleBase = state->Slots.StaleFrames;
}

} // namespace Deko3d

bool GfxRenderingAPIDeko3d::PublishPicaFrameTemporalSample(const Renderer3ds::PicaFrameTemporalSample& sample) {
    // Before the sample's draws, into the device frame that presents them.
    Deko3d::GrassNoteSample(sample.Synthetic, mDevice.Frame());
    return true;
}

void GfxRenderingAPIDeko3d::RememberGrassScene(const Deko3d::PicaTarget& target, const Deko3d::PicaRect& viewport,
                                               const DkScissor& scissor, uint8_t depthCompare) {
    mGrassScene.Target = &target;
    mGrassScene.Viewport = viewport;
    mGrassScene.Scissor = scissor;
    mGrassScene.DepthCompare = depthCompare;
    mGrassScene.Frame = mDevice.Frame();
}

void GfxRenderingAPIDeko3d::DrawGrass(const Deko3d::PicaTargetKey& key, Deko3d::PicaTarget& target) {
    using namespace Deko3d;
    GrassState* state = gGrass.load(std::memory_order_acquire);
    const uint64_t frame = mDevice.Frame();
    // Once per target and frame, into the target the opaque world was just
    // drawn to (its viewport, scissor and depth test: dynamic resolution and
    // widescreen included).
    // Not before this target's grass sources were drawn this frame: the
    // placements of this frame are matched at the first draw point.
    if (state == nullptr || state->DrawFailed || mGrassScene.Target != &target || mGrassScene.Frame != frame ||
        (mGrassDrawn.Frame == frame && mGrassDrawn.Target == &target) || state->SourceFrame != frame ||
        state->SourceNamespace != key.Namespace || state->SourceColorAddress != key.ColorAddress) {
        return;
    }
    mGrassDrawn = {&target, frame};
    const auto begin = std::chrono::steady_clock::now();
    if (!state->DrawReady) {
        state->Vertex = mShaders.Embedded(kGrassVertexKey, std::span<const uint8_t>(kGrassVertexDksh));
        state->Fragment = mShaders.Embedded(kGrassFragmentKey, std::span<const uint8_t>(kGrassFragmentDksh));
        state->FragmentDepth =
            mShaders.Embedded(kGrassFragmentDepthKey, std::span<const uint8_t>(kGrassFragmentDepthDksh));
        // The largest cap of every rate and distance (Whole area): L3+R3 >
        // Video > FPS and "Grass distance" switch the profile live.
        state->SlotInstances = Oot3d::GrassSwitchStreamCapacity();
        const uint32_t streamBytes =
            AlignUp(kStreamSlots * state->SlotInstances * static_cast<uint32_t>(sizeof(Oot3d::GrassStreamInstance)),
                    DK_MEMBLOCK_ALIGNMENT);
        state->StreamMemory =
            mDevice.MakeBlock(streamBytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "grass instances");
        static_assert(kFramesInFlight * kTimestampsPerSlot * kTimestampBytes <= DK_MEMBLOCK_ALIGNMENT);
        state->Timestamps = mDevice.MakeBlock(DK_MEMBLOCK_ALIGNMENT,
                                              DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "grass timer");
        if (state->Vertex == nullptr || state->Fragment == nullptr || state->FragmentDepth == nullptr ||
            state->StreamMemory == nullptr) {
            state->DrawFailed = true;
            Log("grass: drawing off (shaders %d/%d/%d, instance memory %.1f MiB %s)", state->Vertex != nullptr,
                state->Fragment != nullptr, state->FragmentDepth != nullptr,
                static_cast<double>(streamBytes) / (1024.0 * 1024.0), state->StreamMemory != nullptr ? "ok" : "failed");
            return;
        }
        state->Worker = std::make_unique<GrassCullWorker>();
        state->DrawReady = true;
        Log("grass: drawing ready (%.1f MiB of instance memory: %u slots of %u tufts, shaders %zu/%zu/%zu B)",
            static_cast<double>(streamBytes) / (1024.0 * 1024.0), kStreamSlots, state->SlotInstances,
            sizeof(kGrassVertexDksh), sizeof(kGrassFragmentDksh), sizeof(kGrassFragmentDepthDksh));
    }
    UpdatePlacements(*state, frame);
    CollectGpuTime(*state, mDevice.Slot());

    TakeCull(*state);

    DkCmdBuf cmd = mDevice.Cmd();
    uint32_t draws = 0U;
    uint64_t visible = 0U;
    bool timed = false;
    // A cull of another area, or with none of its placements among this
    // frame's (by identity, whatever target this frame draws into), is
    // dropped: never drawn with this frame's cameras.
    if (state->Current.Valid) {
        const auto fit = Oot3d::GrassCullFitsFrame(state->Current.Area, state->Area, state->Current.Identities,
                                                   [&](uint64_t identity) { return state->Frame.Has(identity); });
        if (fit != Oot3d::GrassCullFit::Draw) {
            ++(fit == Oot3d::GrassCullFit::OtherArea ? state->ForeignOtherArea : state->ForeignNoPlacement);
            state->Slots.Reset();
            state->Current = {};
        }
    }
    const auto drawnSlot = state->Slots.Draw(frame);
    if (!drawnSlot) ++state->NoResultFrames;
    else state->DrawnArea = state->Area;
    const auto& current = state->Current;
    const auto timestamp = [&](uint32_t index) {
        dkCmdBufReportCounter(cmd, DkCounter_Timestamp,
                              dkMemBlockGetGpuAddr(state->Timestamps) +
                                  (mDevice.Slot() * kTimestampsPerSlot + index) * kTimestampBytes);
    };
    // Blades first, then tufts, a timestamp between them: the gpu split.
    for (uint32_t pass = 0; pass < 2U && drawnSlot && current.Valid; ++pass) {
        const auto kind = pass == 0U ? Oot3d::GrassStreamKind::Blades : Oot3d::GrassStreamKind::Tufts;
        if (pass == 1U && timed) timestamp(1U);
        for (const auto& range : current.Ranges) {
            if (range.Kind != kind) continue;
            // This target's own placement (its camera), else the placement
            // as drawn into another target this frame (this frame's camera).
            bool otherTarget = false;
            auto* found =
                state->Frame.Find(current.Identities[range.Placement], key.Namespace, key.ColorAddress, &otherTarget);
            if (found == nullptr) continue;
            state->OtherTargetRanges += otherTarget ? 1U : 0U;
            auto& placement = *found;
            if (draws == 0U) {
                FlushPicaInvalidations();
                BindPicaTarget(target);
                ++target.Version;
                if (state->Timestamps != nullptr) {
                    timestamp(0U);
                    timed = true;
                }
                // Opaque, no blend; RGB only (the target's alpha belongs to
                // the native draws); the depth test of the scene's opaque draws.
                DkColorState colorState;
                dkColorStateDefaults(&colorState);
                dkCmdBufBindColorState(cmd, &colorState);
                DkColorWriteState writeState;
                dkColorWriteStateDefaults(&writeState);
                dkColorWriteStateSetMask(&writeState, 0, DkColorMask_R | DkColorMask_G | DkColorMask_B);
                dkCmdBufBindColorWriteState(cmd, &writeState);
                DkDepthStencilState depthStencil;
                dkDepthStencilStateDefaults(&depthStencil);
                depthStencil.depthTestEnable = true;
                depthStencil.depthWriteEnable = true;
                depthStencil.depthCompareOp = static_cast<DkCompareOp>(mGrassScene.DepthCompare);
                depthStencil.stencilTestEnable = false;
                dkCmdBufBindDepthStencilState(cmd, &depthStencil);
                DkRasterizerState rasterizer;
                dkRasterizerStateDefaults(&rasterizer);
                rasterizer.depthClampEnable = 0;
                rasterizer.cullMode = DkFace_None;
                dkCmdBufBindRasterizerState(cmd, &rasterizer);
                const auto& viewport = mGrassScene.Viewport;
                ApplyDkViewport(cmd, static_cast<float>(viewport.X), static_cast<float>(viewport.Y),
                                static_cast<float>(viewport.Width), static_cast<float>(viewport.Height));
                dkCmdBufSetScissors(cmd, 0, &mGrassScene.Scissor, 1);
                std::array<DkVtxAttribState, 5> attributes{};
                const auto attribute = [](uint32_t offset, DkVtxAttribSize size, DkVtxAttribType type) {
                    DkVtxAttribState value{};
                    value.bufferId = 0;
                    value.isFixed = 0;
                    value.offset = offset;
                    value.size = size;
                    value.type = type;
                    value.isBgra = 0;
                    return value;
                };
                attributes[0] = attribute(0U, DkVtxAttribSize_4x32, DkVtxAttribType_Float);
                attributes[1] = attribute(16U, DkVtxAttribSize_1x32, DkVtxAttribType_Uint);
                attributes[2] = attribute(20U, DkVtxAttribSize_1x32, DkVtxAttribType_Uint);
                attributes[3] = attribute(24U, DkVtxAttribSize_1x32, DkVtxAttribType_Uint);
                attributes[4] = attribute(28U, DkVtxAttribSize_1x32, DkVtxAttribType_Uint);
                dkCmdBufBindVtxAttribState(cmd, attributes.data(), static_cast<uint32_t>(attributes.size()));
                const DkVtxBufferState buffer = {static_cast<uint32_t>(sizeof(Oot3d::GrassStreamInstance)), 1U};
                dkCmdBufBindVtxBufferState(cmd, &buffer, 1);
                for (uint32_t kind = 0; kind < 2U; ++kind) {
                    const auto frameUniforms = BuildFrameUniforms(*state, kind == 1U);
                    const StreamSpan span = mDevice.Stream(sizeof(frameUniforms), DK_UNIFORM_BUF_ALIGNMENT);
                    if (span.Cpu != nullptr) {
                        std::memcpy(span.Cpu, &frameUniforms, sizeof(frameUniforms));
                        state->FrameUniforms[kind] = span.Gpu;
                    }
                }
            }
            if (placement.UniformsGpu == DK_GPU_ADDR_INVALID) {
                const StreamSpan span = mDevice.Stream(sizeof(placement.Uniforms), DK_UNIFORM_BUF_ALIGNMENT);
                if (span.Cpu == nullptr) continue;
                std::memcpy(span.Cpu, &placement.Uniforms, sizeof(placement.Uniforms));
                placement.UniformsGpu = span.Gpu;
            }
            const bool blades = range.Kind == Oot3d::GrassStreamKind::Blades;
            const DkGpuAddr frameUniforms = state->FrameUniforms[blades ? 1U : 0U];
            if (frameUniforms == DK_GPU_ADDR_INVALID) continue;
            const DkShader* shaders[] = {state->Vertex,
                                         placement.ExplicitDepth ? state->FragmentDepth : state->Fragment};
            dkCmdBufBindShaders(cmd, DkStageFlag_Vertex | DkStageFlag_Fragment, shaders, 2);
            const uint32_t placementBytes = AlignUp(sizeof(GrassPlacementUniforms), DK_UNIFORM_BUF_ALIGNMENT);
            dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 0, placement.UniformsGpu, placementBytes);
            dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, placement.UniformsGpu, placementBytes);
            dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 1, frameUniforms,
                                      AlignUp(sizeof(GrassFrameUniforms), DK_UNIFORM_BUF_ALIGNMENT));
            const DkGpuAddr instances = dkMemBlockGetGpuAddr(state->StreamMemory) +
                                        (uint64_t{*drawnSlot} * state->SlotInstances + range.First) *
                                            sizeof(Oot3d::GrassStreamInstance);
            const DkBufExtents extent = {instances, range.Count * static_cast<uint32_t>(
                                                                      sizeof(Oot3d::GrassStreamInstance))};
            dkCmdBufBindVtxBuffers(cmd, 0, &extent, 1);
            // Both kinds are BladesPerTuft triangles per instance (grass_vsh.glsl).
            dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3U * state->Profile.BladesPerTuft, range.Count, 0, 0);
            (blades ? state->Blades : state->Tufts) += range.Count;
            visible += range.Count;
            ++draws;
            ++(placement.ExplicitDepth ? state->DepthDraws : state->RasterDraws);
            mDevice.Checkpoint("grass", range.Count, blades ? 1U : 0U);
        }
    }
    if (drawnSlot && draws == 0U && (current.Stats.Tufts + current.Stats.Blades) != 0U) ++state->EmptyFrames;
    if (timed) {
        timestamp(2U);
        state->TimestampPending[mDevice.Slot()] = true;
    }
    state->DrawnNamespace = key.Namespace;
    state->DrawnColorAddress = key.ColorAddress;
    if (draws != 0U) {
        ++state->DrawnFrames;
        state->Visible += visible;
        state->VisibleMax = std::max(state->VisibleMax, visible);
        state->Draws += draws;
    }
    PostCull(*state, frame);
    const double ms = MillisecondsSince(begin);
    state->DrawMs += ms;
    state->DrawMaxMs = std::max(state->DrawMaxMs, ms);
}

} // namespace Fast
#else
namespace Fast {
void Deko3dConfigureGrass(std::string_view, bool) {}
void Deko3dSetGrassFrameRate(bool) {}
void Deko3dSetGrassArea(int, int) {}
bool Deko3dGrassWantsColliders() {
    return false;
}
} // namespace Fast
#endif
