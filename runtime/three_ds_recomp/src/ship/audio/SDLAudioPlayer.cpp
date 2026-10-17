#include "ship/audio/SDLAudioPlayer.h"
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <utility>

namespace Ship {
namespace {

void PutLe16(std::vector<uint8_t>& bytes, uint16_t value) {
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
}

void PutLe32(std::vector<uint8_t>& bytes, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<uint8_t>(value >> shift));
    }
}

// Little-endian PCM16 RIFF/WAVE file.
void WritePcmWave(const std::string& path, const std::vector<int16_t>& samples, int32_t sampleRate,
                  int32_t channels) {
    std::error_code directoryError;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), directoryError);
    const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
    std::vector<uint8_t> header;
    header.reserve(44);
    header.insert(header.end(), {'R', 'I', 'F', 'F'});
    PutLe32(header, 36U + dataBytes);
    header.insert(header.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    PutLe32(header, 16U);
    PutLe16(header, 1U); // PCM
    PutLe16(header, static_cast<uint16_t>(channels));
    PutLe32(header, static_cast<uint32_t>(sampleRate));
    PutLe32(header, static_cast<uint32_t>(sampleRate) * static_cast<uint32_t>(channels) * 2U);
    PutLe16(header, static_cast<uint16_t>(channels * 2));
    PutLe16(header, 16U);
    header.insert(header.end(), {'d', 'a', 't', 'a'});
    PutLe32(header, dataBytes);

    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        SPDLOG_ERROR("audio dump: cannot create {}", path);
        return;
    }
    bool ok = std::fwrite(header.data(), 1, header.size(), file) == header.size();
    for (size_t offset = 0; ok && offset < samples.size();) {
        // Little-endian host (x86/AArch64): the samples are already LE.
        const size_t count = std::min<size_t>(samples.size() - offset, 64U * 1024U);
        ok = std::fwrite(samples.data() + offset, sizeof(int16_t), count, file) == count;
        offset += count;
    }
    ok = std::fclose(file) == 0 && ok;
    if (ok) {
        SPDLOG_INFO("audio dump: wrote {} frames at {} Hz to {}", samples.size() / std::max(channels, 1), sampleRate,
                    path);
    } else {
        SPDLOG_ERROR("audio dump: write to {} failed", path);
    }
}

} // namespace

SDLAudioPlayer::~SDLAudioPlayer() {
    SPDLOG_TRACE("destruct SDL audio player");
    DoClose();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    if (mCaptureActive && !mCapture.empty()) {
        // Session ended before the capture filled: write what was recorded.
        WritePcmWave(mCapturePath, mCapture, GetSampleRate(), mNumChannels);
    }
}

void SDLAudioPlayer::DoClose() {
    if (mDevice != 0) {
        // Pause playback first
        SDL_PauseAudioDevice(mDevice, 1);
        // Clear any queued audio to prevent glitches when reopening
        SDL_ClearQueuedAudio(mDevice);
        SDL_CloseAudioDevice(mDevice);
        mDevice = 0;
    }
}

bool SDLAudioPlayer::DoInit() {
    // Only used by SDL when it converts rates through libsamplerate. On the
    // Switch nothing is converted by SDL: its audren backend plays the device
    // rate through the voice's own sample-rate converter.
    SDL_SetHint(SDL_HINT_AUDIO_RESAMPLING_MODE, "high");
    if (SDL_Init(SDL_INIT_AUDIO) != 0) {
        SPDLOG_ERROR("SDL init error: {}", SDL_GetError());
        return false;
    }

    // Always open with the correct number of output channels
    mNumChannels = this->GetNumOutputChannels();

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = this->GetSampleRate();
    want.format = AUDIO_S16SYS;
    want.channels = mNumChannels;
    want.samples = this->GetSampleLength();
    want.callback = NULL;

    mDevice = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (mDevice == 0) {
        SPDLOG_ERROR("SDL_OpenAudio error: {}", SDL_GetError());
        return false;
    }

    SPDLOG_INFO("SDL Audio initialized: {} channels, {} Hz, {} frames per device buffer (device: {} Hz, {} frames)",
                mNumChannels, this->GetSampleRate(), want.samples, have.freq, have.samples);

#if defined(__SWITCH__)
    // Start only once the queue holds the prebuffer (see DoPlay).
    mPrebuffering = true;
    SDL_PauseAudioDevice(mDevice, 1);
#else
    SDL_PauseAudioDevice(mDevice, 0);
#endif
    return true;
}

