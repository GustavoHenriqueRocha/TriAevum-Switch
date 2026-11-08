#pragma once

// Loss-proof saves on the directory backend (PC and the Switch NRO, which share
// the same code on top of libnx's devoptab). Decisions and rejected alternatives:
// docs/SAVE_SAFETY.pt-BR.md.
//
// - Atomic writes: the first write of a transaction copies the file to
//   <name>.triaevum-tmp and all I/O goes to that copy. Commit rule (per
//   transaction and handle): a file's transaction is committed when the
//   LAST handle that wrote to it Flushes or closes (FSFILE_Close or
//   svcCloseHandle), or on ControlArchive (whole-archive commit, like the
//   3DS journaled save commit). A read-only handle never
//   commits. A failed Write/SetSize (SD full) marks the transaction as
//   failed: it is never installed; it goes to <save>-backups/incompleto-*.
//   Commit = tmp -> <name>.triaevum-new -> <name>. Horizon's rename
//   does not overwrite (EEXIST): the destination becomes <name>.triaevum-old, the new
//   file goes in, and only then is the .old removed; if the new rename fails, the .old is restored.
//   Recovery (first save access of the run: OpenArchive,
//   OpenFile, I/O, GetFormatInfo or savestate restore) installs the .new,
//   restores the .old if the file vanished and moves the uncommitted temporary
//   out of the save (to incompleto-*, never deleted).
// - Rotating backup: before the first write (or DeleteFile) of each
//   file in a run, the previous version goes to
//   <save>-backups/<YYYYMMDD-HHMMSS>/. The copy is written as
//   <name>.triaevum-tmp, verified (size + checksum against the source) and only
//   then renamed; a copy that fails verification is discarded and does not count as a
//   copy. Generation order comes from <save>-backups/.triaevum-geracoes
//   (creation order), not from the date in the name (the console clock can
//   go back). The last kCtrSaveBackupGenerations are kept, never deleting a
//   generation holding the only copy of a file nor the just-created one.
// - Lossless format: FormatSaveData moves the directory to
//   <save>-backups/format-<YYYYMMDD-HHMMSS>/ before recreating it.

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace Oot3dNativeGame {

inline constexpr std::string_view kCtrSaveJournalTemporarySuffix = ".triaevum-tmp";
inline constexpr std::string_view kCtrSaveJournalCommittedSuffix = ".triaevum-new";
inline constexpr std::string_view kCtrSaveJournalPreviousSuffix = ".triaevum-old";
// Generation creation order (one name per line), inside <save>-backups.
inline constexpr std::string_view kCtrSaveBackupOrderFile = ".triaevum-geracoes";
inline constexpr size_t kCtrSaveBackupGenerations = 20U;

// Directory without trailing separator, normalized lexically only (works with
// "sdmc:/...", which devkitA64's std::filesystem treats as relative).
std::filesystem::path CtrLexicalDirectory(const std::filesystem::path& directory);
// <dir>.format, next to the directory (never inside it, even with a trailing '/').
std::filesystem::path CtrSaveFormatPath(const std::filesystem::path& saveDirectory);
// <parent>/<directory name>-backups (savedata -> savedata-backups).
std::filesystem::path CtrSaveBackupRoot(const std::filesystem::path& saveDirectory);
// relative (already decoded, no '..' or root) stays inside root, by
// lexical comparison; no intermediate component may be a symlink (on
// PC a symlinked subdirectory pointing outside is rejected). No
// absolute/canonical: with a "sdmc:/..." root they prepend the cwd on the Switch.
bool CtrArchivePathContained(const std::filesystem::path& root, const std::filesystem::path& relative);
// candidate (absolute or not) is inside root, lexically.
bool CtrPathWithin(const std::filesystem::path& root, const std::filesystem::path& candidate);
// Synthesized format of a legacy save (directory with files and no .format):
// {size, dirs, files, duplicate}. nullopt if the directory has no file.
std::optional<std::array<uint32_t, 4>> CtrLegacySaveFormatInfo(const std::filesystem::path& saveDirectory);
// Deletes the oldest generations beyond keep, never deleting a generation that
// holds the only copy of some file nor keepGeneration (the just-created one).
// Only touches YYYYMMDD-HHMMSS[-n] directories. Order: those not in
// kCtrSaveBackupOrderFile (older than the order file) by name, then
// the listed ones in file order. Copy leftovers (*.triaevum-tmp) do not count.
void PruneCtrSaveBackups(const std::filesystem::path& backupRoot, size_t keep,
                         const std::filesystem::path& keepGeneration = {});
// Replaces destination with source. Tries rename; if the destination exists and rename
// does not overwrite (Horizon: EEXIST), the destination becomes <destination>.triaevum-old,
// source goes in and the .old is removed; if the source rename fails, the .old goes back
// in place. destination is never left without either version on disk.
bool ReplaceCtrSaveFile(const std::filesystem::path& source, const std::filesystem::path& destination);
// PC tests: simulates Horizon's rename (existing destination -> EEXIST).
void SetCtrSaveRenameFailsOnExistingForTesting(bool enabled) noexcept;
// Tests: the rename from from to to fails (EIO) when fault returns true.
void SetCtrSaveRenameFaultForTesting(
    std::function<bool(const std::filesystem::path& from, const std::filesystem::path& to)> fault);
// Tests: called after writing each copy (backup or temporary), before
// verification; allows corrupting the copy (an SD that writes wrong data).
void SetCtrSaveCopyFaultForTesting(std::function<void(const std::filesystem::path& destination)> fault);

// State of one run over a save directory (survives a savestate
// restore: the host passes the same object along).
class CtrSaveJournal {
  public:
    explicit CtrSaveJournal(std::filesystem::path saveDirectory);

    const std::filesystem::path& Directory() const noexcept { return mDirectory; }
    // Is file a save file (not extdata/RomFS)?
    bool Covers(const std::filesystem::path& file) const;
    // Once per run: finishes interrupted swaps and sets aside
    // uncommitted temporaries. The host calls it when creating the journal, i.e.
    // on the first save access of the run (including restore).
    void RecoverOnce();
    // Where to read/write now: the temporary if a write is pending; the
    // .triaevum-new if this run's swap could not install it.
    std::filesystem::path ActivePath(const std::filesystem::path& file) const;
    // Before writing/resizing: backup (once per run), copy to the
    // temporary (once per transaction) and register the handle as a writer.
    // writer = handle identifier (0: the journal assigns a new one).
    bool BeginWrite(const std::filesystem::path& file, uint64_t& writer);
    // Write/SetSize failed after BeginWrite: the transaction is not installed.
    void MarkFailed(const std::filesystem::path& file);
    // FSFILE_Flush of handle writer: commits if it is the only writer.
    bool Flush(const std::filesystem::path& file, uint64_t writer);
    // FSFILE_Close/svcCloseHandle of handle writer: commits if it was the last.
    bool Close(const std::filesystem::path& file, uint64_t writer);
    // ControlArchive(commit): commits all (fails if any failed).
    bool CommitAll();
    // Savestate restore: forgets writers that no longer exist.
    void RetainWriters(const std::set<uint64_t>& writers);
    // DeleteFile: keeps the current version in the backups and discards the transaction.
    bool PrepareDelete(const std::filesystem::path& file);
    // FormatSaveData: commits pending writes and moves the directory (if non-empty) to
    // <save>-backups/format-<date>. False = do not format.
    bool MoveSaveAsideForFormat();

  private:
    struct Transaction {
        std::set<uint64_t> Writers;
        bool Failed = false;
    };
    std::string Key(const std::filesystem::path& file) const;
    bool Backup(const std::filesystem::path& file, bool beforeDelete);
    std::filesystem::path Generation();
    // Commits the transaction (or, if it failed, sets the temporary aside).
    bool CommitNow(const std::string& key);
    // Finishes installing a .triaevum-new left behind.
    bool InstallStranded(const std::string& key);

    std::filesystem::path mDirectory;
    std::filesystem::path mBackupRoot;
    std::filesystem::path mGeneration;
    bool mGenerationRegistered = false;
    std::map<std::string, Transaction> mPending;
    std::set<std::string> mStranded;
    std::set<std::string> mBackedUp;
    uint64_t mNextWriter = 1U;
    bool mRecovered = false;
};

} // namespace Oot3dNativeGame
