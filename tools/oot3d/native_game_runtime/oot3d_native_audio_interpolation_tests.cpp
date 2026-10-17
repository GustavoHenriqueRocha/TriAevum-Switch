// Optional per-source interpolation (config/audio_interpolation): equivalence, quality and cost.
//
//   oot3d_native_audio_interpolation_tests            run every check
//   oot3d_native_audio_interpolation_tests --bench    also time 24 voices per DSP frame
//
// Built with -DTRIAEVUM_INTERPOLATION_REFERENCE against the pre-option audio_core sources it only
// prints the original-mode digest, which is how kOriginalDigest below was obtained.
#include "audio_core/hle/source.h"
#include "audio_core/interpolate.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace AudioCore;
using namespace AudioCore::HLE;
using Configuration = SourceConfiguration::Configuration;
using Sample = StereoBuffer16::value_type;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr PAddr kBase = 0x20000000U;
constexpr std::size_t kMemoryBytes = 1U << 20U;

struct Digest {
    u64 value = 14695981039346656037ULL;
    void Add(u64 data, unsigned bytes) {
        for (unsigned i = 0; i < bytes; ++i) value = (value ^ ((data >> (8 * i)) & 0xFF)) * 1099511628211ULL;
    }
};

// What the guest observes from one source each frame.
struct Observed {
    u8 enabled, dirty;
    u16 sync;
    u32 position;
    u16 current, last;
    bool operator==(const Observed&) const = default;
};

Observed Observe(const SourceStatus::Status& status) {
    return {status.is_enabled, status.current_buffer_id_dirty, static_cast<u16>(status.sync_count),
            static_cast<u32>(status.buffer_position), static_cast<u16>(status.current_buffer_id),
            static_cast<u16>(status.last_buffer_id)};
}

struct RunResult {
    std::vector<Observed> statuses;
    std::vector<StereoFrame16> frames;
    std::vector<nlohmann::json> stepping; // Unread samples, xn1/xn2, position: no frame values.
    u64 digest = 0;
};

