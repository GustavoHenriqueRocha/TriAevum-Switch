// System data archives substitutos (fonte compartilhada, Mii, países, NG word)
// e a fonte do APT:GetSharedFont. Ver oot3d_native_a32_ctr_system_archives.h.
//
// DecompressCtrLz11 e RelocateCtrSharedFont adaptam, com checagem de limites,
// DecompressLZ11 (src/core/hle/service/apt/apt.cpp) e
// BCFNT::RelocateSharedFont (src/core/hle/service/apt/bcfnt/bcfnt.cpp) do
// Azahar: Copyright 2016 Citra Emulator Project, GPL-2.0-or-later.
#include "oot3d_native_a32_ctr_system_archives.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace Oot3dNativeGame {

// Gerados no build a partir de third_party/ctr_open_source_archives/data
// (embed_romfs.cmake).
extern const unsigned char kCtrOpenSourceArchive_shared_font[];
extern const std::size_t kCtrOpenSourceArchive_shared_font_size;
extern const unsigned char kCtrOpenSourceArchive_mii[];
extern const std::size_t kCtrOpenSourceArchive_mii_size;
extern const unsigned char kCtrOpenSourceArchive_country_list[];
extern const std::size_t kCtrOpenSourceArchive_country_list_size;
extern const unsigned char kCtrOpenSourceArchive_bad_word_list[];
extern const std::size_t kCtrOpenSourceArchive_bad_word_list_size;

