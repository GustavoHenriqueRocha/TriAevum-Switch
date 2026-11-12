// TriAevum: decoded ADPCM buffers shared between the DSP sources.
// Licensed under GPLv2 or any later version.
#pragma once

#include "audio_core/codec.h"
#include "audio_core/sample_queue.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

namespace AudioCore {

// Titles play the same ADPCM data again and again: a sequenced song's instrument samples on every
// note, the same sound effects, looping buffers coming round. Decoding is a pure function of the
// encoded bytes, the sample count, the coefficients and the starting predictor state, so a
// buffer with the same inputs and byte-identical encoded data (compared in full, the guest may
// have rewritten it) has exactly the samples and final predictor state of the stored decode.
// Those are handed to the source queue without a copy.
//
// Host only: never serialized, and the guest observes nothing of it. The least recently used
// decodes are dropped beyond kBudgetSamples; a queue still playing one keeps it alive.
class AdpcmDecodeCache {
public:
    using Sample = DecodedSampleQueue::Sample;

    /// Everything a decode depends on besides the encoded bytes.
    struct Origin {
        u32 address = 0;
        u32 length = 0;
        std::array<s16, 16> coefficients{};
        s16 yn1 = 0;
        s16 yn2 = 0;
        bool operator==(const Origin&) const = default;
    };

    /// Puts into `queue` the decode of `origin.length` samples of `encoded` (`bytes` long) with the
    /// predictor starting at `state`, and leaves the final predictor state in `state`, exactly as
    /// Codec::DecodeADPCMInto into the queue would.
    void Decode(const Origin& origin, const u8* encoded, std::size_t bytes,
                Codec::ADPCMState& state, DecodedSampleQueue& queue) {
        ++clock_;
        const u64 key = (u64{origin.length} << 32U) | origin.address;
        const auto [first, last] = entries_.equal_range(key);
        for (auto it = first; it != last; ++it) {
            const auto& entry = it->second;
            if (!(entry->origin == origin)) continue;
            if (entry->encoded.size() == bytes &&
                (bytes == 0 || std::memcmp(entry->encoded.data(), encoded, bytes) == 0)) {
                entry->used = clock_;
                state.yn1 = entry->end_yn1;
                state.yn2 = entry->end_yn2;
                queue.Share(entry, entry->samples.get(), entry->count);
                return;
            }
            // The guest rewrote the data: the new decode below replaces this one (one per origin).
            samples_ -= entry->count;
            entries_.erase(it);
            break;
        }
        auto entry = std::make_shared<Entry>();
        entry->origin = origin;
        entry->count = Codec::ADPCMOutputSamples(origin.length);
        entry->samples.reset(new Sample[entry->count]); // Default-initialized: all written below.
        Codec::DecodeADPCMInto(entry->samples.get(), encoded, origin.length, origin.coefficients,
                               state);
        entry->end_yn1 = state.yn1;
        entry->end_yn2 = state.yn2;
        queue.Share(entry, entry->samples.get(), entry->count);
        if (bytes > kMaximumEntryBytes) return;
        entry->encoded.assign(encoded, encoded + bytes);
        entry->used = clock_;
        samples_ += entry->count;
        entries_.emplace(key, std::move(entry));
        while (samples_ > kBudgetSamples && entries_.size() > 1) {
            auto oldest = entries_.begin();
            for (auto it = entries_.begin(); it != entries_.end(); ++it) {
                if (it->second->used < oldest->second->used) oldest = it;
            }
            samples_ -= oldest->second->count;
            entries_.erase(oldest);
        }
    }

private:
    // 2 M samples: 8 MiB of decoded audio, about a minute at the DSP rate.
    static constexpr std::size_t kBudgetSamples = std::size_t{2} << 20U;
    // Longer buffers (more than ~7.5 s) are decoded but not kept.
    static constexpr std::size_t kMaximumEntryBytes = std::size_t{128} << 10U;

    struct Entry {
        Origin origin;
        std::vector<u8> encoded;
        std::unique_ptr<Sample[]> samples;
        std::size_t count = 0;
        s16 end_yn1 = 0;
        s16 end_yn2 = 0;
        u64 used = 0;
    };

    // By (length, address); several decodes of one buffer differ in coefficients or start state.
    std::unordered_multimap<u64, std::shared_ptr<Entry>> entries_;
    std::size_t samples_ = 0;
    u64 clock_ = 0;
};

} // namespace AudioCore
