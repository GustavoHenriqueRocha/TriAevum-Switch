#pragma once
// deko3d shaders (Switch): precompiled DKSH from shadercache/deko3d.pack
// (tools/oot3d/deko3d_shader_pack.py), loaded into code memory at first use.
// Nothing is compiled on the render thread: a stage missing from the pack
// makes the program unavailable (the draw then uses the parametric uber
// fragment program, or is skipped and counted), and its GLSL text is saved to
// logs/deko3d_shaders/{vert,frag}_<key>.glsl to be packed offline.

#if defined(__SWITCH__)
#include "fast/backends/gfx_deko3d_shader_pack.h"
#include "gfx_deko3d_device.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace Fast::Deko3d {

struct ShaderStoreStats {
    uint64_t PackEntries = 0;
    uint64_t Loaded = 0;
    uint64_t Missing = 0;      // distinct texts not in the pack
    uint64_t CodeBytes = 0;
    uint64_t LoadFailures = 0;
};

class ShaderStore {
  public:
    static constexpr uint32_t kCodeBytes = 32U << 20;

    bool Init(Device& device);
    void Shutdown() noexcept;

    // The shader of a stage text, or nullptr when the pack does not have it
    // (recorded once for offline compilation).
    const DkShader* Find(ShaderStage stage, std::string_view text);
    // An already computed key (internal shaders embedded in the binary).
    const DkShader* FindKey(ShaderStage stage, uint64_t key, std::string_view textForRecord);

    // A DKSH built into the binary (the scanout), loaded once under `key`.
    const DkShader* Embedded(uint64_t key, std::span<const uint8_t> dksh);

    // True once after new code was written: the caller issues
    // dkCmdBufBarrier(DkBarrier_None, DkInvalidateFlags_Shader) before the
    // next draw that might use it.
    bool TakeCodeWritten() noexcept {
        const bool written = mCodeWritten;
        mCodeWritten = false;
        return written;
    }

    [[nodiscard]] const ShaderStoreStats& Stats() const noexcept { return mStats; }

  private:
    const DkShader* Load(uint64_t key, std::span<const uint8_t> dksh);
    void RecordMissing(ShaderStage stage, uint64_t key, std::string_view text);

    DkMemBlock mCode = nullptr;
    uint32_t mCodeUsed = 0;
    bool mCodeWritten = false;
    ShaderPackIndex mPack;
    std::unordered_map<uint64_t, std::unique_ptr<DkShader>> mLoaded;
    std::unordered_set<uint64_t> mMissing;
    ShaderStoreStats mStats{};
};

} // namespace Fast::Deko3d
#endif
