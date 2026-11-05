#pragma once

// System data archives que qualquer título de 3DS pode abrir (fonte
// compartilhada, recursos do Mii, lista de países, NG word) servidos sem
// arquivos do console: são os substitutos open source que o Citra/Azahar
// embutem (tools/oot3d/third_party/ctr_open_source_archives). Cada um é a
// RomFS de um NCCH só com o nível 3 do IVFC, como o FS entrega ao título.
//
// A relocação da fonte para o APT:GetSharedFont segue
// src/core/hle/service/apt/bcfnt/bcfnt.cpp do Azahar (GPL-2.0-or-later).

#include "oot3d_native_a32_ctr_host.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Oot3dNativeGame {

// Archive NCCH/SystemData do fs:USER (OpenFileDirectly/OpenArchive).
constexpr uint32_t kCtrNcchArchiveId = 0x2345678AU;
// Categorias (id alto) e títulos (id baixo), https://3dbrew.org/wiki/Title_list.
constexpr uint32_t kCtrSharedDataTitleHigh = 0x0004009BU;
constexpr uint32_t kCtrSystemDataTitleHigh = 0x000400DBU;
constexpr uint32_t kCtrMiiDataTitleLow = 0x00010202U;
constexpr uint32_t kCtrRegionManifestTitleLow = 0x00010402U;
constexpr uint32_t kCtrNgWordListTitleLow = 0x00010302U;
constexpr uint32_t kCtrSharedFontTitleLow = 0x00014002U;     // JPN/EUR/USA
constexpr uint32_t kCtrSharedFontChnTitleLow = 0x00014102U;
constexpr uint32_t kCtrSharedFontKorTitleLow = 0x00014202U;
constexpr uint32_t kCtrSharedFontTwnTitleLow = 0x00014302U;

// APT:GetSharedFont. O bloco tem o tamanho do console (3272 KB); a fonte fica
// em +0x80, depois de um cabeçalho {status=2, região, tamanho descomprimido},
// com a assinatura trocada de CFNT para CFNU e os ponteiros relocados para o
// endereço devolvido (o do heap linear antigo, como no console e no Azahar).
constexpr uint32_t kCtrSharedFontMemorySize = 0x332000U;
constexpr uint32_t kCtrSharedFontHeaderSize = 0x80U;
constexpr uint32_t kCtrSharedFontAddress = 0x18000000U;
constexpr uint32_t kAptGetSharedFontRequest = 0x00440000U;
constexpr uint32_t kAptGetSharedFontResponse = 0x00440082U;

// Tipo da sessão de arquivo (mesma semântica de "client_session:file:romfs",
// mas sobre os bytes embutidos) e o FilePath que identifica o título; o
// savestate guarda esse path e reabre o mesmo substituto.
inline constexpr std::string_view kCtrSystemArchiveFileType = "client_session:file:system_archive";
std::string CtrSystemArchiveFilePath(uint64_t titleId);  // "system_archive/<id hex>"
std::optional<uint64_t> ParseCtrSystemArchiveFilePath(std::string_view path);

struct CtrSystemArchive {
    uint64_t TitleId = 0;
    std::string_view Name;  // "shared_font", "mii", ...
    std::span<const uint8_t> RomFs;
    // CHN/KOR/TWN recebem a fonte JPN/EUR/USA (sem os ideogramas da região).
    bool Substitute = false;
};

// Substituto de um system data archive pelo title id, ou nullopt.
std::optional<CtrSystemArchive> FindCtrSystemArchive(uint64_t titleId);

// Arquivo da pasta raiz de uma RomFS nível 3 (sem subpastas).
std::optional<std::span<const uint8_t>> FindCtrRomFsRootFile(
    std::span<const uint8_t> romfs, std::u16string_view name);

// LZ11 (cabeçalho 0x11 + tamanho de 24 bits), como os .bcfnt.lz.
bool DecompressCtrLz11(std::span<const uint8_t> input, std::vector<uint8_t>* output);

// Reloca FINF/CMAP/CWDH/TGLP de uma fonte em memory[0x80...] para que os
// ponteiros valham com o bloco mapeado em targetAddress.
bool RelocateCtrSharedFont(std::span<uint8_t> memory, uint32_t targetAddress);

// Conteúdo inteiro do bloco de memória da fonte compartilhada.
bool BuildCtrSharedFontMemory(uint32_t targetAddress, std::vector<uint8_t>* memory);

// Arquivo só de leitura sobre bytes embutidos (RomFS dos substitutos).
class NativeA32CtrMemoryFile final : public NativeA32CtrFile {
  public:
    explicit NativeA32CtrMemoryFile(std::span<const uint8_t> bytes) : mBytes(bytes) {}
    bool Read(uint64_t offset, std::span<uint8_t> destination, uint32_t* returnedSize) override;
    bool Write(uint64_t, std::span<const uint8_t>, uint32_t* writtenSize) override {
        if (writtenSize) *writtenSize = 0U;
        return false;
    }
    bool Resize(uint64_t) override { return false; }
    uint64_t Size() const noexcept override { return mBytes.size(); }

  private:
    std::span<const uint8_t> mBytes;
};

// Mesmo arquivo só de leitura, dono dos bytes (seções do ExeFS do título).
class NativeA32CtrOwnedMemoryFile final : public NativeA32CtrFile {
  public:
    explicit NativeA32CtrOwnedMemoryFile(std::vector<uint8_t> bytes)
        : mBytes(std::move(bytes)), mView(mBytes) {}
    bool Read(uint64_t offset, std::span<uint8_t> destination, uint32_t* returnedSize) override {
        return mView.Read(offset, destination, returnedSize);
    }
    bool Write(uint64_t offset, std::span<const uint8_t> data, uint32_t* writtenSize) override {
        return mView.Write(offset, data, writtenSize);
    }
    bool Resize(uint64_t) override { return false; }
    uint64_t Size() const noexcept override { return mBytes.size(); }

  private:
    std::vector<uint8_t> mBytes;
    NativeA32CtrMemoryFile mView;
};

} // namespace Oot3dNativeGame
