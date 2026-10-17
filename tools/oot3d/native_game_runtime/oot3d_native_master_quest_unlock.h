#pragma once

// Opção "liberar o Master Quest sem zerar o jogo" (config/unlock_master_quest).
//
// Onde o OoT3D guarda o desbloqueio (code.bin EUR canônico; o formato do
// arquivo é o mesmo na versão USA): no arquivo de sistema "data:/system.dat"
// do arquivo de save (34 bytes), não nos slots save00..05.bin.
//
//   +0x00  u8   0xBE = Master Quest desbloqueado (0x00 até zerar o jogo)
//   +0x01  u8[30] identificador aleatório (FUN_00435274 / FUN_0042231c)
//   +0x1F  u8   0xEF = a última escolha da tela de versões foi o Master Quest
//   +0x20  u16  CRC-16/ARC (polinômio 0xA001 refletido, início 0) dos 34
//               bytes com este campo zerado (FUN_002faf90), little-endian
//
// Na memória o arquivo vive em 0x00587934 (EUR). Quem lê o desbloqueio:
//   FUN_003487ec  "desbloqueado?" = byte0 == 0xBE; chamado por EnMag_Init/
//                 Update (logo do título), FUN_003480a8 (COmoteUraSelector:
//                 escolha de duas versões ou só a normal), FileChoose_Init e o
//                 fim do jogo (0x457820, que grava 0xBE via FUN_0047d7c4 e
//                 salva o arquivo com FUN_002f3684 quando ainda não havia).
//   FUN_002e63c8  modo MQ ativo = byte0 == 0xBE && byte0x1F == 0xEF; escolhe
//                 os slots 3..5 (FileSelect_GetSlotBuffer/IsSlotValid); o
//                 mesmo teste inline em FUN_00434f74/FUN_00483888 copia o modo
//                 para gSaveContext.masterQuestFlag (+0x0E).
//   O seletor grava +0x1F (FUN_0030765c = 0xEF, FUN_00307668 = 0) só na RAM.
//
// O jogo lê o arquivo uma única vez no boot (leitura assíncrona de 34 bytes
// direto em 0x00587934, CRC conferido em FUN_0042231c; CRC ruim => zera e
// gera outro identificador) e só o grava no primeiro boot (padrão, byte0 0)
// e no fim do jogo. Por isso a opção age no serviço fs:USER do host, sem
// tocar no código traduzido: a leitura de system.dat devolve byte0 = 0xBE
// com o CRC refeito, e a gravação de system.dat nunca leva para o disco um
// 0xBE que o disco não tinha. O arquivo e os saves no cartão ficam como
// estavam; tirar a opção volta ao estado anterior (os saves MQ continuam lá).

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>

