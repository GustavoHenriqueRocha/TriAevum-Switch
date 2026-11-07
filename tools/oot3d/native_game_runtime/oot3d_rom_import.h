#pragma once

// Prepares game/ (code.bin, exheader.bin, romfs.bin and the native process
// manifest) from the player's own decrypted cartridge dump, on the console.
//
// This is the C++ port of TriAevum's PC input pipeline:
// - tools/triaevum_release/ctr_rom.py (NCSD/NCCH layout, backwards-LZSS .code);
// - tools/triaevum_release/input_copy_adapter.py with the COPY-only recipe
//   input_coverage/recipes/adapters/oot3d-usa-ef210566-copies.json (USA code
//   rebuilt into the canonical EUR image; offsets only, compiled into
//   oot3d_rom_import_usa_copies.inc);
// - tools/triaevum_release/oot3d_region_assets.py (resource algorithm
//   oot3d_usa_resources_to_eur_v2: the USA RomFS becomes a Level-3 service
//   view with us -> eu names, rebuilt hash chains and QM language slots);
// - tools/oot3d/native_a32_runtime/build_process_manifest.py (manifest JSON,
//   same bytes as json.dumps(indent=2, sort_keys=True) + "\n").
// Revisions are identified by hashes only (tools/triaevum_release/
// supported_revisions.json and input_coverage/coverage.json). Only decrypted
// dumps are accepted; there is no key or decryption support.
//
// Provenance: ctr_rom.py (Copyright Citra Emulator Project / Azahar Emulator
// Project, Copyright TriAevum contributors) and oot3d_region_assets.py
// (Copyright TriAevum contributors) are GPL-2.0-or-later, and so are the parts
// of oot3d_rom_import.cpp ported from them. ctr_rom.py adapts the bounds and
// backwards-LZSS decode of Azahar's src/core/file_sys/ncch_container.cpp
// (LZSS_Decompress); the RomFS name hash of oot3d_region_assets.py follows
// Azahar's src/core/file_sys/layered_fs.cpp (CalcHash, itself from 3dbrew),
// both at Azahar revision beb5681ee7f85586501b16b083a961b707092cd7
// (https://github.com/azahar-emu/azahar). See THIRD_PARTY_NOTICES.md.
//
// Pure standard C++ (std::filesystem and stdio), so it builds on the host.

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace Oot3dNativeGame::RomImport {

class ImportError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

using Digest = std::array<std::uint8_t, 32>;

// Streaming SHA-256 (ARMv8 SHA-2 instructions when the target has them).
class Sha256 {
public:
    Sha256();
    void Update(const void* data, std::size_t size);
    [[nodiscard]] Digest Finish();

private:
    std::array<std::uint32_t, 8> mState;
    std::array<std::uint8_t, 64> mBlock{};
    std::size_t mBlockSize = 0;
    std::uint64_t mLength = 0;
};

[[nodiscard]] Digest Sha256Of(std::span<const std::uint8_t> data);
[[nodiscard]] std::string Hex(const Digest& digest);

struct FileIdentity {
    std::uint64_t Bytes;
    const char* Sha256; // lowercase hex
};

[[nodiscard]] bool Matches(const FileIdentity& identity, std::uint64_t bytes,
                           const Digest& digest);

enum class Adaptation {
    None,     // inputs are already the canonical EUR execution inputs
    UsaToEur, // COPY recipe for code, oot3d_usa_resources_to_eur_v2 for RomFS
};

struct Revision {
    const char* Id;   // TriAevum recipe id
    const char* Name; // shown to the player
    FileIdentity SourceCode, SourceExheader, SourceRomFs;
    FileIdentity Code, Exheader, RomFs; // execution inputs written to game/
    Adaptation Adapt;
};

[[nodiscard]] std::span<const Revision> SupportedRevisions();
// One line per supported revision, for error screens.
[[nodiscard]] std::string SupportedRevisionsText();

enum class SourceKind {
    Cartridge, // a .3ds/.cci file (NCSD) or a bare NCCH
    Extracted, // a folder with code.bin (decompressed), exheader.bin, romfs.bin (IVFC)
};

struct Source {
    SourceKind Kind;
    std::filesystem::path Path; // the file, or the folder
};

// Decrypted dumps in `root` (*.3ds/*.cci, sorted by name), then root/import/
// when it holds any of code.bin, exheader.bin or romfs.bin.
[[nodiscard]] std::vector<Source> FindSources(const std::filesystem::path& root);

struct Plan {
    Source Input;
    const Revision* Recipe = nullptr;
    std::uint64_t ProgramId = 0;
    std::vector<std::uint8_t> Code;     // decompressed source .code
    std::vector<std::uint8_t> Exheader; // 2048 bytes
    std::filesystem::path RomFsPath;    // file holding the RomFS (IVFC) image
    std::uint64_t RomFsOffset = 0;
    std::uint64_t RomFsSize = 0;
};

// Parses the container, reads ExHeader and .code and matches a supported
// revision. The RomFS hash is only checked while importing (one pass).
[[nodiscard]] Plan Identify(const Source& source);

struct Progress {
    const char* Stage;
    std::uint64_t Done;
    std::uint64_t Total;
};
using ProgressCallback = std::function<void(const Progress&)>;

struct RomFsAdaptationStats {
    std::uint32_t Files = 0;
    std::uint32_t QmRecords = 0;
};

struct Result {
    const Revision* Recipe = nullptr;
    Digest Code{}, Exheader{}, RomFs{};
    std::uint64_t RomFsBytes = 0;
    RomFsAdaptationStats Resources;
};

inline constexpr const char* kManifestName = "oot3d_native_process_manifest.json";
inline constexpr const char* kReceiptName = "rom_import.json";

// Writes game/ through *.partial files, verifies them (re-read from storage)
// against the recipe and only then replaces the previous files. The previous
// manifest is removed before the first rename, so an interrupted commit
// leaves no manifest and the next boot imports again. The source is never
// modified. Throws ImportError; partial files are removed on failure.
[[nodiscard]] Result Import(const Plan& plan, const std::filesystem::path& gameDirectory,
                            const ProgressCallback& progress = {});

// True when game/ holds a completed import of exactly this source (same
// file names, sizes and modification times), so boot can skip the import.
[[nodiscard]] bool ImportIsCurrent(const Source& source,
                                   const std::filesystem::path& gameDirectory);

// Deletes the player's dump after a successful import (player opt-in only).
void RemoveSource(const Source& source);

// ---- Building blocks, exercised by the host tests ----

[[nodiscard]] std::vector<std::uint8_t> DecompressExeFsCode(std::span<const std::uint8_t> compressed);

// COPY program: LEB128 pairs (source offset, length), `count` pairs.
[[nodiscard]] std::vector<std::uint8_t> ApplyCopyProgram(std::span<const std::uint8_t> source,
                                                         std::span<const std::uint8_t> program,
                                                         std::size_t count,
                                                         std::size_t outputSize);

[[nodiscard]] std::uint32_t RomFsNameHash(std::uint32_t parent,
                                          std::span<const std::uint8_t> utf16Name);

[[nodiscard]] std::vector<std::uint8_t> AdaptQmTable(std::span<const std::uint8_t> table,
                                                     std::uint64_t fileSize);

struct RomFsPatch {
    std::uint64_t Offset; // in the output (Level-3) image
    std::vector<std::uint8_t> Bytes;
};

struct RomFsAdaptation {
    std::uint64_t Base = 0; // bytes of the source image before Level 3 (IVFC superblock)
    std::uint64_t Size = 0; // output size
    std::vector<RomFsPatch> Patches;
    RomFsAdaptationStats Stats;
};

// Reads `count` bytes at `offset` of the source RomFS image (from its first byte).
using RomFsReader = std::function<void(std::uint64_t offset, std::size_t count, std::uint8_t* out)>;

// oot3d_usa_resources_to_eur_v2 as patches over the source image.
[[nodiscard]] RomFsAdaptation PlanUsaToEurRomFs(const RomFsReader& read, std::uint64_t imageSize);

// The manifest JSON for game/ (SD-relative paths).
[[nodiscard]] std::string BuildProcessManifest(std::span<const std::uint8_t> exheader,
                                               std::span<const std::uint8_t> code,
                                               std::uint64_t romfsFileSize,
                                               std::uint64_t romfsServiceOffset);

} // namespace Oot3dNativeGame::RomImport