// A random title: queued and embedded buffers in all formats, loops, play positions, rate and
// interpolation changes, resets, sleep/wakeup and save/restore round trips.
RunResult RunSequence(u32 seed, unsigned ticks, bool keepStepping) {
    std::mt19937 random(seed);
    std::vector<u8> memory(kMemoryBytes);
    for (auto& byte : memory) byte = static_cast<u8>(random());
    // Smooth PCM16 regions so the interpolators see audio-like and full-scale data alike.
    for (std::size_t i = 0; i < kMemoryBytes / 4; i += 2) {
        const s16 value = static_cast<s16>(20000.0 * std::sin(i * 0.013) * std::cos(i * 0.0007));
        std::memcpy(memory.data() + i, &value, 2);
    }
    Source source(0);
    source.SetMemoryResolver([&](PAddr address, std::size_t bytes) -> const u8* {
        if (address < kBase || address - kBase + bytes > kMemoryBytes) return nullptr;
        return memory.data() + (address - kBase);
    });
    Configuration config{};
    config.format.Assign(Configuration::Format::PCM16); // Titles never leave these zero.
    config.mono_or_stereo.Assign(Configuration::MonoOrStereo::Mono);
    s16_le coefficients[16];
    for (auto& coefficient : coefficients) coefficient = static_cast<s16>(random() % 4096) - 2048;
    const float rates[] = {0.0625f, 0.25f, 0.5f, 0.7213f, 1.0f, 1.0001f, 1.3333f, 2.0f, 2.97f, 4.5f, 9.0f};
    u16 nextId = 1;
    RunResult result;
    Digest digest;
    auto pickBuffer = [&](u32& address, u32& length) {
        address = kBase + static_cast<u32>(random() % (kMemoryBytes / 2));
        const unsigned kind = random() % 4;
        length = kind == 0 ? random() % 4 : kind == 1 ? random() % 200 : random() % 4000 + 1;
    };
    for (unsigned tick = 0; tick < ticks; ++tick) {
        const unsigned action = random() % 100;
        if (action < 4) {
            config.reset_flag.Assign(1);
        } else if (action < 6) {
            config.partial_reset_flag.Assign(1);
        }
        if (random() % 8 == 0) {
            config.enable = random() % 6 != 0;
            config.enable_dirty.Assign(1);
        }
        if (random() % 6 == 0) {
            config.rate_multiplier = rates[random() % std::size(rates)];
            config.rate_multiplier_dirty.Assign(1);
        }
        if (random() % 10 == 0) {
            config.interpolation_mode = static_cast<Configuration::InterpolationMode>(random() % 3);
            config.interpolation_dirty.Assign(1);
        }
        if (random() % 15 == 0) {
            const auto format = static_cast<Configuration::Format>(random() % 3);
            config.format.Assign(format);
            config.mono_or_stereo.Assign(format == Configuration::Format::ADPCM || random() % 2
                                             ? Configuration::MonoOrStereo::Mono
                                             : Configuration::MonoOrStereo::Stereo);
            config.format_dirty.Assign(1);
            config.mono_or_stereo_dirty.Assign(1);
        }
        if (random() % 20 == 0) config.adpcm_coefficients_dirty.Assign(1);
        if (random() % 9 == 0) {
            u32 address, length;
            pickBuffer(address, length);
            config.physical_address = address;
            config.length = length;
            config.buffer_id = nextId++;
            // A zero-length looping buffer spins the original DSP loop forever: never generated.
            config.is_looping.Assign(length != 0 && random() % 3 == 0);
            config.adpcm_dirty.Assign(random() % 2);
            config.adpcm_ps = static_cast<u16>(random() & 0x7F);
            config.adpcm_yn[0] = static_cast<u16>(random());
            config.adpcm_yn[1] = static_cast<u16>(random());
            config.embedded_buffer_dirty.Assign(1);
            if (random() % 2) {
                config.play_position = length ? random() % length : 0;
                config.play_position_dirty.Assign(1);
            }
        }
        if (random() % 4 == 0) {
            config.buffers_dirty = 0;
            for (unsigned i = 0; i < 4; ++i) {
                if (random() % 3) continue;
                auto& buffer = config.buffers[i];
                u32 address, length;
                pickBuffer(address, length);
                buffer.physical_address = address;
                buffer.length = length;
                buffer.buffer_id = nextId++;
                buffer.is_looping = length != 0 && random() % 5 == 0;
                buffer.adpcm_dirty = random() % 2;
                buffer.adpcm_ps = static_cast<u16>(random() & 0x7F);
                config.buffers_dirty = config.buffers_dirty | (1U << i);
            }
            if (config.buffers_dirty) config.buffer_queue_dirty.Assign(1);
        }
        if (random() % 25 == 0) {
            config.sync_count = static_cast<u16>(random());
            config.sync_count_dirty.Assign(1);
        }
        if (random() % 30 == 0) {
            config.gain[0][0] = 1.0f;
            config.gain_0_dirty.Assign(1);
        }

        const Observed observed = Observe(source.Tick(config, coefficients));
        const auto state = source.CaptureState();
        const auto frame = state.at("current_frame").get<StereoFrame16>();
        result.statuses.push_back(observed);
        result.frames.push_back(frame);
        digest.Add(observed.enabled, 1);
        digest.Add(observed.dirty, 1);
        digest.Add(observed.sync, 2);
        digest.Add(observed.position, 4);
        digest.Add(observed.current, 2);
        digest.Add(observed.last, 2);
        for (const auto& sample : frame) {
            digest.Add(static_cast<u16>(sample[0]), 2);
            digest.Add(static_cast<u16>(sample[1]), 2);
        }
        if (keepStepping) {
            auto stepping = state.at("state");
            auto& interpolation = stepping.at("interpolation_state");
            interpolation.erase("history");
            stepping.erase("filters"); // Filter memory follows the sample values.
            result.stepping.push_back(std::move(stepping));
        }

        const unsigned host = random() % 50;
        if (host == 0) {
            source.Sleep();
            source.Wakeup();
        } else if (host == 1) {
            std::string error;
            Source restored(0);
            Require(restored.RestoreState(nlohmann::json::from_msgpack(nlohmann::json::to_msgpack(state)), &error),
                    "source restore: " + error);
            Require(restored.CaptureState() == state, "source save/restore round trip differs");
            Require(source.RestoreState(state, &error), "source restore: " + error);
        }
    }
    result.digest = digest.value;
    return result;
}

