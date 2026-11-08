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
// - grass cull worker (service thread, cores 1|2): culls per cluster and per
//   tuft, adds the fill tufts near the camera and writes the 32-byte
//   instances straight into GPU-visible memory (grass_instance_stream.h);
//   its result is drawn from the next frame on;
// - placement builder worker (service thread): builds placements, thinned
//   evenly to the profile's cap (ThinGrassAnchorsToLimit).
// The game thread only reads Link's and Epona's colliders when drawing is on
// (CollectNativeGrassColliders, a few guest reads per step).

#include "fast/backends/gfx_deko3d.h"

#if defined(__SWITCH__)
#include "fast/backends/gfx_deko3d_pica_pipeline.h"
#include "fast/oot3d/grass_draw_preparation.h"
#include "fast/oot3d/grass_persistent_placement_cache.h"
#include "fast/oot3d/grass_instance_stream.h"
#include "fast/oot3d/grass_interaction_bridge.h"
#include "fast/oot3d/grass_scene_bridge.h"
#include "fast/oot3d/grass_scene_hold.h"
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
#include <bit>
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
// Per device slot: before the grass, after the tuft pass (config/grass_lod;
// the same as the first without it), after the near draws (single blades),
// after the far ones (tufts).
constexpr uint32_t kTimestampsPerSlot = 4U;
// Keys of the grass stages in the ShaderStore (not text hashes).
constexpr uint64_t kGrassVertexKey = 0xDE0C3D56A5500001ULL;
constexpr uint64_t kGrassFragmentKey = 0xDE0C3D56A5500002ULL;
constexpr uint64_t kGrassFragmentDepthKey = 0xDE0C3D56A5500003ULL;
constexpr uint64_t kGrassTuftVertexKey = 0xDE0C3D56A5500004ULL;
constexpr uint64_t kGrassLodFarVertexKey = 0xDE0C3D56A5500005ULL;
constexpr uint64_t kGrassLodNearVertexKey = 0xDE0C3D56A5500006ULL;
// config/grass_lod: one GrassTuft (grass_lod_common.glsl) per instance of a
// stream slot, in two buffers taken in turn by the draw points that run the
// tuft pass (the one written was last read before the previous pass's barrier).
constexpr uint32_t kTuftBytes = 64U;
constexpr uint32_t kTuftBuffers = 2U;
// No vertex attributes (the blade programs read their tufts from a buffer).
constexpr DkVtxAttribState kNoAttribute{};

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

// std140 GrassLod of grass_lod_common.glsl (per draw).
struct GrassLodUniforms {
    std::array<float, 4> Scale{}; // focal, level-0 distance, 1 / ramp length, unused
    std::array<float, 4> Pixels{}; // kGrassLodPixels
    std::array<float, 4> Draw{};  // 1 / blades, target blades, 1 / target, level of the first layout
    std::array<uint32_t, 4> Index{}; // float bits of 1 / (3 x blades), 3 x blades
};
static_assert(sizeof(GrassLodUniforms) == 64U);

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
        bool Lod = false; // the kinds of config/grass_lod on (GrassStreamSettings::Lod)
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
    // config/grass_hold: until the posted and the running job are done, at
    // most `timeout`; true when idle.
    [[nodiscard]] bool WaitIdle(std::chrono::microseconds timeout) {
        std::unique_lock lock(mMutex);
        return mIdle.wait_for(lock, timeout, [this] { return !mJob.has_value() && !mRunning; });
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
            result.Lod = job.Settings.Lod;
            result.Identities = std::move(job.Identities);
            result.Valid = true;
            std::scoped_lock lock(mMutex);
            mResult = std::move(result);
            mRunning = false;
            mIdle.notify_all();
        }
    }

    std::mutex mMutex;
    std::condition_variable mWake;
    std::condition_variable mIdle; // a job finished (WaitIdle)
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
    // config/grass_lod (L3+R3 > Video > "Grass detail", live): the next
    // cull job takes it; each cull is drawn the way it was culled.
    bool Lod = true;
    Oot3d::InteractiveGrassSettings Settings;
    // config/grass_fill (L3+R3 > Video > "Grass near Link", live): fixed
    // fill tufts (nothing grows near Link) or test.9's growing ones; the
    // next cull job takes it.
    bool Fill = true;
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
    // config/grass_lod: the tuft pass, the far and the near blade programs,
    // and the tuft buffers (made at the first cull culled with it).
    const DkShader* TuftVertex = nullptr;
    const DkShader* LodFarVertex = nullptr;
    const DkShader* LodNearVertex = nullptr;
    DkMemBlock TuftMemory = nullptr;
    bool LodFailed = false;
    uint64_t TuftPasses = 0; // draw points that ran the tuft pass (which buffer is next)
    // This draw point's placement of each range of the drawn cull (scratch).
    std::vector<FramePlacement*> RangePlacements;
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
    DkGpuAddr FrameUniforms = DK_GPU_ADDR_INVALID; // both kinds (grass_vsh.glsl blends them by distance)
    bool Posted = false;

    // Accumulated between logs.
    uint64_t Frames = 0;
    uint64_t Published = 0;
    double ObserveMs = 0.0, ObserveMaxMs = 0.0;
    double UpdateMs = 0.0, UpdateMaxMs = 0.0;
    double DrawMs = 0.0, DrawMaxMs = 0.0;
    uint64_t AnchorsMax = 0;
    // The placement builder's totals at the last log (builds and disk loads
    // per window).
    Oot3d::GrassAsyncPlacementStats BuilderBase{};
    uint64_t DrawnFrames = 0;
    uint64_t Visible = 0, VisibleMax = 0, Tufts = 0, Blades = 0, Draws = 0;
    uint64_t ClustersVisible = 0, ClustersTested = 0, OverBudgetFrames = 0;
    uint64_t CullJobs = 0;
    double CullMs = 0.0, CullMaxMs = 0.0;
    uint64_t GpuSamples = 0;
    double GpuMs = 0.0, GpuMaxMs = 0.0, GpuBladesMs = 0.0, GpuTuftsMs = 0.0, GpuPassMs = 0.0;
    // Instances per draw of a drawn frame (near: single blades and their
    // turn into tufts, or test.9's blades; 5, 3, 1 blades) and their vertices
    // (blades x 3; the tuft pass's points apart).
    uint64_t LevelNear = 0, Level5 = 0, Level3 = 0, Level1 = 0, Vertices = 0, PassPoints = 0, LodFrames = 0;
    uint64_t RasterDraws = 0, DepthDraws = 0;
    uint64_t CullInstances = 0, CullInstancesMax = 0, CullThinned = 0, CullFill = 0, CullOffScreen = 0;
    uint64_t EmptyFrames = 0;   // a draw point with a cull result but nothing of it drawn
    uint64_t NoResultFrames = 0; // a draw point before the scene's first cull finished
    uint64_t FreshBase = 0, StaleBase = 0;
    // The game's area (GrassArea) the placements of this frame belong to.
    uint32_t Area = 0xffffffffU;
    uint32_t DrawnArea = 0xffffffffU;
    // Area and scene changes (CheckGrassArea); culls of another scene dropped
    // and draw points whose cull had none of the frame's placements (kept for
    // the next frame, GrassCullForDrawPoint); cull ranges drawn with their
    // placement as drawn into another target this frame.
    uint64_t AreaChanges = 0, SceneChanges = 0, ForeignOtherScene = 0, NoPlacementFrames = 0, OtherTargetRanges = 0;
    // Draw points without grass right after one with grass, in one scene.
    Oot3d::GrassGapCounter Gaps;
    uint64_t GapsBase = 0;
    // In-between frames, and those that had the real frame's placements but
    // none of its source draws to take their camera from.
    uint64_t InBetweenFrames = 0, InBetweenWithoutCamera = 0;
    // config/grass_hold: the scene's wait for its first grass (frame start,
    // GrassBeginFrame), its draw points without a cull before that grass,
    // and the waits of the log window.
    Oot3d::GrassSceneHold Hold{0U};
    uint64_t SceneNoResult = 0;
    uint64_t HoldMicros = 0, HoldFrames = 0, HoldTimeouts = 0;
};

