#pragma once
// config/grass_hold (SWITCH_PORT.md, "Grass at a scene's start"): in a
// scene's first frames the render thread waits at frame start for the
// scene's first grass, so the fade-in shows it instead of a bare ground
// that grows grass a moment later. Two waits, each bounded by what is left
// of the scene's budget:
// - placements: the frame before queued some (built, or loaded from the SD
//   card) and none of them is ready yet;
// - cull: they are ready and their first cull is still running on the cull
//   worker (its result is drawn at this frame's draw point).
// The window closes at the scene's first grass draw, after
// kGrassSceneHoldFrames frames, or with the budget spent. The game thread
// follows once the frame queue is full, so the fade-in starts that much
// later (as with config/texture_pack_hold).

#include <algorithm>
#include <cstdint>

namespace Fast::Oot3d {

inline constexpr uint32_t kGrassSceneHoldDefaultMs = 1500U;
inline constexpr uint32_t kGrassSceneHoldLimitMs = 2500U;
inline constexpr uint64_t kGrassSceneHoldFrames = 120U;

enum class GrassHoldWait : uint8_t {
    None,
    Placements,
    Cull,
};

// One scene visit: frames waited, the time, and waits that ended with the
// budget (nothing ready yet), by kind.
struct GrassSceneHoldStats {
    uint64_t Frames = 0;          // frames of the scene so far
    uint64_t HoldFrames = 0;
    uint64_t HoldMicros = 0;
    uint64_t PlacementWaits = 0;
    uint64_t CullWaits = 0;
    uint64_t Timeouts = 0;
    uint64_t FirstDrawnFrame = 0; // the scene frame of its first grass (0: none yet)
};

class GrassSceneHold {
  public:
    explicit GrassSceneHold(uint32_t budgetMs = kGrassSceneHoldDefaultMs) noexcept { SetBudget(budgetMs); }
    void SetBudget(uint32_t budgetMs) noexcept {
        mBudgetMicros = uint64_t{std::min(budgetMs, kGrassSceneHoldLimitMs)} * 1000U;
    }
    [[nodiscard]] uint64_t BudgetMicros() const noexcept { return mBudgetMicros; }
    // A scene change: a new window and budget.
    void BeginScene() noexcept {
        mStats = {};
        mLeftMicros = mBudgetMicros;
    }
    // Once per frame, at its start.
    void BeginFrame() noexcept { ++mStats.Frames; }
    // The window: budget left, no grass drawn yet, within the scene's first frames.
    [[nodiscard]] bool Open() const noexcept {
        return mLeftMicros != 0U && mStats.FirstDrawnFrame == 0U && mStats.Frames <= kGrassSceneHoldFrames;
    }
    // What this frame waits for: `placementsPending` (placements requested
    // and not made yet), `placementsReady` (the last update had ready ones)
    // and `cullRunning` (the cull worker is busy).
    [[nodiscard]] GrassHoldWait Next(bool placementsPending, bool placementsReady, bool cullRunning) const noexcept {
        if (!Open()) return GrassHoldWait::None;
        if (placementsPending) return GrassHoldWait::Placements;
        if (placementsReady && cullRunning) return GrassHoldWait::Cull;
        return GrassHoldWait::None;
    }
    [[nodiscard]] uint64_t LeftMicros() const noexcept { return mLeftMicros; }
    // The wait of this frame: how long, and whether what it waited for came.
    void Waited(GrassHoldWait kind, uint64_t micros, bool done) noexcept {
        if (kind == GrassHoldWait::None) return;
        mLeftMicros -= std::min(mLeftMicros, micros);
        ++mStats.HoldFrames;
        mStats.HoldMicros += micros;
        ++(kind == GrassHoldWait::Placements ? mStats.PlacementWaits : mStats.CullWaits);
        if (!done) ++mStats.Timeouts;
    }
    // Grass drawn this frame: the first closes the window.
    // True for the scene's first.
    bool Drawn() noexcept {
        if (mStats.FirstDrawnFrame != 0U) return false;
        mStats.FirstDrawnFrame = std::max<uint64_t>(mStats.Frames, 1U);
        return true;
    }
    [[nodiscard]] const GrassSceneHoldStats& Stats() const noexcept { return mStats; }

  private:
    uint64_t mBudgetMicros = 0;
    uint64_t mLeftMicros = 0;
    GrassSceneHoldStats mStats;
};

} // namespace Fast::Oot3d