constexpr u32 kSeeds = 24;
constexpr unsigned kTicks = 1500;

u64 OriginalDigest() {
    Digest all;
    for (u32 seed = 1; seed <= kSeeds; ++seed) all.Add(RunSequence(seed, kTicks, false).digest, 8);
    return all.value;
}

#if !defined(TRIAEVUM_INTERPOLATION_REFERENCE)
using AudioInterp::Quality;

// Digest of OriginalDigest() produced by the audio_core sources before the option existed
// (topscreen-3.0, built with -DTRIAEVUM_INTERPOLATION_REFERENCE).
constexpr u64 kOriginalDigest = 0x26605ac4f22ad8d8ULL;

void OriginalModeIsUnchanged() {
    AudioInterp::OverrideQuality(Quality::Original);
    const u64 digest = OriginalDigest();
    char text[64];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(digest));
    Require(digest == kOriginalDigest, std::string("original mode digest changed: ") + text);
    std::cout << "original: " << kSeeds << " random sources x " << kTicks
              << " frames bit-identical to the previous code (digest " << text << ")\n";
}

void SteppingIsUnchanged() {
    std::size_t audible = 0, changed = 0;
    for (u32 seed = 1; seed <= 8; ++seed) {
        AudioInterp::OverrideQuality(Quality::Original);
        const auto reference = RunSequence(seed, kTicks, true);
        for (Quality quality : {Quality::Cubic, Quality::Sinc}) {
            AudioInterp::OverrideQuality(quality);
            const auto actual = RunSequence(seed, kTicks, true);
            Require(actual.statuses == reference.statuses,
                    std::string(AudioInterp::QualityName(quality)) + ": guest-visible status differs");
            Require(actual.stepping == reference.stepping,
                    std::string(AudioInterp::QualityName(quality)) + ": consumed samples/positions differ");
            for (std::size_t frame = 0; frame < reference.frames.size(); ++frame) {
                const auto& samples = reference.frames[frame];
                audible += std::any_of(samples.begin(), samples.end(),
                                       [](const Sample& sample) { return sample[0] || sample[1]; });
                changed += actual.frames[frame] != samples;
            }
        }
    }
    Require(audible > 1000 && changed > 1000, "random sequences produce too little audio");
    std::cout << "cubic/sinc: status, buffer positions, ids, unread samples, xn1/xn2 and fposition "
                 "identical to original on 8 random sequences (" << audible << " audible frames, "
              << changed << " with different sample values)\n";
}

// ----- signal tests on the resampler itself ------------------------------------------------------

struct Rendered {
    std::vector<float> left;
    std::vector<double> positions; // Virtual-stream position of each output, in input samples.
};

