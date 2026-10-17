// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include "audio_core/interpolate.h"
#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#include "common/assert.h"

namespace AudioCore::AudioInterp {

// Calculations are done in fixed point with 24 fractional bits.
// (This is not verified. This was chosen for minimal error.)
constexpr u64 scale_factor = 1 << 24;
constexpr u64 scale_mask = scale_factor - 1;

/// Here we step over the input in steps of rate, until we consume all of the input.
/// Three adjacent samples are passed to fn each step.
template <typename Function>
static void StepOverSamples(State& state, StereoBuffer16& input, float rate, StereoFrame16& output,
                            std::size_t& outputi, Function fn) {
    ASSERT(rate > 0);

    if (input.empty())
        return;

    // The two historical samples are a virtual prefix, not inserted into storage.
    const auto sample = [&](std::size_t index) -> const std::array<s16, 2>& {
        if (index == 0) return state.xn2;
        if (index == 1) return state.xn1;
        return input[index - 2];
    };

    const u64 step_size = static_cast<u64>(rate * scale_factor);
    u64 fposition = state.fposition;
    std::size_t inputi = 0;

    while (outputi < output.size()) {
        inputi = static_cast<std::size_t>(fposition / scale_factor);

        if (inputi >= input.size()) {
            inputi = input.size();
            break;
        }

        u64 fraction = fposition & scale_mask;
        output[outputi++] = fn(fraction, sample(inputi), sample(inputi + 1), sample(inputi + 2));

        fposition += step_size;
    }

    const auto xn2 = sample(inputi);
    const auto xn1 = sample(inputi + 1);
    state.xn2 = xn2;
    state.xn1 = xn1;
    state.fposition = fposition - inputi * scale_factor;

    input.Consume(inputi);
}

void None(State& state, StereoBuffer16& input, float rate, StereoFrame16& output,
          std::size_t& outputi) {
    StepOverSamples(
        state, input, rate, output, outputi,
        [](u64 fraction, const auto& x0, const auto& x1, const auto& x2) { return x0; });
}

void Linear(State& state, StereoBuffer16& input, float rate, StereoFrame16& output,
            std::size_t& outputi) {
    // Note on accuracy: Some values that this produces are +/- 1 from the actual firmware.
    StepOverSamples(state, input, rate, output, outputi,
                    [](u64 fraction, const auto& x0, const auto& x1, const auto& x2) {
                        // This is a saturated subtraction. (Verified by black-box fuzzing.)
                        s64 delta0 = std::clamp<s64>(x1[0] - x0[0], -32768, 32767);
                        s64 delta1 = std::clamp<s64>(x1[1] - x0[1], -32768, 32767);

                        return std::array<s16, 2>{
                            static_cast<s16>(x0[0] + fraction * delta0 / scale_factor),
                            static_cast<s16>(x0[1] + fraction * delta1 / scale_factor),
                        };
                    });
}

// ---------------------------------------------------------------------------------------------
// TriAevum: optional high-quality per-source interpolation.
//
// The guest only observes the stepping (how many samples are consumed per frame, which determines
// buffer positions, buffer completion and loop timing). HighQuality() reproduces StepOverSamples'
// stepping verbatim and only replaces the value computed for each output position. The stream seen
// by the interpolator is continuous across calls, buffers and loops: virtual index 0 is xn2, 1 is
// xn1, 2.. is the unread input, and State::history keeps the kHistorySamples before xn2.
//
// At every output the original loop guarantees samples up to virtual index i + 2. The sinc kernel
// (up to kSincMaxTaps taps around i + f - kSincDelay) therefore never reads past i + 2, at the cost of a
// constant kSincDelay-sample delay; Catmull-Rom uses i - 1 .. i + 2 and adds no delay.
// ---------------------------------------------------------------------------------------------

namespace {

using Sample = std::array<s16, 2>;

// The original stepping guarantees virtual index i + 2 at every output.
constexpr std::ptrdiff_t kForward = 2;
static_assert(kSincMaxTaps / 2 - kSincDelay == static_cast<std::size_t>(kForward));
static_assert(kSincDelay + kSincMaxTaps / 2 - 1 <= kHistorySamples);

// Polyphase tables in Q15 fixed point, kPhases + 1 rows per bucket; the two dot products of the
// rows around the fractional position are blended linearly. Integer
// arithmetic keeps the output bit-identical on every host (the tests run on x86, the game on
// AArch64) and maps to NEON widening multiply-accumulates.
#ifndef TRIAEVUM_SINC_PHASE_BITS
#define TRIAEVUM_SINC_PHASE_BITS 8
#endif
constexpr unsigned kPhaseBits = TRIAEVUM_SINC_PHASE_BITS;
constexpr std::size_t kPhases = std::size_t{1} << kPhaseBits;
constexpr unsigned kPhaseShift = 24 - kPhaseBits;
constexpr int kCoefficientBits = 15;
constexpr unsigned kBlendBits = 12; // Fraction bits used to blend adjacent phases.
constexpr unsigned kBlendDrop = kPhaseShift - kBlendBits;
constexpr double kKaiserBeta = 8.0;

// Upsampling (rate <= 1) uses kSincBaseTaps taps with the cutoff at the input Nyquist frequency,
// so integer positions reproduce the input exactly. A decimating source uses the smallest bucket
// whose rate is >= its own: cutoff at the output Nyquist frequency (1 / rate) and a kernel widened
// with the rate (up to kSincMaxTaps) so the transition band keeps its width relative to the cutoff.
constexpr std::size_t kSincBaseTaps = kSincMaxTaps / 2;
struct SincBucket {
    float max_rate;
    std::size_t taps;
};
constexpr std::size_t BucketTaps(float rate) {
    // kSincBaseTaps * min(rate, 2), rounded up to a multiple of 4.
    const float scaled = static_cast<float>(kSincBaseTaps) * (rate < 2.0f ? rate : 2.0f);
    return (static_cast<std::size_t>(scaled) + 3) / 4 * 4;
}
static_assert(kSincBaseTaps == 16, "Render() instantiates 16, 20, 24 and 32 taps");
constexpr std::array<SincBucket, 8> kBuckets{{
    {1.0f, BucketTaps(1.0f)},
    {1.25f, BucketTaps(1.25f)},
    {1.5f, BucketTaps(1.5f)},
    {2.0f, BucketTaps(2.0f)},
    {2.5f, BucketTaps(2.5f)},
    {3.0f, BucketTaps(3.0f)},
    {4.0f, BucketTaps(4.0f)},
    {6.0f, BucketTaps(6.0f)},
}};

struct SincTable {
    std::size_t taps = 0;
    std::ptrdiff_t back = 0; // Tap 0 reads virtual index i - back.
    std::vector<s16> rows;   // (kPhases + 1) rows of `taps` Q15 coefficients.
};

double BesselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 64; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-17 * sum) break;
    }
    return sum;
}

