// deko3d shaders (Switch). See gfx_deko3d_shaders.h.
#if defined(__SWITCH__)
#include "gfx_deko3d_shaders.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace Fast::Deko3d {
namespace {
constexpr const char* kPackDirectory = "sdmc:/switch/oot3dre/shadercache/";
constexpr const char* kPackConfig = "sdmc:/switch/oot3dre/config/deko3d_pack";
constexpr const char* kMissingDirectory = "sdmc:/switch/oot3dre/logs/deko3d_shaders";

// The whole file in one read (nothing if it cannot be read). From the fork
// author's ShaderStore::Init: TriAevum-Switch commit a7424d3b by Gustavo
// Henrique Rocha ("leitura do pack de uma vez"), licence as the repository
// (LICENSE): a per-byte istreambuf_iterator copy over the SD card delays boot.
std::optional<std::vector<uint8_t>> ReadWholeFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return std::nullopt;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(file.tellg()));
    file.seekg(0);
    if (!bytes.empty() && !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        return std::nullopt;
    }
    return bytes;
}
} // namespace

bool ShaderStore::Init(Device& device) {
    mCode = device.MakeBlock(kCodeBytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached |
                                             DkMemBlockFlags_Code,
                             "shader code");
    if (mCode == nullptr) {
        return false;
    }
    // config/deko3d_pack: another pack of shadercache/ for an A/B (e.g.
    // deko3d_reference.pack, the same stages without the pass-through TEV
    // rewrite). A bad name or a missing file falls back to deko3d.pack.
    std::string name(kDefaultShaderPackName);
    std::optional<std::vector<uint8_t>> bytes;
    {
        std::ifstream config(kPackConfig);
        std::string line;
        if (config && std::getline(config, line)) {
            if (const auto chosen = ShaderPackNameFromConfig(line)) {
                bytes = ReadWholeFile(kPackDirectory + *chosen);
                if (bytes) {
                    name = *chosen;
                } else {
                    Log("shaders: config/deko3d_pack names %s, not found; reading %s", chosen->c_str(), name.c_str());
                }
            } else {
                Log("shaders: config/deko3d_pack \"%s\" is not a pack name in shadercache/; reading %s", line.c_str(),
                    name.c_str());
            }
        }
    }
    const std::string path = kPackDirectory + name;
    if (!bytes) {
        bytes = ReadWholeFile(path);
    }
    if (!bytes) {
        Log("shaders: %s not found (every program will be missing)", path.c_str());
        return true;
    }
    std::string error;
    if (!mPack.Parse(std::move(*bytes), &error)) {
        Log("shaders: %s rejected: %s", path.c_str(), error.c_str());
        return true;
    }
    mStats.PackEntries = mPack.Count();
    const size_t skipped = mPack.CountWithFlag(kShaderPackFlagTevPassthroughSkip);
    Log("shaders: %zu precompiled stages in the pack", mPack.Count());
    Log("shaders: pack %s flags=%u tev_passthrough=%s (%zu stages compiled with pass-through TEV stages skipped)",
        name.c_str(), static_cast<unsigned>(mPack.Flags()),
        (mPack.Flags() & kShaderPackFlagTevPassthroughSkip) != 0U ? "skip" : "keep", skipped);
    return true;
}

void ShaderStore::Shutdown() noexcept {
    mLoaded.clear();
    if (mCode != nullptr) {
        dkMemBlockDestroy(mCode);
        mCode = nullptr;
    }
    mCodeUsed = 0;
}

const DkShader* ShaderStore::Find(ShaderStage stage, std::string_view text) {
    return FindKey(stage, ShaderTextKey(stage, text), text);
}

const DkShader* ShaderStore::FindKey(ShaderStage stage, uint64_t key, std::string_view textForRecord) {
    if (const auto found = mLoaded.find(key); found != mLoaded.end()) {
        return &found->second->Shader;
    }
    ShaderStage packedStage = stage;
    const auto dksh = mPack.Find(key, &packedStage);
    if (dksh.empty() || packedStage != stage) {
        RecordMissing(stage, key, textForRecord);
        return nullptr;
    }
    return Load(key, dksh);
}

const DkShader* ShaderStore::Embedded(uint64_t key, std::span<const uint8_t> dksh) {
    if (const auto found = mLoaded.find(key); found != mLoaded.end()) {
        return &found->second->Shader;
    }
    return Load(key, dksh);
}

// The whole DKSH (control section first) goes to code memory and the maker
// gets no separate control pointer, as Aurora's renderer loads them.
const DkShader* ShaderStore::Load(uint64_t key, std::span<const uint8_t> dksh) {
    // A shader using more scratch than the queue has takes the GPU down.
    const auto scratch = DkshPerWarpScratchBytes(dksh);
    if (!scratch.has_value() || *scratch > kPerWarpScratchBytes) {
        if (mStats.LoadFailures++ < 10U) {
            Log("shaders: %016llx needs %d bytes of scratch per warp (queue has %u): refused",
                static_cast<unsigned long long>(key), scratch.has_value() ? static_cast<int>(*scratch) : -1,
                kPerWarpScratchBytes);
        }
        return nullptr;
    }
    const uint32_t at = AlignUp(mCodeUsed, DK_SHADER_CODE_ALIGNMENT);
    if (static_cast<uint64_t>(at) + dksh.size() > kCodeBytes - DK_SHADER_CODE_UNUSABLE_SIZE) {
        if (mStats.LoadFailures++ < 10U) {
            Log("shaders: code memory full (%u KiB used)", mCodeUsed >> 10);
        }
        return nullptr;
    }
    std::memcpy(static_cast<uint8_t*>(dkMemBlockGetCpuAddr(mCode)) + at, dksh.data(), dksh.size());
    auto shader = std::make_unique<Loaded>();
    shader->Needs = {DkshGprCount(dksh).value_or(0U), *scratch};
    DkShaderMaker maker;
    dkShaderMakerDefaults(&maker, mCode, at);
    dkShaderInitialize(&shader->Shader, &maker);
    if (!dkShaderIsValid(&shader->Shader)) {
        if (mStats.LoadFailures++ < 10U) {
            Log("shaders: %016llx (%zu bytes) is not a valid shader", static_cast<unsigned long long>(key),
                dksh.size());
        }
        return nullptr;
    }
    mCodeUsed = at + static_cast<uint32_t>(dksh.size());
    mStats.CodeBytes = mCodeUsed;
    ++mStats.Loaded;
    mCodeWritten = true;
    return &mLoaded.emplace(key, std::move(shader)).first->second->Shader;
}

void ShaderStore::RecordMissing(ShaderStage stage, uint64_t key, std::string_view text) {
    if (!mMissing.insert(key).second) {
        return;
    }
    ++mStats.Missing;
    if (text.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(kMissingDirectory, error);
    char name[64];
    std::snprintf(name, sizeof(name), "/%s_%016llx.glsl", stage == ShaderStage::Vertex ? "vert" : "frag",
                  static_cast<unsigned long long>(key));
    const std::string path = std::string(kMissingDirectory) + name;
    if (std::filesystem::exists(path, error)) {
        return;
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (mStats.Missing <= 20U) {
        Log("shaders: %s %016llx not in the pack (saved for offline compilation)",
            stage == ShaderStage::Vertex ? "vertex" : "fragment", static_cast<unsigned long long>(key));
    }
}

} // namespace Fast::Deko3d
#endif
