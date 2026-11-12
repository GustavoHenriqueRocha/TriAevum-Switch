// TriAevum host storage adapter. Audio algorithms remain Azahar's.
// Licensed under GPLv2 or any later version.
#pragma once

#include "audio_core/audio_types.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace AudioCore {

// One decoded source buffer, consumed without copying its unread tail.
// Serialized state contains unread samples only, never this host cursor.
//
// The samples live either in the queue's own storage, reused from one decoded buffer to the next
// (Overwrite: the DSP mix runs on the game thread and the original per-buffer vector cost an
// allocation, a zero fill of every sample and a free each time a buffer was dequeued), or in an
// immutable decode shared with other queues (Share: AdpcmDecodeCache). Only the unread samples are
// observable, so neither changes a value.
class DecodedSampleQueue {
public:
    using Sample = StereoBuffer16::value_type;

    DecodedSampleQueue() = default;
    DecodedSampleQueue(const StereoBuffer16& samples) { Assign(samples.data(), samples.size()); }
    DecodedSampleQueue(const DecodedSampleQueue& other) {
        const auto remaining = other.Remaining();
        Assign(remaining.data(), remaining.size());
    }
    DecodedSampleQueue& operator=(const DecodedSampleQueue& other) {
        if (this != &other) {
            const auto remaining = other.Remaining();
            if (remaining.size() > capacity_) {
                DecodedSampleQueue copy(other);
                *this = std::move(copy);
            } else {
                std::copy(remaining.begin(), remaining.end(), data_.get());
                shared_.reset();
                base_ = data_.get();
                size_ = remaining.size();
                first_ = 0;
            }
        }
        return *this;
    }
    DecodedSampleQueue(DecodedSampleQueue&& other) noexcept
        : data_(std::move(other.data_)), shared_(std::move(other.shared_)),
          base_(std::exchange(other.base_, nullptr)), size_(std::exchange(other.size_, 0)),
          capacity_(std::exchange(other.capacity_, 0)), first_(std::exchange(other.first_, 0)) {}
    DecodedSampleQueue& operator=(DecodedSampleQueue&& other) noexcept {
        if (this != &other) {
            data_ = std::move(other.data_);
            shared_ = std::move(other.shared_);
            base_ = std::exchange(other.base_, nullptr);
            size_ = std::exchange(other.size_, 0);
            capacity_ = std::exchange(other.capacity_, 0);
            first_ = std::exchange(other.first_, 0);
        }
        return *this;
    }

    DecodedSampleQueue& operator=(const StereoBuffer16& samples) {
        Sample* destination = Overwrite(samples.size());
        std::copy(samples.begin(), samples.end(), destination);
        return *this;
    }

    /// Replaces the contents with `count` samples the caller writes through the returned pointer
    /// (all of them, before any other use of the queue). The cursor restarts at the first sample.
    Sample* Overwrite(std::size_t count) {
        shared_.reset();
        // Keep the storage unless it is far larger than needed (a long buffer played once).
        if (count > capacity_ || (capacity_ > kShrinkAbove && count < capacity_ / 4)) {
            data_.reset();
            capacity_ = 0;
            if (count != 0) data_.reset(new Sample[count]); // Default-initialized: no zero fill.
            capacity_ = count;
        }
        base_ = data_.get();
        size_ = count;
        first_ = 0;
        return data_.get();
    }

    /// Replaces the contents with `count` immutable samples kept alive by `owner`, without a copy.
    /// The cursor restarts at the first sample.
    void Share(std::shared_ptr<const void> owner, const Sample* samples, std::size_t count) {
        shared_ = std::move(owner);
        base_ = samples;
        size_ = count;
        first_ = 0;
    }

    std::span<const Sample> Remaining() const noexcept {
        return std::span<const Sample>(base_ + first_, size_ - first_);
    }
    const Sample* data() const noexcept { return base_ + first_; }
    std::size_t size() const noexcept { return size_ - first_; }
    bool empty() const noexcept { return first_ == size_; }
    const Sample& operator[](std::size_t index) const noexcept { return base_[first_ + index]; }
    auto begin() const noexcept { return Remaining().begin(); }
    auto end() const noexcept { return Remaining().end(); }
    void clear() noexcept {
        shared_.reset();
        base_ = data_.get();
        size_ = 0;
        first_ = 0;
    }

    void Consume(std::size_t count) {
        if (count > size()) throw std::out_of_range("decoded audio sample cursor");
        first_ += count;
    }

    StereoBuffer16 CopyRemaining() const { return {begin(), end()}; }

private:
    static constexpr std::size_t kShrinkAbove = std::size_t{1} << 16U; // Samples (256 KiB).

    void Assign(const Sample* samples, std::size_t count) {
        std::copy(samples, samples + count, Overwrite(count));
    }

    std::unique_ptr<Sample[]> data_;
    std::shared_ptr<const void> shared_; // Owner of the samples when they are shared.
    const Sample* base_ = nullptr;       // data_.get() or the shared samples.
    std::size_t size_ = 0;
    std::size_t capacity_ = 0;
    std::size_t first_ = 0;
};
} // namespace AudioCore
