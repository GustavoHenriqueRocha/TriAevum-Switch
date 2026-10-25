// deko3d shaders (Switch). See gfx_deko3d_shaders.h.
#if defined(__SWITCH__)
#include "gfx_deko3d_shaders.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace Fast::Deko3d {
namespace {
constexpr const char* kPackPath = "sdmc:/switch/oot3dre/shadercache/deko3d.pack";
constexpr const char* kMissingDirectory = "sdmc:/switch/oot3dre/logs/deko3d_shaders";
} // namespace

bool ShaderStore::Init(Device& device) {
    mCode = device.MakeBlock(kCodeBytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached |
                                             DkMemBlockFlags_Code,
                             "shader code");
    if (mCode == nullptr) {
        return false;
    }
    std::ifstream file(kPackPath, std::ios::binary);
    if (!file) {
        Log("shaders: %s not found (every program will be missing)", kPackPath);
        return true;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::string error;
    if (!mPack.Parse(std::move(bytes), &error)) {
        Log("shaders: %s rejected: %s", kPackPath, error.c_str());
        return true;
    }
    mStats.PackEntries = mPack.Count();
    Log("shaders: %zu precompiled stages in the pack", mPack.Count());
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
        return found->second.get();
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
        return found->second.get();
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
    auto shader = std::make_unique<DkShader>();
    DkShaderMaker maker;
    dkShaderMakerDefaults(&maker, mCode, at);
    dkShaderInitialize(shader.get(), &maker);
    if (!dkShaderIsValid(shader.get())) {
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
    return mLoaded.emplace(key, std::move(shader)).first->second.get();
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
