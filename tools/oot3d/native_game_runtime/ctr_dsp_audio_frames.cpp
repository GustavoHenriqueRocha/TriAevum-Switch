#include "ctr_dsp_audio_frames.h"

#include "oot3d_native_a32_ctr_host.h"
#include "oot3d_native_a32_dsp_hle.h"

#include <vector>

namespace Oot3dNativeGame {

std::optional<uint32_t> DeliverPendingDspAudioFrames(
    NativeA32CtrHostServices& host, NativeA32DspHle& dspHle,
    NativeA32Memory& memory, const std::function<bool()>& runGuest,
    std::string* error) {
    const uint32_t frames = host.TakePendingDspAudioFrames();
    std::vector<int16_t> samples;
    uint32_t delivered = 0;
    while (delivered < frames) {
        samples.clear();
        if (!dspHle.ProcessFrame(memory, samples, error)) {
            return std::nullopt;
        }
        if (!host.SignalDspAudioFrame()) {
            break;
        }
        ++delivered;
        if (!runGuest()) {
            break;
        }
    }
    return delivered;
}

} // namespace Oot3dNativeGame