// Set once before the first frame (Deko3dConfigureGrass), read by the render
// thread afterwards.
std::atomic<GrassState*> gGrass{nullptr};
// Written by the game thread (Deko3dSetGrassArea), read by the render thread.
std::atomic<uint32_t> gGrassArea{0xffffffffU};
// Read by the game thread once per step: true only with config/grass = draw.
bool gGrassWantsColliders = false;

GrassFrameUniforms BuildFrameUniforms(const GrassState& state) {
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
    // Single blades to SingleBladeDistance, turning into tufts over the next half of it.
    frame.Distance = {state.Stream.DrawDistance, state.Stream.FadeStart, state.Stream.SingleBladeDistance,
                      1.5F * state.Stream.SingleBladeDistance};
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
    state.Stream.Lod = state.Lod;
    state.Stream.FillFixed = state.Fill;
    Oot3d::ApplyGrassSwitchProfile(state.Profile, state.Settings);
}

// config/grass_lod in deko3d.txt: what the next culls do.
void LogLod(const GrassState& state) {
    if (!state.Lod) {
        Deko3d::Log("grass: lod off (config/grass_lod = 0): test.9's grass, every vertex does all the work, "
                    "5 blades everywhere");
        return;
    }
    const auto& px = Oot3d::kGrassLodPixels;
    Deko3d::Log("grass: lod on (config/grass_lod): a tuft pass once per tuft, then its blades; 5 -> 3 blades as "
                "a blade slot narrows from %.1f to %.1f px, 3 -> 1 from %.1f to %.1f px (1200x720), never "
                "nearer than %.0f; single blades within %.0f",
                static_cast<double>(px[0]), static_cast<double>(px[1]), static_cast<double>(px[2]),
                static_cast<double>(px[3]), static_cast<double>(Oot3d::GrassLodMinimumDistance(state.Stream)),
                static_cast<double>(1.5F * state.Stream.SingleBladeDistance));
}

// config/grass_fill in deko3d.txt: what the next culls do.
void LogFill(const GrassState& state) {
    if (!state.Fill) {
        Deko3d::Log("grass: fill grows (config/grass_fill = 0): test.9's fill tufts, each growing out of the ground "
                    "over a quarter of its distance (%.2f per tuft at %.0f), thinned tufts growing in",
                    static_cast<double>(Oot3d::GrassStreamFillMultiplier(state.Stream, Oot3d::kGrassLinkDistance)),
                    static_cast<double>(Oot3d::kGrassLinkDistance));
        return;
    }
    Deko3d::Log("grass: fill fixed (config/grass_fill): %.2f tufts per tuft to %.0f units wherever the camera is, "
                "narrowing to test.9's by %.0f; nothing grows",
                static_cast<double>(Oot3d::GrassFixedFillCount(state.Stream)),
                static_cast<double>(Oot3d::kGrassFixedFillNear),
                static_cast<double>(Oot3d::GrassFixedFillFar(state.Stream)));
}

