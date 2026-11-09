#pragma once

// Prepara o game/ (code.bin, exheader.bin, romfs.bin e o manifesto do processo
// nativo) a partir do dump decriptado do cartucho do próprio jogador, no console.
//
// É o port em C++ do pipeline de entrada do TriAevum no PC:
// - tools/triaevum_release/ctr_rom.py (layout NCSD/NCCH, .code em LZSS reverso);
// - tools/triaevum_release/input_copy_adapter.py com a receita só de COPY
//   input_coverage/recipes/adapters/oot3d-usa-ef210566-copies.json (code da USA
//   remontado na imagem EUR canônica; só offsets, compilados em
//   oot3d_rom_import_usa_copies.inc);
// - tools/triaevum_release/oot3d_region_assets.py (algoritmo de recursos
//   oot3d_usa_resources_to_eur_v2: a RomFS da USA vira uma visão de serviço
//   Level-3 com nomes us -> eu, cadeias de hash refeitas e slots de idioma do QM);
// - tools/oot3d/native_a32_runtime/build_process_manifest.py (JSON do manifesto,
//   mesmos bytes de json.dumps(indent=2, sort_keys=True) + "\n").
// As revisões são identificadas só por hash (tools/triaevum_release/
// supported_revisions.json e input_coverage/coverage.json). Só dumps
// decriptados; não há chave nem decriptação.
//
// Procedência: ctr_rom.py (Copyright Citra Emulator Project / Azahar Emulator
// Project, Copyright TriAevum contributors) e oot3d_region_assets.py
// (Copyright TriAevum contributors) são GPL-2.0-or-later, assim como as partes
// de oot3d_rom_import.cpp portadas deles. O ctr_rom.py adapta os limites e o
// LZSS reverso do src/core/file_sys/ncch_container.cpp do Azahar
// (LZSS_Decompress); o hash de nome da RomFS do oot3d_region_assets.py segue o
// src/core/file_sys/layered_fs.cpp do Azahar (CalcHash, que vem do 3dbrew),
// ambos na revisão beb5681ee7f85586501b16b083a961b707092cd7 do Azahar
// (https://github.com/azahar-emu/azahar). Ver THIRD_PARTY_NOTICES.md.
//
// Só C++ padrão (std::filesystem e stdio): compila no host.

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

// SHA-256 em fluxo (instruções SHA-2 do ARMv8 quando o alvo tem).
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
    const char* Sha256; // hex minúsculo
};

[[nodiscard]] bool Matches(const FileIdentity& identity, std::uint64_t bytes,
                           const Digest& digest);

enum class Adaptation {
    None,     // as entradas já são as entradas de execução EUR canônicas
    UsaToEur, // receita COPY no code, oot3d_usa_resources_to_eur_v2 na RomFS
};

struct Revision {
    const char* Id;   // id da receita do TriAevum
    const char* Name; // mostrado ao jogador
    FileIdentity SourceCode, SourceExheader, SourceRomFs;
    FileIdentity Code, Exheader, RomFs; // entradas de execução gravadas no game/
    Adaptation Adapt;
};

[[nodiscard]] std::span<const Revision> SupportedRevisions();
// Uma linha por revisão suportada, para as telas de erro.
[[nodiscard]] std::string SupportedRevisionsText();

enum class SourceKind {
    Cartridge, // um arquivo .3ds/.cci (NCSD) ou um NCCH solto
    Extracted, // uma pasta com code.bin (descomprimido), exheader.bin, romfs.bin (IVFC)
};

struct Source {
    SourceKind Kind;
    std::filesystem::path Path; // o arquivo ou a pasta
};

// Dumps decriptados em `root` (*.3ds/*.cci, ordenados pelo nome) e depois
// root/import/, quando ela tem algum de code.bin, exheader.bin ou romfs.bin.
[[nodiscard]] std::vector<Source> FindSources(const std::filesystem::path& root);

struct Plan {
    Source Input;
    const Revision* Recipe = nullptr;
    std::uint64_t ProgramId = 0;
    std::vector<std::uint8_t> Code;     // .code de origem descomprimido
    std::vector<std::uint8_t> Exheader; // 2048 bytes
    std::filesystem::path RomFsPath;    // arquivo que contém a imagem RomFS (IVFC)
    std::uint64_t RomFsOffset = 0;
    std::uint64_t RomFsSize = 0;
};

// Lê o contêiner, o ExHeader e o .code e acha a revisão suportada. O hash da
// RomFS só é conferido durante a importação (uma passada só).
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

// Grava o game/ por arquivos *.partial, confere cada um (relido do
// armazenamento) contra a receita e só então substitui os anteriores. O
// manifesto anterior sai antes do primeiro rename: um commit interrompido
// deixa o game/ sem manifesto e o próximo boot importa de novo. A origem nunca
// é alterada. Lança ImportError; os parciais são apagados na falha.
[[nodiscard]] Result Import(const Plan& plan, const std::filesystem::path& gameDirectory,
                            const ProgressCallback& progress = {});

// Verdadeiro quando o game/ tem uma importação completa exatamente desta
// origem (mesmos nomes, tamanhos e datas de modificação): o boot pula a importação.
[[nodiscard]] bool ImportIsCurrent(const Source& source,
                                   const std::filesystem::path& gameDirectory);

// Apaga o dump do jogador depois de uma importação bem-sucedida (só se ele pedir).
void RemoveSource(const Source& source);

// ---- Peças usadas pelos testes de host ----

[[nodiscard]] std::vector<std::uint8_t> DecompressExeFsCode(std::span<const std::uint8_t> compressed);

// Programa COPY: pares LEB128 (offset na origem, tamanho), `count` pares.
[[nodiscard]] std::vector<std::uint8_t> ApplyCopyProgram(std::span<const std::uint8_t> source,
                                                         std::span<const std::uint8_t> program,
                                                         std::size_t count,
                                                         std::size_t outputSize);

[[nodiscard]] std::uint32_t RomFsNameHash(std::uint32_t parent,
                                          std::span<const std::uint8_t> utf16Name);

[[nodiscard]] std::vector<std::uint8_t> AdaptQmTable(std::span<const std::uint8_t> table,
                                                     std::uint64_t fileSize);

struct RomFsPatch {
    std::uint64_t Offset; // na imagem de saída (Level-3)
    std::vector<std::uint8_t> Bytes;
};

struct RomFsAdaptation {
    std::uint64_t Base = 0; // bytes da imagem de origem antes do Level 3 (superbloco IVFC)
    std::uint64_t Size = 0; // tamanho da saída
    std::vector<RomFsPatch> Patches;
    RomFsAdaptationStats Stats;
};

// Lê `count` bytes em `offset` da imagem RomFS de origem (a partir do primeiro byte).
using RomFsReader = std::function<void(std::uint64_t offset, std::size_t count, std::uint8_t* out)>;

// oot3d_usa_resources_to_eur_v2 como patches sobre a imagem de origem.
[[nodiscard]] RomFsAdaptation PlanUsaToEurRomFs(const RomFsReader& read, std::uint64_t imageSize);

// O JSON do manifesto do game/ (caminhos relativos ao manifesto).
[[nodiscard]] std::string BuildProcessManifest(std::span<const std::uint8_t> exheader,
                                               std::span<const std::uint8_t> code,
                                               std::uint64_t romfsFileSize,
                                               std::uint64_t romfsServiceOffset);

} // namespace Oot3dNativeGame::RomImport
