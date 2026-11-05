// Saves à prova de perda no backend de pasta (ver o .h e
// docs/pesquisas/2026-11-05-saves-seguros.md).
#include "oot3d_native_a32_ctr_save_safety.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>
#include <vector>

#if !defined(_WIN32) && !defined(__SWITCH__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Oot3dNativeGame {

namespace {

bool gRenameFailsOnExisting = false;

bool HasSuffix(const std::string& text, std::string_view suffix) {
    return text.size() > suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool IsJournalName(const std::string& name) {
    return HasSuffix(name, kCtrSaveJournalTemporarySuffix) ||
           HasSuffix(name, kCtrSaveJournalCommittedSuffix);
}

std::filesystem::path WithSuffix(const std::filesystem::path& file, std::string_view suffix) {
    return std::filesystem::path(file.string() + std::string(suffix));
}

std::string Timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &local);
    return text;
}

// base/name, ou base/name-2, -3...: nunca reaproveita uma pasta existente.
std::filesystem::path UnusedPath(const std::filesystem::path& base, const std::string& name) {
    std::error_code error;
    auto candidate = base / name;
    for (unsigned index = 2U; std::filesystem::exists(candidate, error); ++index)
        candidate = base / (name + "-" + std::to_string(index));
    return candidate;
}

// Cópia por stream (5 KB por arquivo): não depende do copy_file da libstdc++
// do devkitA64 (sendfile/permissões) no devoptab.
bool CopyBytes(const std::filesystem::path& source, const std::filesystem::path& destination) {
    std::ifstream input(source, std::ios::binary);
    if (!input) return false;
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (input.bad()) return false;
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    return !output.fail();
}

// fsync antes da troca. No Switch não há o que chamar além do que já foi
// feito: cada escrita abre, escreve e fecha o arquivo, e o fsFileClose da
// libnx descarrega no SD. No Windows o fechamento do stream basta.
bool SyncToDisk(const std::filesystem::path& path, bool directory) {
#if !defined(_WIN32) && !defined(__SWITCH__)
    const int descriptor = ::open(path.c_str(), directory ? (O_RDONLY | O_DIRECTORY) : O_RDONLY);
    if (descriptor < 0) return false;
    const bool synced = ::fsync(descriptor) == 0;
    ::close(descriptor);
    return synced;
#else
    static_cast<void>(path);
    static_cast<void>(directory);
    return true;
#endif
}

bool CopyTree(const std::filesystem::path& source, const std::filesystem::path& destination) {
    std::error_code error;
    std::filesystem::create_directories(destination, error);
    if (error) return false;
    for (std::filesystem::recursive_directory_iterator entry(source, error), end; !error && entry != end;
         entry.increment(error)) {
        const auto target = destination / entry->path().lexically_relative(source);
        if (entry->is_directory(error)) {
            std::filesystem::create_directories(target, error);
        } else if (!CopyBytes(entry->path(), target)) {
            return false;
        }
    }
    return !error;
}

// Gerações: AAAAMMDD-HHMMSS ou AAAAMMDD-HHMMSS-n. Devolve (data, n).
std::optional<std::pair<std::string, unsigned>> ParseGeneration(const std::string& name) {
    const auto digits = [&](size_t from, size_t count) {
        return name.size() >= from + count &&
               std::all_of(name.begin() + static_cast<std::ptrdiff_t>(from),
                           name.begin() + static_cast<std::ptrdiff_t>(from + count),
                           [](char c) { return c >= '0' && c <= '9'; });
    };
    if (!digits(0U, 8U) || name.size() < 15U || name[8] != '-' || !digits(9U, 6U)) return std::nullopt;
    if (name.size() == 15U) return std::make_pair(name, 1U);
    if (name[15] != '-' || !digits(16U, name.size() - 16U) || name.size() > 22U) return std::nullopt;
    return std::make_pair(name.substr(0U, 15U), static_cast<unsigned>(std::stoul(name.substr(16U))));
}

std::set<std::string> FilesIn(const std::filesystem::path& directory) {
    std::set<std::string> files;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator entry(directory, error), end; !error && entry != end;
         entry.increment(error)) {
        if (entry->is_regular_file(error))
            files.insert(entry->path().lexically_relative(directory).generic_string());
    }
    return files;
}

} // namespace

std::filesystem::path CtrLexicalDirectory(const std::filesystem::path& directory) {
    auto normal = directory.lexically_normal();
    while (!normal.empty() && !normal.has_filename() && normal != normal.root_path())
        normal = normal.parent_path();
    return normal;
}

std::filesystem::path CtrSaveFormatPath(const std::filesystem::path& saveDirectory) {
    return std::filesystem::path(CtrLexicalDirectory(saveDirectory).string() + ".format");
}