std::array<SincTable, kBuckets.size()> BuildSincTables() {
    std::array<SincTable, kBuckets.size()> tables;
    const double pi = 3.14159265358979323846;
    const double window_scale = BesselI0(kKaiserBeta);
    for (std::size_t bucket = 0; bucket < kBuckets.size(); ++bucket) {
        SincTable& table = tables[bucket];
        table.taps = kBuckets[bucket].taps;
        const std::size_t half = table.taps / 2;
        table.back = static_cast<std::ptrdiff_t>(kSincDelay + half - 1);
        table.rows.resize((kPhases + 1) * table.taps);
        const double cutoff = 1.0 / kBuckets[bucket].max_rate;
        std::vector<double> row(table.taps);
        for (std::size_t phase = 0; phase <= kPhases; ++phase) {
            const double fraction = static_cast<double>(phase) / kPhases;
            double sum = 0.0;
            for (std::size_t tap = 0; tap < table.taps; ++tap) {
                // Tap `tap` reads x[i - back + tap]; the interpolated point is
                // i - kSincDelay + fraction, at distance (half - 1 - tap + fraction).
                const double x = static_cast<double>(half) - 1.0 - static_cast<double>(tap) +
                                 fraction;
                const double r = x / static_cast<double>(half);
                double value = 0.0;
                if (std::abs(r) < 1.0) {
                    const double arg = pi * cutoff * x;
                    const double sinc = x == 0.0 ? 1.0 : std::sin(arg) / arg;
                    value = sinc * BesselI0(kKaiserBeta * std::sqrt(1.0 - r * r)) / window_scale;
                }
                row[tap] = value;
                sum += value;
            }
            // Unity DC gain on every phase (exactly 1 << kCoefficientBits after rounding): no
            // fixed-pattern ripple from the fractional position.
            s16* const out = table.rows.data() + phase * table.taps;
            int total = 0;
            std::size_t largest = 0;
            for (std::size_t tap = 0; tap < table.taps; ++tap) {
                const long value = std::lround(row[tap] / sum * (1 << kCoefficientBits));
                out[tap] = static_cast<s16>(std::clamp<long>(value, -32768, 32767));
                total += out[tap];
                if (std::abs(row[tap]) > std::abs(row[largest])) largest = tap;
            }
            // The residual goes to the largest tap, or to its larger neighbour when that tap is
            // already saturated (phase 0 of the upsampling kernel is a unit impulse).
            std::size_t target = largest;
            const int residual = (1 << kCoefficientBits) - total;
            if (out[largest] + residual > 32767) {
                target = largest + 1 < table.taps &&
                                 (largest == 0 || std::abs(row[largest + 1]) >=
                                                      std::abs(row[largest - 1]))
                             ? largest + 1
                             : largest - 1;
            }
            out[target] = static_cast<s16>(out[target] + residual);
            // Dot() accumulates in 32 bits: |sum| <= 32768 * L1(row) must stay below 2^31.
            int l1 = 0;
            for (std::size_t tap = 0; tap < table.taps; ++tap) l1 += std::abs(out[tap]);
            if (l1 >= 65536) throw std::logic_error("sinc kernel L1 norm overflows Q15 accumulator");
        }
    }
    return tables;
}