namespace {

uint32_t Load32(std::span<const uint8_t> bytes, size_t offset) {
    uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;  // Host little-endian, como o resto do HLE.
}

uint64_t Load64(std::span<const uint8_t> bytes, size_t offset) {
    uint64_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

void Store32(std::span<uint8_t> bytes, size_t offset, uint32_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

std::span<const uint8_t> Bytes(const unsigned char* data, std::size_t size) {
    return {reinterpret_cast<const uint8_t*>(data), size};
}

constexpr uint64_t TitleId(uint32_t high, uint32_t low) {
    return (static_cast<uint64_t>(high) << 32U) | low;
}

} // namespace

std::string CtrSystemArchiveFilePath(uint64_t titleId) {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string path = "system_archive/";
    for (int shift = 60; shift >= 0; shift -= 4) path.push_back(kDigits[(titleId >> shift) & 0xFU]);
    return path;
}

std::optional<uint64_t> ParseCtrSystemArchiveFilePath(std::string_view path) {
    constexpr std::string_view kPrefix = "system_archive/";
    if (!path.starts_with(kPrefix) || path.size() != kPrefix.size() + 16U) return std::nullopt;
    uint64_t titleId = 0;
    for (const char c : path.substr(kPrefix.size())) {
        const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (digit < 0) return std::nullopt;
        titleId = (titleId << 4U) | static_cast<uint64_t>(digit);
    }
    return titleId;
}

std::optional<CtrSystemArchive> FindCtrSystemArchive(uint64_t titleId) {
    const auto font = Bytes(kCtrOpenSourceArchive_shared_font, kCtrOpenSourceArchive_shared_font_size);
    switch (titleId) {
    case TitleId(kCtrSharedDataTitleHigh, kCtrSharedFontTitleLow):
        return CtrSystemArchive{titleId, "shared_font", font, false};
    case TitleId(kCtrSharedDataTitleHigh, kCtrSharedFontChnTitleLow):
    case TitleId(kCtrSharedDataTitleHigh, kCtrSharedFontKorTitleLow):
    case TitleId(kCtrSharedDataTitleHigh, kCtrSharedFontTwnTitleLow):
        return CtrSystemArchive{titleId, "shared_font", font, true};
    case TitleId(kCtrSharedDataTitleHigh, kCtrMiiDataTitleLow):
        return CtrSystemArchive{
            titleId, "mii", Bytes(kCtrOpenSourceArchive_mii, kCtrOpenSourceArchive_mii_size), false};
    case TitleId(kCtrSharedDataTitleHigh, kCtrRegionManifestTitleLow):
        return CtrSystemArchive{
            titleId, "country_list",
            Bytes(kCtrOpenSourceArchive_country_list, kCtrOpenSourceArchive_country_list_size), false};
    case TitleId(kCtrSystemDataTitleHigh, kCtrNgWordListTitleLow):
        return CtrSystemArchive{
            titleId, "bad_word_list",
            Bytes(kCtrOpenSourceArchive_bad_word_list, kCtrOpenSourceArchive_bad_word_list_size), false};
    default:
        return std::nullopt;
    }
}

std::optional<std::span<const uint8_t>> FindCtrRomFsRootFile(
    std::span<const uint8_t> romfs, std::u16string_view name) {
    // Cabeçalho do nível 3: tamanho (0x28), tabelas de hash/metadados de
    // pastas e arquivos (offset, tamanho) e o offset dos dados.
    if (romfs.size() < 0x28U || Load32(romfs, 0) != 0x28U) return std::nullopt;
    const uint64_t metaOffset = Load32(romfs, 0x1C);
    const uint64_t metaSize = Load32(romfs, 0x20);
    const uint64_t dataOffset = Load32(romfs, 0x24);
    if (metaOffset > romfs.size() || metaSize > romfs.size() - metaOffset ||
        dataOffset > romfs.size()) {
        return std::nullopt;
    }
    // Entrada: pai, irmão, offset u64, tamanho u64, próximo do hash,
    // tamanho do nome, nome UTF-16LE (alinhado a 4).
    uint64_t entry = 0;
    while (entry + 0x20U <= metaSize) {
        const size_t at = static_cast<size_t>(metaOffset + entry);
        const uint32_t parent = Load32(romfs, at);
        const uint64_t offset = Load64(romfs, at + 8U);
        const uint64_t size = Load64(romfs, at + 16U);
        const uint32_t nameBytes = Load32(romfs, at + 28U);
        if (nameBytes > metaSize - entry - 0x20U) return std::nullopt;
        if (parent == 0U && nameBytes == name.size() * 2U) {
            bool same = true;
            for (size_t index = 0; same && index < name.size(); ++index) {
                const size_t c = at + 0x20U + index * 2U;
                same = static_cast<char16_t>(romfs[c] | (romfs[c + 1U] << 8U)) == name[index];
            }
            if (same) {
                const uint64_t begin = dataOffset + offset;
                if (begin < dataOffset || begin > romfs.size() || size > romfs.size() - begin)
                    return std::nullopt;
                return romfs.subspan(static_cast<size_t>(begin), static_cast<size_t>(size));
            }
        }
        entry += 0x20U + ((nameBytes + 3U) & ~3U);
    }
    return std::nullopt;
}

bool DecompressCtrLz11(std::span<const uint8_t> input, std::vector<uint8_t>* output) {
    if (output == nullptr || input.size() < 4U || input[0] != 0x11U) return false;
    size_t in = 4U;
    size_t size = input[1] | (input[2] << 8U) | (static_cast<size_t>(input[3]) << 16U);
    if (size == 0U) {
        if (input.size() < 8U) return false;
        size = Load32(input, 4U);
        in = 8U;
    }
    output->assign(size, 0U);
    size_t out = 0U;
    while (out < size) {
        if (in >= input.size()) return false;
        const uint8_t flags = input[in++];
        for (int bit = 7; bit >= 0 && out < size; --bit) {
            if ((flags & (1U << bit)) == 0U) {
                if (in >= input.size()) return false;
                (*output)[out++] = input[in++];
                continue;
            }
            if (in + 1U >= input.size()) return false;
            const uint32_t byte1 = input[in];
            uint32_t length = 0U, distance = 0U;
            if ((byte1 >> 4U) == 0U) {
                if (in + 2U >= input.size()) return false;
                const uint32_t byte2 = input[in + 1U], byte3 = input[in + 2U];
                length = (((byte1 & 0xFU) << 4U) | (byte2 >> 4U)) + 0x11U;
                distance = (((byte2 & 0xFU) << 8U) | byte3) + 1U;
                in += 3U;
            } else if ((byte1 >> 4U) == 1U) {
                if (in + 3U >= input.size()) return false;
                const uint32_t byte2 = input[in + 1U], byte3 = input[in + 2U],
                               byte4 = input[in + 3U];
                length = (((byte1 & 0xFU) << 12U) | (byte2 << 4U) | (byte3 >> 4U)) + 0x111U;
                distance = (((byte3 & 0xFU) << 8U) | byte4) + 1U;
                in += 4U;
            } else {
                const uint32_t byte2 = input[in + 1U];
                length = (byte1 >> 4U) + 1U;
                distance = (((byte1 & 0xFU) << 8U) | byte2) + 1U;
                in += 2U;
            }
            if (distance > out || length > size - out) return false;
            for (uint32_t index = 0; index < length; ++index, ++out)
                (*output)[out] = (*output)[out - distance];
        }
    }
    return true;
}

bool RelocateCtrSharedFont(std::span<uint8_t> memory, uint32_t targetAddress) {
    constexpr size_t kStart = kCtrSharedFontHeaderSize;
    // CFNT: assinatura, BOM u16, tamanho do cabeçalho u16, versão, tamanho
    // do arquivo, número de blocos.
    if (memory.size() < kStart + 0x14U) return false;
    const auto readOnly = std::span<const uint8_t>(memory);
    const uint16_t headerSize = static_cast<uint16_t>(memory[kStart + 6U] | (memory[kStart + 7U] << 8U));
    const uint32_t blocks = Load32(readOnly, kStart + 0x10U);
    const auto walk = [&](auto&& visit) {
        size_t at = kStart + headerSize;
        for (uint32_t block = 0; block < blocks; ++block) {
            if (at + 8U > memory.size()) return false;
            const uint32_t sectionSize = Load32(readOnly, at + 4U);
            if (sectionSize < 8U || sectionSize > memory.size() - at) return false;
            if (!visit(at, sectionSize)) return false;
            at += sectionSize;
        }
        return true;
    };
    const auto magic = [&](size_t at, const char* name) {
        return std::memcmp(memory.data() + at, name, 4U) == 0;
    };
    // Descobre a base antiga comparando os ponteiros do FINF com a posição
    // real das primeiras seções (os ponteiros apontam 8 bytes após o
    // cabeçalho da seção).
    uint32_t firstCmap = 0, firstCwdh = 0, firstTglp = 0;
    std::optional<std::array<uint32_t, 3>> finfPointers;  // tglp, cwdh, cmap
    if (!walk([&](size_t at, uint32_t size) {
            if (firstCmap == 0U && magic(at, "CMAP")) firstCmap = static_cast<uint32_t>(at);
            else if (firstCwdh == 0U && magic(at, "CWDH")) firstCwdh = static_cast<uint32_t>(at);
            else if (firstTglp == 0U && magic(at, "TGLP")) firstTglp = static_cast<uint32_t>(at);
            else if (magic(at, "FINF")) {
                if (size < 0x20U) return false;
                finfPointers = std::array<uint32_t, 3>{Load32(readOnly, at + 0x10U),
                                                       Load32(readOnly, at + 0x14U),
                                                       Load32(readOnly, at + 0x18U)};
            }
            return true;
        })) {
        return false;
    }
    if (!finfPointers || firstCmap == 0U || firstCwdh == 0U || firstTglp == 0U) return false;
    const uint32_t previousBase = ((*finfPointers)[2] - 8U) - firstCmap;
    if (previousBase != ((*finfPointers)[1] - 8U) - firstCwdh ||
        previousBase != ((*finfPointers)[0] - 8U) - firstTglp) {
        return false;
    }
    const uint32_t delta = targetAddress - previousBase;
    const auto add = [&](size_t at, bool keepZero) {
        const uint32_t value = Load32(readOnly, at);
        if (!keepZero || value != 0U) Store32(memory, at, value + delta);
    };
    return walk([&](size_t at, uint32_t size) {
        if (magic(at, "FINF")) {
            add(at + 0x10U, false);  // tglp
            add(at + 0x14U, false);  // cwdh
            add(at + 0x18U, false);  // cmap
        } else if (magic(at, "CMAP")) {
            if (size < 0x14U) return false;
            add(at + 0x10U, true);   // próximo CMAP
        } else if (magic(at, "CWDH")) {
            if (size < 0x10U) return false;
            add(at + 0x0CU, true);   // próximo CWDH
        } else if (magic(at, "TGLP")) {
            if (size < 0x20U) return false;
            add(at + 0x1CU, false);  // dados das folhas
        }
        return true;
    });
}

bool BuildCtrSharedFontMemory(uint32_t targetAddress, std::vector<uint8_t>* memory) {
    if (memory == nullptr) return false;
    const auto archive = FindCtrSystemArchive(
        TitleId(kCtrSharedDataTitleHigh, kCtrSharedFontTitleLow));
    if (!archive) return false;
    const auto compressed = FindCtrRomFsRootFile(archive->RomFs, u"cbf_std.bcfnt.lz");
    std::vector<uint8_t> font;
    if (!compressed || !DecompressCtrLz11(*compressed, &font) || font.size() < 4U ||
        std::memcmp(font.data(), "CFNT", 4U) != 0 ||
        font.size() > kCtrSharedFontMemorySize - kCtrSharedFontHeaderSize) {
        return false;
    }
    memory->assign(kCtrSharedFontMemorySize, 0U);
    Store32(*memory, 0x0U, 2U);  // status: carregada
    Store32(*memory, 0x4U, 1U);  // região da fonte: JPN/EUR/USA
    Store32(*memory, 0x8U, static_cast<uint32_t>(font.size()));
    std::copy(font.begin(), font.end(), memory->begin() + kCtrSharedFontHeaderSize);
    (*memory)[kCtrSharedFontHeaderSize + 3U] = 'U';  // CFNT -> CFNU
    return RelocateCtrSharedFont(*memory, targetAddress);
}

bool NativeA32CtrMemoryFile::Read(uint64_t offset, std::span<uint8_t> destination,
                                  uint32_t* returnedSize) {
    if (returnedSize) *returnedSize = 0U;
    if (offset > mBytes.size()) return false;
    const size_t count = std::min<uint64_t>(destination.size(), mBytes.size() - offset);
    std::copy_n(mBytes.begin() + static_cast<std::ptrdiff_t>(offset), count, destination.begin());
    if (returnedSize) *returnedSize = static_cast<uint32_t>(count);
    return true;
}

} // namespace Oot3dNativeGame