// Plays `signal` (mono) split into buffers of the given lengths, `frames` DSP frames long.
Rendered Play(Quality quality, const std::vector<s16>& signal, const std::vector<std::size_t>& splits,
              float rate, unsigned frames, bool loopWhole = false) {
    AudioInterp::State state{};
    std::vector<StereoBuffer16> buffers;
    std::size_t offset = 0;
    for (std::size_t length : splits) {
        StereoBuffer16 buffer;
        for (std::size_t i = 0; i < length; ++i) buffer.push_back({signal[offset + i], signal[offset + i]});
        offset += length;
        buffers.push_back(std::move(buffer));
    }
    std::size_t next = 0;
    DecodedSampleQueue queue;
    Rendered rendered;
    const u64 step = static_cast<u64>(rate * (1ULL << 24));
    double consumed = -2.0; // Virtual index 2 is the first input sample.
    for (unsigned frame = 0; frame < frames; ++frame) {
        StereoFrame16 output{};
        std::size_t position = 0;
        while (position < output.size()) {
            if (queue.empty()) {
                if (next == buffers.size()) {
                    if (!loopWhole) break;
                    next = 0;
                }
                queue = buffers[next++];
            }
            const std::size_t before = queue.size();
            const u64 fposition = state.fposition;
            const std::size_t first = position;
            if (quality == Quality::Original) {
                AudioInterp::Linear(state, queue, rate, output, position);
            } else {
                AudioInterp::HighQuality(quality, false, state, queue, rate, output, position);
            }
            for (std::size_t k = first; k < position; ++k) {
                rendered.left.push_back(output[k][0]);
                rendered.positions.push_back(consumed + static_cast<double>(fposition + (k - first) * step) / (1 << 24));
            }
            consumed += static_cast<double>(before - queue.size());
        }
        if (position < output.size()) break;
    }
    return rendered;
}

double Delay(Quality quality) {
    return quality == Quality::Sinc ? static_cast<double>(AudioInterp::kSincDelay) : 0.0;
}

// SNR (dB) of a rendered sine against the ideal one, skipping the start-up transient.
double SineSnr(const Rendered& rendered, Quality quality, double cyclesPerSample, double amplitude,
               std::size_t period = 0) {
    const double pi = 3.14159265358979323846;
    double signal = 0.0, noise = 0.0;
    for (std::size_t n = 0; n < rendered.left.size(); ++n) {
        if (rendered.positions[n] < 64.0) continue; // Start-up from the zero history.
        double t = rendered.positions[n] - Delay(quality);
        if (period != 0) t = std::fmod(t, static_cast<double>(period));
        const double ideal = amplitude * std::sin(2.0 * pi * cyclesPerSample * t);
        signal += ideal * ideal;
        noise += (rendered.left[n] - ideal) * (rendered.left[n] - ideal);
    }
    return 10.0 * std::log10(signal / std::max(noise, 1e-9));
}

std::vector<s16> Sine(std::size_t length, double cyclesPerSample, double amplitude) {
    std::vector<s16> samples(length);
    for (std::size_t i = 0; i < length; ++i) {
        samples[i] = static_cast<s16>(std::lround(amplitude * std::sin(2.0 * 3.14159265358979323846 * cyclesPerSample * i)));
    }
    return samples;
}