const SincTable& TableFor(float rate) {
    static const auto tables = BuildSincTables();
    for (std::size_t bucket = 0; bucket < kBuckets.size(); ++bucket) {
        if (rate <= kBuckets[bucket].max_rate * 1.0001f) return tables[bucket];
    }
    return tables.back();
}

inline s16 RoundQ15(s32 accumulator) {
    return static_cast<s16>(std::clamp<s32>(
        (accumulator + (1 << (kCoefficientBits - 1))) >> kCoefficientBits, -32768, 32767));
}

inline s16 ToSample(float value) {
    // Round half away from zero without a data-dependent branch.
    value = std::clamp(value, -32768.0f, 32767.0f);
    return static_cast<s16>(static_cast<int>(value + std::copysign(0.5f, value)));
}

/// The interpolator's view of the stream before this call.
struct OldStream {
    const State& state;
    const Sample* input;
    std::size_t size;

    Sample At(std::ptrdiff_t index) const {
        if (index < 0) return state.history[kHistorySamples + index];
        if (index == 0) return state.xn2;
        if (index == 1) return state.xn1;
        return input[index - 2];
    }
};

constexpr std::ptrdiff_t kChunk = 512;

/// Copies virtual indices [lo, hi] to planar arrays; returns whether both channels are equal.
template <typename T>
bool Gather(const OldStream& old, std::ptrdiff_t lo, std::ptrdiff_t hi, T* __restrict left,
            T* __restrict right) {
    std::ptrdiff_t index = lo;
    int differ = 0;
    for (; index <= hi && index < 2; ++index) {
        const Sample sample = old.At(index);
        left[index - lo] = static_cast<T>(sample[0]);
        right[index - lo] = static_cast<T>(sample[1]);
        differ |= sample[0] ^ sample[1];
    }
    if (index <= hi) {
        const Sample* source = old.input + (index - 2);
        T* __restrict l = left + (index - lo);
        T* __restrict r = right + (index - lo);
        const std::ptrdiff_t count = hi - index + 1;
        for (std::ptrdiff_t n = 0; n < count; ++n) {
            l[n] = static_cast<T>(source[n][0]);
            r[n] = static_cast<T>(source[n][1]);
            differ |= source[n][0] ^ source[n][1];
        }
    }
    return differ == 0;
}

/// Gathers the input in chunks and calls emit(k, base, fraction, left, right, mono) per output,
/// where left/right[base] holds virtual index i - back.
template <typename T, typename Emit>
void ForEachOutput(const OldStream& old, u64 start, u64 step, std::size_t count,
                   std::ptrdiff_t back, Emit&& emit) {
    alignas(16) T left[kChunk];
    alignas(16) T right[kChunk];
    std::size_t k = 0;
    while (k < count) {
        const std::ptrdiff_t lo = static_cast<std::ptrdiff_t>((start + k * step) >> 24) - back;
        // Outputs j with (start + j * step) >> 24 <= lo + kChunk - 1 - kForward fit in this chunk.
        std::size_t end = count;
        if (step != 0) {
            const u64 limit =
                (static_cast<u64>(lo + kChunk - kForward) << 24) - start; // Exclusive.
            end = std::min<std::size_t>(count, static_cast<std::size_t>((limit + step - 1) / step));
        }
        const std::ptrdiff_t hi =
            static_cast<std::ptrdiff_t>((start + (end - 1) * step) >> 24) + kForward;
        const bool mono = Gather(old, lo, hi, left, right);
        u64 position = start + k * step;
        for (; k < end; ++k, position += step) {
            emit(k, static_cast<std::ptrdiff_t>(position >> 24) - back - lo,
                 position & scale_mask, left, right, mono);
        }
    }
}

