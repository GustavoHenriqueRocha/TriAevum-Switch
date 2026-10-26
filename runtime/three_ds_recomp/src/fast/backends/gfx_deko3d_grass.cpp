// config/grass on the deko3d backend (docs/DEKO3D_PROGRESSO.md, grama leve).
// p6z: the CPU side only. Opaque world draws of the scene whose texture has a
// rule of the product preset become grass sources (pica_grass_source.h), the
// placements are built by one worker, and every few seconds deko3d.txt gets
// how many blades the current rooms would have and what that cost. Nothing is
// drawn yet.

#include "fast/backends/gfx_deko3d.h"

#if defined(__SWITCH__)
#include "fast/oot3d/graphics_settings.h"
#include "fast/oot3d/graphics_settings_persistence.h"
#include "fast/oot3d/grass_scene_bridge.h"
#include "fast/oot3d/pica_grass_source.h"
#include "gfx_deko3d_api.h"
#include "gfx_deko3d_device.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>

namespace Fast {
namespace {

// Limits for the Switch (the PC preset is tuned for a desktop GPU).
constexpr uint32_t kMaxBladesPerRoom = 20000U;
constexpr uint64_t kLogEveryFrames = 600U;

struct GrassState {
    Oot3d::InteractiveGrassSettings Settings;
    Oot3d::PicaGrassSourceState Sources;
    Oot3d::PicaGrassPlacements Placements;
    uint64_t DrawOrder = 0;
    uint64_t DrawFrame = ~0ULL;
    // Accumulated between logs.
    uint64_t Frames = 0;
    uint64_t Published = 0;
    double ObserveMs = 0.0;
    double ObserveMaxMs = 0.0;
    double UpdateMs = 0.0;
    double UpdateMaxMs = 0.0;
    uint64_t AnchorsMax = 0;
};

// Set once before the first frame (Deko3dConfigureGrass), read by the render
// thread afterwards.
std::atomic<GrassState*> gGrass{nullptr};

double MillisecondsSince(std::chrono::steady_clock::time_point begin) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
}

} // namespace

void Deko3dConfigureGrass(std::string_view graphicsJson) {
    if (gGrass.load(std::memory_order_acquire) != nullptr) {
        return;
    }
    std::FILE* file = std::fopen("sdmc:/switch/oot3dre/config/grass", "rb");
    if (file == nullptr) {
        return;
    }
    std::fclose(file);
    auto state = std::make_unique<GrassState>();
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
    state->Settings.MaxInstancesPerRoom =
        state->Settings.MaxInstancesPerRoom == 0U ? kMaxBladesPerRoom
                                                  : std::min(state->Settings.MaxInstancesPerRoom, kMaxBladesPerRoom);
    Deko3d::Log("grass: on (config/grass), %zu rules, %.1f blades/m2, at most %u per room", state->Settings.Rules.size(),
                static_cast<double>(state->Settings.Generation.InstancesPerSquareMeter),
                state->Settings.MaxInstancesPerRoom);
    gGrass.store(state.release(), std::memory_order_release);
}

namespace Deko3d {

void GrassObserveDraw(const GfxNativePicaDrawView& draw, uint64_t frame) {
    GrassState* state = gGrass.load(std::memory_order_acquire);
    if (state == nullptr || draw.CompositionDomain != decltype(draw.CompositionDomain)::Scene ||
        draw.Composition.Layer != decltype(draw.Composition.Layer)::OpaqueWorld) {
        return;
    }
    if (state->DrawFrame != frame) {
        state->DrawFrame = frame;
        state->DrawOrder = 0;
    }
    const auto begin = std::chrono::steady_clock::now();
    try {
        if (Oot3d::PublishPicaGrassSource(draw, frame, state->Settings, state->Sources, frame, state->DrawOrder++)) {
            ++state->Published;
        }
    } catch (const std::exception& exception) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            Log("grass: source failed (%s)", exception.what());
        }
    }
    const double ms = MillisecondsSince(begin);
    state->ObserveMs += ms;
    state->ObserveMaxMs = std::max(state->ObserveMaxMs, ms);
}

void GrassEndFrame(uint64_t frame) {
    GrassState* state = gGrass.load(std::memory_order_acquire);
    if (state == nullptr) {
        return;
    }
    const auto begin = std::chrono::steady_clock::now();
    try {
        (void)state->Placements.Update(frame, state->Settings);
    } catch (const std::exception& exception) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            Log("grass: placement failed (%s)", exception.what());
        }
    }
    if (frame > 3U) {
        Oot3d::GrassSceneBridge::Instance().PruneBeforeFrame(frame - 2U);
    }
    const double ms = MillisecondsSince(begin);
    state->UpdateMs += ms;
    state->UpdateMaxMs = std::max(state->UpdateMaxMs, ms);
    const auto& stats = state->Placements.Stats();
    state->AnchorsMax = std::max(state->AnchorsMax, stats.Anchors);
    if (++state->Frames < kLogEveryFrames) {
        return;
    }
    const double frames = static_cast<double>(state->Frames);
    Log("grass: frame %llu rules_seen %u meshes %u placements %u queued %u blades %llu (max %llu) | "
        "sources %.3f ms/frame (max %.2f, %llu published) placements %.3f ms/frame (max %.2f)",
        static_cast<unsigned long long>(frame), stats.Rules, stats.Meshes, stats.Ready, stats.Queued,
        static_cast<unsigned long long>(stats.Anchors), static_cast<unsigned long long>(state->AnchorsMax),
        state->ObserveMs / frames, state->ObserveMaxMs, static_cast<unsigned long long>(state->Published),
        state->UpdateMs / frames, state->UpdateMaxMs);
    state->Frames = 0;
    state->Published = 0;
    state->ObserveMs = state->ObserveMaxMs = 0.0;
    state->UpdateMs = state->UpdateMaxMs = 0.0;
    state->AnchorsMax = 0;
}

} // namespace Deko3d
} // namespace Fast
#else
namespace Fast {
void Deko3dConfigureGrass(std::string_view) {}
} // namespace Fast
#endif