namespace Oot3dNativeGame {

inline constexpr std::size_t kSystemDataSize = 34U;
inline constexpr std::size_t kSystemDataChecksumOffset = 0x20U;
inline constexpr std::size_t kSystemDataModeOffset = 0x1FU;
inline constexpr std::uint8_t kSystemDataMasterQuestUnlocked = 0xBEU;
inline constexpr std::uint8_t kSystemDataMasterQuestMode = 0xEFU;
inline constexpr std::uint32_t kSystemDataGuestAddressEur = 0x00587934U;

// CRC-16/ARC (FUN_002faf90): polinômio 0xA001 refletido, início 0.
[[nodiscard]] inline std::uint16_t Crc16Arc(std::span<const std::uint8_t> data) noexcept {
    std::uint16_t crc = 0U;
    for (const std::uint8_t value : data) {
        crc = static_cast<std::uint16_t>(crc ^ value);
        for (int bit = 0; bit < 8; ++bit)
            crc = static_cast<std::uint16_t>((crc & 1U) ? (crc >> 1U) ^ 0xA001U : crc >> 1U);
    }
    return crc;
}

// O CRC do system.dat: os 34 bytes com o campo do CRC zerado.
[[nodiscard]] inline std::uint16_t
SystemDataChecksum(std::span<const std::uint8_t> data) noexcept {
    std::array<std::uint8_t, kSystemDataSize> copy{};
    for (std::size_t index = 0; index < kSystemDataSize && index < data.size(); ++index)
        copy[index] = data[index];
    copy[kSystemDataChecksumOffset] = 0U;
    copy[kSystemDataChecksumOffset + 1U] = 0U;
    return Crc16Arc(copy);
}

[[nodiscard]] inline std::uint16_t
StoredSystemDataChecksum(std::span<const std::uint8_t> data) noexcept {
    return static_cast<std::uint16_t>(data[kSystemDataChecksumOffset] |
                                      (data[kSystemDataChecksumOffset + 1U] << 8U));
}

[[nodiscard]] inline bool
IsValidSystemData(std::span<const std::uint8_t> data) noexcept {
    return data.size() >= kSystemDataSize &&
           StoredSystemDataChecksum(data) == SystemDataChecksum(data);
}

inline void StoreSystemDataChecksum(std::span<std::uint8_t> data) noexcept {
    const std::uint16_t crc = SystemDataChecksum(data);
    data[kSystemDataChecksumOffset] = static_cast<std::uint8_t>(crc);
    data[kSystemDataChecksumOffset + 1U] = static_cast<std::uint8_t>(crc >> 8U);
}

// O arquivo de sistema na raiz do arquivo de save ("/system.dat").
[[nodiscard]] inline bool IsSystemDataPath(const std::filesystem::path &path) {
    return path.filename() == "system.dat";
}

// Leitura: `bytes` é o que o host vai devolver ao jogo a partir de
// `offset`. Só mexe numa leitura do arquivo inteiro a partir do início
// com CRC válido (um arquivo corrompido continua corrompido, e o jogo o
// trata como sempre). Devolve true se marcou o desbloqueio.
inline bool ApplyMasterQuestUnlockToSystemDataRead(std::uint64_t offset,
                                                   std::span<std::uint8_t> bytes) noexcept {
    if (offset != 0U || bytes.size() < kSystemDataSize || !IsValidSystemData(bytes) ||
        bytes[0] == kSystemDataMasterQuestUnlocked)
        return false;
    bytes[0] = kSystemDataMasterQuestUnlocked;
    StoreSystemDataChecksum(bytes);
    return true;
}

// Gravação: `bytes` é o que o jogo pediu para gravar a partir de `offset`;
// `diskFirstByte` é o byte 0 que está no disco agora (nullopt: arquivo vazio
// ou novo). Com a opção ligada, o 0xBE na RAM veio da leitura acima, então
// ele não vai para o disco: volta o byte do disco (0 num arquivo novo, o
// valor do jogo sem desbloqueio) e o CRC é refeito. Um CRC já inválido na
// gravação fica como veio. Devolve true se alterou `bytes`.
inline bool GuardMasterQuestUnlockOnSystemDataWrite(std::uint64_t offset,
                                                    std::span<std::uint8_t> bytes,
                                                    std::optional<std::uint8_t> diskFirstByte) noexcept {
    if (offset != 0U || bytes.size() < kSystemDataSize ||
        bytes[0] != kSystemDataMasterQuestUnlocked ||
        diskFirstByte == kSystemDataMasterQuestUnlocked || !IsValidSystemData(bytes))
        return false;
    bytes[0] = diskFirstByte.value_or(0U);
    StoreSystemDataChecksum(bytes);
    return true;
}

// "1"/"y"/"t" ou arquivo vazio ligam; "0"/"n"/"f" desligam.
[[nodiscard]] inline bool ParseMasterQuestUnlockSetting(const std::string &text) noexcept {
    for (const char c : text) {
        if (c == '0' || c == 'n' || c == 'N' || c == 'f' || c == 'F')
            return false;
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            return true;
    }
    return true;
}

namespace MasterQuestUnlockDetail {
inline int &TestOverride() noexcept {
    static int value = -1;
    return value;
}
} // namespace MasterQuestUnlockDetail

// Testes: força a opção (true/false) ou volta à leitura normal (nullopt).
inline void SetMasterQuestUnlockOptionForTesting(std::optional<bool> enabled) noexcept {
    MasterQuestUnlockDetail::TestOverride() = enabled ? (*enabled ? 1 : 0) : -1;
}

// Lido uma vez: no Switch, a presença de sdmc:/switch/oot3dre/config/
// unlock_master_quest liga (conteúdo "0" desliga); fora dele a variável
// TRIAEVUM_UNLOCK_MASTER_QUEST (com o mesmo formato).
[[nodiscard]] inline bool MasterQuestUnlockOptionEnabled() {
    if (const int forced = MasterQuestUnlockDetail::TestOverride(); forced >= 0)
        return forced != 0;
    static const bool enabled = [] {
#if defined(__SWITCH__)
        std::ifstream file("sdmc:/switch/oot3dre/config/unlock_master_quest");
        if (!file)
            return false;
        std::string text;
        std::getline(file, text);
        return ParseMasterQuestUnlockSetting(text);
#else
        const char *value = std::getenv("TRIAEVUM_UNLOCK_MASTER_QUEST");
        return value != nullptr && ParseMasterQuestUnlockSetting(value);
#endif
    }();
    return enabled;
}

} // namespace Oot3dNativeGame
