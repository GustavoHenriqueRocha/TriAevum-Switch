#pragma once

// Option "unlock Master Quest without beating the game" (config/unlock_master_quest).
//
// Where OoT3D stores the unlock (canonical EUR code.bin; the file format is
// the same in the USA version): in the system file "data:/system.dat" of the
// save archive (34 bytes), not in the save00..05.bin slots.
//
//   +0x00  u8   0xBE = Master Quest unlocked (0x00 until the game is beaten)
//   +0x01  u8[30] random identifier (FUN_00435274 / FUN_0042231c)
//   +0x1F  u8   0xEF = the last choice on the version screen was Master Quest
//   +0x20  u16  CRC-16/ARC (reflected polynomial 0xA001, init 0) of the 34
//               bytes with this field zeroed (FUN_002faf90), little-endian
//
// In memory the file lives at 0x00587934 (EUR). Readers of the unlock:
//   FUN_003487ec  "unlocked?" = byte0 == 0xBE; called by EnMag_Init/
//                 Update (title logo), FUN_003480a8 (COmoteUraSelector:
//                 choice of two versions or only the normal one),
//                 FileChoose_Init and the game ending (0x457820, which writes
//                 0xBE via FUN_0047d7c4 and saves the file with FUN_002f3684
//                 when it was not set yet).
//   FUN_002e63c8  MQ mode active = byte0 == 0xBE && byte0x1F == 0xEF; selects
//                 slots 3..5 (FileSelect_GetSlotBuffer/IsSlotValid); the
//                 same inline test in FUN_00434f74/FUN_00483888 copies the mode
//                 into gSaveContext.masterQuestFlag (+0x0E).
//   The selector writes +0x1F (FUN_0030765c = 0xEF, FUN_00307668 = 0) in RAM only.
//
// The game reads the file only once at boot (async 34-byte read straight
// into 0x00587934, CRC checked in FUN_0042231c; bad CRC => zeroes it and
// generates a new identifier) and only writes it on first boot (default,
// byte0 0) and at the game ending. So the option acts in the host fs:USER
// service, without touching translated code: reading system.dat returns
// byte0 = 0xBE with the CRC recomputed, and writing system.dat never puts on
// disk a 0xBE the disk did not have. The file and the saves on the card stay
// as they were; removing the option restores the previous state (the MQ
// saves remain there).

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

// CRC-16/ARC (FUN_002faf90): reflected polynomial 0xA001, init 0.
[[nodiscard]] inline std::uint16_t Crc16Arc(std::span<const std::uint8_t> data) noexcept {
    std::uint16_t crc = 0U;
    for (const std::uint8_t value : data) {
        crc = static_cast<std::uint16_t>(crc ^ value);
        for (int bit = 0; bit < 8; ++bit)
            crc = static_cast<std::uint16_t>((crc & 1U) ? (crc >> 1U) ^ 0xA001U : crc >> 1U);
    }
    return crc;
}

// The system.dat CRC: the 34 bytes with the CRC field zeroed.
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

// The system file at the root of the save archive ("/system.dat").
[[nodiscard]] inline bool IsSystemDataPath(const std::filesystem::path &path) {
    return path.filename() == "system.dat";
}

// Read: `bytes` is what the host will return to the game starting at
// `offset`. Only touches a read of the whole file from the start with a
// valid CRC (a corrupted file stays corrupted, and the game handles it as
// always). Returns true if it set the unlock.
inline bool ApplyMasterQuestUnlockToSystemDataRead(std::uint64_t offset,
                                                   std::span<std::uint8_t> bytes) noexcept {
    if (offset != 0U || bytes.size() < kSystemDataSize || !IsValidSystemData(bytes) ||
        bytes[0] == kSystemDataMasterQuestUnlocked)
        return false;
    bytes[0] = kSystemDataMasterQuestUnlocked;
    StoreSystemDataChecksum(bytes);
    return true;
}

// Write: `bytes` is what the game asked to write starting at `offset`;
// `diskFirstByte` is the byte 0 currently on disk (nullopt: empty or new
// file). With the option on, the 0xBE in RAM came from the read above, so it
// does not go to disk: the disk byte is restored (0 in a new file, the game's
// value without the unlock) and the CRC is recomputed. A CRC already invalid
// on write is left as is. Returns true if it changed `bytes`.
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

// "1"/"y"/"t" or an empty file enable; "0"/"n"/"f" disable.
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

// Tests: force the option (true/false) or restore normal reading (nullopt).
inline void SetMasterQuestUnlockOptionForTesting(std::optional<bool> enabled) noexcept {
    MasterQuestUnlockDetail::TestOverride() = enabled ? (*enabled ? 1 : 0) : -1;
}

// Read once: on Switch, the presence of sdmc:/switch/oot3dre/config/
// unlock_master_quest enables it (content "0" disables); elsewhere the
// TRIAEVUM_UNLOCK_MASTER_QUEST variable (same format).
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
