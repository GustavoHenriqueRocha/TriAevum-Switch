#include "ctr_dsp_audio_frames.h"

#include "oot3d_native_a32_ctr_host.h"

namespace Oot3dNativeGame {

uint32_t DeliverPendingDspAudioFrames(NativeA32CtrHostServices& host,
                                      uint32_t frames,
                                      const std::function<void()>& mixFrame,
                                      const std::function<bool()>& runGuest) {
    uint32_t delivered = 0;
    while (delivered < frames) {
        mixFrame();
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