/// Exact 32-bit dot product of Q15 coefficients and samples. Integer addition is associative and
/// cannot overflow here (L1 norm checked when the tables are built), so the NEON and portable
/// versions return identical values.
template <std::size_t Taps>
inline s32 Dot(const s16* __restrict coefficients, const s16* __restrict samples) {
    static_assert(Taps % 4 == 0);
#if defined(__aarch64__) && defined(__ARM_NEON)
    int32x4_t sum = vdupq_n_s32(0);
    std::size_t tap = 0;
    for (; tap + 8 <= Taps; tap += 8) {
        const int16x8_t c = vld1q_s16(coefficients + tap);
        const int16x8_t x = vld1q_s16(samples + tap);
        sum = vmlal_s16(sum, vget_low_s16(c), vget_low_s16(x));
        sum = vmlal_high_s16(sum, c, x);
    }
    if constexpr (Taps % 8 != 0) {
        sum = vmlal_s16(sum, vld1_s16(coefficients + tap), vld1_s16(samples + tap));
    }
    return vaddvq_s32(sum);
#else
    s32 sum = 0;
    for (std::size_t tap = 0; tap < Taps; ++tap) {
        sum += static_cast<s32>(coefficients[tap]) * static_cast<s32>(samples[tap]);
    }
    return sum;
#endif
}

template <std::size_t Taps>
void RenderSinc(const SincTable& table, const OldStream& old, u64 start, u64 step,
                std::size_t count, StereoFrame16& output, std::size_t first) {
    const s16* const rows = table.rows.data();
    ForEachOutput<s16>(old, start, step, count, table.back,
                       [&](std::size_t k, std::ptrdiff_t base, u64 fraction, const s16* left,
                           const s16* right, bool mono) {
                           // Rows `phase` and `phase + 1` (row kPhases is the next sample's
                           // phase 0), blended by the remaining fraction bits.
                           const std::size_t phase =
                               static_cast<std::size_t>(fraction >> kPhaseShift);
                           const s64 blend = static_cast<s64>(
                               (fraction & ((u64{1} << kPhaseShift) - 1)) >> kBlendDrop);
                           const s16* const row0 = rows + phase * Taps;
                           const s16* const row1 = row0 + Taps;
                           const auto sample = [&](const s16* x) {
                               const s32 a = Dot<Taps>(row0, x);
                               const s32 b = Dot<Taps>(row1, x);
                               return RoundQ15(a + static_cast<s32>(
                                                       ((static_cast<s64>(b) - a) * blend) >>
                                                       kBlendBits));
                           };
                           const s16 l = sample(left + base);
                           output[first + k] = {l, mono ? l : sample(right + base)};
                       });
}

inline float CatmullRom(const float* x, float t) {
    return x[1] + 0.5f * t *
                      (x[2] - x[0] +
                       t * (2.0f * x[0] - 5.0f * x[1] + 4.0f * x[2] - x[3] +
                            t * (3.0f * (x[1] - x[2]) + x[3] - x[0])));
}

void RenderCubic(const OldStream& old, u64 start, u64 step, std::size_t count,
                 StereoFrame16& output, std::size_t first) {
    ForEachOutput<float>(old, start, step, count, 1,
                         [&](std::size_t k, std::ptrdiff_t base, u64 fraction, const float* left,
                             const float* right, bool mono) {
                             const float t = static_cast<float>(fraction) * (1.0f / scale_factor);
                             const s16 l = ToSample(CatmullRom(left + base, t));
                             output[first + k] = {
                                 l, mono ? l : ToSample(CatmullRom(right + base, t))};
                         });
}