void QualityTests() {
    constexpr double kAmplitude = 16000.0;
    constexpr double kOutputRate = 32728.0;
    std::printf("SNR em dB (senoide, ideal = sinal continuo; maior e melhor)\n");
    std::printf("%8s %9s %8s %8s %8s\n", "rate", "tom (Hz)", "linear", "cubic", "sinc");
    const float rates[] = {0.25f, 0.5f, 0.7213f, 1.0f, 1.3f, 1.9f, 2.9f};
    for (float rate : rates) {
        const double inputRate = kOutputRate * rate;
        const double nyquist = 0.5 * std::min(inputRate, kOutputRate);
        for (double fraction : {0.1, 0.25, 0.5}) {
            const double hertz = fraction * nyquist;
            const double cycles = hertz / inputRate;
            const auto signal = Sine(static_cast<std::size_t>(rate * 160 * 40) + 64, cycles, kAmplitude);
            double snr[3];
            for (Quality quality : {Quality::Original, Quality::Cubic, Quality::Sinc}) {
                snr[static_cast<int>(quality)] = SineSnr(Play(quality, signal, {signal.size()}, rate, 38), quality, cycles, kAmplitude);
            }
            std::printf("%8.4f %9.0f %8.1f %8.1f %8.1f\n", rate, hertz, snr[0], snr[1], snr[2]);
            // At rate 1 every position is an input sample: all modes reproduce the input.
            if (snr[0] < 90.0) Require(snr[2] > snr[0] + 10.0, "sinc is not clearly better than linear");
            Require(snr[2] >= std::min(snr[0], 75.0), "sinc SNR too low");
            Require(snr[1] >= snr[0] - 0.5, "cubic is worse than linear");
        }
    }

    // Aliasing: a tone above the output Nyquist frequency should vanish when decimating.
    std::printf("Aliasing: tom acima de 16364 Hz com decimacao (residuo em dB re. amplitude)\n");
    for (float rate : {1.6f, 2.5f}) {
        const double inputRate = kOutputRate * rate;
        const double hertz = 0.8 * inputRate / 2.0;
        const double cycles = hertz / inputRate;
        const auto signal = Sine(static_cast<std::size_t>(rate * 160 * 40) + 64, cycles, kAmplitude);
        double level[3];
        for (Quality quality : {Quality::Original, Quality::Cubic, Quality::Sinc}) {
            const auto rendered = Play(quality, signal, {signal.size()}, rate, 38);
            double power = 0.0;
            for (std::size_t n = 64; n < rendered.left.size(); ++n) power += rendered.left[n] * rendered.left[n];
            power /= static_cast<double>(rendered.left.size() - 64);
            level[static_cast<int>(quality)] = 10.0 * std::log10(std::max(power, 1e-9) / (kAmplitude * kAmplitude / 2));
        }
        std::printf("  rate %.2f tom %.0f Hz: linear %.1f  cubic %.1f  sinc %.1f\n", rate, hertz, level[0], level[1], level[2]);
        Require(level[2] < level[0] - 15.0, "sinc does not suppress aliasing");
    }

    // Buffer boundaries: splitting the stream into chained buffers of any size must not change a
    // single output sample (no restart, no click), in every mode.
    std::mt19937 random(77);
    for (float rate : {0.37f, 1.0f, 1.77f, 4.2f}) {
        const auto signal = Sine(static_cast<std::size_t>(rate * 160 * 30) + 64, 0.031, kAmplitude);
        std::vector<std::size_t> splits;
        for (std::size_t left = signal.size(); left != 0;) {
            const std::size_t length = std::min<std::size_t>(left, 1 + random() % (random() % 2 ? 3 : 300));
            splits.push_back(length);
            left -= length;
        }
        for (Quality quality : {Quality::Original, Quality::Cubic, Quality::Sinc}) {
            const auto whole = Play(quality, signal, {signal.size()}, rate, 28);
            const auto pieces = Play(quality, signal, splits, rate, 28);
            Require(whole.left == pieces.left, std::string(AudioInterp::QualityName(quality)) +
                                                   ": buffer boundaries change the output");
        }
    }
    std::printf("Fronteiras de buffer: saida identica com o fluxo partido em buffers de 1..300 amostras\n");

    // Loops: a looping buffer holding whole periods plays as a seamless sine across the loop point.
    for (float rate : {0.6f, 1.0f, 1.45f}) {
        constexpr std::size_t kPeriod = 25, kLoop = kPeriod * 12;
        const auto loop = Sine(kLoop, 1.0 / kPeriod, kAmplitude);
        double snr[3], jump[3];
        for (Quality quality : {Quality::Original, Quality::Cubic, Quality::Sinc}) {
            const auto rendered = Play(quality, loop, {kLoop}, rate, 30, true);
            snr[static_cast<int>(quality)] = SineSnr(rendered, quality, 1.0 / kPeriod, kAmplitude, kLoop);
            // Largest error of any output near a loop point.
            double worst = 0.0;
            for (std::size_t n = 64; n < rendered.left.size(); ++n) {
                const double t = rendered.positions[n] - Delay(quality);
                const double local = std::fmod(t + kLoop, static_cast<double>(kLoop));
                if (local > 10.0 && local < kLoop - 10.0) continue;
                const double ideal = kAmplitude * std::sin(2.0 * 3.14159265358979323846 * t / kPeriod);
                worst = std::max(worst, std::abs(rendered.left[n] - ideal));
            }
            jump[static_cast<int>(quality)] = worst;
        }
        std::printf("Loop rate %.2f: SNR linear %.1f cubic %.1f sinc %.1f dB; maior erro junto ao ponto de loop: "
                    "linear %.0f cubic %.0f sinc %.0f\n",
                    rate, snr[0], snr[1], snr[2], jump[0], jump[1], jump[2]);
        Require(snr[2] > 60.0 && jump[2] < 40.0, "sinc loop junction is not seamless");
        Require(jump[1] <= jump[0] + 1.0, "cubic loop junction worse than linear");
    }

    // Start from silence (reset state): no overshoot beyond the signal's own range.
    {
        std::vector<s16> step(4000, 30000);
        for (Quality quality : {Quality::Cubic, Quality::Sinc}) {
            const auto rendered = Play(quality, step, {step.size()}, 0.77f, 20);
            const float peak = *std::max_element(rendered.left.begin(), rendered.left.end());
            Require(peak <= 32767.0f, "start-up overshoot clipped");
        }
    }
}

