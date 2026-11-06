// fs:USER OpenDirectory e a sessão de diretório (Read/Close).
//
// Semântica do Azahar: SaveDataArchive::OpenDirectory (erros por estado do
// caminho), DiskDirectory::Read (entrada de 0x228 bytes, nome UTF-16 + 8.3 do
// FileUtil::SplitFilename83) e Service::FS::Directory (IPC). Só o backend de
// pasta (save e extdata); com NativeA32CtrFilesystem o pedido é falha nomeada.
//
// O diretório é relido a cada Read, em ordem de nome, e FileOffset guarda
// quantas entradas já foram entregues: o savestate só precisa do caminho e do
// cursor. Arquivos do diário de saves (oot3d_native_a32_ctr_save_safety.h)
// são do host e não aparecem; o tamanho é o do arquivo ativo do diário, o
// mesmo que o File:GetSize devolve.

#include "oot3d_native_a32_ctr_host.h"

#include "oot3d_native_a32_ctr_host_internal.h"
#include "oot3d_native_a32_ctr_save_safety.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace Oot3dNativeGame {

namespace {

constexpr size_t kDirectoryNameUnits = 0x106U;

struct DirectoryEntry {
    std::filesystem::path Name;
    bool Directory = false;
    uint64_t Size = 0;
};

bool IsSaveJournalName(std::string_view name) {
    for (const auto suffix : {kCtrSaveJournalTemporarySuffix, kCtrSaveJournalCommittedSuffix,
                              kCtrSaveJournalPreviousSuffix}) {
        if (name.size() > suffix.size() && name.ends_with(suffix)) return true;
    }
    return false;
}

std::vector<DirectoryEntry> ListDirectory(const std::filesystem::path& directory,
                                          CtrSaveJournal* journal) {
    std::vector<DirectoryEntry> entries;
    std::error_code error;
    for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end;
         it.increment(error)) {
        const auto name = it->path().filename();
        if (journal != nullptr && IsSaveJournalName(name.string())) continue;
        std::error_code typeError;
        const auto status = it->symlink_status(typeError);
        if (typeError) continue;
        DirectoryEntry entry{name, std::filesystem::is_directory(status), 0U};
        if (!entry.Directory) {
            if (!std::filesystem::is_regular_file(status)) continue;
            const auto active = journal != nullptr ? journal->ActivePath(it->path()) : it->path();
            entry.Size = std::filesystem::file_size(active, typeError);
            if (typeError) continue;
        }
        entries.push_back(std::move(entry));
    }
    std::sort(entries.begin(), entries.end(), [](const DirectoryEntry& a, const DirectoryEntry& b) {
        return a.Name.string() < b.Name.string();
    });
    return entries;
}

std::u16string NameUtf16(const std::filesystem::path& name) {
    try {
        return name.u16string();
    } catch (const std::exception&) {
        // Nome do host que não é UTF-8 válido: byte a byte.
        const std::string bytes = name.string();
        return std::u16string(bytes.begin(), bytes.end());
    }
}

// FileUtil::SplitFilename83 do Azahar.
void SplitFilename83(const std::string& name, uint8_t* shortName, uint8_t* extension) {
    constexpr std::string_view forbidden = ".\"/\\[]:;=, ";
    std::fill_n(shortName, 8, static_cast<uint8_t>(' '));
    std::fill_n(extension, 3, static_cast<uint8_t>(' '));
    auto point = name.rfind('.');
    if (point == name.size() - 1U) point = name.rfind('.', point);
    size_t used = 0;
    for (const char letter : name.substr(0, point)) {
        if (forbidden.find(letter) != std::string_view::npos) continue;
        if (used == 8U) {
            shortName[6] = '~';
            shortName[7] = '1';
            break;
        }
        shortName[used++] = static_cast<uint8_t>(std::toupper(static_cast<unsigned char>(letter)));
    }
    if (point != std::string::npos) {
        used = 0;
        for (const char letter : name.substr(point + 1U, 3U))
            extension[used++] = static_cast<uint8_t>(std::toupper(static_cast<unsigned char>(letter)));
    }
}