std::filesystem::path CtrSaveBackupRoot(const std::filesystem::path& saveDirectory) {
    const auto directory = CtrLexicalDirectory(saveDirectory);
    return directory.parent_path() / (directory.filename().string() + "-backups");
}

bool CtrPathWithin(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    if (root.empty() || candidate.empty()) return false;
    const auto within = candidate.lexically_normal().lexically_relative(CtrLexicalDirectory(root));
    return !within.empty() && within != "." && *within.begin() != "..";
}

bool CtrArchivePathContained(const std::filesystem::path& root, const std::filesystem::path& relative) {
    if (root.empty() || relative.empty() || relative.has_root_name() || relative.has_root_directory())
        return false;
    for (const auto& part : relative)
        if (part == "..") return false;
    const auto base = CtrLexicalDirectory(root);
    const auto target = (base / relative).lexically_normal();
    const auto within = target.lexically_relative(base);
    if (within.empty() || within == "." || *within.begin() == ".." || !target.has_filename()) return false;
    // Componentes intermediários (entre a raiz e o arquivo) não podem ser
    // symlink. A raiz em si pode (o usuário pode apontar savedata para outro
    // disco). No sdmc não existe symlink e o lstat vai direto ao devoptab.
    auto walk = base;
    auto part = within.begin();
    for (auto next = std::next(part); next != within.end(); ++part, ++next) {
        walk /= *part;
        std::error_code error;
        if (std::filesystem::symlink_status(walk, error).type() == std::filesystem::file_type::symlink)
            return false;
    }
    return true;
}

std::filesystem::path CtrSwitchSaveDataDirectory(bool oot3dTitle, std::optional<uint64_t> titleId) {
    if (oot3dTitle) return std::filesystem::path(kOot3dSwitchSaveDataDirectory);
    char id[17];
    std::snprintf(id, sizeof(id), "%016llx", static_cast<unsigned long long>(titleId.value_or(0U)));
    return std::filesystem::path(kTriAevumSwitchTitlesRoot) / id / "savedata";
}

std::filesystem::path ResolveCtrTitleSaveDataDirectory(const std::filesystem::path& requested,
                                                       bool oot3dTitle,
                                                       std::optional<uint64_t> titleId) {
    if (oot3dTitle) return requested;
    const std::filesystem::path oot3dSave(kOot3dSwitchSaveDataDirectory);
    const auto normal = CtrLexicalDirectory(requested);
    if (normal == oot3dSave || CtrPathWithin(oot3dSave, normal) || CtrPathWithin(normal, oot3dSave)) {
        const auto own = CtrSwitchSaveDataDirectory(false, titleId);
        std::fprintf(stderr,
                     "TRIAEVUM_SAVE_LAYOUT título %016llx não usa a pasta de save do OoT3D (%s); "
                     "usando %s\n",
                     static_cast<unsigned long long>(titleId.value_or(0U)), normal.string().c_str(),
                     own.string().c_str());
        return own;
    }
    return requested;
}

std::optional<std::array<uint32_t, 4>> CtrLegacySaveFormatInfo(const std::filesystem::path& saveDirectory) {
    uint64_t bytes = 0U;
    uint32_t files = 0U, directories = 0U;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator entry(saveDirectory, error), end;
         !error && entry != end; entry.increment(error)) {
        std::error_code typeError;
        if (entry->is_directory(typeError)) {
            ++directories;
        } else if (entry->is_regular_file(typeError) && !IsJournalName(entry->path().filename().string())) {
            ++files;
            bytes += entry->file_size(typeError);
        }
    }
    if (files == 0U) return std::nullopt;
    const uint64_t rounded = std::min<uint64_t>((bytes + 0x1FFU) & ~uint64_t{0x1FFU}, 0xFFFFFE00U);
    return std::array<uint32_t, 4>{static_cast<uint32_t>(rounded), directories, files, 0U};
}