void LogProfile(const GrassState& state) {
    const auto distance = Oot3d::GrassSwitchDistanceWord(state.Distance);
    if (state.Distance == Oot3d::GrassSwitchDistance::Area) {
        Deko3d::Log("grass: %s distance=area: every tuft of the area, at most %u visible (nearest clusters first), "
                    "fill to %.0f",
                    state.Fps60 ? "60 fps" : "30 fps", state.Stream.MaxInstances,
                    static_cast<double>(state.Stream.FillDistance));
        return;
    }
    Deko3d::Log("grass: %s distance=%.*s: full density to %.0f, thinning to %.0f (shrinking from %.0f), single "
                "blades within %.0f (tufts from %.0f), fill to %.0f, at most %u visible",
                state.Fps60 ? "60 fps" : "30 fps", static_cast<int>(distance.size()), distance.data(),
                static_cast<double>(state.Stream.FullDensityDistance), static_cast<double>(state.Stream.DrawDistance),
                static_cast<double>(state.Stream.FadeStart), static_cast<double>(state.Stream.SingleBladeDistance),
                static_cast<double>(1.5F * state.Stream.SingleBladeDistance),
                static_cast<double>(state.Stream.FillDistance), state.Stream.MaxInstances);
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
    // shader pack), and the twins of the preset's rule textures are built in
    // (GrassKnownTwins): a ground that takes its rule from a twin drawn in
    // another area grows grass from the first frame of a session that starts
    // there (a save in the Forest Temple had none until Sacred Forest Meadow
    // was visited, in that session or, with the store, in an earlier one).
    const size_t kept = Oot3d::GrassTextureSourceCache::Instance().SetSignatureStore(
        "sdmc:/switch/oot3dre/shadercache/grass_texture_signatures.bin");
    Deko3d::Log("grass: %zu rule texture signatures kept from earlier sessions "
                "(shadercache/grass_texture_signatures.bin), %zu built-in twins of the rule textures",
                kept, Oot3d::GrassKnownTwins().size());
    state->Lod = Oot3d::ReadGrassSwitchLod(config);
    state->Fill = Oot3d::ReadGrassSwitchFill(config);
    ApplyProfile(*state, fps60, Oot3d::ReadGrassSwitchDistance(config));
    state->Placements = std::make_unique<Oot3d::PicaGrassPlacements>(state->Profile.RetainedBytes);
    // A ground placed in an earlier visit or session loads from the SD card
    // instead of being built again (the PC's cache; one write per new ground,
    // 2 s after its build, on the builder worker).
    const bool placementCache = Oot3d::ReadGrassPlacementCache(config);
    if (placementCache) {
        state->Placements->SetCacheDirectory("sdmc:/switch/oot3dre/shadercache/grass_placements_v1",
                                             Oot3d::kGrassPlacementCacheBytes, Oot3d::kGrassPlacementCacheEntries);
    }
    const bool reuse = Oot3d::ReadGrassPlacementReuse(config);
    state->Placements->SetDiskReuse(reuse);
    state->Hold.SetBudget(Oot3d::ReadGrassSceneHoldMs(config));
    Deko3d::Log("grass: placement cache %s (config/grass_placement_cache; shadercache/grass_placements_v1, at most "
                "%llu MiB)",
                placementCache ? "on" : "off",
                static_cast<unsigned long long>(Oot3d::kGrassPlacementCacheBytes >> 20));
    Deko3d::Log("grass: placement reuse %s (config/grass_placement_reuse: a stored ground loads although its first "
                "camera rounded its transform by up to %g / %g units or it sits at other guest addresses), hold %llu "
                "ms per scene for its first grass (config/grass_hold)",
                reuse ? "on" : "off", static_cast<double>(Oot3d::kGrassPlacementReuseRotation),
                static_cast<double>(Oot3d::kGrassPlacementReuseTranslation),
                static_cast<unsigned long long>(state->Hold.BudgetMicros() / 1000U));
    Deko3d::Log("grass: on (config/grass = %s), %zu rules, %.0f tufts/m2 of %u blades, at most %u per placement, "
                "%.1f MiB retained",
                mode == Oot3d::GrassSwitchMode::Draw ? "draw" : "placements", state->Settings.Rules.size(),
                static_cast<double>(state->Profile.TuftsPerSquareMeter), state->Profile.BladesPerTuft,
                state->Profile.MaxAnchorsPerPlacement,
                static_cast<double>(state->Profile.RetainedBytes) / (1024.0 * 1024.0));
    LogProfile(*state);
    LogLod(*state);
    LogFill(*state);
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
    LogFill(*state);
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

bool GrassWorkPending() {
    const GrassState* state = gGrass.load(std::memory_order_acquire);
    if (state == nullptr || state->Mode != Oot3d::GrassSwitchMode::Draw || !state->DrawReady || state->DrawFailed)
        return false;
    // As GrassBeginFrame's hold: requested placements still being made.
    return state->Placements->Stats().Queued != 0U && state->Placements->BuilderStats().Pending != 0U;
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

// config/grass_hold: at a scene's start, this frame waits for what the
// scene's first grass still needs (grass_scene_hold.h): placements the last
// frame queued, else the first cull of the ready ones.
void GrassBeginFrame() {
    GrassState* state = gGrass.load(std::memory_order_acquire);
    if (state == nullptr || state->Mode != Oot3d::GrassSwitchMode::Draw || !state->DrawReady || state->DrawFailed)
        return;
    state->Hold.BeginFrame();
    if (!state->Hold.Open()) return;
    // Placements requested and not made yet (failed or invalid requests aside).
    const auto& placements = state->Placements->Stats();
    const bool pending = placements.Queued != 0U && state->Placements->BuilderStats().Pending != 0U;
    const auto kind = state->Hold.Next(pending, placements.Ready != 0U,
                                       state->Worker != nullptr && state->Worker->Busy());
    if (kind == Oot3d::GrassHoldWait::None) return;
    const auto begin = std::chrono::steady_clock::now();
    const auto budget = std::chrono::microseconds(state->Hold.LeftMicros());
    const bool done = kind == Oot3d::GrassHoldWait::Placements ? state->Placements->WaitForPending(budget)
                                                               : state->Worker->WaitIdle(budget);
    const auto waited = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count());
    state->Hold.Waited(kind, waited, done);
    state->HoldMicros += waited;
    ++state->HoldFrames;
    state->HoldTimeouts += done ? 0U : 1U;
    Deko3dNoteRenderHold(waited); // not the frame's work (config/pacer_entry_recover)
}

// A scene change: the drawn cull, the sources' memo (whose geometry
// identities the new scene may reuse) and this and the last real frame's
// placements are dropped, so no grass of the previous scene is ever drawn in
// the new one. A room change within the scene keeps them all
// (ClassifyGrassAreaChange): the previous room's ground is still drawn.
void CheckGrassArea(GrassState& state) {
    const uint32_t area = gGrassArea.load(std::memory_order_relaxed);
    const auto change = Oot3d::ClassifyGrassAreaChange(state.Area, area);
    if (change == Oot3d::GrassAreaChange::None) return;
    state.Area = area;
    ++state.AreaChanges;
    if (change == Oot3d::GrassAreaChange::Room) return;
    ++state.SceneChanges;
    state.Slots.Reset();
    state.Current = {};
    state.Sources = std::make_unique<Oot3d::PicaGrassSourceState>();
    state.Frame.Clear();
    state.RealFrame.Clear();
    state.InBetween.Clear();
    state.SourceFrame = ~0ULL;
    // config/grass_hold: a new window and budget for the scene's first grass.
    state.Hold.BeginScene();
    state.SceneNoResult = 0;
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
    state.FrameUniforms = DK_GPU_ADDR_INVALID;
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
// (end-of-pipe timestamps: before the grass, after the tuft pass, after the
// near draws, after the far ones).
void CollectGpuTime(GrassState& state, uint32_t slot) {
    if (state.Timestamps == nullptr || !state.TimestampPending[slot]) return;
    state.TimestampPending[slot] = false;
    const auto* reports = static_cast<const uint8_t*>(dkMemBlockGetCpuAddr(state.Timestamps)) +
                          slot * kTimestampsPerSlot * kTimestampBytes;
    std::array<uint64_t, kTimestampsPerSlot> t{};
    for (uint32_t i = 0; i < kTimestampsPerSlot; ++i) std::memcpy(&t[i], reports + i * kTimestampBytes + 8U, 8U);
    if (t[3] <= t[0] || t[1] < t[0] || t[2] < t[1] || t[3] < t[2]) return;
    // Report timestamps count GPU ticks (614.4 MHz on the Tegra X1).
    const auto ms = [](uint64_t ticks) { return static_cast<double>(ticks * 625U / 384U) / 1.0e6; };
    ++state.GpuSamples;
    state.GpuMs += ms(t[3] - t[0]);
    state.GpuMaxMs = std::max(state.GpuMaxMs, ms(t[3] - t[0]));
    state.GpuPassMs += ms(t[1] - t[0]);
    state.GpuBladesMs += ms(t[2] - t[1]);
    state.GpuTuftsMs += ms(t[3] - t[2]);
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
    const uint64_t instances = result.Stats.Instances();
    state.CullInstances += instances;
    state.CullInstancesMax = std::max(state.CullInstancesMax, instances);
    state.CullThinned += result.Stats.AnchorsThinned;
    state.CullFill += result.Stats.Fill;
    state.CullOffScreen += result.Stats.OffScreen;
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
        LogFill(*state);
    }
    // L3+R3 > Video > "Grass detail" (config/grass_lod) while the game runs:
    // the next cull job culls for it, and each cull is drawn its own way.
    if (const auto lod = Oot3d::PublishedGrassSwitchLod(); lod && *lod != state->Lod) {
        state->Lod = *lod;
        state->Stream.Lod = *lod;
        LogLod(*state);
    }
    // L3+R3 > Video > "Grass near Link" (config/grass_fill): the next cull
    // job's fill (A/B with test.9's within one session).
    if (const auto fill = Oot3d::PublishedGrassSwitchFill(); fill && *fill != state->Fill) {
        state->Fill = *fill;
        state->Stream.FillFixed = *fill;
        LogFill(*state);
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
    const auto builder = state->Placements->BuilderStats();
    const auto& base = state->BuilderBase;
    const uint64_t builds = builder.Builds - base.Builds, loads = builder.DiskHits - base.DiskHits;
    const double loadMs = builder.DiskLoadMilliseconds - base.DiskLoadMilliseconds;
    const double buildMs = builder.BuildMilliseconds - base.BuildMilliseconds - loadMs;
    // Loads that reused a ground of another first camera / address
    // (config/grass_placement_reuse, largest offsets this session), and why
    // the builds were not loaded: no file, a file of another transform
    // (nearest offsets this session), another address (exact matching), damaged.
    Log("grass: frame %llu %s %s | rules_seen %u meshes %u placements %u queued %u tufts %llu (max %llu) "
        "retained %.1f MiB | sources %.3f ms/frame (max %.2f, %llu published) placements %.3f ms/frame (max %.2f) | "
        "built %llu in %.0f ms (longest %.0f ms this session), loaded %llu in %.0f ms (reused %llu of another "
        "transform, up to %g / %g; %llu of another address), written %llu | not loaded: no file %llu, other "
        "transform %llu (nearest %g / %g), other address %llu, damaged %llu",
        static_cast<unsigned long long>(frame), state->Mode == Oot3d::GrassSwitchMode::Draw ? "draw" : "placements",
        state->Fps60 ? "fps60" : "fps30", stats.Rules, stats.Meshes, stats.Ready, stats.Queued,
        static_cast<unsigned long long>(stats.Anchors), static_cast<unsigned long long>(state->AnchorsMax),
        static_cast<double>(stats.ResidentBytes) / (1024.0 * 1024.0), state->ObserveMs / frames,
        state->ObserveMaxMs, static_cast<unsigned long long>(state->Published), state->UpdateMs / frames,
        state->UpdateMaxMs, static_cast<unsigned long long>(builds), buildMs, builder.BuildMaxMilliseconds,
        static_cast<unsigned long long>(loads), loadMs,
        static_cast<unsigned long long>(builder.DiskReusedTransform - base.DiskReusedTransform),
        static_cast<double>(builder.DiskReusedRotation), static_cast<double>(builder.DiskReusedTranslation),
        static_cast<unsigned long long>(builder.DiskReusedGeometry - base.DiskReusedGeometry),
        static_cast<unsigned long long>(builder.DiskWrites - base.DiskWrites),
        static_cast<unsigned long long>(builder.DiskMisses - base.DiskMisses),
        static_cast<unsigned long long>(builder.DiskOtherTransform - base.DiskOtherTransform),
        static_cast<double>(builder.DiskOtherTransformRotation),
        static_cast<double>(builder.DiskOtherTransformTranslation),
        static_cast<unsigned long long>(builder.DiskOtherGeometry - base.DiskOtherGeometry),
        static_cast<unsigned long long>(builder.DiskRejects - base.DiskRejects));
    state->BuilderBase = builder;
    if (state->Mode == Oot3d::GrassSwitchMode::Draw) {
        const double jobs = std::max<double>(1.0, static_cast<double>(state->CullJobs));
        const double gpu = std::max<double>(1.0, static_cast<double>(state->GpuSamples));
        Log("grass: frame %llu drawn %llu/%.0f frames | visible %.0f (max %llu, budget %u) tufts %.0f blades %.0f "
            "draws %.1f clusters %.0f/%.0f over_budget %llu | worker %.3f ms/cull (max %.2f, %llu culls) | "
            "render draw %.3f ms/frame (max %.2f) | gpu %.3f ms/frame (max %.2f, %llu samples; blades %.3f tufts %.3f "
            "pass %.3f) | lod %s (%llu/%llu drawn frames) near %.0f l5 %.0f l3 %.0f l1 %.0f vertices %.0f "
            "pass_points %.0f%s%s",
            static_cast<unsigned long long>(frame), static_cast<unsigned long long>(state->DrawnFrames), frames,
            static_cast<double>(state->Visible) / drawn, static_cast<unsigned long long>(state->VisibleMax),
            state->Stream.MaxInstances, static_cast<double>(state->Tufts) / drawn,
            static_cast<double>(state->Blades) / drawn, static_cast<double>(state->Draws) / drawn,
            static_cast<double>(state->ClustersVisible) / jobs, static_cast<double>(state->ClustersTested) / jobs,
            static_cast<unsigned long long>(state->OverBudgetFrames), state->CullMs / jobs, state->CullMaxMs,
            static_cast<unsigned long long>(state->CullJobs), state->DrawMs / frames, state->DrawMaxMs,
            state->GpuMs / gpu, state->GpuMaxMs, static_cast<unsigned long long>(state->GpuSamples),
            state->GpuBladesMs / gpu, state->GpuTuftsMs / gpu, state->GpuPassMs / gpu, state->Lod ? "on" : "off",
            static_cast<unsigned long long>(state->LodFrames), static_cast<unsigned long long>(state->DrawnFrames),
            static_cast<double>(state->LevelNear) / drawn, static_cast<double>(state->Level5) / drawn,
            static_cast<double>(state->Level3) / drawn, static_cast<double>(state->Level1) / drawn,
            static_cast<double>(state->Vertices) / drawn, static_cast<double>(state->PassPoints) / drawn,
            state->LodFailed ? " | LOD OFF (setup failed, see above)" : "",
            state->DrawFailed ? " | DRAW OFF (setup failed, see above)" : "");
        // Which fragment program drew (gl_FragDepth turns early depth
        // testing off), how many draw points reused an older cull, and what
        // the culls selected.
        Log("grass: frame %llu fragment raster %llu draws, depth-writing %llu | culls fresh %llu stale %llu "
            "(frames that redrew an older cull), no result %llu, nothing drawn %llu | cull instances %.0f "
            "(max %llu) thinned %.0f fill %.0f off screen %.0f | distance=%s full %.0f max %.0f fill %.0f %s | "
            "area %s scene %d room %d, changes %llu "
            "(scene %llu), foreign culls dropped %llu (other scene), without a shared placement %llu, gaps %llu, "
            "ranges drawn with another target's placement %llu | empty placements %u | in-between frames %llu "
            "(without a camera %llu) | hold %llu ms over %llu frames (%llu timed out)",
            static_cast<unsigned long long>(frame), static_cast<unsigned long long>(state->RasterDraws),
            static_cast<unsigned long long>(state->DepthDraws),
            static_cast<unsigned long long>(state->Slots.FreshFrames - state->FreshBase),
            static_cast<unsigned long long>(state->Slots.StaleFrames - state->StaleBase),
            static_cast<unsigned long long>(state->NoResultFrames), static_cast<unsigned long long>(state->EmptyFrames),
            static_cast<double>(state->CullInstances) / jobs, static_cast<unsigned long long>(state->CullInstancesMax),
            static_cast<double>(state->CullThinned) / jobs, static_cast<double>(state->CullFill) / jobs,
            static_cast<double>(state->CullOffScreen) / jobs,
            Oot3d::GrassSwitchDistanceWord(state->Distance).data(),
            static_cast<double>(state->Stream.FullDensityDistance),
            static_cast<double>(state->Stream.DrawDistance), static_cast<double>(state->Stream.FillDistance),
            state->Stream.FillFixed ? "fixed" : "grows",
            state->DrawnArea == 0xffffffffU ? "none" : "drawn",
            state->DrawnArea == 0xffffffffU ? -1 : static_cast<int>(state->DrawnArea >> 8U),
            state->DrawnArea == 0xffffffffU ? -1 : static_cast<int>(static_cast<int8_t>(state->DrawnArea & 0xffU)),
            static_cast<unsigned long long>(state->AreaChanges), static_cast<unsigned long long>(state->SceneChanges),
            static_cast<unsigned long long>(state->ForeignOtherScene),
            static_cast<unsigned long long>(state->NoPlacementFrames),
            static_cast<unsigned long long>(state->Gaps.Gaps - state->GapsBase),
            static_cast<unsigned long long>(state->OtherTargetRanges), state->Placements->Stats().Empty,
            static_cast<unsigned long long>(state->InBetweenFrames),
            static_cast<unsigned long long>(state->InBetweenWithoutCamera),
            static_cast<unsigned long long>(state->HoldMicros / 1000U),
            static_cast<unsigned long long>(state->HoldFrames),
            static_cast<unsigned long long>(state->HoldTimeouts));
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
    state->GpuMs = state->GpuMaxMs = state->GpuBladesMs = state->GpuTuftsMs = state->GpuPassMs = 0.0;
    state->LevelNear = state->Level5 = state->Level3 = state->Level1 = state->Vertices = state->PassPoints = 0;
    state->LodFrames = 0;
    state->RasterDraws = state->DepthDraws = 0;
    state->CullInstances = state->CullInstancesMax = state->CullThinned = state->CullFill = state->CullOffScreen = 0;
    state->EmptyFrames = state->NoResultFrames = 0;
    state->FreshBase = state->Slots.FreshFrames;
    state->AreaChanges = state->SceneChanges = state->ForeignOtherScene = state->NoPlacementFrames = 0;
    state->OtherTargetRanges = 0;
    state->GapsBase = state->Gaps.Gaps;
    state->InBetweenFrames = state->InBetweenWithoutCamera = 0;
    state->HoldMicros = state->HoldFrames = state->HoldTimeouts = 0;
    state->StaleBase = state->Slots.StaleFrames;
}

} // namespace Deko3d

bool GfxRenderingAPIDeko3d::PublishPicaFrameTemporalSample(const Renderer3ds::PicaFrameTemporalSample& sample) {
    // Before the sample's draws, into the device frame that presents them.
    Deko3d::GrassNoteSample(sample.Synthetic, mDevice.Frame());
    // The sample's per-pixel draws count under its frame kind.
    BeginLightingSample(sample.Synthetic);
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
        state->TuftVertex = mShaders.Embedded(kGrassTuftVertexKey, std::span<const uint8_t>(kGrassTuftVertexDksh));
        state->LodFarVertex =
            mShaders.Embedded(kGrassLodFarVertexKey, std::span<const uint8_t>(kGrassLodFarVertexDksh));
        state->LodNearVertex =
            mShaders.Embedded(kGrassLodNearVertexKey, std::span<const uint8_t>(kGrassLodNearVertexDksh));
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
    // A cull of another scene is dropped: never drawn with this frame's
    // cameras. One with none of its placements among this frame's (by
    // identity, whatever target this frame draws into) is left out of this
    // frame only. A cull of the previous room of the scene is drawn.
    const auto pick = Oot3d::GrassCullForDrawPoint(state->Slots, frame, state->Current.Area, state->Area,
                                                   state->Current.Identities,
                                                   [&](uint64_t identity) { return state->Frame.Has(identity); });
    if (pick.Fit == Oot3d::GrassCullFit::OtherScene) {
        ++state->ForeignOtherScene;
        state->Current = {};
    } else if (pick.Fit == Oot3d::GrassCullFit::NoPlacement) {
        ++state->NoPlacementFrames;
    }
    const auto drawnSlot = pick.Slot;
    if (!pick.Fit || pick.Fit == Oot3d::GrassCullFit::OtherScene) {
        ++state->NoResultFrames;
        ++state->SceneNoResult;
    }
    if (drawnSlot) state->DrawnArea = state->Area;
    const auto& current = state->Current;
    const auto timestamp = [&](uint32_t index) {
        dkCmdBufReportCounter(cmd, DkCounter_Timestamp,
                              dkMemBlockGetGpuAddr(state->Timestamps) +
                                  (mDevice.Slot() * kTimestampsPerSlot + index) * kTimestampBytes);
    };
    // config/grass_lod: a cull culled with it is drawn by the tuft pass and
    // the blade programs (their tuft buffers made at its first draw); without
    // them grass_vsh.glsl draws every kind as test.9 did (and the next culls
    // are test.9's).
    bool lod = drawnSlot && current.Valid && current.Lod && !state->LodFailed;
    if (lod && state->TuftMemory == nullptr) {
        const uint32_t tuftBytes = AlignUp(kTuftBuffers * state->SlotInstances * kTuftBytes, DK_MEMBLOCK_ALIGNMENT);
        state->TuftMemory = mDevice.MakeBlock(tuftBytes, DkMemBlockFlags_GpuCached, "grass tufts");
        if (state->TuftMemory == nullptr || state->TuftVertex == nullptr || state->LodFarVertex == nullptr ||
            state->LodNearVertex == nullptr) {
            state->LodFailed = true;
            state->Lod = state->Stream.Lod = false;
            Log("grass: lod off (shaders %d/%d/%d, tuft memory %.1f MiB %s): test.9's grass", state->TuftVertex != nullptr,
                state->LodFarVertex != nullptr, state->LodNearVertex != nullptr,
                static_cast<double>(tuftBytes) / (1024.0 * 1024.0), state->TuftMemory != nullptr ? "ok" : "failed");
        } else {
            Log("grass: lod ready (%.1f MiB of tuft memory: %u buffers of %u tufts, shaders %zu/%zu/%zu B)",
                static_cast<double>(tuftBytes) / (1024.0 * 1024.0), kTuftBuffers, state->SlotInstances,
                sizeof(kGrassTuftVertexDksh), sizeof(kGrassLodFarVertexDksh), sizeof(kGrassLodNearVertexDksh));
        }
        lod = !state->LodFailed;
    }
    // Each range's placement: this target's own (its camera), else the
    // placement as drawn into another target this frame (this frame's camera).
    auto& found = state->RangePlacements;
    found.assign(drawnSlot && current.Valid ? current.Ranges.size() : 0U, nullptr);
    for (size_t index = 0; index < found.size(); ++index) {
        bool otherTarget = false;
        found[index] = state->Frame.Find(current.Identities[current.Ranges[index].Placement], key.Namespace,
                                         key.ColorAddress, &otherTarget);
        if (found[index] != nullptr) state->OtherTargetRanges += otherTarget ? 1U : 0U;
    }
    const auto placementUniforms = [&](FramePlacement& placement) {
        if (placement.UniformsGpu == DK_GPU_ADDR_INVALID) {
            const StreamSpan span = mDevice.Stream(sizeof(placement.Uniforms), DK_UNIFORM_BUF_ALIGNMENT);
            if (span.Cpu == nullptr) return false;
            std::memcpy(span.Cpu, &placement.Uniforms, sizeof(placement.Uniforms));
            placement.UniformsGpu = span.Gpu;
        }
        return state->FrameUniforms != DK_GPU_ADDR_INVALID;
    };
    const uint32_t placementBytes = AlignUp(sizeof(GrassPlacementUniforms), DK_UNIFORM_BUF_ALIGNMENT);
    const uint32_t frameBytes = AlignUp(sizeof(GrassFrameUniforms), DK_UNIFORM_BUF_ALIGNMENT);
    const auto bindUniforms = [&](const FramePlacement& placement) {
        dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 0, placement.UniformsGpu, placementBytes);
        dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, placement.UniformsGpu, placementBytes);
        dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 1, state->FrameUniforms, frameBytes);
    };
    // GrassLod of one draw (grass_lod_common.glsl): this frame's camera's
    // focal length, the levels, and the draw's blades.
    const auto lodUniforms = [&](const FramePlacement& placement, Oot3d::GrassStreamKind kind) {
        const auto parameters = Oot3d::GrassLodParametersFor(state->Stream, placement.Uniforms.PositionToClip);
        const uint32_t blades = Oot3d::GrassLodBlades(kind);
        const uint32_t target = Oot3d::GrassLodTargetBlades(kind);
        GrassLodUniforms u;
        u.Scale = {parameters.Focal, parameters.MinimumDistance, parameters.RampInverse, 0.0F};
        u.Pixels = Oot3d::kGrassLodPixels;
        const float level = kind == Oot3d::GrassStreamKind::Tufts3 ? 1.0F
                            : kind == Oot3d::GrassStreamKind::Tufts1 ? 2.0F
                                                                      : 0.0F;
        u.Draw = {1.0F / static_cast<float>(blades), static_cast<float>(target), 1.0F / static_cast<float>(target),
                  level};
        u.Index = {std::bit_cast<uint32_t>(1.0F / static_cast<float>(3U * blades)), 3U * blades, 0U, 0U};
        const StreamSpan span = mDevice.Stream(sizeof(u), DK_UNIFORM_BUF_ALIGNMENT);
        if (span.Cpu == nullptr) return false;
        std::memcpy(span.Cpu, &u, sizeof(u));
        dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 2, span.Gpu, AlignUp(sizeof(u), DK_UNIFORM_BUF_ALIGNMENT));
        return true;
    };
    const DkGpuAddr instances = drawnSlot ? dkMemBlockGetGpuAddr(state->StreamMemory) +
                                                uint64_t{*drawnSlot} * state->SlotInstances *
                                                    sizeof(Oot3d::GrassStreamInstance)
                                          : DK_GPU_ADDR_INVALID;
    const DkGpuAddr tufts = lod ? dkMemBlockGetGpuAddr(state->TuftMemory) +
                                      (state->TuftPasses % kTuftBuffers) * uint64_t{state->SlotInstances} * kTuftBytes
                                : DK_GPU_ADDR_INVALID;
    // The instance stream's attributes (grass_vsh.glsl, grass_tuft_vsh.glsl).
    const auto bindInstanceAttributes = [&](uint32_t divisor) {
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
        const DkVtxBufferState buffer = {static_cast<uint32_t>(sizeof(Oot3d::GrassStreamInstance)), divisor};
        dkCmdBufBindVtxBufferState(cmd, &buffer, 1);
    };
    const auto bindRasterizer = [&](bool enabled) {
        DkRasterizerState rasterizer;
        dkRasterizerStateDefaults(&rasterizer);
        rasterizer.rasterizerEnable = enabled ? 1U : 0U;
        rasterizer.depthClampEnable = 0;
        rasterizer.cullMode = DkFace_None;
        dkCmdBufBindRasterizerState(cmd, &rasterizer);
    };
    // At the first thing drawn: the target, its state, the frame uniforms.
    bool begun = false;
    const auto beginDrawing = [&] {
        if (begun) return;
        begun = true;
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
        bindRasterizer(true);
        const auto& viewport = mGrassScene.Viewport;
        ApplyDkViewport(cmd, static_cast<float>(viewport.X), static_cast<float>(viewport.Y),
                        static_cast<float>(viewport.Width), static_cast<float>(viewport.Height));
        dkCmdBufSetScissors(cmd, 0, &mGrassScene.Scissor, 1);
        // grass_vsh.glsl: one instance per tuft; the blade programs pull their tufts.
        if (lod) dkCmdBufBindVtxAttribState(cmd, &kNoAttribute, 0);
        else bindInstanceAttributes(1U);
        const auto frameUniforms = BuildFrameUniforms(*state);
        const StreamSpan span = mDevice.Stream(sizeof(frameUniforms), DK_UNIFORM_BUF_ALIGNMENT);
        if (span.Cpu != nullptr) {
            std::memcpy(span.Cpu, &frameUniforms, sizeof(frameUniforms));
            state->FrameUniforms = span.Gpu;
        }
    };
    // config/grass_lod: the tuft pass, one point per tuft of each placement
    // (its ranges are contiguous), the rasterizer off; then every write must
    // land before a blade program reads it (and the L1 holds none of it).
    uint64_t passPoints = 0U;
    if (lod) {
        for (size_t index = 0; index < found.size();) {
            const auto& range = current.Ranges[index];
            size_t next = index + 1U;
            uint32_t end = range.First + range.Count;
            for (; next < found.size() && current.Ranges[next].Placement == range.Placement; ++next)
                end = current.Ranges[next].First + current.Ranges[next].Count;
            FramePlacement* placement = found[index];
            index = next;
            if (placement == nullptr) continue;
            if (!begun) {
                beginDrawing();
                bindRasterizer(false);
                bindInstanceAttributes(0U); // one vertex per tuft
            }
            if (!placementUniforms(*placement) || !lodUniforms(*placement, Oot3d::GrassStreamKind::Tufts)) continue;
            const DkShader* shaders[] = {state->TuftVertex, state->Fragment};
            dkCmdBufBindShaders(cmd, DkStageFlag_Vertex | DkStageFlag_Fragment, shaders, 2);
            bindUniforms(*placement);
            const uint32_t count = end - range.First;
            const DkBufExtents extent = {instances + uint64_t{range.First} * sizeof(Oot3d::GrassStreamInstance),
                                         count * static_cast<uint32_t>(sizeof(Oot3d::GrassStreamInstance))};
            dkCmdBufBindVtxBuffers(cmd, 0, &extent, 1);
            dkCmdBufBindStorageBuffer(cmd, DkStage_Vertex, 0, tufts + uint64_t{range.First} * kTuftBytes,
                                      count * kTuftBytes);
            dkCmdBufDraw(cmd, DkPrimitive_Points, count, 1, 0, 0);
            passPoints += count;
        }
        if (passPoints != 0U) {
            dkCmdBufBarrier(cmd, DkBarrier_Primitives, DkInvalidateFlags_Image);
            ++state->TuftPasses;
        }
        if (begun) {
            bindRasterizer(true);
            dkCmdBufBindVtxAttribState(cmd, &kNoAttribute, 0);
        }
    }
    if (timed) timestamp(1U);
    // Near draws (single blades) first, then the far ones (tufts), a
    // timestamp between them: the gpu split.
    for (uint32_t pass = 0; pass < 2U; ++pass) {
        if (pass == 1U && timed) timestamp(2U);
        for (size_t index = 0; index < found.size(); ++index) {
            const auto& range = current.Ranges[index];
            const bool blades = range.Kind == Oot3d::GrassStreamKind::Blades;
            if (blades != (pass == 0U) || found[index] == nullptr) continue;
            auto& placement = *found[index];
            beginDrawing();
            if (!placementUniforms(placement)) continue;
            uint32_t vertices = 0U;
            if (lod) {
                if (!lodUniforms(placement, range.Kind)) continue;
                const DkShader* shaders[] = {blades ? state->LodNearVertex : state->LodFarVertex,
                                             placement.ExplicitDepth ? state->FragmentDepth : state->Fragment};
                dkCmdBufBindShaders(cmd, DkStageFlag_Vertex | DkStageFlag_Fragment, shaders, 2);
                bindUniforms(placement);
                dkCmdBufBindStorageBuffer(cmd, DkStage_Vertex, 0, tufts + uint64_t{range.First} * kTuftBytes,
                                          range.Count * kTuftBytes);
                if (blades)
                    dkCmdBufBindStorageBuffer(
                        cmd, DkStage_Vertex, 1, instances + uint64_t{range.First} * sizeof(Oot3d::GrassStreamInstance),
                        range.Count * static_cast<uint32_t>(sizeof(Oot3d::GrassStreamInstance)));
                vertices = 3U * Oot3d::GrassLodBlades(range.Kind) * range.Count;
                // No instancing: the vertices of several tufts share a warp (grass_lod_vsh.glsl).
                dkCmdBufDraw(cmd, DkPrimitive_Triangles, vertices, 1, 0, 0);
            } else {
                const DkShader* shaders[] = {state->Vertex,
                                             placement.ExplicitDepth ? state->FragmentDepth : state->Fragment};
                dkCmdBufBindShaders(cmd, DkStageFlag_Vertex | DkStageFlag_Fragment, shaders, 2);
                bindUniforms(placement);
                const DkBufExtents extent = {instances + uint64_t{range.First} * sizeof(Oot3d::GrassStreamInstance),
                                             range.Count * static_cast<uint32_t>(sizeof(Oot3d::GrassStreamInstance))};
                dkCmdBufBindVtxBuffers(cmd, 0, &extent, 1);
                // Every kind is BladesPerTuft triangles per instance (grass_vsh.glsl).
                dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3U * state->Profile.BladesPerTuft, range.Count, 0, 0);
                vertices = 3U * state->Profile.BladesPerTuft * range.Count;
            }
            (blades ? state->Blades : state->Tufts) += range.Count;
            switch (lod ? range.Kind : (blades ? Oot3d::GrassStreamKind::Blades : Oot3d::GrassStreamKind::Tufts)) {
            case Oot3d::GrassStreamKind::Blades: state->LevelNear += range.Count; break;
            case Oot3d::GrassStreamKind::Tufts: state->Level5 += range.Count; break;
            case Oot3d::GrassStreamKind::Tufts3: state->Level3 += range.Count; break;
            case Oot3d::GrassStreamKind::Tufts1: state->Level1 += range.Count; break;
            }
            state->Vertices += vertices;
            visible += range.Count;
            ++draws;
            ++(placement.ExplicitDepth ? state->DepthDraws : state->RasterDraws);
            mDevice.Checkpoint("grass", range.Count, blades ? 1U : 0U);
        }
    }
    if (drawnSlot && draws == 0U && current.Stats.Instances() != 0U) ++state->EmptyFrames;
    state->Gaps.DrawPoint(state->Area, drawnSlot.has_value(), draws != 0U);
    if (timed) {
        timestamp(3U);
        state->TimestampPending[mDevice.Slot()] = true;
    }
    state->DrawnNamespace = key.Namespace;
    state->DrawnColorAddress = key.ColorAddress;
    if (draws != 0U) {
        ++state->DrawnFrames;
        state->LodFrames += lod ? 1U : 0U;
        state->PassPoints += passPoints;
        state->Visible += visible;
        state->VisibleMax = std::max(state->VisibleMax, visible);
        state->Draws += draws;
        // The scene's first grass: when it came, and what waited for it.
        if (state->Hold.Drawn()) {
            const auto& hold = state->Hold.Stats();
            Log("grass: scene %d first drawn at its frame %llu (draw points without grass before: %llu) | hold %llu "
                "ms over %llu frames (placements %llu, cull %llu, timed out %llu; budget %llu ms)",
                state->Area == 0xffffffffU ? -1 : static_cast<int>(state->Area >> 8U),
                static_cast<unsigned long long>(hold.FirstDrawnFrame),
                static_cast<unsigned long long>(state->SceneNoResult),
                static_cast<unsigned long long>(hold.HoldMicros / 1000U),
                static_cast<unsigned long long>(hold.HoldFrames),
                static_cast<unsigned long long>(hold.PlacementWaits),
                static_cast<unsigned long long>(hold.CullWaits), static_cast<unsigned long long>(hold.Timeouts),
                static_cast<unsigned long long>(state->Hold.BudgetMicros() / 1000U));
        }
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
