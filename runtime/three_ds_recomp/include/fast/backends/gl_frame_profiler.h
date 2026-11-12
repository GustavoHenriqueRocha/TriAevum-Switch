#pragma once

// Per-step profile of the OpenGL backend's render thread (Switch: Mesa/nouveau).
//
// Enabled only by the file sdmc:/switch/oot3dre/config/gpu_timing (desktop:
// TRIAEVUM_GL_GPU_TIMING). The optional content is N, the GPU sampling
// (1 frame every N, default 30). Without the file, each marker point is an
// atomic load and a branch; the GL call counters don't even exist
// (they are hooks installed on the glad pointers only when enabled).
//
// Measures, per host frame (step, interpolated or repeated):
//  - GPU: glQueryCounter(GL_TIMESTAMP) on every step change in a sampled
//    frame; result read frames later with GL_QUERY_RESULT_AVAILABLE
//    (never blocks; with no result in time, the sample is dropped);
//  - render thread CPU per step (monotonic clock on every change);
//  - GL call counters (draws, clears, FBOs, uploads, syncs).
// The report (TakeReport) becomes a "glperf" line in logs/perf.txt.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "fast/backends/gl_pica_draw_phases.h"

namespace Fast::GlPerf {

enum class Stage : uint8_t {
    Begin,          // frame start until the first step
    Clear,          // clears (window, PICA targets, fills)
    PicaTop,        // PICA draws in the top-screen target (displayed sample)
    PicaBottom,     // PICA draws in the bottom-screen target
    PicaOffscreen,  // PICA draws in other targets (shadows, render-to-texture)
    PicaCanonical,  // draws of the canonical (non-displayed) execution of a new frame
    Transfer,       // display transfers (target blit -> display image)
    PostAa,         // FXAA/SMAA into an intermediate image
    PostEasu,       // FSR EASU into an intermediate image
    Scanout,        // last pass into the window (RCAS, direct EASU/FXAA or copy)
    Ui,             // integrated UI (TopScreen HUD/menus)
    Present,        // EndFrame until after the swap
    // CPU only (they don't change the GPU step):
    Other,          // render thread commands outside the steps above
    Pacing,         // pacer wait before the swap
    Idle,           // render thread with no command
    Count
};
inline constexpr size_t kStageCount = static_cast<size_t>(Stage::Count);
inline constexpr size_t kGpuStageCount = static_cast<size_t>(Stage::Other);

namespace detail {
// Header-only switch: code outside the renderer library (render command
// stream, UI renderer, their standalone tests) only reads these, so it needs
// no link dependency; Init() fills the table when the profiler is on.
struct Hooks {
    void (*Mark)(Stage) = nullptr;
    void (*MarkPicaDraw)(uint32_t, uint32_t) = nullptr;
    void (*CountPica)(uint32_t) = nullptr;
    void (*FrameBegin)() = nullptr;
    void (*FrameEnd)() = nullptr;
    void (*SampleBegin)(bool, bool) = nullptr;
    void (*SampleEnd)() = nullptr;
    Stage (*Current)() = nullptr;
};
inline Hooks gHooks{};
inline std::atomic<bool> gEnabled{false};
// Last closed frame (FrameEnd): most expensive step on the render thread CPU
// (excluding pacing/idle) << 24 | microseconds (up to 2^24-1). ~0: none.
inline std::atomic<uint32_t> gLastFrameTop{0xFFFFFFFFU};
} // namespace detail

inline bool Enabled() {
    return detail::gEnabled.load(std::memory_order_acquire);
}

// Most expensive step (CPU) of the last closed frame, for logs/spikes.txt;
// false with the profile off or before the first frame.
inline bool LastFrameTopStage(uint8_t* stage, float* milliseconds) {
    if (!Enabled()) return false;
    const uint32_t packed = detail::gLastFrameTop.load(std::memory_order_relaxed);
    if (packed == 0xFFFFFFFFU) return false;
    *stage = static_cast<uint8_t>(packed >> 24U);
    *milliseconds = static_cast<float>(packed & 0xFFFFFFU) / 1000.0F;
    return true;
}

// Reads the configuration once (current GL context). Without the file: nothing.
void Init();

inline void Mark(Stage stage) {
    if (Enabled()) detail::gHooks.Mark(stage);
}
inline void MarkPicaDraw(uint32_t framebufferWidth, uint32_t framebufferHeight) {
    if (Enabled()) detail::gHooks.MarkPicaDraw(framebufferWidth, framebufferHeight);
}
// PICA path counters (PicaGlGpuLean::CountBit bits): draws with
// discard / gl_FragDepth / image, with blend, logic op COPY and identity
// blend off, no-effect draws skipped, depth+stencil clears.
inline void CountPica(uint32_t bits) {
    if (bits != 0U && Enabled()) detail::gHooks.CountPica(bits);
}
inline void FrameBegin() {
    if (Enabled()) detail::gHooks.FrameBegin();
}
inline void FrameEnd() {
    if (Enabled()) detail::gHooks.FrameEnd();
}

// CPU phases of the PICA draw (gl_pica_draw_phases.h); only call with Enabled().
using Phase = PicaDrawPhases::Phase;
using Section = PicaDrawPhases::Section;
using DrawCount = PicaDrawPhases::Count;
inline PicaDrawPhases::Tracker& Phases() {
    return PicaDrawPhases::gTracker;
}
inline void PhaseBegin() {
    if (Enabled()) Phases().Begin(PicaDrawPhases::Ticks());
}
inline void PhaseLap(Phase phase) {
    if (Enabled()) Phases().Lap(phase, PicaDrawPhases::Ticks());
}
inline void PhaseEnd() {
    if (Enabled()) Phases().End(PicaDrawPhases::Ticks());
}
inline void PhaseCount(DrawCount what, uint64_t value = 1U) {
    if (Enabled()) Phases().Add(what, value);
}

// Wall time of a render thread section (glperf step_sec_<name>_ms).
class SectionScope {
public:
    explicit SectionScope(Section section)
        : mArmed(Enabled()), mSection(section) {
        if (mArmed) mStart = PicaDrawPhases::Ticks();
    }
    ~SectionScope() {
        if (mArmed) {
            const uint64_t now = PicaDrawPhases::Ticks();
            if (now > mStart) Phases().AddSection(mSection, now - mStart);
        }
    }
    SectionScope(const SectionScope&) = delete;
    SectionScope& operator=(const SectionScope&) = delete;

private:
    bool mArmed;
    Section mSection;
    uint64_t mStart = 0U;
};

// Nested step: returns to the previous step at scope end.
class Scope {
public:
    explicit Scope(Stage stage) : mArmed(Enabled()) {
        if (mArmed) {
            mPrevious = detail::gHooks.Current();
            detail::gHooks.Mark(stage);
        }
    }
    ~Scope() {
        if (mArmed) detail::gHooks.Mark(mPrevious);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    bool mArmed;
    Stage mPrevious = Stage::Other;
};

// A render thread command: Other during, Idle after.
class CommandScope {
public:
    CommandScope() : mArmed(Enabled()) {
        if (mArmed) detail::gHooks.Mark(Stage::Other);
    }
    ~CommandScope() {
        if (mArmed) detail::gHooks.Mark(Stage::Idle);
    }
    CommandScope(const CommandScope&) = delete;
    CommandScope& operator=(const CommandScope&) = delete;

private:
    bool mArmed;
};

// Execution of a visual sample. authoritative = new game frame
// (native/current execution); present = displayed sample. The draws of a
// non-displayed execution count as PicaCanonical; the host frame that
// executes a new frame is "step", one that only displays a repeated sample
// (interpolated between two frames already seen) is "interp", the rest is "hold".
class SampleScope {
public:
    SampleScope(bool authoritative, bool present) : mArmed(Enabled()) {
        if (mArmed) detail::gHooks.SampleBegin(authoritative, present);
    }
    ~SampleScope() {
        if (mArmed) detail::gHooks.SampleEnd();
    }
    SampleScope(const SampleScope&) = delete;
    SampleScope& operator=(const SampleScope&) = delete;

private:
    bool mArmed;
};

// "glperf ..." line (no '\n') with per-frame averages since the last
// report; empty when off. Callable from another thread.
std::string TakeReport(uint64_t hostFrames);

} // namespace Fast::GlPerf