void SavestateTests() {
    // The extra history is serialized (only when non-zero) and restored exactly.
    AudioInterp::OverrideQuality(Quality::Sinc);
    Source source(0);
    std::vector<u8> memory(4096);
    for (std::size_t i = 0; i < memory.size(); ++i) memory[i] = static_cast<u8>(i * 37);
    source.SetMemoryResolver([&](PAddr address, std::size_t bytes) -> const u8* {
        return address >= kBase && address - kBase + bytes <= memory.size() ? memory.data() + (address - kBase) : nullptr;
    });
    Configuration config{};
    s16_le coefficients[16]{};
    config.enable = 1;
    config.enable_dirty.Assign(1);
    config.rate_multiplier = 0.83f;
    config.rate_multiplier_dirty.Assign(1);
    config.format.Assign(Configuration::Format::PCM16);
    config.mono_or_stereo.Assign(Configuration::MonoOrStereo::Stereo);
    config.format_dirty.Assign(1);
    config.mono_or_stereo_dirty.Assign(1);
    config.physical_address = kBase;
    config.length = 1024;
    config.is_looping.Assign(1);
    config.embedded_buffer_dirty.Assign(1);
    source.Tick(config, coefficients);
    for (unsigned i = 0; i < 3; ++i) source.Tick(config, coefficients);
    const auto snapshot = source.CaptureState();
    Require(snapshot.at("state").at("interpolation_state").contains("history"), "history not serialized");
    Source restored(0);
    restored.SetMemoryResolver([&](PAddr address, std::size_t bytes) -> const u8* {
        return address >= kBase && address - kBase + bytes <= memory.size() ? memory.data() + (address - kBase) : nullptr;
    });
    std::string error;
    Require(restored.RestoreState(nlohmann::json::from_msgpack(nlohmann::json::to_msgpack(snapshot)), &error), error);
    for (unsigned i = 0; i < 20; ++i) {
        source.Tick(config, coefficients);
        restored.Tick(config, coefficients);
        Require(source.CaptureState() == restored.CaptureState(), "restored sinc playback differs");
    }
    // Snapshots from before the option (no history key) still load; history starts at zero.
    auto legacy = snapshot;
    legacy["state"]["interpolation_state"].erase("history");
    Require(restored.RestoreState(legacy, &error), "legacy snapshot rejected");
    std::cout << "savestate: sinc history serialized, restored playback identical, legacy snapshots load\n";
}

// ----- cost --------------------------------------------------------------------------------------

