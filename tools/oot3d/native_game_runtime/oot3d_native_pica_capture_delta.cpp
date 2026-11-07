#include "oot3d_native_pica_capture_delta.h"

#include "oot3d_native_capture_clock.h"
#include "oot3d_native_pica_submission.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>

namespace Oot3dNativeGame {

namespace {

constexpr size_t kWordsPerLine = 16U;
constexpr size_t kVectorsPerLine = 4U;

} // namespace

uint32_t Oot3dPicaCaptureDeltaLog::Append(const Oot3dPicaDrawPacketSource& source,
                                          AppendCost& cost) {
    Entry entry;
    entry.Keyframe = mEntries.empty();
    if (entry.Keyframe) {
        entry.Registers = Oot3dPicaStateDirtyLines::AllRegisters;
        entry.VertexUniforms = Oot3dPicaStateDirtyLines::AllUniforms;
        entry.GeometryUniforms = Oot3dPicaStateDirtyLines::AllUniforms;
        entry.DefaultAttributes = Oot3dPicaStateDirtyLines::AllAttributes;
    } else {
        entry.Registers = source.DirtyLines->Registers;
        entry.VertexUniforms = source.DirtyLines->VertexUniforms;
        entry.GeometryUniforms = source.DirtyLines->GeometryUniforms;
        entry.DefaultAttributes = source.DirtyLines->DefaultAttributes;
    }
    entry.FirstLine = static_cast<uint32_t>(mLineCount);
    const size_t lines = static_cast<size_t>(std::popcount(entry.Registers)) +
                         std::popcount(entry.VertexUniforms) +
                         std::popcount(entry.GeometryUniforms) +
                         std::popcount(entry.DefaultAttributes);
    Line* out = ReserveLines(lines);
    mLineCount += lines;
    const uint64_t started = CaptureTicks();
    const uint32_t* registers = source.Registers->data();
    for (uint64_t bits = entry.Registers; bits != 0U; bits &= bits - 1U) {
        const size_t line = static_cast<size_t>(std::countr_zero(bits));
        std::memcpy(out++, registers + line * kWordsPerLine, sizeof(Line));
    }
    const uint64_t registersDone = CaptureTicks();
    const auto copyVectors = [&out](uint32_t bits, const std::array<float, 4>* vectors) {
        for (; bits != 0U; bits &= bits - 1U) {
            const size_t line = static_cast<size_t>(std::countr_zero(bits));
            std::memcpy(out++, vectors + line * kVectorsPerLine, sizeof(Line));
        }
    };
    copyVectors(entry.VertexUniforms, source.VertexShader->FloatUniforms.data());
    copyVectors(entry.GeometryUniforms, source.GeometryShader->FloatUniforms.data());
    copyVectors(entry.DefaultAttributes, source.DefaultAttributes->data());
    cost.RegisterTicks += registersDone - started;
    cost.UniformTicks += CaptureTicks() - registersDone;
    cost.Bytes += lines * sizeof(Line);
    source.DirtyLines->Clear();
    mEntries.push_back(entry);
    return static_cast<uint32_t>(mEntries.size() - 1U);
}

Oot3dPicaCaptureDeltaLog::Line* Oot3dPicaCaptureDeltaLog::ReserveLines(size_t count) {
    if (mLineCount + count > mLineCapacity) {
        const size_t capacity = std::max<size_t>(mLineCount + count, mLineCapacity * 2U + 256U);
        std::unique_ptr<Line[]> grown(new Line[capacity]);
        if (mLineCount != 0U) std::memcpy(grown.get(), mLines.get(), mLineCount * sizeof(Line));
        mLines = std::move(grown);
        mLineCapacity = capacity;
    }
    return mLines.get() + mLineCount;
}

void Oot3dPicaCaptureDeltaLog::Reset() noexcept {
    std::lock_guard lock(mMutex);
    mEntries.clear();
    mLineCount = 0U;
    mCursor = 0U;
    mNextEntry = 0U;
}

void Oot3dPicaCaptureDeltaLog::Apply(const Entry& entry) noexcept {
    const Line* in = mLines.get() + entry.FirstLine;
    for (uint64_t bits = entry.Registers; bits != 0U; bits &= bits - 1U) {
        const size_t line = static_cast<size_t>(std::countr_zero(bits));
        std::memcpy(mRegisters.data() + line * kWordsPerLine, in++, sizeof(Line));
    }
    const auto applyVectors = [&in](uint32_t bits, std::array<float, 4>* vectors) {
        for (; bits != 0U; bits &= bits - 1U) {
            const size_t line = static_cast<size_t>(std::countr_zero(bits));
            std::memcpy(vectors + line * kVectorsPerLine, in++, sizeof(Line));
        }
    };
    applyVectors(entry.VertexUniforms, mVertexUniforms.data());
    applyVectors(entry.GeometryUniforms, mGeometryUniforms.data());
    applyVectors(entry.DefaultAttributes, mDefaultAttributes.data());
}

void Oot3dPicaCaptureDeltaLog::MaterializeThrough(
    std::span<Oot3dPicaDrawSubmission> draws, size_t last) {
    std::lock_guard lock(mMutex);
    if (last >= draws.size() || mCursor > last) return;
    const uint64_t started = CaptureTicks();
    uint64_t materialized = 0U;
    for (; mCursor <= last; ++mCursor) {
        auto& draw = draws[mCursor];
        if (draw.DeltaEntry == kOot3dPicaNoDeltaEntry) continue;
        if (draw.DeltaEntry != mNextEntry || mNextEntry >= mEntries.size()) {
            throw std::logic_error("capture delta log replayed out of draw order");
        }
        Apply(mEntries[mNextEntry++]);
        auto& packet = draw.Packet;
        packet.Registers = mRegisters;
        packet.VertexShader.FloatUniforms = mVertexUniforms;
        packet.GeometryShader.FloatUniforms = mGeometryUniforms;
        packet.DefaultAttributes = mDefaultAttributes;
        draw.DeltaEntry = kOot3dPicaNoDeltaEntry;
        ++materialized;
    }
    sMaterializedDraws.fetch_add(materialized, std::memory_order_relaxed);
    sMaterializeTicks.fetch_add(CaptureTicks() - started, std::memory_order_relaxed);
}

std::shared_ptr<Oot3dPicaCaptureDeltaLog> Oot3dPicaCaptureDeltaLogPool::Acquire(
    const std::shared_ptr<Oot3dPicaCaptureDeltaLogPool>& pool) {
    std::unique_ptr<Oot3dPicaCaptureDeltaLog> log;
    {
        std::lock_guard lock(pool->mMutex);
        if (!pool->mFree.empty()) {
            log = std::move(pool->mFree.back());
            pool->mFree.pop_back();
        }
    }
    if (log == nullptr) log = std::make_unique<Oot3dPicaCaptureDeltaLog>();
    return std::shared_ptr<Oot3dPicaCaptureDeltaLog>(
        log.release(), [pool](Oot3dPicaCaptureDeltaLog* released) {
            released->Reset();
            std::unique_ptr<Oot3dPicaCaptureDeltaLog> owned(released);
            std::lock_guard lock(pool->mMutex);
            // More than the few alive at once (the queue's, batches in
            // flight) would only pin memory.
            if (pool->mFree.size() < 8U) pool->mFree.push_back(std::move(owned));
        });
}

} // namespace Oot3dNativeGame
