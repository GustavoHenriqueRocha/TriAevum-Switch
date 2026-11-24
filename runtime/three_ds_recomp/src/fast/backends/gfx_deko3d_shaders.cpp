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

// The pack opened for reading (nothing if it is not there). Only its header
// and entry table go to memory, each in one fread (the fork author's
// a7424d3b, "leitura do pack de uma vez": a per-byte istreambuf_iterator copy
// over the SD card delayed boot); each DKSH is read on first use.
std::FILE* OpenPack(const std::string& path) {
    return std::fopen(path.c_str(), "rb");
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
    std::FILE* file = nullptr;
    {
        std::ifstream config(kPackConfig);
        std::string line;
        if (config && std::getline(config, line)) {
            if (const auto chosen = ShaderPackNameFromConfig(line)) {
                file = OpenPack(kPackDirectory + *chosen);
                if (file != nullptr) {
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
    if (file == nullptr) {
        file = OpenPack(path);
    }
    if (file == nullptr) {
        Log("shaders: %s not found (every program will be missing)", path.c_str());
        return true;
    }
    // Only the header and the entry table go to memory (24 bytes per stage);
    // the blobs are read on first use, so the pack can hold every predicted
    // stage without taking the game's RAM.
    std::vector<uint8_t> table(16U);
    std::string error;
    uint64_t fileSize = 0;
    if (std::fseek(file, 0, SEEK_END) == 0) {
        const long end = std::ftell(file);
        fileSize = end > 0 ? static_cast<uint64_t>(end) : 0U;
    }
    const uint64_t tableBytes = std::fseek(file, 0, SEEK_SET) == 0 &&
                                        std::fread(table.data(), 1U, table.size(), file) == table.size()
                                    ? ShaderPackTableBytes(table)
                                    : 0U;
    if (tableBytes < table.size() || tableBytes > fileSize) {
        Log("shaders: %s rejected: header or entry table unreadable", path.c_str());
        std::fclose(file);
        return true;
    }
    table.resize(static_cast<size_t>(tableBytes));
    if (std::fread(table.data() + 16, 1U, table.size() - 16U, file) != table.size() - 16U ||
        !mPack.ParseTable(table, fileSize, &error)) {
        Log("shaders: %s rejected: %s", path.c_str(), error.empty() ? "entry table unreadable" : error.c_str());
        std::fclose(file);
        return true;
    }
    mPackFile = file;
    mStats.PackEntries = mPack.Count();
    const size_t skipped = mPack.CountWithFlag(kShaderPackFlagTevPassthroughSkip);
    Log("shaders: %zu precompiled stages in the pack (index %llu KiB, %llu MiB on the SD card)", mPack.Count(),
        static_cast<unsigned long long>(tableBytes >> 10), static_cast<unsigned long long>(fileSize >> 20));
    Log("shaders: pack %s flags=%u tev_passthrough=%s (%zu stages compiled with pass-through TEV stages skipped)",
        name.c_str(), static_cast<unsigned>(mPack.Flags()),
        (mPack.Flags() & kShaderPackFlagTevPassthroughSkip) != 0U ? "skip" : "keep", skipped);
    return true;
}

void ShaderStore::Shutdown() noexcept {
    mLoaded.clear();
    if (mPackFile != nullptr) {
        std::fclose(mPackFile);
        mPackFile = nullptr;
    }
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
    const auto entry = mPack.Entry(key);
    if (!entry.has_value() || entry->Stage != static_cast<uint32_t>(stage) || mPackFile == nullptr) {
        RecordMissing(stage, key, textForRecord);
        return nullptr;
    }
    mBlob.resize(entry->Size);
    uint32_t magic = 0;
    if (std::fseek(mPackFile, static_cast<long>(entry->Offset), SEEK_SET) != 0 ||
        std::fread(mBlob.data(), 1U, mBlob.size(), mPackFile) != mBlob.size() ||
        (std::memcpy(&magic, mBlob.data(), sizeof(magic)), magic != kDkshMagic)) {
        if (mStats.LoadFailures++ < 10U) {
            Log("shaders: %016llx could not be read from the pack", static_cast<unsigned long long>(key));
        }
        return nullptr;
    }
    ++mStats.PackReads;
    return Load(key, mBlob);
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
