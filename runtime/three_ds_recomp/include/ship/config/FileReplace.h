#pragma once

// Puts a finished temporary file in place of a config (or cache) file.
//
// POSIX rename() and Windows MoveFileEx replace an existing destination in one
// step. Horizon's FS (the Switch SD card) refuses with EEXIST instead. There
// the old file first moves aside to "<target>.bak", the temporary takes its
// name, and the backup goes. If the console loses power or the game is closed
// between the two moves, the target is missing and its backup holds the last
// complete file: RestoreInterruptedReplace() moves it back, and every loader
// of a file written this way calls it before it looks for the file.
//
// Header-only so the small config libraries (controls, TopScreen UI) use the
// same code as Ship::WriteConfigFileAtomically without linking the ship
// runtime.

#include <filesystem>
#include <system_error>

namespace Ship {

inline std::filesystem::path ReplaceBackupPath(const std::filesystem::path& target) {
    std::filesystem::path backup = target;
    backup += ".bak";
    return backup;
}

// Moves `temporary` onto `target`, replacing an existing file. On failure the
// previous target is still in place (or back in place) and `temporary` stays
// for the caller to remove.
inline std::error_code ReplaceFileWithTemporary(const std::filesystem::path& temporary,
                                                const std::filesystem::path& target) {
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    if (error != std::errc::file_exists) {
        return error;
    }
    std::error_code probe;
    if (!std::filesystem::is_regular_file(target, probe)) {
        return error;
    }
    // Horizon: no rename over an existing file. A backup left by an earlier
    // completed replace is stale (the target exists), so it goes first.
    const auto backup = ReplaceBackupPath(target);
    std::filesystem::remove(backup, probe);
    error.clear();
    std::filesystem::rename(target, backup, error);
    if (error) {
        return error;
    }
    std::filesystem::rename(temporary, target, error);
    if (error) {
        std::error_code restore;
        std::filesystem::rename(backup, target, restore);
        return error;
    }
    std::filesystem::remove(backup, probe);
    return {};
}

// Finishes the recovery of a replace cut off between its two moves: when the
// target is missing and its backup exists, the backup becomes the target
// again. Returns false only when a needed restore failed.
inline bool RestoreInterruptedReplace(const std::filesystem::path& target) {
    if (target.empty()) {
        return true;
    }
    const auto backup = ReplaceBackupPath(target);
    std::error_code error;
    if (!std::filesystem::is_regular_file(backup, error)) {
        return true;
    }
    if (std::filesystem::exists(target, error) || error) {
        return true;
    }
    std::filesystem::rename(backup, target, error);
    return !error;
}

} // namespace Ship
