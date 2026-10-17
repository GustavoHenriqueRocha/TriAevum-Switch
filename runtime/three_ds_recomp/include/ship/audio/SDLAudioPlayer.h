#pragma once
#include "AudioPlayer.h"
#include <SDL2/SDL.h>

#include <string>
#include <vector>

namespace Ship {
/**
 * @brief AudioPlayer implementation backed by SDL2's audio subsystem.
 *
 * SDLAudioPlayer uses `SDL_OpenAudioDevice` to open a platform audio output, then
 * pushes interleaved PCM samples in DoPlay(). It supports both stereo and 6-channel
 * (5.1) output depending on the AudioChannelsSetting configured in AudioPlayer.
 *
 * This backend is available on all platforms that Ship supports.
 */
class SDLAudioPlayer final : public AudioPlayer {
  public:
    /**
     * @brief Constructs an SDLAudioPlayer with the given audio settings.
     * @param settings Sample rate, buffer size, desired buffered frames, and channel mode.
     */
    SDLAudioPlayer(AudioSettings settings) : AudioPlayer(settings) {
    }
    ~SDLAudioPlayer();

    /**
     * @brief Returns the number of audio frames currently queued in the SDL audio device.
     *
     * Used by the audio subsystem to decide how many frames to produce per game tick.
     */
    int Buffered() override;

    /** @brief Queue underruns recovered by re-priming (Switch only; 0 elsewhere). */
    uint64_t OutputUnderruns() const override;

    /**
     * @brief Records the first @p maximumSeconds of PCM exactly as queued to SDL
     *        and writes it as a PCM16 WAV file (off the game thread once full,
     *        or at destruction if the session ends first).
     */
    bool StartPcmCapture(const std::string& path, uint32_t maximumSeconds);

  protected:
    /**
     * @brief Opens the SDL audio device with the configured settings.
     * @return true if the device was opened successfully.
     */
    bool DoInit() override;

    /**
     * @brief Closes the SDL audio device and releases its resources.
     */
    void DoClose() override;

    /**
     * @brief Queues interleaved PCM samples to the SDL audio device.
     * @param buf Interleaved sample data (stereo: L,R,… or surround: FL,FR,C,LFE,SL,SR,…).
     * @param len Length of @p buf in bytes.
     */
    void DoPlay(const uint8_t* buf, size_t len) override;
    void DoFlush() override;

  private:
    void CapturePcm(const uint8_t* buf, size_t len);

    SDL_AudioDeviceID mDevice = 0; ///< Handle to the opened SDL audio device.
    int32_t mNumChannels = 2;      ///< Number of output channels (2 for stereo, 6 for 5.1).
    bool mPrebuffering = false;    ///< Device paused until the queue holds the prebuffer (Switch).
    uint64_t mOutputUnderruns = 0; ///< Queue underruns recovered by re-priming (Switch).
    bool mCaptureActive = false;
    std::string mCapturePath;
    size_t mCaptureLimitSamples = 0;
    std::vector<int16_t> mCapture;
};
} // namespace Ship
