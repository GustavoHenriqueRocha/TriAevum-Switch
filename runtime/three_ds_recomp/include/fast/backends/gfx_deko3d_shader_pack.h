#pragma once

// Precompiled deko3d shaders (DKSH, from devkitPro's uam) packed in one file,
// sdmc:/switch/oot3dre/shadercache/deko3d.pack, made offline by
// tools/oot3d/deko3d_shader_pack.py. Each entry is one shader stage, keyed
// by ShaderTextKey of the GLSL text the backend compiles (the OpenGL 4.3
// translation of the PICA program, the same text the GL backend gives Mesa).
// Pure code: tested on the host (oot3d_deko3d_backend_tests).
//
// Layout (little endian):
//   header  { u32 magic "DKP1"; u32 version (1); u32 count; u32 reserved }
//   entries { u64 key; u32 offset; u32 size; u32 stage; u32 reserved } x count,
//           sorted by key, unique
//   blobs   DKSH files at their offsets (from the start of the file)

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Fast::Deko3d {

enum class ShaderStage : uint32_t {
    Vertex = 0,
    Fragment = 1,
};

// FNV-1a 64 over the stage byte and the text (the pack tool computes the same).
inline uint64_t ShaderTextKey(ShaderStage stage, std::string_view text) {
    uint64_t hash = 1469598103934665603ULL;
    hash ^= static_cast<uint8_t>(stage);
    hash *= 1099511628211ULL;
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

constexpr uint32_t kShaderPackMagic = 0x31504B44U; // "DKP1"
constexpr uint32_t kShaderPackVersion = 1U;
constexpr uint32_t kDkshMagic = 0x48534B44U;       // "DKSH"

// Per-warp scratch (local memory) the queue gives every shader
// (DkQueueMaker::perWarpScratchMemorySize, a multiple of 0x200): two
// lighting fragment shaders of the pack need 4096 bytes (105 GPRs, spills),
// above deko3d's default 2048. (Not the crash leaving Link's house, p6g:
// that was the command memory, kCommandGuardBytes.)
constexpr uint32_t kPerWarpScratchBytes = 8U << 10;

// The per-warp scratch a DKSH's first program needs (DkshHeader: magic,
// header_sz, control_sz, code_sz, programs_off, num_programs; its
// DkshProgramHeader: type, entrypoint, num_gprs, constbuf1_off,
// constbuf1_sz, per_warp_scratch_sz), or nothing if the blob is malformed.
[[nodiscard]] inline std::optional<uint32_t> DkshPerWarpScratchBytes(std::span<const uint8_t> dksh) {
    const auto read = [&](size_t at, uint32_t& value) {
        if (at + sizeof(value) > dksh.size()) {
            return false;
        }
        std::memcpy(&value, dksh.data() + at, sizeof(value));
        return true;
    };
    uint32_t magic = 0;
    uint32_t programsOffset = 0;
    uint32_t programs = 0;
    uint32_t scratch = 0;
    if (!read(0, magic) || magic != kDkshMagic || !read(16, programsOffset) || !read(20, programs) || programs == 0U ||
        !read(static_cast<size_t>(programsOffset) + 20U, scratch)) {
        return std::nullopt;
    }
    return scratch;
}

struct ShaderPackEntry {
    uint64_t Key = 0;
    uint32_t Offset = 0;
    uint32_t Size = 0;
    uint32_t Stage = 0;
    uint32_t Reserved = 0;
};
static_assert(sizeof(ShaderPackEntry) == 24);

class ShaderPackIndex {
  public:
    // Takes the whole file. False (with a reason) on any inconsistency; the
    // index is then empty.
    bool Parse(std::vector<uint8_t> bytes, std::string* error) {
        mBytes.clear();
        mEntries.clear();
        const auto fail = [&](const char* why) {
            if (error != nullptr) {
                *error = why;
            }
            mBytes.clear();
            mEntries.clear();
            return false;
        };
        uint32_t header[4]{};
        if (bytes.size() < sizeof(header)) {
            return fail("pack shorter than its header");
        }
        std::memcpy(header, bytes.data(), sizeof(header));
        if (header[0] != kShaderPackMagic) {
            return fail("not a DKP1 pack");
        }
        if (header[1] != kShaderPackVersion) {
            return fail("unsupported pack version");
        }
        const uint64_t count = header[2];
        const uint64_t tableEnd = sizeof(header) + count * sizeof(ShaderPackEntry);
        if (tableEnd > bytes.size()) {
            return fail("entry table past the end of the pack");
        }
        mEntries.resize(static_cast<size_t>(count));
        if (count != 0U) {
            std::memcpy(mEntries.data(), bytes.data() + sizeof(header), static_cast<size_t>(count) * sizeof(ShaderPackEntry));
        }
        for (size_t index = 0; index < mEntries.size(); ++index) {
            const ShaderPackEntry& entry = mEntries[index];
            if (index != 0U && mEntries[index - 1U].Key >= entry.Key) {
                return fail("entries not sorted by unique key");
            }
            if (entry.Stage > static_cast<uint32_t>(ShaderStage::Fragment)) {
                return fail("entry with an unknown stage");
            }
            if (entry.Offset < tableEnd || static_cast<uint64_t>(entry.Offset) + entry.Size > bytes.size() ||
                entry.Size < 4U) {
                return fail("entry blob outside the pack");
            }
            uint32_t magic = 0;
            std::memcpy(&magic, bytes.data() + entry.Offset, sizeof(magic));
            if (magic != kDkshMagic) {
                return fail("entry blob is not DKSH");
            }
        }
        mBytes = std::move(bytes);
        return true;
    }

    // The DKSH of a key (and its stage), or an empty span.
    [[nodiscard]] std::span<const uint8_t> Find(uint64_t key, ShaderStage* stage = nullptr) const {
        const auto found = std::lower_bound(mEntries.begin(), mEntries.end(), key,
                                            [](const ShaderPackEntry& entry, uint64_t value) {
                                                return entry.Key < value;
                                            });
        if (found == mEntries.end() || found->Key != key) {
            return {};
        }
        if (stage != nullptr) {
            *stage = static_cast<ShaderStage>(found->Stage);
        }
        return {mBytes.data() + found->Offset, found->Size};
    }

    [[nodiscard]] size_t Count() const noexcept {
        return mEntries.size();
    }

  private:
    std::vector<uint8_t> mBytes;
    std::vector<ShaderPackEntry> mEntries;
};

} // namespace Fast::Deko3d
