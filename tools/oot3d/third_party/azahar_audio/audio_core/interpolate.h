// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include "audio_core/audio_types.h"
#include "audio_core/sample_queue.h"
#include "common/common_types.h"

namespace AudioCore::AudioInterp {

/// A variable length buffer of signed PCM16 stereo samples.
using StereoBuffer16 = DecodedSampleQueue;

/// Number of samples kept in State::history.
constexpr std::size_t kHistorySamples = 32;

struct State {
    /// Two historical samples.
    std::array<s16, 2> xn1 = {}; ///< x[n-1]
    std::array<s16, 2> xn2 = {}; ///< x[n-2]
    /// Current fractional position.
    u64 fposition = 0;
    /// TriAevum: samples preceding xn2 (history[kHistorySamples - 1] is x[n-3]).
    /// Only the optional high-quality interpolators read or write it; the
    /// original None/Linear paths never touch it, so it stays zero there.
    std::array<std::array<s16, 2>, kHistorySamples> history = {};
};

/**
 * TriAevum host option: interpolation quality used for the per-source resampling.
 * Original reproduces Citra/Azahar bit for bit. Cubic and Sinc only change sample values: the
 * stepping (positions, consumed samples, xn1/xn2, fposition) is the original one.
 */
enum class Quality : u8 { Original = 0, Cubic = 1, Sinc = 2 };

/// Parses "original", "cubic" or "sinc" (case-insensitive, surrounding blanks ignored).
std::optional<Quality> ParseQuality(std::string_view text);
const char* QualityName(Quality quality);

/// The configured quality: sdmc:/switch/oot3dre/config/audio_interpolation on the Switch, the
/// TRIAEVUM_AUDIO_INTERPOLATION environment variable elsewhere. Read once; absent = Original.
Quality ActiveQuality();
/// Replaces the configured quality (tests and benchmarks). std::nullopt restores the configuration.
void OverrideQuality(std::optional<Quality> quality);

/**
 * High-quality counterpart of None/Linear. Steps over the input exactly as the original functions
 * (same outputs written, samples consumed, xn1/xn2 and fposition) and additionally maintains
 * State::history. With zero_order the output equals None's; otherwise it is the band-limited
 * (Sinc) or Catmull-Rom (Cubic) interpolation of the stream.
 * The sinc kernel (16 taps when upsampling, widened up to kSincMaxTaps when decimating) only reads
 * up to the lookahead the original stepping guarantees: it adds a constant delay of kSincDelay
 * input samples (0.43 ms at 32728 Hz, independent of pitch changes); cubic adds none.
 */
void HighQuality(Quality quality, bool zero_order, State& state, StereoBuffer16& input, float rate,
                 StereoFrame16& output, std::size_t& outputi);

constexpr std::size_t kSincMaxTaps = 32;
constexpr std::size_t kSincDelay = kSincMaxTaps / 2 - 2;

/**
 * No interpolation. This is equivalent to a zero-order hold. There is a two-sample predelay.
 * @param state Interpolation state.
 * @param input Input buffer.
 * @param rate Stretch factor. Must be a positive non-zero value.
 *             rate > 1.0 performs decimation and rate < 1.0 performs upsampling.
 * @param output The resampled audio buffer.
 * @param outputi The index of output to start writing to.
 */
void None(State& state, StereoBuffer16& input, float rate, StereoFrame16& output,
          std::size_t& outputi);

/**
 * Linear interpolation. This is equivalent to a first-order hold. There is a two-sample predelay.
 * @param state Interpolation state.
 * @param input Input buffer.
 * @param rate Stretch factor. Must be a positive non-zero value.
 *             rate > 1.0 performs decimation and rate < 1.0 performs upsampling.
 * @param output The resampled audio buffer.
 * @param outputi The index of output to start writing to.
 */
void Linear(State& state, StereoBuffer16& input, float rate, StereoFrame16& output,
            std::size_t& outputi);

} // namespace AudioCore::AudioInterp
