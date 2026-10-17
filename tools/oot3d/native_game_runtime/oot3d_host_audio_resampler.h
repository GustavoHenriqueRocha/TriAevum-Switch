#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Oot3dNativeGame {

// Streaming stereo s16 resampler between the DSP HLE output (32728 Hz) and the
// host audio device. Windowed-sinc (Kaiser) polyphase filter with every phase
// normalised to unit DC gain, float accumulation, rounding and saturation back
// to s16. The rate can be nudged by a small ratio so the host queue stays at
// its target without pitch jumps or dropped chunks.
class HostAudioResampler {
  public:
    static constexpr size_t HalfTaps = 24;
    static constexpr size_t Taps = HalfTaps * 2;
    static constexpr size_t Phases = 512;
    static constexpr double MaximumRateAdjustment = 0.01;

    HostAudioResampler(uint32_t inputRate, uint32_t outputRate);

    uint32_t InputRate() const { return mInputRate; }
    uint32_t OutputRate() const { return mOutputRate; }

    // Positive values consume the input faster (fewer output frames); clamped
    // to +/-MaximumRateAdjustment.
    void SetRateAdjustment(double adjustment);
    double RateAdjustment() const { return mRateAdjustment; }

    // Appends the resampled interleaved stereo frames to output.
    void Process(std::span<const int16_t> interleavedStereo,
                 std::vector<int16_t>& output);

    // Forgets the history (after a flush of the host queue or a savestate load).
    void Reset();

  private:
    uint32_t mInputRate = 1;
    uint32_t mOutputRate = 1;
    uint64_t mBaseStep = 0; // input frames per output frame, 32.32 fixed point
    uint64_t mStep = 0;
    uint64_t mFraction = 0; // position between mPosition and mPosition + 1
    double mRateAdjustment = 0.0;
    size_t mPosition = 0;    // index into mLeft/mRight of the current frame
    std::vector<float> mTable; // (Phases + 1) rows of Taps coefficients
    std::vector<float> mLeft;
    std::vector<float> mRight;
};

} // namespace Oot3dNativeGame