std::array<uint8_t, kDirectoryEntrySize> EncodeEntry(const DirectoryEntry& entry) {
    std::array<uint8_t, kDirectoryEntrySize> raw{};
    const auto name = NameUtf16(entry.Name);
    const size_t units = std::min(name.size(), kDirectoryNameUnits - 1U);
    for (size_t i = 0; i < units; ++i) {
        raw[i * 2U] = static_cast<uint8_t>(name[i] & 0xFFU);
        raw[i * 2U + 1U] = static_cast<uint8_t>(name[i] >> 8U);
    }
    const std::string narrow = entry.Name.string();
    SplitFilename83(narrow, &raw[0x20C], &raw[0x216]);
    raw[0x21C] = entry.Directory ? 1U : 0U;
    raw[0x21D] = !narrow.empty() && narrow[0] == '.' ? 1U : 0U;
    raw[0x21E] = entry.Directory ? 0U : 1U;  // SD com o bit de arquivo nunca limpo
    for (size_t i = 0; i < 8U; ++i) raw[0x220 + i] = static_cast<uint8_t>(entry.Size >> (i * 8U));
    return raw;
}

} // namespace

NativeA32HostResult NativeA32CtrHostServices::OpenFsDirectory(
    oot3d::recomp::a32::GuestState& state, NativeA32Memory& memory,
    const std::optional<std::string>& sessionType, uint32_t commandBuffer,
    NativeA32CtrSvcEvent event) {
    uint64_t archiveHandle = 0;
    uint32_t pathType = 0, pathSize = 0, descriptor = 0, pathAddress = 0;
    if (!memory.Read64(commandBuffer + 4U, &archiveHandle, nullptr) ||
        !memory.Read32(commandBuffer + 12U, &pathType) ||
        !memory.Read32(commandBuffer + 16U, &pathSize) ||
        !memory.Read32(commandBuffer + 20U, &descriptor) ||
        !memory.Read32(commandBuffer + 24U, &pathAddress) ||
        (descriptor & 0x3FFU) != 2U || (descriptor >> 14U) != pathSize) {
        return FailFsRequest(state, memory, commandBuffer, sessionType, event,
                             kFsInvalidPath, "open-directory-path-descriptor");
    }
    const auto archive = std::find_if(mArchives.begin(), mArchives.end(),
        [archiveHandle](const ArchiveRecord& record) { return record.Handle == archiveHandle; });
    const bool diskArchive = archive != mArchives.end() && mConfig.Filesystem == nullptr &&
                             !archive->Root.empty() &&
                             (archive->Id == kFsSaveDataArchiveId || IsCtrExtDataArchiveId(archive->Id));
    if (archive != mArchives.end() && !diskArchive) {
        return FailFsRequest(state, memory, commandBuffer, sessionType, event,
                             kFsNotImplemented, "open-directory-archive-without-disk-backend");
    }
    const auto relative = DecodeArchiveRelativePath(memory, pathType, pathSize, pathAddress, true);
    uint32_t result = kFsInvalidArchiveHandle;
    uint32_t session = 0;
    if (archive != mArchives.end()) {
        result = kFsInvalidPath;
        // Sem seguir symlink: nem no meio (CtrArchivePathContained) nem no fim.
        if (relative && (relative->empty() || CtrArchivePathContained(archive->Root, *relative))) {
            const auto target = relative->empty() ? archive->Root : archive->Root / *relative;
            std::error_code error;
            const auto status = std::filesystem::symlink_status(target, error);
            if (std::filesystem::is_directory(status)) {
                auto directory = std::make_shared<KernelObject>();
                directory->Type = std::string(kFsDirectoryType);
                directory->FilePath = target;
                directory->FileOffset = 0;
                session = CreateHandle(directory);
                result = kResultSuccess;
            } else {
                // Arquivo no lugar (ou no meio) do caminho: UnexpectedFileOrDirectory.
                bool fileInPath = false;
                auto prefix = archive->Root;
                for (const auto& part : *relative) {
                    prefix /= part;
                    std::error_code partError;
                    if (std::filesystem::is_regular_file(prefix, partError)) {
                        fileInPath = true;
                        break;
                    }
                }
                result = fileInPath ? kFsUnexpectedFileOrDirectory : kFsPathNotFound;
            }
        }
    }
    if (!memory.Write32(commandBuffer, kFsOpenDirectoryResponse) ||
        !memory.Write32(commandBuffer + 4U, result) ||
        !memory.Write32(commandBuffer + 8U, kMoveHandleDescriptor) ||
        !memory.Write32(commandBuffer + 12U, session)) {
        if (session != 0U) CloseHandle(session);
        return {NativeA32HostAction::Fault, std::nullopt, 0U, "OpenDirectory reply write failed"};
    }
    state.r[0] = kResultSuccess;
    event.Name += ":OpenDirectory:" + DescribeLowPath(memory, pathType, pathSize, pathAddress);
    event.Detail = result;
    event.Handled = true;
    mSvcEvents.push_back(std::move(event));
    return {NativeA32HostAction::Resume};
}