int SDLAudioPlayer::Buffered() {
    return SDL_GetQueuedAudioSize(mDevice) / (sizeof(int16_t) * mNumChannels);
}

uint64_t SDLAudioPlayer::OutputUnderruns() const {
    return mOutputUnderruns;
}

bool SDLAudioPlayer::StartPcmCapture(const std::string& path, uint32_t maximumSeconds) {
    if (mCaptureActive || maximumSeconds == 0 || GetSampleRate() <= 0) {
        return false;
    }
    mCapturePath = path;
    mCaptureLimitSamples = static_cast<size_t>(GetSampleRate()) * maximumSeconds * static_cast<size_t>(mNumChannels);
    mCapture.clear();
    mCapture.reserve(mCaptureLimitSamples);
    mCaptureActive = true;
    SPDLOG_INFO("audio dump: recording {} s of output PCM to {}", maximumSeconds, path);
    return true;
}

void SDLAudioPlayer::CapturePcm(const uint8_t* buf, size_t len) {
    if (!mCaptureActive) {
        return;
    }
    const auto* samples = reinterpret_cast<const int16_t*>(buf);
    const size_t count = std::min(len / sizeof(int16_t), mCaptureLimitSamples - mCapture.size());
    mCapture.insert(mCapture.end(), samples, samples + count);
    if (mCapture.size() < mCaptureLimitSamples) {
        return;
    }
    // Full: the file is written off the game thread.
    mCaptureActive = false;
    std::thread([path = mCapturePath, samples = std::move(mCapture), rate = GetSampleRate(),
                 channels = mNumChannels]() { WritePcmWave(path, samples, rate, channels); })
        .detach();
    mCapture = {};
}

void SDLAudioPlayer::DoPlay(const uint8_t* buf, size_t len) {
#if defined(__SWITCH__)
    // The recompiled game delivers audio in bursts (two guest refreshes after
    // a long frame); dropping a whole chunk at 6000 queued samples was heard
    // as crackling on the console. Allow ~0.5 s before dropping.
    constexpr int kMaximumQueuedSamples = 16384;
    const int buffered = Buffered();
    if (!mPrebuffering && buffered == 0) {
        // The device drained the queue and SDL padded its buffer with
        // silence. Pause (SDL then feeds the voice clean silence) and refill
        // the prebuffer once instead of stuttering on every short refill.
        ++mOutputUnderruns;
        mPrebuffering = true;
        SDL_PauseAudioDevice(mDevice, 1);
        SPDLOG_WARN("SDL audio queue underrun #{}; re-priming", mOutputUnderruns);
    }
#else
    constexpr int kMaximumQueuedSamples = 6000;
    const int buffered = Buffered();
#endif
    if (buffered < kMaximumQueuedSamples) {
        // Don't fill the audio buffer too much in case this happens
        SDL_QueueAudio(mDevice, buf, len);
        CapturePcm(buf, len);
    }
#if defined(__SWITCH__)
    if (mPrebuffering) {
        const int prebuffer = std::max(GetDesiredBuffered(), GetSampleLength() * 2);
        if (Buffered() >= prebuffer) {
            mPrebuffering = false;
            SDL_PauseAudioDevice(mDevice, 0);
        }
    }
#endif
}


void SDLAudioPlayer::DoFlush() {
    if (mDevice != 0) {
        SDL_ClearQueuedAudio(mDevice);
#if defined(__SWITCH__)
        mPrebuffering = true;
        SDL_PauseAudioDevice(mDevice, 1);
#endif
    }
}
} // namespace Ship
