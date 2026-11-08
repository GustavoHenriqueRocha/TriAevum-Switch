// Saves à prova de perda no backend de pasta (ver o .h e
// docs/pesquisas/2026-11-05-saves-seguros.md).
#include "oot3d_native_a32_ctr_save_safety.h"
#include "oot3d_game_log.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#if !defined(_WIN32) && !defined(__SWITCH__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Oot3dNativeGame {

namespace {

// TRIAEVUM_SAVE_* lines go to stderr and to the game log
// (logs/oot3d_native_game.log): on the Switch stderr is a null device unless
// nxlink is connected.
void SaveLog(const char* format, ...) {
    char line[1024];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    std::fprintf(stderr, "%s\n", line);
    WriteGameLog(line);
}

bool gRenameFailsOnExisting = false;
std::function<bool(const std::filesystem::path&, const std::filesystem::path&)> gRenameFault;
std::function<void(const std::filesystem::path&)> gCopyFault;

// rename com os ganchos de teste: falha injetada (EIO) e a semântica do
// Horizon (destino existente -> EEXIST).
void RenamePath(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& error) {
    error.clear();
    if (gRenameFault && gRenameFault(from, to)) {
        error = std::make_error_code(std::errc::io_error);
        return;
    }
    if (gRenameFailsOnExisting && std::filesystem::exists(to, error)) {
        error = std::make_error_code(std::errc::file_exists);
        return;
    }
    error.clear();
    std::filesystem::rename(from, to, error);
}

bool HasSuffix(const std::string& text, std::string_view suffix) {
    return text.size() > suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool IsJournalName(const std::string& name) {
    return HasSuffix(name, kCtrSaveJournalTemporarySuffix) ||
           HasSuffix(name, kCtrSaveJournalCommittedSuffix) ||
           HasSuffix(name, kCtrSaveJournalPreviousSuffix);
}

std::filesystem::path WithSuffix(const std::filesystem::path& file, std::string_view suffix) {
    return std::filesystem::path(file.string() + std::string(suffix));
}

std::filesystem::path WithoutSuffix(const std::filesystem::path& file, std::string_view suffix) {
    auto text = file.string();
    text.resize(text.size() - suffix.size());
    return std::filesystem::path(text);
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

// FNV-1a de 64 bits: checksum da conferência das cópias.
uint64_t Checksum(const std::vector<char>& bytes) {
    uint64_t hash = 0xCBF29CE484222325ULL;
    for (const char byte : bytes) {
        hash ^= static_cast<uint8_t>(byte);
        hash *= 0x100000001B3ULL;
    }
    return hash;
}

bool ReadBytes(const std::filesystem::path& path, std::vector<char>& bytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return !input.bad();
}

// Cópia por stream (5 KB por arquivo): não depende do copy_file da libstdc++
// do devkitA64 (sendfile/permissões) no devoptab. A cópia só vale depois de
// relida e conferida contra a origem (tamanho + checksum): SD cheio grava
// uma cópia truncada sem erro visível em todo lugar. Cópia que não confere é
// apagada (é cópia de um original que continua intacto).
bool CopyBytes(const std::filesystem::path& source, const std::filesystem::path& destination) {
    std::vector<char> bytes;
    if (!ReadBytes(source, bytes)) return false;
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    {
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
    }
    if (gCopyFault) gCopyFault(destination);
    std::vector<char> copy;
    const bool verified = ReadBytes(destination, copy) && copy.size() == bytes.size() &&
                          Checksum(copy) == Checksum(bytes);
    if (!verified) {
        SaveLog("TRIAEVUM_SAVE_COPY_MISMATCH %s -> %s (%zu of %zu bytes; copy discarded)",
                source.string().c_str(), destination.string().c_str(), copy.size(), bytes.size());
        std::filesystem::remove(destination, error);
    }
    return verified;
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

// Cópia de backup: grava <destino>.triaevum-tmp, confere e só então dá o nome
// final. Uma queda no meio deixa só o .triaevum-tmp, que o prune não conta.
bool CopyBackup(const std::filesystem::path& source, std::filesystem::path destination) {
    std::error_code error;
    if (std::filesystem::exists(destination, error))
        destination = UnusedPath(destination.parent_path(), destination.filename().string());
    const auto partial = WithSuffix(destination, kCtrSaveJournalTemporarySuffix);
    if (!CopyBytes(source, partial)) return false;
    RenamePath(partial, destination, error);
    if (error) {
        std::filesystem::remove(partial, error);
        return false;
    }
    return true;
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

// Arquivos de uma árvore. Sem withJournal, sobras do diário e de cópia
// interrompida (*.triaevum-tmp/-new/-old) não contam.
std::set<std::string> FilesIn(const std::filesystem::path& directory, bool withJournal) {
    std::set<std::string> files;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator entry(directory, error), end; !error && entry != end;
         entry.increment(error)) {
        if (entry->is_regular_file(error) && (withJournal || !IsJournalName(entry->path().filename().string())))
            files.insert(entry->path().lexically_relative(directory).generic_string());
    }
    return files;
}

std::vector<std::string> ReadBackupOrder(const std::filesystem::path& backupRoot) {
    std::vector<std::string> names;
    std::ifstream input(backupRoot / std::string(kCtrSaveBackupOrderFile));
    for (std::string line; std::getline(input, line);)
        if (ParseGeneration(line)) names.push_back(line);  // linha cortada por queda não conta
    return names;
}

void AppendBackupOrder(const std::filesystem::path& backupRoot, const std::string& name) {
    std::ofstream output(backupRoot / std::string(kCtrSaveBackupOrderFile), std::ios::app);
    output << name << '\n';
}

// Põe os arquivos (de dentro de directory) em <backups>/incompleto-<data>/,
// com o mesmo caminho relativo. Nunca apaga sem antes ter a cópia conferida.
void MoveAside(const std::filesystem::path& directory, const std::filesystem::path& backupRoot,
               const std::vector<std::filesystem::path>& files, const char* reason) {
    if (files.empty()) return;
    const auto aside = UnusedPath(backupRoot, "incompleto-" + Timestamp());
    for (const auto& path : files) {
        const auto destination = aside / path.lexically_relative(directory);
        std::error_code moveError;
        std::filesystem::create_directories(destination.parent_path(), moveError);
        std::filesystem::rename(path, destination, moveError);
        if (moveError && CopyBytes(path, destination)) {
            moveError.clear();
            std::filesystem::remove(path, moveError);
        }
        SaveLog("TRIAEVUM_SAVE_RECOVERED %s %s -> %s%s", reason, path.string().c_str(),
                destination.string().c_str(), moveError ? " (FAILED)" : "");
    }
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

void PruneCtrSaveBackups(const std::filesystem::path& backupRoot, size_t keep,
                         const std::filesystem::path& keepGeneration) {
    struct Generation {
        // (posição no arquivo de ordem, ou -1 se não está nele; data; n)
        std::tuple<long, std::string, unsigned> Order;
        std::filesystem::path Path;
        std::set<std::string> Files;
        bool Protected = false;
    };
    const auto order = ReadBackupOrder(backupRoot);
    std::vector<Generation> generations;
    std::error_code error;
    for (std::filesystem::directory_iterator entry(backupRoot, error), end; !error && entry != end;
         entry.increment(error)) {
        std::error_code typeError;
        if (!entry->is_directory(typeError)) continue;
        const auto name = entry->path().filename().string();
        const auto parsed = ParseGeneration(name);
        if (!parsed) continue;
        const auto listed = std::find(order.begin(), order.end(), name);
        const long rank = listed == order.end() ? -1L : static_cast<long>(listed - order.begin());
        generations.push_back({{rank, parsed->first, parsed->second},
                               entry->path(),
                               FilesIn(entry->path(), false),
                               !keepGeneration.empty() && keepGeneration.filename() == entry->path().filename()});
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
        if (generations[index].Protected || !coveredLater) {  // recém-criada ou única cópia: fica
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
    RenamePath(source, destination, error);
    if (error == std::errc::file_exists) {
        // Horizon: rename por cima de arquivo existente dá EEXIST. Nunca
        // remover o destino antes do novo entrar: ele vira <destino>.triaevum-old
        // e só sai depois; se o rename do novo falha, volta para o lugar.
        // Queda no meio: a recuperação instala o .new e devolve o .old.
        const auto previous = WithSuffix(destination, kCtrSaveJournalPreviousSuffix);
        std::error_code asideError;
        RenamePath(destination, previous, asideError);
        if (asideError == std::errc::file_exists) {
            // .old de uma troca anterior: o destino é mais novo que ele.
            std::error_code staleError;
            std::filesystem::remove(previous, staleError);
            RenamePath(destination, previous, asideError);
        }
        if (asideError) return false;  // destino e source intactos
        RenamePath(source, destination, error);
        if (error) {
            std::error_code backError;
            RenamePath(previous, destination, backError);
            SaveLog("TRIAEVUM_SAVE_REPLACE_FAILED %s: %s", destination.string().c_str(),
                    backError ? "versions in .triaevum-old and .triaevum-new until the recovery"
                              : "previous version put back; the new one stays in .triaevum-new");
            return false;
        }
        std::error_code removeError;
        std::filesystem::remove(previous, removeError);
    }
    if (!error) SyncToDisk(CtrLexicalDirectory(destination.parent_path()), true);
    return !error;
}

void SetCtrSaveRenameFailsOnExistingForTesting(bool enabled) noexcept {
    gRenameFailsOnExisting = enabled;
}

void SetCtrSaveRenameFaultForTesting(
    std::function<bool(const std::filesystem::path& from, const std::filesystem::path& to)> fault) {
    gRenameFault = std::move(fault);
}

void SetCtrSaveCopyFaultForTesting(std::function<void(const std::filesystem::path& destination)> fault) {
    gCopyFault = std::move(fault);
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
    std::vector<std::filesystem::path> committed, temporary, previous;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator entry(mDirectory, error), end; !error && entry != end;
         entry.increment(error)) {
        std::error_code typeError;
        if (!entry->is_regular_file(typeError)) continue;
        const auto name = entry->path().filename().string();
        if (HasSuffix(name, kCtrSaveJournalCommittedSuffix)) committed.push_back(entry->path());
        if (HasSuffix(name, kCtrSaveJournalTemporarySuffix)) temporary.push_back(entry->path());
        if (HasSuffix(name, kCtrSaveJournalPreviousSuffix)) previous.push_back(entry->path());
    }
    // <nome>.triaevum-new só existe depois que o temporário foi escrito por
    // inteiro: é a versão confirmada e termina a troca.
    for (const auto& path : committed) {
        const auto target = WithoutSuffix(path, kCtrSaveJournalCommittedSuffix);
        const bool finished = ReplaceCtrSaveFile(path, target);
        if (!finished) mStranded.insert(Key(target));  // a E/S lê o .new até instalar
        SaveLog("TRIAEVUM_SAVE_RECOVERED interrupted replace of %s %s", target.string().c_str(),
                finished ? "finished" : "FAILED (reading from .triaevum-new)");
    }
    // <nome>.triaevum-old: versão anterior tirada do lugar por uma troca. Se o
    // arquivo sumiu (queda entre os dois renames, sem .new), ela volta; se o
    // arquivo está lá, ele é mais novo e o .old sobrou.
    for (const auto& path : previous) {
        const auto target = WithoutSuffix(path, kCtrSaveJournalPreviousSuffix);
        std::error_code fixError;
        const bool present = std::filesystem::exists(target, fixError);
        if (present) std::filesystem::remove(path, fixError);
        else std::filesystem::rename(path, target, fixError);
        SaveLog("TRIAEVUM_SAVE_RECOVERED %s %s%s", target.string().c_str(),
                present ? "leftover previous version removed" : "previous version put back",
                fixError ? " (FAILED)" : "");
    }
    // Temporário sem confirmação: escrita que não chegou ao Flush/Close. O
    // save continua o anterior; o temporário vai para os backups.
    MoveAside(mDirectory, mBackupRoot, temporary, "escrita não confirmada");
}

std::filesystem::path CtrSaveJournal::ActivePath(const std::filesystem::path& file) const {
    const auto key = Key(file);
    if (mPending.contains(key)) return WithSuffix(file, kCtrSaveJournalTemporarySuffix);
    if (mStranded.contains(key)) return WithSuffix(file, kCtrSaveJournalCommittedSuffix);
    return file;
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
    auto destination = Generation() / file.lexically_normal().lexically_relative(mDirectory);
    // Apagar depois de já ter escrito nesta execução: a geração tem a versão
    // de antes da execução; a de agora (com o progresso da sessão) vai junto.
    if (already) destination = WithSuffix(destination, ".apagado");
    if (!CopyBackup(source, destination)) {
        SaveLog("TRIAEVUM_SAVE_BACKUP_FAILED %s -> %s", source.string().c_str(), destination.string().c_str());
        if (std::filesystem::is_empty(mGeneration, error) && !error) std::filesystem::remove(mGeneration, error);
        return false;
    }
    mBackedUp.insert(key);
    SaveLog("TRIAEVUM_SAVE_BACKUP %s -> %s", source.string().c_str(), destination.string().c_str());
    if (!mGenerationRegistered) {
        // Primeira cópia conferida da execução: entra na ordem e roda o prune
        // (que nunca apaga esta geração).
        mGenerationRegistered = true;
        AppendBackupOrder(mBackupRoot, mGeneration.filename().string());
        PruneCtrSaveBackups(mBackupRoot, kCtrSaveBackupGenerations, mGeneration);
    }
    return true;
}

bool CtrSaveJournal::InstallStranded(const std::string& key) {
    const std::filesystem::path file(key);
    if (!ReplaceCtrSaveFile(WithSuffix(file, kCtrSaveJournalCommittedSuffix), file)) return false;
    mStranded.erase(key);
    return true;
}

bool CtrSaveJournal::BeginWrite(const std::filesystem::path& file, uint64_t& writer) {
    RecoverOnce();
    if (writer == 0U) writer = mNextWriter++;
    const auto key = Key(file);
    if (auto pending = mPending.find(key); pending != mPending.end()) {
        pending->second.Writers.insert(writer);
        return true;
    }
    // Versão confirmada que ainda não entrou (rename falhou): termina antes,
    // senão a nova transação partiria do arquivo antigo e o .new seria
    // sobrescrito. Se ainda não dá, a escrita falha e o .new fica.
    if (mStranded.contains(key) && !InstallStranded(key)) return false;
    // Sem backup o save continua protegido pela troca atômica: não bloqueia.
    Backup(file, false);
    const auto temporary = WithSuffix(file, kCtrSaveJournalTemporarySuffix);
    std::error_code error;
    const bool copied = std::filesystem::exists(file, error)
                            ? CopyBytes(file, temporary)
                            : static_cast<bool>(std::ofstream(temporary, std::ios::binary | std::ios::trunc));
    if (!copied) {
        std::filesystem::remove(temporary, error);  // cópia parcial de um original intacto
        return false;
    }
    mPending[key].Writers.insert(writer);
    return true;
}

void CtrSaveJournal::MarkFailed(const std::filesystem::path& file) {
    if (auto pending = mPending.find(Key(file)); pending != mPending.end()) pending->second.Failed = true;
}

bool CtrSaveJournal::CommitNow(const std::string& key) {
    const auto pending = mPending.find(key);
    if (pending == mPending.end()) return true;
    const std::filesystem::path file(key);
    const auto temporary = WithSuffix(file, kCtrSaveJournalTemporarySuffix);
    if (pending->second.Failed) {
        // Write/SetSize falhou: o temporário tem buraco ou tamanho errado.
        // Nunca é instalado; o save continua o anterior.
        mPending.erase(pending);
        MoveAside(mDirectory, mBackupRoot, {temporary}, "transação com escrita que falhou");
        return false;
    }
    const auto committed = WithSuffix(file, kCtrSaveJournalCommittedSuffix);
    if (!SyncToDisk(temporary, false)) return false;
    std::error_code error;
    RenamePath(temporary, committed, error);  // destino não existe: vale no Horizon
    if (error) return false;
    mPending.erase(pending);
    if (!ReplaceCtrSaveFile(committed, file)) {
        mStranded.insert(key);
        SaveLog("TRIAEVUM_SAVE_COMMIT_FAILED %s (kept in %s until it is installed)", file.string().c_str(),
                committed.string().c_str());
        return false;
    }
    return true;
}

bool CtrSaveJournal::Flush(const std::filesystem::path& file, uint64_t writer) {
    const auto key = Key(file);
    const auto pending = mPending.find(key);
    if (pending == mPending.end() || !pending->second.Writers.contains(writer)) return true;
    if (pending->second.Writers.size() > 1U) return !pending->second.Failed;  // outro escritor ainda no meio
    return CommitNow(key);
}

bool CtrSaveJournal::Close(const std::filesystem::path& file, uint64_t writer) {
    const auto key = Key(file);
    const auto pending = mPending.find(key);
    if (pending == mPending.end() || pending->second.Writers.erase(writer) == 0U) return true;
    if (!pending->second.Writers.empty()) return !pending->second.Failed;
    return CommitNow(key);
}

bool CtrSaveJournal::CommitAll() {
    bool ok = true;
    std::vector<std::string> keys;
    for (const auto& [key, transaction] : mPending) keys.push_back(key);
    for (const auto& key : keys) ok = CommitNow(key) && ok;
    return ok;
}

void CtrSaveJournal::RetainWriters(const std::set<uint64_t>& writers) {
    for (auto& [key, transaction] : mPending)
        std::erase_if(transaction.Writers, [&](uint64_t writer) { return !writers.contains(writer); });
    if (!writers.empty()) mNextWriter = std::max(mNextWriter, *writers.rbegin() + 1U);
}

bool CtrSaveJournal::PrepareDelete(const std::filesystem::path& file) {
    RecoverOnce();
    if (!Backup(file, true)) return false;
    const auto key = Key(file);
    std::error_code error;
    if (mPending.erase(key) != 0U) std::filesystem::remove(WithSuffix(file, kCtrSaveJournalTemporarySuffix), error);
    // .new que não entrou: já está no backup (.apagado); não pode voltar na
    // recuperação depois do DeleteFile.
    if (mStranded.erase(key) != 0U) std::filesystem::remove(WithSuffix(file, kCtrSaveJournalCommittedSuffix), error);
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
        if (!CopyTree(mDirectory, destination) || FilesIn(destination, true) != FilesIn(mDirectory, true))
            return false;
        error.clear();
        std::filesystem::remove_all(mDirectory, error);
        if (error) return false;
    }
    // O .format anterior vai junto, ao lado da cópia (format-<data>.format).
    if (std::filesystem::is_regular_file(CtrSaveFormatPath(mDirectory), error))
        CopyBytes(CtrSaveFormatPath(mDirectory), CtrSaveFormatPath(destination));
    SaveLog("TRIAEVUM_SAVE_FORMAT previous save moved to %s", destination.string().c_str());
    mPending.clear();
    mStranded.clear();
    mBackedUp.clear();
    return true;
}

} // namespace Oot3dNativeGame
