#pragma once

// Precompiled deko3d shaders (DKSH, from devkitPro's uam) packed in one file,
// sdmc:/switch/oot3dre/shadercache/deko3d.pack, made offline by
// tools/oot3d/deko3d_shader_pack.py. Each entry is one shader stage, keyed
// by ShaderTextKey of the GLSL text the backend compiles (the OpenGL 4.3
// translation of the PICA program, the same text the GL backend gives Mesa).
// Pure code: tested on the host (oot3d_deko3d_backend_tests).
//
// Layout (little endian):
//   header  { u32 magic "DKP1"; u32 version (1); u32 count; u32 flags }
//   entries { u64 key; u32 offset; u32 size; u32 stage; u32 flags } x count,
//           sorted by key, unique
//   blobs   DKSH files at their offsets (from the start of the file)
// Flags (0 in packs made before tevskip, which wrote the words as reserved):
//   kShaderPackFlagTevPassthroughSkip: the entry's DKSH was compiled from its
//   text with the pass-through TEV stages written as copies (the pack tool's
//   skip_tev_passthrough; exact, tests/switch/test_tev_passthrough.py); the
//   header has it when any entry has it. The key stays the unmodified text's.
//
// Which pack is read: shadercache/deko3d.pack, or the file named by the first
// line of config/deko3d_pack (e.g. deko3d_reference.pack, built without the
// rewrite, for an A/B of the same session's places).

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
constexpr uint32_t kShaderPackFlagTevPassthroughSkip = 1U;
constexpr std::string_view kDefaultShaderPackName = "deko3d.pack";

// The pack file name config/deko3d_pack's first line selects in shadercache/:
// surrounding spaces and CR ignored; only [A-Za-z0-9._-] ending in ".pack",
// not starting with '.', at most 64 characters (no path). Anything else (or no
// file): nothing, and the default pack is read.
[[nodiscard]] inline std::optional<std::string> ShaderPackNameFromConfig(std::string_view line) {
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        line.remove_prefix(1);
    }
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r' || line.back() == '\n')) {
        line.remove_suffix(1);
    }
    constexpr std::string_view suffix = ".pack";
    if (line.size() <= suffix.size() || line.size() > 64U || line.front() == '.' ||
        line.substr(line.size() - suffix.size()) != suffix) {
        return std::nullopt;
    }
    for (const char c : line) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '_' || c == '-';
        if (!ok) {
            return std::nullopt;
        }
    }
    return std::string(line);
}

// Per-warp scratch (local memory) the queue gives every shader
// (DkQueueMaker::perWarpScratchMemorySize, a multiple of 0x200): the lit
// parametric programs once needed 4096 bytes (105 GPRs, spills; none since
// lighting3), above deko3d's default 2048. (Not the crash leaving Link's
// house, p6g: that was the command memory, kCommandGuardBytes.)
constexpr uint32_t kPerWarpScratchBytes = 8U << 10;

// A word of a DKSH's first program header (DkshHeader: magic, header_sz,
// control_sz, code_sz, programs_off, num_programs; its DkshProgramHeader:
// type, entrypoint, num_gprs (+8), constbuf1_off, constbuf1_sz,
// per_warp_scratch_sz (+20)), or nothing if the blob is malformed.
[[nodiscard]] inline std::optional<uint32_t> DkshProgramWord(std::span<const uint8_t> dksh, size_t offset) {
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
    uint32_t word = 0;
    if (!read(0, magic) || magic != kDkshMagic || !read(16, programsOffset) || !read(20, programs) || programs == 0U ||
        !read(static_cast<size_t>(programsOffset) + offset, word)) {
        return std::nullopt;
    }
    return word;
}

// The per-warp scratch (local memory) a DKSH's first program needs.
[[nodiscard]] inline std::optional<uint32_t> DkshPerWarpScratchBytes(std::span<const uint8_t> dksh) {
    return DkshProgramWord(dksh, 20U);
}

// The registers (GPRs per thread) a DKSH's first program uses.
[[nodiscard]] inline std::optional<uint32_t> DkshGprCount(std::span<const uint8_t> dksh) {
    return DkshProgramWord(dksh, 8U);
}

struct ShaderPackEntry {
    uint64_t Key = 0;
    uint32_t Offset = 0;
    uint32_t Size = 0;
    uint32_t Stage = 0;
    uint32_t Flags = 0;
};
static_assert(sizeof(ShaderPackEntry) == 24);

class ShaderPackIndex {
  public:
    // Takes the whole file. False (with a reason) on any inconsistency; the
    // index is then empty.
    bool Parse(std::vector<uint8_t> bytes, std::string* error) {
        mBytes.clear();
        mEntries.clear();
        mFlags = 0;
        const auto fail = [&](const char* why) {
            if (error != nullptr) {
                *error = why;
            }
            mBytes.clear();
            mEntries.clear();
            mFlags = 0;
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
        mFlags = header[3];
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
    // Header flags (kShaderPackFlag*).
    [[nodiscard]] uint32_t Flags() const noexcept {
        return mFlags;
    }

    // Entries whose flags have every bit of `flag`.
    [[nodiscard]] size_t CountWithFlag(uint32_t flag) const noexcept {
        return static_cast<size_t>(std::count_if(mEntries.begin(), mEntries.end(), [flag](const ShaderPackEntry& entry) {
            return (entry.Flags & flag) == flag;
        }));
    }

  private:
    std::vector<uint8_t> mBytes;
    std::vector<ShaderPackEntry> mEntries;
    uint32_t mFlags = 0;
};

} // namespace Fast::Deko3d
