#include "oot3d_host_audio_resampler.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Oot3dNativeGame {
namespace {

constexpr double kPi = 3.14159265358979323846;
// ~80 dB stopband for the Kaiser window.
constexpr double kKaiserBeta = 8.0;
// Fraction of the lower Nyquist frequency kept as the filter's cutoff: the
// transition band of a 48-tap filter ends just below Nyquist.
constexpr double kCutoffScale = 0.94;
constexpr double kFixedOne = 4294967296.0; // 2^32

double BesselI0(double x) {
    double sum = 1.0;
    double term = 1.0;
    const double half = x * 0.5;
    for (int k = 1; k < 64; ++k) {
        term *= (half / k) * (half / k);
        sum += term;
        if (term < sum * 1e-17) {
            break;
        }
    }
    return sum;
}

int16_t RoundToS16(float value) {
    const long rounded = std::lrintf(value);
    return static_cast<int16_t>(std::clamp<long>(rounded, -32768L, 32767L));
}

} // namespace

HostAudioResampler::HostAudioResampler(uint32_t inputRate, uint32_t outputRate)
    : mInputRate(inputRate), mOutputRate(outputRate) {
    if (inputRate == 0 || outputRate == 0) {
        throw std::invalid_argument("host audio resampler needs non-zero rates");
    }
    mBaseStep = static_cast<uint64_t>(
        std::llround(static_cast<double>(inputRate) / outputRate * kFixedOne));
    mStep = mBaseStep;

    // Cutoff in cycles per input frame: below the input Nyquist when
    // upsampling, below the output Nyquist when downsampling.
    const double cutoff =
        0.5 * kCutoffScale *
        std::min(1.0, static_cast<double>(outputRate) / inputRate);
    const double i0Beta = BesselI0(kKaiserBeta);
    mTable.resize((Phases + 1) * Taps);
    for (size_t phase = 0; phase <= Phases; ++phase) {
        const double offset = static_cast<double>(phase) / Phases;
        float* row = &mTable[phase * Taps];
        double sum = 0.0;
        for (size_t tap = 0; tap < Taps; ++tap) {
            // Tap i weighs input frame (position - (HalfTaps - 1) + i).
            const double distance =
                offset - (static_cast<double>(tap) - (HalfTaps - 1));
            const double normalized = distance / HalfTaps;
            double window = 0.0;
            if (std::abs(normalized) < 1.0) {
                window = BesselI0(kKaiserBeta *
                                  std::sqrt(1.0 - normalized * normalized)) /
                         i0Beta;
            }
            const double argument = 2.0 * cutoff * distance;
            const double sinc =
                std::abs(argument) < 1e-12
                    ? 1.0
                    : std::sin(kPi * argument) / (kPi * argument);
            const double value = 2.0 * cutoff * sinc * window;
            row[tap] = static_cast<float>(value);
            sum += value;
        }
        // Unit DC gain on every phase: unequal phase gains are heard as a
        // tone/noise at the beat between the two rates.
        for (size_t tap = 0; tap < Taps; ++tap) {
            row[tap] = static_cast<float>(row[tap] / sum);
        }
    }
    Reset();
}

void HostAudioResampler::SetRateAdjustment(double adjustment) {
    mRateAdjustment = std::clamp(adjustment, -MaximumRateAdjustment,
                                 MaximumRateAdjustment);
    mStep = static_cast<uint64_t>(
        std::llround(static_cast<double>(mBaseStep) * (1.0 + mRateAdjustment)));
}

void HostAudioResampler::Reset() {
    mLeft.assign(HalfTaps - 1, 0.0F);
    mRight.assign(HalfTaps - 1, 0.0F);
    mPosition = HalfTaps - 1;
    mFraction = 0;
}

void HostAudioResampler::Process(std::span<const int16_t> interleavedStereo,
                                 std::vector<int16_t>& output) {
    const size_t frames = interleavedStereo.size() / 2U;
    mLeft.reserve(mLeft.size() + frames);
    mRight.reserve(mRight.size() + frames);
    for (size_t frame = 0; frame < frames; ++frame) {
        mLeft.push_back(static_cast<float>(interleavedStereo[frame * 2U]));
        mRight.push_back(static_cast<float>(interleavedStereo[frame * 2U + 1U]));
    }

    const double expected =
        static_cast<double>(frames) * kFixedOne / static_cast<double>(mStep);
    output.reserve(output.size() + 2U * (static_cast<size_t>(expected) + 2U));
    constexpr double kPhaseScale = static_cast<double>(Phases) / kFixedOne;
    while (mPosition + HalfTaps < mLeft.size()) {
        const double phasePosition = static_cast<double>(mFraction) * kPhaseScale;
        const size_t phase = std::min<size_t>(
            static_cast<size_t>(phasePosition), Phases - 1U);
        const float blend = static_cast<float>(phasePosition - phase);
        const float* first = &mTable[phase * Taps];
        const float* second = first + Taps;
        const float* left = &mLeft[mPosition - (HalfTaps - 1)];
        const float* right = &mRight[mPosition - (HalfTaps - 1)];
        float left0 = 0.0F, left1 = 0.0F, right0 = 0.0F, right1 = 0.0F;
        for (size_t tap = 0; tap < Taps; ++tap) {
            left0 += first[tap] * left[tap];
            left1 += second[tap] * left[tap];
            right0 += first[tap] * right[tap];
            right1 += second[tap] * right[tap];
        }
        output.push_back(RoundToS16(left0 + (left1 - left0) * blend));
        output.push_back(RoundToS16(right0 + (right1 - right0) * blend));

        mFraction += mStep;
        mPosition += static_cast<size_t>(mFraction >> 32U);
        mFraction &= 0xFFFFFFFFULL;
    }

    // Keep HalfTaps - 1 frames of history before the current position.
    const size_t consumed = mPosition - (HalfTaps - 1);
    if (consumed != 0) {
        const size_t erase = std::min(consumed, mLeft.size());
        mLeft.erase(mLeft.begin(), mLeft.begin() + static_cast<std::ptrdiff_t>(erase));
        mRight.erase(mRight.begin(), mRight.begin() + static_cast<std::ptrdiff_t>(erase));
        mPosition -= erase;
    }
}

} // namespace Oot3dNativeGame
