#pragma once

// Dynamic resolution of the top-screen scene in the OpenGL PICA backend
// (config/dynamic_resolution; controller in
// tools/oot3d/native_game_runtime/oot3d_native_dynamic_resolution.h).
//
// The images keep their allocation (config/resolution_scale); a lower scale
// draws into the bottom-left sub-rectangle of the same target, the display
// copy keeps that sub-rectangle (plus a replicated border for the filters)
// and the scanout (linear, FSR EASU, RCAS) reads only it. Nothing is
// reallocated when the scale moves.
//
// Render thread only. The frame code sets Requested and bumps FrameSerial
// before every step frame; the backend applies the scale to the top-screen
// target at the first of two points of that frame:
//  - a colour clear of the target (its content starts again), or
//  - the frame's first draw into the target (tour19, pgo5n: OoT3D's top
//    scene never reached the colour-clear path, drs_applied stayed 0 for a
//    whole tour while the controller went down to 2.25). There the backend
//    clears the colour itself (a logical clear: what the frame does not
//    draw is black instead of pixels at the other scale).
// Once per FrameSerial and target, so a frame never mixes two scales. The
// counters are atomics because perf.txt reads them on the guest thread.

#include <atomic>
#include <cstdint>

namespace Fast::PicaGlDynamicScale {

struct State {
    // Scale asked for the top-screen scene; 0: off (the configured scale).
    float Requested = 0.0F;
    // Set by the backend at initialisation: the configured scale can be
    // lowered (above 1, no SMAA/FXAA, whose images have the source extent).
    bool Supported = false;
    float MaxScale = 1.0F;
    // Presented top-screen frames and the sum of their scales (x1000),
    // and scale changes applied to a top-screen target.
    std::atomic<uint64_t> PresentedFrames{0U};
    std::atomic<uint64_t> PresentedScaleMilli{0U};
    std::atomic<uint64_t> Applied{0U};
    // Bumped by the frame code with every step frame's request.
    uint32_t FrameSerial = 0U;
    // Of Applied: changes made at a frame's first draw (logical clear).
    std::atomic<uint64_t> AppliedAtDraw{0U};
    // Eligible targets that left dynamic resolution (Shadow2D).
    std::atomic<uint64_t> ExemptTargets{0U};
};

// Per-target bookkeeping of the frame-start application.
struct TargetPhase {
    float Scale = 1.0F;       // drawn scale now
    uint32_t Serial = 0U;     // FrameSerial last looked at
};

// A colour clear of an eligible target: the content starts again, the
// desired scale applies at once. Returns the scale to draw at.
[[nodiscard]] inline float ScaleAtColourClear(TargetPhase& phase,
                                              uint32_t frameSerial,
                                              float desired) noexcept {
    phase.Serial = frameSerial;
    phase.Scale = desired;
    return desired;
}

// A draw into an eligible target. The first one of a new FrameSerial moves
// the scale (true: the caller changes the extent and clears the colour);
// later draws of the same frame never do, whatever Requested says.
[[nodiscard]] inline bool ScaleAtDraw(TargetPhase& phase, uint32_t frameSerial,
                                      float desired) noexcept {
    if (phase.Serial == frameSerial) {
        return false;
    }
    phase.Serial = frameSerial;
    if (desired == phase.Scale) {
        return false;
    }
    phase.Scale = desired;
    return true;
}

inline State gState{};

} // namespace Fast::PicaGlDynamicScale
