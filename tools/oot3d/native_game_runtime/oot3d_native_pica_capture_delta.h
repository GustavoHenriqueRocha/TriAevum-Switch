#pragma once

// Capture delta (config/capture_delta): the per-draw copy of the register
// file, float uniforms and default attributes leaves the game thread.
//
// The queue appends, per draw, only the 64-byte lines the frontend marked
// dirty since the previous draw (Oot3dPicaStateDirtyLines); the first draw of
// every log is a keyframe (all 100 lines, 6.4 KiB). The submission's packet
// holds everything else and leaves those three arrays unset
// (Oot3dPicaStateArraysDeferred). Whoever consumes the submissions first
// replays the log in draw order into a running image and copies the image
// into each packet: the plan builder (under its build lock, core 1 or the
// render thread) for deferred batches, the queue itself for every other take.
// The materialized arrays are byte for byte what the full copy at the draw
// would have written (oot3d_native_capture_delta_tests).

#include "oot3d_native_pica_frontend.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace Oot3dNativeGame {

struct Oot3dPicaDrawSubmission;

// A submission whose arrays were written at the draw (no log entry).
inline constexpr uint32_t kOot3dPicaNoDeltaEntry = 0xFFFFFFFFU;

class Oot3dPicaCaptureDeltaLog final {
  public:
    struct AppendCost {
        size_t Bytes = 0;
        uint64_t RegisterTicks = 0;
        uint64_t UniformTicks = 0;
    };

    // Capture side (the queue, under its lock). Appends the draw's lines
    // (all of them for the first entry), clears the source's dirty lines and
    // returns the entry index.
    uint32_t Append(const Oot3dPicaDrawPacketSource& source, AppendCost& cost);
    [[nodiscard]] size_t Entries() const noexcept { return mEntries.size(); }
    [[nodiscard]] bool Keyframe(uint32_t entry) const noexcept {
        return entry < mEntries.size() && mEntries[entry].Keyframe;
    }
    // Empty, capacity kept.
    void Reset() noexcept;

    // Consumer side: fills the arrays of draws[0..last] that refer to an
    // entry and are not filled yet. `draws` is the vector the entries were
    // appended for, in the same order. Serialized internally.
    void MaterializeThrough(std::span<Oot3dPicaDrawSubmission> draws, size_t last);
    void MaterializeAll(std::span<Oot3dPicaDrawSubmission> draws) {
        if (!draws.empty()) MaterializeThrough(draws, draws.size() - 1U);
    }

    // Cumulative over every log (logs/perf.txt): the replay and copy cost,
    // on whichever thread consumed the draws.
    struct Totals {
        uint64_t Draws = 0;
        uint64_t Ticks = 0;
    };
    [[nodiscard]] static Totals MaterializeTotals() noexcept {
        return {sMaterializedDraws.load(std::memory_order_relaxed),
                sMaterializeTicks.load(std::memory_order_relaxed)};
    }

  private:
    struct Line {
        uint32_t Words[16];
    };
    static_assert(sizeof(Line) == 64U);
    struct Entry {
        uint64_t Registers = 0;
        uint32_t VertexUniforms = 0;
        uint32_t GeometryUniforms = 0;
        uint32_t DefaultAttributes = 0;
        uint32_t FirstLine = 0;
        bool Keyframe = false;
    };

    void Apply(const Entry& entry) noexcept;
    // Room for `count` more lines (grown without zeroing: every line is
    // written before it is read).
    Line* ReserveLines(size_t count);

    std::vector<Entry> mEntries;
    std::unique_ptr<Line[]> mLines;
    size_t mLineCount = 0;
    size_t mLineCapacity = 0;
    std::mutex mMutex;
    // Draws [0, mCursor) are materialized; mNextEntry is the next to replay.
    size_t mCursor = 0;
    uint32_t mNextEntry = 0;
    // The running image (the state after entry mNextEntry - 1).
    std::array<uint32_t, 0x300> mRegisters{};
    std::array<std::array<float, 4>, Oot3dPicaShaderState::FloatUniformCount>
        mVertexUniforms{};
    std::array<std::array<float, 4>, Oot3dPicaShaderState::FloatUniformCount>
        mGeometryUniforms{};
    std::array<std::array<float, 4>, 16> mDefaultAttributes{};

    static inline std::atomic<uint64_t> sMaterializedDraws{0};
    static inline std::atomic<uint64_t> sMaterializeTicks{0};
};

// Recycled logs: one is handed over with every drained batch, so a steady
// game allocates none. Outlives its user (a log may die on any thread).
class Oot3dPicaCaptureDeltaLogPool final {
  public:
    static std::shared_ptr<Oot3dPicaCaptureDeltaLog> Acquire(
        const std::shared_ptr<Oot3dPicaCaptureDeltaLogPool>& pool);

  private:
    std::mutex mMutex;
    std::vector<std::unique_ptr<Oot3dPicaCaptureDeltaLog>> mFree;
};

} // namespace Oot3dNativeGame