void Benchmark() {
    // 24 voices as in OoT3D: mostly mono ADPCM, a few stereo PCM16 (music/ambience), varied pitch.
    std::vector<u8> memory(kMemoryBytes);
    std::mt19937 random(9);
    for (auto& byte : memory) byte = static_cast<u8>(random());
    for (std::size_t i = 0; i < kMemoryBytes / 2; i += 2) {
        const s16 value = static_cast<s16>(12000.0 * std::sin(i * 0.02));
        std::memcpy(memory.data() + i, &value, 2);
    }
    const auto resolver = [&](PAddr address, std::size_t bytes) -> const u8* {
        return address >= kBase && address - kBase + bytes <= memory.size() ? memory.data() + (address - kBase) : nullptr;
    };
    constexpr unsigned kVoices = 24, kFrames = 4000;
    // Thread CPU time, best of several interleaved rounds: the build machine is shared.
    const auto run = [&](Quality quality) {
        AudioInterp::OverrideQuality(quality);
        std::vector<Source> sources;
        std::vector<Configuration> configs(kVoices);
        sources.reserve(kVoices);
        s16_le coefficients[16];
        for (unsigned i = 0; i < 16; ++i) coefficients[i] = static_cast<s16>(i * 97 % 2048) - 1024;
        for (unsigned voice = 0; voice < kVoices; ++voice) {
            sources.emplace_back(voice);
            sources.back().SetMemoryResolver(resolver);
            auto& config = configs[voice];
            const bool stereo = voice % 6 == 0;
            config.enable = 1;
            config.enable_dirty.Assign(1);
            config.rate_multiplier = 0.4f + 0.07f * voice; // 0.40 .. 2.01
            config.rate_multiplier_dirty.Assign(1);
            config.format.Assign(stereo ? Configuration::Format::PCM16 : Configuration::Format::ADPCM);
            config.mono_or_stereo.Assign(stereo ? Configuration::MonoOrStereo::Stereo : Configuration::MonoOrStereo::Mono);
            config.format_dirty.Assign(1);
            config.mono_or_stereo_dirty.Assign(1);
            config.adpcm_coefficients_dirty.Assign(1);
            config.physical_address = kBase + (stereo ? 0 : kMemoryBytes / 2) + voice * 64;
            config.length = 14 * 1000;
            config.is_looping.Assign(1);
            config.embedded_buffer_dirty.Assign(1);
        }
        const auto now = [] {
            timespec time{};
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time);
            return static_cast<double>(time.tv_sec) + time.tv_nsec * 1e-9;
        };
        u64 sink = 0;
        const double begin = now();
        for (unsigned frame = 0; frame < kFrames; ++frame) {
            for (unsigned voice = 0; voice < kVoices; ++voice) {
                sink += sources[voice].Tick(configs[voice], coefficients).buffer_position;
            }
        }
        Require(sink != 0, "benchmark produced nothing");
        return (now() - begin) / kFrames * 1e3;
    };
    double best[3] = {1e9, 1e9, 1e9};
    for (unsigned round = 0; round < 7; ++round) {
        for (Quality quality : {Quality::Original, Quality::Cubic, Quality::Sinc}) {
            best[static_cast<int>(quality)] = std::min(best[static_cast<int>(quality)], run(quality));
        }
    }
    for (Quality quality : {Quality::Original, Quality::Cubic, Quality::Sinc}) {
        const double perFrame = best[static_cast<int>(quality)];
        std::printf("bench %-8s 24 vozes: %.4f ms por quadro DSP, %.3f ms por passo de 33,3 ms (6,82 quadros), "
                    "+%.3f ms/passo sobre o original\n",
                    AudioInterp::QualityName(quality), perFrame, perFrame * 6.82, (perFrame - best[0]) * 6.82);
    }
}
#endif
} // namespace

int main(int argc, char** argv) {
    try {
#if defined(TRIAEVUM_INTERPOLATION_REFERENCE)
        (void)argc;
        (void)argv;
        std::printf("%016llx\n", static_cast<unsigned long long>(OriginalDigest()));
#else
        OriginalModeIsUnchanged();
        SteppingIsUnchanged();
        QualityTests();
        SavestateTests();
        if (argc > 1 && std::string(argv[1]) == "--bench") Benchmark();
        AudioInterp::OverrideQuality(std::nullopt);
        std::cout << "audio interpolation: PASS\n";
#endif
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