NativeA32HostResult NativeA32CtrHostServices::HandleDirectoryRequest(
    oot3d::recomp::a32::GuestState& state, NativeA32Memory& memory,
    const std::optional<std::string>& sessionType, uint32_t commandBuffer,
    NativeA32CtrSvcEvent event) {
    const auto* handle = LookupHandle(state.r[0]);
    if (handle == nullptr || handle->Object == nullptr) {
        return FailFsRequest(state, memory, commandBuffer, sessionType, event,
                             kInvalidSessionHandle, "directory-handle");
    }
    auto& directory = *handle->Object;
    if (event.Detail == kDirectoryCloseRequest) {
        if (!memory.Write32(commandBuffer, kDirectoryCloseResponse) ||
            !memory.Write32(commandBuffer + 4U, kResultSuccess)) {
            return {NativeA32HostAction::Fault, std::nullopt, 0U, "Directory:Close reply write failed"};
        }
        directory.FileClosed = true;
        state.r[0] = kResultSuccess;
        event.Name += ":Directory:Close";
        event.Handled = true;
        mSvcEvents.push_back(std::move(event));
        return {NativeA32HostAction::Resume};
    }
    if (event.Detail != kDirectoryReadRequest) {
        return FailFsRequest(state, memory, commandBuffer, sessionType, event,
                             kFsNotImplemented, "unsupported-directory-command");
    }
    uint32_t count = 0, descriptor = 0, address = 0;
    if (!memory.Read32(commandBuffer + 4U, &count) ||
        !memory.Read32(commandBuffer + 8U, &descriptor) ||
        !memory.Read32(commandBuffer + 12U, &address)) {
        return FailFsRequest(state, memory, commandBuffer, sessionType, event,
                             kInvalidBufferDescriptor, "directory-read-unreadable");
    }
    const uint32_t permission = descriptor & 0xFU;
    const uint64_t bytes = static_cast<uint64_t>(count) * kDirectoryEntrySize;
    if ((permission != 0xCU && permission != 0xEU) || (descriptor >> 4U) < bytes ||
        (bytes != 0U && !memory.IsWritable(address, static_cast<uint32_t>(bytes)))) {
        return FailFsRequest(state, memory, commandBuffer, sessionType, event,
                             kInvalidBufferDescriptor, "directory-read-buffer");
    }
    auto* journal = SaveJournal();
    // Covers() é só para o que fica dentro do save; a raiz também é dele.
    if (journal != nullptr && journal->Directory() != CtrLexicalDirectory(directory.FilePath) &&
        !journal->Covers(directory.FilePath))
        journal = nullptr;
    const auto entries = ListDirectory(directory.FilePath, journal);
    uint32_t read = 0;
    for (uint64_t index = directory.FileOffset; index < entries.size() && read < count; ++index, ++read) {
        const auto raw = EncodeEntry(entries[index]);
        if (!memory.WriteBytes(address + read * kDirectoryEntrySize, raw)) {
            return {NativeA32HostAction::Fault, std::nullopt, 0U, "Directory:Read entry write failed"};
        }
    }
    directory.FileOffset += read;
    if (!memory.Write32(commandBuffer, kDirectoryReadResponse) ||
        !memory.Write32(commandBuffer + 4U, kResultSuccess) ||
        !memory.Write32(commandBuffer + 8U, read) ||
        !memory.Write32(commandBuffer + 12U, descriptor) ||
        !memory.Write32(commandBuffer + 16U, address)) {
        return {NativeA32HostAction::Fault, std::nullopt, 0U, "Directory:Read reply write failed"};
    }
    state.r[0] = kResultSuccess;
    event.Name += ":Directory:Read:" + std::to_string(read);
    event.Handled = true;
    mSvcEvents.push_back(std::move(event));
    return {NativeA32HostAction::Resume};
}

} // namespace Oot3dNativeGame