void Render(Quality quality, const OldStream& old, u64 start, u64 step, std::size_t count,
            float rate, StereoFrame16& output, std::size_t first) {
    if (quality == Quality::Cubic) {
        RenderCubic(old, start, step, count, output, first);
        return;
    }
    const SincTable& table = TableFor(rate);
    switch (table.taps) {
    case 16: RenderSinc<16>(table, old, start, step, count, output, first); break;
    case 20: RenderSinc<20>(table, old, start, step, count, output, first); break;
    case 24: RenderSinc<24>(table, old, start, step, count, output, first); break;
    default: RenderSinc<32>(table, old, start, step, count, output, first); break;
    }
}

std::atomic<int> g_override{-1};

Quality ReadConfiguredQuality() {
#if defined(__SWITCH__)
    constexpr const char* source = "sdmc:/switch/oot3dre/config/audio_interpolation";
    std::ifstream file(source);
    if (!file) return Quality::Original;
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
#else
    constexpr const char* source = "TRIAEVUM_AUDIO_INTERPOLATION";
    const char* value = std::getenv(source);
    if (value == nullptr) return Quality::Original;
    const std::string text(value);
#endif
    const auto parsed = ParseQuality(text);
    if (!parsed) {
        std::fprintf(stderr, "audio: ignoring invalid %s (use original, cubic or sinc)\n",
                     source);
        return Quality::Original;
    }
    if (*parsed != Quality::Original) {
        std::fprintf(stderr, "audio: per-source interpolation = %s\n", QualityName(*parsed));
    }
    if (*parsed == Quality::Sinc) {
        TableFor(1.0f); // Build the tables once, here, rather than inside a later frame.
    }
    return *parsed;
}

} // namespace

std::optional<Quality> ParseQuality(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    std::string lower(text);
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.empty() || lower == "original") return Quality::Original;
    if (lower == "cubic") return Quality::Cubic;
    if (lower == "sinc") return Quality::Sinc;
    return std::nullopt;
}

const char* QualityName(Quality quality) {
    switch (quality) {
    case Quality::Cubic: return "cubic";
    case Quality::Sinc: return "sinc";
    default: return "original";
    }
}

Quality ActiveQuality() {
    const int forced = g_override.load(std::memory_order_relaxed);
    if (forced >= 0) return static_cast<Quality>(forced);
    static const Quality configured = ReadConfiguredQuality();
    return configured;
}

void OverrideQuality(std::optional<Quality> quality) {
    g_override.store(quality ? static_cast<int>(*quality) : -1, std::memory_order_relaxed);
}

void HighQuality(Quality quality, bool zero_order, State& state, StereoBuffer16& input, float rate,
                 StereoFrame16& output, std::size_t& outputi) {
    ASSERT(rate > 0);

    if (input.empty())
        return;

    // Stepping identical to StepOverSamples: same expressions, same exit conditions.
    const u64 step_size = static_cast<u64>(rate * scale_factor);
    const u64 start = state.fposition;
    const std::size_t first = outputi;
    u64 fposition = start;
    std::size_t inputi = 0;

    while (outputi < output.size()) {
        inputi = static_cast<std::size_t>(fposition / scale_factor);

        if (inputi >= input.size()) {
            inputi = input.size();
            break;
        }

        ++outputi;
        fposition += step_size;
    }

    const OldStream old{state, input.Remaining().data(), input.size()};
    const std::size_t count = outputi - first;
    if (zero_order || quality == Quality::Original) {
        for (std::size_t k = 0; k < count; ++k) {
            output[first + k] =
                old.At(static_cast<std::ptrdiff_t>((start + k * step_size) >> 24));
        }
    } else if (count != 0) {
        Render(quality, old, start, step_size, count, rate, output, first);
    }

    // Advance the virtual stream by inputi samples, history included.
    decltype(state.history) history;
    for (std::size_t h = 0; h < kHistorySamples; ++h) {
        history[h] = old.At(static_cast<std::ptrdiff_t>(inputi) -
                            static_cast<std::ptrdiff_t>(kHistorySamples) +
                            static_cast<std::ptrdiff_t>(h));
    }
    const auto xn2 = old.At(static_cast<std::ptrdiff_t>(inputi));
    const auto xn1 = old.At(static_cast<std::ptrdiff_t>(inputi) + 1);
    state.history = history;
    state.xn2 = xn2;
    state.xn1 = xn1;
    state.fposition = fposition - inputi * scale_factor;

    input.Consume(inputi);
}

} // namespace AudioCore::AudioInterp
