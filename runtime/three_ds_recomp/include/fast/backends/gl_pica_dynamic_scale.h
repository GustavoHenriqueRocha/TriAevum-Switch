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
// Render thread only (the frame code sets Requested before a step frame;
// the backend applies it to the top-screen target at that target's next
// colour clear, so a frame never mixes two scales and a frame that builds
// on the previous one's pixels keeps the scale they were drawn at). The
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
};

inline State gState{};

} // namespace Fast::PicaGlDynamicScale
