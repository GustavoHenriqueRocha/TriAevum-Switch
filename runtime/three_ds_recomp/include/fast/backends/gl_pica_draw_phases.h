#pragma once

// CPU phases of a PICA draw in the GL backend (glperf step_draw_<phase>_ms).
// GL-free and allocation-free; only the render thread writes. The accumulator is
// "sequential": Lap(phase) charges the time since the previous mark to the given phase,
// so every instant of SubmitPicaDraw falls into exactly one phase (one
// clock read per mark). Clock: the ARM system counter on the
// Switch (19.2 MHz), steady_clock on the host. Active only with config/gpu_timing;
// when off, callers don't even read the clock (see GlPerf::Enabled()).
//
// Besides the per-draw phases there are "sections" (wall time of render
// thread stretches outside the draw, to break down step_cpu_other) and per-frame counters.

#include <array>
#include <chrono>
#include <cstdint>

namespace Fast::PicaDrawPhases {

enum class Phase : uint8_t {
    Validate,   // checks, composition, frame start, step marker
    Program,    // GetPicaGlProgram (key + map lookup; compile if new)
    Target,     // target, pending fills, shadow
    State,      // binds (FBO/VAO/program/textures/image) and raster state
    Uniforms,   // memcmp + copy into the ring + bind range
    Vertices,   // validation, persistent geometry registration, ring, attribs
    TexHash,    // texture content hash (only when the frame didn't bring it)
    TexLookup,  // key + texture map lookup (includes lighting LUT)
    TexUpload,  // new texture creation (decode + glTexImage)
    Indices,    // copy of the indices into the ring
    Draw,       // glDrawElements/Arrays (incl. corner passes)
    Rest,       // whatever is left until the end of SubmitPicaDraw
    Count
};
inline constexpr size_t kPhaseCount = static_cast<size_t>(Phase::Count);

inline constexpr std::array<const char*, kPhaseCount> kPhaseNames{
    "validate", "program", "target", "state", "uniforms", "vertices",
    "tex_hash", "tex_lookup", "tex_upload", "indices", "draw",
    "rest"};

// Render thread stretches outside the draw (wall time, may nest).
enum class Section : uint8_t {
    ResolvePlans,  // deferred plans of closed frames
    PinCopy,       // PinGpuCopyResources
    SceneEnv,      // scene environment / light publication
    Execute,       // picaPresentationScheduler.Execute (includes the draws)
    VisualSample,  // whole ExecuteVisualSample (includes PinCopy and Execute)
    WindowAcquire, // bind + window clear: blocked wait for the free
                   // swapchain buffer (nwindowDequeueBuffer), not work
    Count
};
inline constexpr size_t kSectionCount = static_cast<size_t>(Section::Count);

inline constexpr std::array<const char*, kSectionCount> kSectionNames{
    "resolve_plans", "pin_copy", "scene_env", "execute", "visual_sample", "acquire"};

enum class Count : uint8_t {
    Draws,            // measured draws
    TexLookups,       // texture lookups
    TexHashes,        // content hashes computed on this render thread
    TexHashBytes,     // bytes hashed
    TexUploads,       // textures created (decode + upload)
    TexUploadBytes,   // GPU bytes of created textures
    UniformSent,      // uniform blocks sent
    UniformSentBytes, // uniform bytes sent
    UniformSkipped,   // blocks equal to the previous one (not sent)
    UniformSkippedBytes,
    VertexBytes,      // vertex bytes copied (ring/own buffer)
    IndexBytes,       // index bytes copied
    Count
};
inline constexpr size_t kCountCount = static_cast<size_t>(Count::Count);

inline constexpr std::array<const char*, kCountCount> kCountNames{
    "draws", "tex_lookups", "tex_hashes", "tex_hash_bytes", "tex_uploads",
    "tex_upload_bytes", "uniform_sent", "uniform_sent_bytes",
    "uniform_skipped", "uniform_skipped_bytes", "vertex_bytes", "index_bytes"};

// Clock. Ticks() is cheap; TicksToNs() converts a sum.
inline uint64_t Ticks() noexcept {
#if defined(__SWITCH__) && defined(__aarch64__)
    uint64_t value;
    asm volatile("mrs %0, cntpct_el0" : "=r"(value)); // armGetSystemTick(): Horizon blocks cntvct_el0 at EL0 (Data Abort)
    return value;
#else
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
#endif
}
inline uint64_t TicksToNs(uint64_t ticks) noexcept {
#if defined(__SWITCH__) && defined(__aarch64__)
    constexpr uint64_t kFrequency = 19200000ULL;  // Switch CNTFRQ
    return ticks / kFrequency * 1000000000ULL +
           ticks % kFrequency * 1000000000ULL / kFrequency;
#else
    return ticks;
#endif
}

struct FrameTotals {
    std::array<uint64_t, kPhaseCount> PhaseTicks{};
    std::array<uint64_t, kSectionCount> SectionTicks{};
    std::array<uint64_t, kCountCount> Counts{};
    uint64_t SubmitTicks = 0U;  // Begin -> End of all draws
};

class Tracker {
public:
    // Start of a draw: time since the last mark is not charged.
    void Begin(uint64_t now) noexcept {
        mStart = mLast = now;
        mOpen = true;
    }
    // Charges the time since the previous mark to `phase`. Outside a draw
    // (Begin not called) does nothing.
    void Lap(Phase phase, uint64_t now) noexcept {
        if (!mOpen) return;
        if (now > mLast) {
            mFrame.PhaseTicks[static_cast<size_t>(phase)] += now - mLast;
        }
        mLast = now;
    }
    // End of the draw: the remainder goes to Rest and the draw total is added.
    void End(uint64_t now) noexcept {
        if (!mOpen) return;
        Lap(Phase::Rest, now);
        if (now > mStart) mFrame.SubmitTicks += now - mStart;
        ++mFrame.Counts[static_cast<size_t>(Count::Draws)];
        mOpen = false;
    }
    bool Open() const noexcept { return mOpen; }

    void Add(Count what, uint64_t value = 1U) noexcept {
        mFrame.Counts[static_cast<size_t>(what)] += value;
    }
    void AddSection(Section section, uint64_t ticks) noexcept {
        mFrame.SectionTicks[static_cast<size_t>(section)] += ticks;
    }
    // Frame totals; resets for the next one.
    FrameTotals TakeFrame() noexcept {
        const FrameTotals result = mFrame;
        mFrame = {};
        mOpen = false;
        return result;
    }

private:
    FrameTotals mFrame{};
    uint64_t mStart = 0U;
    uint64_t mLast = 0U;
    bool mOpen = false;
};

// Window sum (several frames) -> ns per phase; used by the report.
struct WindowTotals {
    std::array<uint64_t, kPhaseCount> PhaseNs{};
    std::array<uint64_t, kSectionCount> SectionNs{};
    std::array<uint64_t, kCountCount> Counts{};
    uint64_t SubmitNs = 0U;

    void Accumulate(const FrameTotals& frame) noexcept {
        for (size_t i = 0; i < kPhaseCount; ++i) {
            PhaseNs[i] += TicksToNs(frame.PhaseTicks[i]);
        }
        for (size_t i = 0; i < kSectionCount; ++i) {
            SectionNs[i] += TicksToNs(frame.SectionTicks[i]);
        }
        for (size_t i = 0; i < kCountCount; ++i) Counts[i] += frame.Counts[i];
        SubmitNs += TicksToNs(frame.SubmitTicks);
    }
};

// Single instance (render thread).
inline Tracker gTracker;

} // namespace Fast::PicaDrawPhases