void PruneCtrSaveBackups(const std::filesystem::path& backupRoot, size_t keep) {
    struct Generation {
        std::pair<std::string, unsigned> Order;
        std::filesystem::path Path;
        std::set<std::string> Files;
    };
    std::vector<Generation> generations;
    std::error_code error;
    for (std::filesystem::directory_iterator entry(backupRoot, error), end; !error && entry != end;
         entry.increment(error)) {
        std::error_code typeError;
        if (!entry->is_directory(typeError)) continue;
        if (auto order = ParseGeneration(entry->path().filename().string()))
            generations.push_back({*order, entry->path(), FilesIn(entry->path())});
    }
    std::sort(generations.begin(), generations.end(),
              [](const Generation& a, const Generation& b) { return a.Order < b.Order; });
    size_t index = 0U;
    while (generations.size() > keep && index + 1U < generations.size()) {
        const bool coveredLater = std::all_of(
            generations[index].Files.begin(), generations[index].Files.end(), [&](const std::string& file) {
                return std::any_of(generations.begin() + static_cast<std::ptrdiff_t>(index + 1U),
                                   generations.end(),
                                   [&](const Generation& later) { return later.Files.contains(file); });
            });
        if (!coveredLater) {  // única cópia de algum arquivo: fica
            ++index;
            continue;
        }
        std::error_code removeError;
        std::filesystem::remove_all(generations[index].Path, removeError);
        if (removeError) {
            ++index;
            continue;
        }
        generations.erase(generations.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

bool ReplaceCtrSaveFile(const std::filesystem::path& source, const std::filesystem::path& destination) {
    std::error_code error;
    if (gRenameFailsOnExisting && std::filesystem::exists(destination, error))
        error = std::make_error_code(std::errc::file_exists);
    else
        std::filesystem::rename(source, destination, error);
    if (error == std::errc::file_exists) {
        // Horizon: rename por cima de arquivo existente dá EEXIST. Entre o
        // remove e o rename a versão completa está em source (.triaevum-new),
        // que a recuperação instala se a execução cair aqui.
        std::error_code removeError;
        std::filesystem::remove(destination, removeError);
        error.clear();
        std::filesystem::rename(source, destination, error);
    }
    if (!error) SyncToDisk(CtrLexicalDirectory(destination.parent_path()), true);
    return !error;
}

void SetCtrSaveRenameFailsOnExistingForTesting(bool enabled) noexcept {
    gRenameFailsOnExisting = enabled;
}

CtrSaveJournal::CtrSaveJournal(std::filesystem::path saveDirectory)
    : mDirectory(CtrLexicalDirectory(saveDirectory)), mBackupRoot(CtrSaveBackupRoot(saveDirectory)) {}

std::string CtrSaveJournal::Key(const std::filesystem::path& file) const {
    return file.lexically_normal().generic_string();
}

bool CtrSaveJournal::Covers(const std::filesystem::path& file) const {
    return CtrPathWithin(mDirectory, file);
}

void CtrSaveJournal::RecoverOnce() {
    if (mRecovered) return;
    mRecovered = true;
    std::vector<std::filesystem::path> committed, temporary;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator entry(mDirectory, error), end; !error && entry != end;
         entry.increment(error)) {
        std::error_code typeError;
        if (!entry->is_regular_file(typeError)) continue;
        const auto name = entry->path().filename().string();
        if (HasSuffix(name, kCtrSaveJournalCommittedSuffix)) committed.push_back(entry->path());
        if (HasSuffix(name, kCtrSaveJournalTemporarySuffix)) temporary.push_back(entry->path());
    }
    // <nome>.triaevum-new só existe depois que o temporário foi escrito por
    // inteiro: é a versão confirmada e termina a troca.
    for (const auto& path : committed) {
        auto target = path.string();
        target.resize(target.size() - kCtrSaveJournalCommittedSuffix.size());
        const bool finished = ReplaceCtrSaveFile(path, target);
        std::fprintf(stderr, "TRIAEVUM_SAVE_RECOVERED troca interrompida de %s %s\n", target.c_str(),
                     finished ? "concluída" : "FALHOU");
    }
    // Temporário sem confirmação: escrita que não chegou ao Flush/Close. O
    // save continua o anterior; o temporário vai para os backups.
    if (temporary.empty()) return;
    const auto aside = UnusedPath(mBackupRoot, "incompleto-" + Timestamp());
    for (const auto& path : temporary) {
        const auto destination = aside / path.lexically_relative(mDirectory);
        std::error_code moveError;
        std::filesystem::create_directories(destination.parent_path(), moveError);
        std::filesystem::rename(path, destination, moveError);
        if (moveError && CopyBytes(path, destination)) {
            moveError.clear();
            std::filesystem::remove(path, moveError);
        }
        std::fprintf(stderr, "TRIAEVUM_SAVE_RECOVERED escrita não confirmada %s -> %s%s\n",
                     path.string().c_str(), destination.string().c_str(), moveError ? " (FALHOU)" : "");
    }
}

std::filesystem::path CtrSaveJournal::ActivePath(const std::filesystem::path& file) const {
    return mPending.contains(Key(file)) ? WithSuffix(file, kCtrSaveJournalTemporarySuffix) : file;
}

std::filesystem::path CtrSaveJournal::Generation() {
    if (mGeneration.empty()) mGeneration = UnusedPath(mBackupRoot, Timestamp());
    return mGeneration;
}

bool CtrSaveJournal::Backup(const std::filesystem::path& file, bool beforeDelete) {
    const auto key = Key(file);
    const bool already = mBackedUp.contains(key);
    if (already && !beforeDelete) return true;
    const auto source = ActivePath(file);
    std::error_code error;
    if (!std::filesystem::is_regular_file(source, error)) {
        mBackedUp.insert(key);  // arquivo novo nesta execução: nada a guardar
        return true;
    }
    const bool firstOfRun = mGeneration.empty();
    auto destination = Generation() / file.lexically_normal().lexically_relative(mDirectory);
    // Apagar depois de já ter escrito nesta execução: a geração tem a versão
    // de antes da execução; a de agora (com o progresso da sessão) vai junto.
    if (already) destination = WithSuffix(destination, ".apagado");
    if (!CopyBytes(source, destination)) {
        std::fprintf(stderr, "TRIAEVUM_SAVE_BACKUP_FAILED %s -> %s\n", source.string().c_str(),
                     destination.string().c_str());
        return false;
    }
    mBackedUp.insert(key);
    if (firstOfRun) PruneCtrSaveBackups(mBackupRoot, kCtrSaveBackupGenerations);
    return true;
}

bool CtrSaveJournal::BeginWrite(const std::filesystem::path& file) {
    RecoverOnce();
    const auto key = Key(file);
    if (mPending.contains(key)) return true;
    // Sem backup o save continua protegido pela troca atômica: não bloqueia.
    Backup(file, false);
    const auto temporary = WithSuffix(file, kCtrSaveJournalTemporarySuffix);
    std::error_code error;
    const bool copied = std::filesystem::exists(file, error)
                            ? CopyBytes(file, temporary)
                            : static_cast<bool>(std::ofstream(temporary, std::ios::binary | std::ios::trunc));
    if (!copied) return false;
    mPending.insert(key);
    return true;
}

bool CtrSaveJournal::Commit(const std::filesystem::path& file) {
    const auto key = Key(file);
    if (!mPending.contains(key)) return true;
    const auto temporary = WithSuffix(file, kCtrSaveJournalTemporarySuffix);
    const auto committed = WithSuffix(file, kCtrSaveJournalCommittedSuffix);
    if (!SyncToDisk(temporary, false)) return false;
    std::error_code error;
    std::filesystem::rename(temporary, committed, error);  // destino não existe: vale no Horizon
    if (error) return false;
    mPending.erase(key);
    if (!ReplaceCtrSaveFile(committed, file)) {
        std::fprintf(stderr, "TRIAEVUM_SAVE_COMMIT_FAILED %s (fica em %s até a próxima execução)\n",
                     file.string().c_str(), committed.string().c_str());
        return false;
    }
    return true;
}

bool CtrSaveJournal::CommitAll() {
    bool ok = true;
    const auto pending = mPending;
    for (const auto& key : pending) ok = Commit(std::filesystem::path(key)) && ok;
    return ok;
}

bool CtrSaveJournal::PrepareDelete(const std::filesystem::path& file) {
    RecoverOnce();
    if (!Backup(file, true)) return false;
    const auto key = Key(file);
    if (mPending.erase(key) != 0U) {
        std::error_code error;
        std::filesystem::remove(WithSuffix(file, kCtrSaveJournalTemporarySuffix), error);
    }
    return true;
}

bool CtrSaveJournal::MoveSaveAsideForFormat() {
    RecoverOnce();
    CommitAll();
    std::error_code error;
    if (!std::filesystem::exists(mDirectory, error) || std::filesystem::is_empty(mDirectory, error)) {
        return !error;
    }
    std::filesystem::create_directories(mBackupRoot, error);
    const auto destination = UnusedPath(mBackupRoot, "format-" + Timestamp());
    error.clear();
    std::filesystem::rename(mDirectory, destination, error);
    if (error) {
        // Sem rename de pasta: copia tudo, confere e só então esvazia.
        if (!CopyTree(mDirectory, destination) || FilesIn(destination) != FilesIn(mDirectory)) return false;
        error.clear();
        std::filesystem::remove_all(mDirectory, error);
        if (error) return false;
    }
    // O .format anterior vai junto, ao lado da cópia (format-<data>.format).
    if (std::filesystem::is_regular_file(CtrSaveFormatPath(mDirectory), error))
        CopyBytes(CtrSaveFormatPath(mDirectory), CtrSaveFormatPath(destination));
    std::fprintf(stderr, "TRIAEVUM_SAVE_FORMAT save anterior movido para %s\n", destination.string().c_str());
    mPending.clear();
    mBackedUp.clear();
    return true;
}

} // namespace Oot3dNativeGame
