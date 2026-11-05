#include "oot3d_native_a32_ctr_host.h"
#include "oot3d_native_master_quest_unlock.h"
#include "oot3d_native_a32_ctr_system_archives.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
namespace {
using namespace Oot3dNativeGame;
using oot3d::recomp::a32::GuestState;
void Require(bool ok, std::string_view message) {
    if (!ok) {
        std::cerr << "oot3d_native_a32_ctr_fs_tests: " << message << '\n';
        std::exit(1);
    }
}
NativeA32CtrHostConfig MakeConfig() { return {}; }
} // namespace
// Filesystem regressions exercised through the public SVC/IPC boundary.
#include "triaevum/filesystem_service_adapter.h"
#include "triaevum/rooted_filesystem_backend.h"
#include "triaevum/service_registry.h"
#include "triaevum_ctr_filesystem_client_bridge.h"

namespace {
struct TestFsStorage {
    std::vector<uint8_t> Bytes{1, 2, 3, 4};
    bool FailRead = false, FailWrite = false, FailResize = false;
    bool ShortRead = false, ShortWrite = false;
    unsigned Closes = 0;
};
class TestFsFile final : public NativeA32CtrFile {
  public:
    explicit TestFsFile(TestFsStorage &storage) : Storage(storage) {}
    ~TestFsFile() override { ++Storage.Closes; }
    bool Read(uint64_t offset, std::span<uint8_t> bytes, uint32_t *count) override {
        if (Storage.FailRead)
            return false;
        *count = static_cast<uint32_t>(
            offset >= Storage.Bytes.size()
                ? 0U
                : std::min<uint64_t>(bytes.size(), Storage.Bytes.size() - offset));
        if (Storage.ShortRead && *count)
            --*count;
        if (*count)
            std::copy_n(Storage.Bytes.data() + offset, *count, bytes.data());
        return true;
    }
    bool Write(uint64_t offset, std::span<const uint8_t> bytes, uint32_t *count) override {
        if (Storage.FailWrite)
            return false;
        *count = static_cast<uint32_t>(bytes.size());
        if (Storage.ShortWrite && *count)
            --*count;
        Storage.Bytes.resize(std::max<uint64_t>(Storage.Bytes.size(), offset + *count));
        std::copy_n(bytes.data(), *count, Storage.Bytes.data() + offset);
        return true;
    }
    bool Resize(uint64_t size) override {
        if (Storage.FailResize)
            return false;
        Storage.Bytes.resize(static_cast<size_t>(size));
        return true;
    }
    uint64_t Size() const noexcept override { return Storage.Bytes.size(); }
    TestFsStorage &Storage;
};
class TestFs final : public NativeA32CtrFilesystem {
  public:
    bool EnsureSaveRoot() override { return !FailEnsure; }
    bool Open(NativeA32CtrFilesystemRoot, std::string_view, uint32_t,
              std::shared_ptr<NativeA32CtrFile> *file) override {
        ++Opens;
        if (FailOpen)
            return false;
        *file = std::make_shared<TestFsFile>(Storage);
        return true;
    }
    TestFsStorage Storage;
    bool FailOpen = false, FailEnsure = false;
    unsigned Opens = 0;
};
struct FsFixture {
    static constexpr uint32_t Tls = 0x1FF82000U, Cmd = Tls + 0x80U, Buffer = Tls + 0x200U;
    static NativeA32CtrHostConfig Config(NativeA32CtrFilesystem &fs) {
        auto config = MakeConfig();
        config.Filesystem = &fs;
        config.RomFsContentPath = "inputs/romfs";
        config.RomFsImageSize = 4U;
        return config;
    }
    explicit FsFixture(NativeA32CtrFilesystem &fs) : Host(Config(fs)), Process(Registry, Host) {
        std::string error;
        Require(Memory.MapRegion({"tls", Tls, 0x1000U, true, false, {}}, &error), error);
        State.thread_pointer = Tls;
        const std::array<uint8_t, 5> port{'s', 'r', 'v', ':', 0};
        Require(Memory.WriteBytes(Buffer, port), "port");
        State.r[1] = Buffer;
        NativeA32HostContext context{Process, 0U};
        Require(Host.HandleSvc(0x2D, State, Memory, context).Action == NativeA32HostAction::Resume,
                "srv open");
        const auto srv = State.r[1];
        Call(srv, {0x00050100U, 0x553A7366U, 0x00524553U, 7U});
        Fs = Word(3);
        Require(Memory.Fill(Buffer, 16U, 0U), "path");
        Call(Fs, {0x08030204U, 0U, 3U, 1U, 1U, 2U, 12U, 1U, 0U, 0x4002U, Buffer, 0x30002U, Buffer});
        Rom = Word(3);
        Require(Rom && Word(1) == 0U, "rom open");
        Call(Fs, {0x080C00C2U, 4U, 1U, 1U, 0x4002U, Buffer});
        Archive = Word(2);
        Require(Archive && Word(1) == 0U, "save archive");
        const std::array<uint8_t, 6> save{'s', 'a', 'v', 'e', 0, 0};
        Require(Memory.WriteBytes(Buffer, save), "save path");
        Call(Fs, {0x080201C2U, 0U, Archive, 0U, 3U, 5U, 3U, 0U, 0x14002U, Buffer});
        Save = Word(3);
        Require(Save && Word(1) == 0U, "save file");
    }
    NativeA32HostResult Call(uint32_t handle, std::initializer_list<uint32_t> words) {
        uint32_t address = Cmd;
        for (auto word : words) {
            Require(Memory.Write32(address, word), "request");
            address += 4;
        }
        State.r[0] = handle;
        NativeA32HostContext context{Process, 0U};
        auto result = Host.HandleSvc(0x32U, State, Memory, context);
        Require(result.Action != NativeA32HostAction::Wait, "FS request orphaned a guest thread");
        return result;
    }
    uint32_t Word(unsigned i) {
        uint32_t v = 0U;
        Require(Memory.Read32(Cmd + i * 4U, &v), "reply word");
        return v;
    }
    void Read(uint32_t handle, uint64_t offset, uint32_t length, uint32_t descriptor = 0x4CU) {
        Require(Call(handle, {0x080200C2U, static_cast<uint32_t>(offset),
                              static_cast<uint32_t>(offset >> 32U), length, descriptor, Buffer})
                        .Action == NativeA32HostAction::Resume,
                "read reply");
    }
    NativeA32CtrHostServices Host;
    const oot3d::recomp::a32::Registry Registry{};
    NativeA32Process Process;
    NativeA32Memory Memory;
    GuestState State{};
    uint32_t Fs = 0U, Rom = 0U, Save = 0U, Archive = 0U;
};
void TestFsFailuresAndRestore() {
    TestFs fs;
    FsFixture f(fs);
    Require(f.Memory.Fill(FsFixture::Buffer, 4U, 0xA5U), "sentinel");
    f.Read(f.Rom, 3U, 4U);
    uint8_t tail = 0U;
    Require(f.State.r[0] == 0U && f.Word(0) == 0x08020082U && f.Word(1) == 0U && f.Word(2) == 1U &&
                f.Word(3) == 0x4CU && f.Word(4) == FsFixture::Buffer &&
                f.Memory.Read8(FsFixture::Buffer + 1U, &tail) && tail == 0xA5U,
            "short EOF read overwrote tail");
    f.Read(f.Rom, 9U, 4U);
    Require(f.Word(1) == 0U && f.Word(2) == 0U, "past EOF reply");
    f.Read(f.Rom, UINT64_MAX, 4U);
    Require(f.Word(1) == 0xE0E046C1U && f.Word(2) == 0U, "overflow result");
    f.Read(f.Rom, 0U, 4U, 0x3CU);
    Require(f.State.r[0] == 0xD9001830U, "short descriptor result");
    f.Read(f.Rom, 0U, 4U, 0x4AU);
    Require(f.State.r[0] == 0xD9001830U, "wrong direction result");
    f.Read(f.Rom, 0U, 0xFFFFFFFFU, 0xFFFFFFFCU);
    Require(f.State.r[0] == 0xD9001830U, "huge descriptor allocated");
    f.Read(f.Rom, 0U, 4U, 0x4EU);
    Require(f.Word(1) == 0U && f.Word(2) == 4U, "read-write descriptor rejected");
    fs.Storage.FailRead = true;
    f.Read(f.Rom, 0U, 4U);
    Require(f.Word(1) == 0xD9604400U && f.Word(2) == 0U && f.State.r[0] == 0U,
            "backend failure was not an IPC error");
    fs.Storage.FailRead = false;
    fs.Storage.ShortRead = true;
    f.Read(f.Save, 0U, 4U);
    Require(f.Word(1) == 0U && f.Word(2) == 3U, "backend short read rejected");
    fs.Storage.ShortRead = false;
    for (bool shortWrite : {false, true}) {
        fs.Storage.FailWrite = !shortWrite;
        fs.Storage.ShortWrite = shortWrite;
        f.Call(f.Save, {0x08030102U, 0U, 0U, 4U, 1U, 0x4AU, FsFixture::Buffer});
        Require(f.Word(1) == 0xD9604400U && f.Word(2) == 0U, "failed write acknowledged");
    }
    fs.Storage.FailWrite = false;
    fs.Storage.ShortWrite = false;
    fs.Storage.FailResize = true;
    f.Call(f.Save, {0x08050080U, 8U, 0U});
    Require(f.Word(1) == 0xD9604400U, "failed resize acknowledged");
    fs.Storage.FailResize = false;
    f.Call(f.Rom, {0x08030102U, 0U, 0U, 4U, 0U, 0x4AU, FsFixture::Buffer});
    Require(f.Word(1) == 0xC92044E6U, "write to RomFS accepted");
    f.Call(f.Rom, {0x08050080U, 8U, 0U});
    Require(f.Word(1) == 0xC92044E6U, "resize RomFS accepted");
    f.Call(f.Fs, {0x08010002U, 0U, 0U});
    Require(f.State.r[0] == 0xD9001830U, "malformed initialize");
    f.Call(f.Fs, {0x09990000U});
    Require(f.Word(0) == 0x09990040U && f.Word(1) == 0xD8C047F4U, "unknown FS command");
    f.Call(f.Rom, {0x09980000U});
    Require(f.Word(1) == 0xD8C047F4U, "unknown file command");
    fs.FailEnsure = true;
    f.Call(f.Fs, {0x080C00C2U, 4U, 1U, 1U, 0x4002U, FsFixture::Buffer});
    Require(f.Word(1) == 0xD9604400U && f.Word(2) == 0U && f.Word(3) == 0U, "save root failure");
    fs.FailEnsure = false;
    const std::array<uint8_t, 5> createPath{'s', 'a', 'v', 'e', 0};
    Require(f.Memory.WriteBytes(FsFixture::Buffer, createPath), "create path");
    fs.FailOpen = true;
    f.Call(f.Fs, {0x08080202U, 0U, f.Archive, 0U, 3U, 5U, 0U, 4U, 0U, 0x14002U, FsFixture::Buffer});
    Require(f.Word(1) == 0xD9604400U, "CreateFile backend failure");
    Require(f.Memory.Fill(FsFixture::Buffer, 12U, 0U), "RomFS path");
    f.Call(f.Fs, {0x08030204U, 0U, 3U, 1U, 1U, 2U, 12U, 1U, 0U, 0x4002U, FsFixture::Buffer,
                  0x30002U, FsFixture::Buffer});
    Require(f.Word(1) == 0xD9604400U && f.Word(3) == 0U, "OpenFileDirectly backend failure");
    fs.FailOpen = false;
    auto snapshot = f.Host.CaptureState();
    std::string error;
    const auto count = f.Host.RuntimeProfile().FsUnservedRequests;
    fs.FailOpen = true;
    Require(!f.Host.RestoreState(snapshot, f.Process, &error),
            "restore accepted unavailable backing");
    fs.FailOpen = false;
    Require(f.Host.RestoreState(snapshot, f.Process, &error), error);
    Require(f.Host.RuntimeProfile().FsUnservedRequests == count, "restore rewound diagnostics");
    f.Read(f.Rom, 0U, 4U);
    Require(f.Word(1) == 0U && f.Word(2) == 4U, "restored read");
    for (auto &object : snapshot["kernel_objects"])
        if (object["type"] == "client_session:file:romfs")
            object["file_offset"] = 1U;
    Require(!f.Host.RestoreState(snapshot, f.Process, &error),
            "restore accepted mismatched RomFS view");
    f.Read(f.Rom, 0U, 4U);
    Require(f.Word(1) == 0U, "failed restore changed live state");
    auto closedBefore = fs.Storage.Closes;
    f.Call(f.Rom, {0x08080000U});
    Require(fs.Storage.Closes == closedBefore + 1U, "Close retained backing");
    snapshot = f.Host.CaptureState();
    auto opens = fs.Opens;
    Require(f.Host.RestoreState(snapshot, f.Process, &error), error);
    Require(fs.Opens == opens + 1U, "restore reopened closed RomFS");
    f.Read(f.Rom, 0U, 4U);
    Require(f.State.r[0] == 0xC920181AU, "closed file read");
    NativeA32HostContext context{f.Process, 0U};
    f.State.r[0] = f.Rom;
    Require(f.Host.HandleSvc(0x23U, f.State, f.Memory, context).Action ==
                NativeA32HostAction::Resume,
            "Close consumed kernel handle");
    f.Read(f.Rom, 0U, 4U);
    Require(f.State.r[0] == 0xD8E007F7U, "stale handle parked");
    // A readable request with a read-only reply buffer must not mutate the file.
    const std::array<uint32_t, 7> request{0x08030102U, 0U, 0U, 4U, 1U, 0x4AU, FsFixture::Buffer};
    std::array<uint8_t, 0x1000> readOnlyTls{};
    std::memcpy(readOnlyTls.data() + 0x80U, request.data(), sizeof(request));
    Require(
        f.Memory.MapRegion(
            {"readonly-tls", 0x20000000U, readOnlyTls.size(), false, false, readOnlyTls}, &error),
        error);
    const auto savedBytes = fs.Storage.Bytes;
    f.State.thread_pointer = 0x20000000U;
    f.State.r[0] = f.Save;
    Require(f.Host.HandleSvc(0x32U, f.State, f.Memory, context).Action ==
                    NativeA32HostAction::Fault &&
                fs.Storage.Bytes == savedBytes,
            "unwritable reply mutated save data");
    // No request memory: return a transport error without doing a backend write.
    f.State.thread_pointer = 0xFFFFFFFFU - 0x80U;
    f.State.r[0] = f.Save;
    Require(f.Host.HandleSvc(0x32U, f.State, f.Memory, context).Action ==
                    NativeA32HostAction::Resume &&
                f.State.r[0] == 0xD9001830U,
            "unmapped TLS parked");
    f.Host.WriteFsRequestSummary();
}

void TestFullHistoryIntentionalWait() {
    auto config = MakeConfig();
    config.RetainFullSvcHistory = true;
    NativeA32CtrHostServices host(config);
    const oot3d::recomp::a32::Registry registry{};
    NativeA32Process process(registry, host);
    NativeA32Memory memory;
    GuestState state{};
    NativeA32HostContext context{process, 0U};
    state.r[0] = 1000U;
    Require(host.HandleSvc(0x0AU, state, memory, context).Action == NativeA32HostAction::Wait,
            "full history turned a finite sleep into a fault");
    Require(host.HandleSvc(0xFFFFU, state, memory, context).Action == NativeA32HostAction::Fault,
            "full history missed an unimplemented SVC");
}

void TestFsRootedRoundTrip() {
    using namespace triaevum::module;
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("triaevum-ctr-fs-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    {
        std::ofstream out(directory / "romfs", std::ios::binary);
        out.write("abcd", 4);
    }
    RootedFilesystemConfigV1 config;
    config.contentFiles.emplace("inputs/romfs", directory / "romfs");
    config.saveRoot = directory / "save";
    std::string error;
    auto rooted = RootedFilesystemBackendV1::Create(std::move(config), &error);
    Require(rooted != nullptr, error);
    FilesystemHostServiceAdapterV1 adapter(*rooted);
    HostServiceRegistry registry({});
    Require(registry.Register(TRIAEVUM_SERVICE_FILESYSTEM_V1,
                              FilesystemHostServiceAdapterV1::Invoke,
                              &adapter) == ServiceRegistrationResult::Registered,
            "register FS");
    auto api = registry.SealAndCreateHostApi();
    FilesystemServiceClientV1 client(&api);
    TriAevumCtrFilesystemClientBridge bridge(client);
    {
        FsFixture f(bridge);
        f.Call(f.Save, {0x08030102U, 0U, 0U, 4U, 1U, 0x4AU, FsFixture::Buffer});
        Require(f.Word(1) == 0U, "rooted write");
        f.Read(f.Save, 3U, 4U);
        Require(f.Word(2) == 1U, "rooted short read");
        auto snapshot = f.Host.CaptureState();
        const auto before = rooted->Stats();
        Require(f.Host.RestoreState(snapshot, f.Process, &error), error);
        const auto after = rooted->Stats();
        Require(after.opens == before.opens + 2U && after.closes == before.closes + 2U,
                "restore leaked rooted handles");
        f.Read(f.Save, 0U, 4U);
        Require(f.Word(1) == 0U && f.Word(2) == 4U, "restored rooted read");
        f.Call(f.Save, {0x08050080U, 8U, 0U});
        Require(f.Word(1) == 0U, "resize after EOF");
        f.Call(f.Save, {0x08090000U});
        Require(f.Word(1) == 0U, "flush");
        f.Call(f.Save, {0x08080000U});
        Require(f.Word(1) == 0U, "close");
        bool removed = false;
        Require(bridge.RemoveFile("save", &removed) && removed,
                "closed session still owns rooted file");
        Require(f.Host.RestoreState(f.Host.CaptureState(), f.Process, &error),
                "closed deleted file reopened on restore");
    }
    Require(rooted->Stats().opens == rooted->Stats().closes, "rooted handles leaked");
    std::filesystem::remove_all(directory);
}

// config/unlock_master_quest: system.dat (34 bytes; +0x00 0xBE = MQ liberado,
// +0x1F 0xEF = modo MQ, +0x20 CRC-16/ARC) sai liberado na leitura e o disco
// nunca recebe esse 0xBE.
std::vector<uint8_t> MakeSystemData(uint8_t unlocked, uint8_t mode) {
    std::vector<uint8_t> data(kSystemDataSize, 0U);
    data[0] = unlocked;
    for (size_t index = 1; index <= 30U; ++index)
        data[index] = static_cast<uint8_t>('A' + index % 26U);
    data[kSystemDataModeOffset] = mode;
    StoreSystemDataChecksum(data);
    return data;
}
void TestMasterQuestUnlockPolicy() {
    const std::array<uint8_t, 9> check{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    Require(Crc16Arc(check) == 0xBB3DU, "CRC-16/ARC check value");
    auto locked = MakeSystemData(0U, 0U);
    Require(IsValidSystemData(locked), "fixture CRC");
    auto read = locked;
    Require(ApplyMasterQuestUnlockToSystemDataRead(0U, read) && read[0] == 0xBEU &&
                IsValidSystemData(read) &&
                std::equal(read.begin() + 1, read.begin() + 0x20, locked.begin() + 1),
            "read did not unlock only byte 0");
    auto again = read;
    Require(!ApplyMasterQuestUnlockToSystemDataRead(0U, again) && again == read,
            "already unlocked data changed");
    auto corrupt = locked;
    corrupt[5] ^= 1U;
    const auto corruptCopy = corrupt;
    Require(!ApplyMasterQuestUnlockToSystemDataRead(0U, corrupt) && corrupt == corruptCopy,
            "corrupt system data patched");
    auto partial = locked;
    Require(!ApplyMasterQuestUnlockToSystemDataRead(4U, partial) &&
                !ApplyMasterQuestUnlockToSystemDataRead(0U, std::span<uint8_t>(partial.data(), 33U)),
            "partial read patched");
    auto write = MakeSystemData(0xBEU, 0xEFU);
    Require(GuardMasterQuestUnlockOnSystemDataWrite(0U, write, uint8_t{0U}) && write[0] == 0U &&
                write[kSystemDataModeOffset] == 0xEFU && IsValidSystemData(write),
            "write guard kept the injected unlock");
    auto fresh = MakeSystemData(0xBEU, 0U);
    Require(GuardMasterQuestUnlockOnSystemDataWrite(0U, fresh, std::nullopt) && fresh[0] == 0U &&
                IsValidSystemData(fresh),
            "write guard on a new file");
    auto real = MakeSystemData(0xBEU, 0U);
    const auto realCopy = real;
    Require(!GuardMasterQuestUnlockOnSystemDataWrite(0U, real, uint8_t{0xBEU}) && real == realCopy,
            "write guard dropped a real unlock");
    auto plain = MakeSystemData(0U, 0U);
    const auto plainCopy = plain;
    Require(!GuardMasterQuestUnlockOnSystemDataWrite(0U, plain, uint8_t{0U}) && plain == plainCopy,
            "write guard changed a locked write");
    Require(ParseMasterQuestUnlockSetting("") && ParseMasterQuestUnlockSetting("1\n") &&
                !ParseMasterQuestUnlockSetting("0") && !ParseMasterQuestUnlockSetting(" no"),
            "option parsing");
    Require(IsSystemDataPath("system.dat") && IsSystemDataPath("/sd/savedata/system.dat") &&
                !IsSystemDataPath("save00.bin"),
            "system.dat path");
}
void TestMasterQuestUnlockThroughFs() {
    for (const bool enabled : {false, true}) {
        SetMasterQuestUnlockOptionForTesting(enabled);
        TestFs fs;
        FsFixture f(fs);
        const auto disk = MakeSystemData(0U, 0U);
        fs.Storage.Bytes = disk;
        const std::array<uint8_t, 12> path{'/', 's', 'y', 's', 't', 'e', 'm', '.', 'd', 'a', 't', 0};
        Require(f.Memory.WriteBytes(FsFixture::Buffer, path), "system.dat path");
        f.Call(f.Fs, {0x080201C2U, 0U, f.Archive, 0U, 3U, 12U, 3U, 0U, 0x30002U, FsFixture::Buffer});
        const uint32_t system = f.Word(3);
        Require(system && f.Word(1) == 0U, "system.dat open");
        const uint32_t data = FsFixture::Buffer + 0x40U;
        f.Call(system, {0x080200C2U, 0U, 0U, 34U, (34U << 4U) | 0xCU, data});
        Require(f.Word(1) == 0U && f.Word(2) == 34U, "system.dat read");
        std::vector<uint8_t> seen(kSystemDataSize);
        Require(f.Memory.ReadBytes(data, seen), "system.dat read bytes");
        Require(IsValidSystemData(seen) && seen[0] == (enabled ? 0xBEU : 0U) &&
                    std::equal(seen.begin() + 1, seen.begin() + 0x20, disk.begin() + 1),
                "system.dat read through fs:USER");
        // O jogo marca o modo MQ (+0x1F) na RAM e regrava o arquivo.
        seen[kSystemDataModeOffset] = 0xEFU;
        StoreSystemDataChecksum(seen);
        Require(f.Memory.WriteBytes(data, seen), "system.dat write bytes");
        f.Call(system, {0x08030102U, 0U, 0U, 34U, 1U, (34U << 4U) | 0xAU, data});
        Require(f.Word(1) == 0U && f.Word(2) == 34U, "system.dat write");
        Require(fs.Storage.Bytes.size() == kSystemDataSize && fs.Storage.Bytes[0] == 0U &&
                    fs.Storage.Bytes[kSystemDataModeOffset] == 0xEFU &&
                    IsValidSystemData(fs.Storage.Bytes),
                "system.dat on disk");
        std::vector<uint8_t> after(kSystemDataSize);
        Require(f.Memory.ReadBytes(data, after) && after == seen, "guest buffer changed by write");
        // Outros arquivos do save passam intactos.
        fs.Storage.Bytes = disk;
        f.Call(f.Save, {0x080200C2U, 0U, 0U, 34U, (34U << 4U) | 0xCU, data});
        Require(f.Memory.ReadBytes(data, seen) && seen == disk, "save file patched");
    }
    SetMasterQuestUnlockOptionForTesting(std::nullopt);
}
} // namespace


// Super Mario 3D Land: GetFormatInfo (0x0845) antes de abrir o save e
// FormatSaveData (0x084C) na primeira vez; sem isso o jogo ia para err:f.
void TestSaveDataFormatInfo() {
    const auto directory =
        std::filesystem::temp_directory_path() / "triaevum_fs_format_test" / "save";
    std::filesystem::remove_all(directory.parent_path());
    auto config = MakeConfig();
    config.SaveDataDirectory = directory;
    NativeA32CtrHostServices host(config);
    const oot3d::recomp::a32::Registry registry{};
    NativeA32Process process(registry, host);
    NativeA32Memory memory;
    GuestState state{};
    constexpr uint32_t tls = FsFixture::Tls, cmd = FsFixture::Cmd, buffer = FsFixture::Buffer;
    std::string error;
    Require(memory.MapRegion({"tls", tls, 0x1000U, true, false, {}}, &error), error);
    state.thread_pointer = tls;
    NativeA32HostContext context{process, 0U};
    const auto call = [&](uint32_t handle, std::initializer_list<uint32_t> words) {
        uint32_t address = cmd;
        for (const uint32_t word : words) {
            Require(memory.Write32(address, word), "request");
            address += 4;
        }
        state.r[0] = handle;
        Require(host.HandleSvc(0x32U, state, memory, context).Action == NativeA32HostAction::Resume,
                "FS request did not resume");
    };
    const auto word = [&](unsigned index) {
        uint32_t value = 0U;
        Require(memory.Read32(cmd + index * 4U, &value), "reply word");
        return value;
    };
    const std::array<uint8_t, 5> port{'s', 'r', 'v', ':', 0};
    Require(memory.WriteBytes(buffer, port), "port");
    state.r[1] = buffer;
    Require(host.HandleSvc(0x2D, state, memory, context).Action == NativeA32HostAction::Resume,
            "srv open");
    call(state.r[1], {0x00050100U, 0x553A7366U, 0x00524553U, 7U});
    const uint32_t fs = word(3);
    Require(memory.Fill(buffer, 16U, 0U), "path");

    call(fs, {0x084500C2U, 4U, 1U, 1U, 0x4002U, buffer});
    Require(word(0) == 0x08450140U && word(1) == 0xC8A04554U,
            "an unformatted save did not answer NotFormatted");
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "lixo.bin") << "x";
    call(fs, {0x084C0242U, 4U, 1U, 1U, 0x10U, 8U, 16U, 8U, 16U, 1U, 0x4002U, buffer});
    Require(word(0) == 0x084C0040U && word(1) == 0U, "FormatSaveData failed");
    Require(std::filesystem::is_directory(directory) &&
                std::filesystem::is_empty(directory),
            "FormatSaveData did not leave an empty save");
    call(fs, {0x084500C2U, 4U, 1U, 1U, 0x4002U, buffer});
    Require(word(1) == 0U && word(2) == 0x10U * 512U && word(3) == 8U &&
                word(4) == 16U && word(5) == 1U,
            "GetFormatInfo did not return the formatted layout");
    std::filesystem::remove_all(directory.parent_path());
}

// Extdata compartilhado de Play Coins (SharedExtSaveData 7, NAND, 0xF000000B):
// Super Mario 3D Land abre no boot. Backend de pasta ao lado do save.
void TestSharedExtDataPlayCoins() {
    const auto root = std::filesystem::temp_directory_path() / "triaevum_fs_extdata_test";
    const auto directory = root / "save";
    std::filesystem::remove_all(root);
    auto config = MakeConfig();
    config.SaveDataDirectory = directory;
    NativeA32CtrHostServices host(config);
    const oot3d::recomp::a32::Registry registry{};
    NativeA32Process process(registry, host);
    NativeA32Memory memory;
    GuestState state{};
    constexpr uint32_t tls = FsFixture::Tls, cmd = FsFixture::Cmd, buffer = FsFixture::Buffer;
    constexpr uint32_t data = tls + 0x400U;
    std::string error;
    Require(memory.MapRegion({"tls", tls, 0x1000U, true, false, {}}, &error), error);
    state.thread_pointer = tls;
    NativeA32HostContext context{process, 0U};
    const auto call = [&](uint32_t handle, std::initializer_list<uint32_t> words) {
        uint32_t address = cmd;
        for (const uint32_t word : words) {
            Require(memory.Write32(address, word), "request");
            address += 4;
        }
        state.r[0] = handle;
        Require(host.HandleSvc(0x32U, state, memory, context).Action == NativeA32HostAction::Resume,
                "FS request did not resume");
    };
    const auto word = [&](unsigned index) {
        uint32_t value = 0U;
        Require(memory.Read32(cmd + index * 4U, &value), "reply word");
        return value;
    };
    const std::array<uint8_t, 5> port{'s', 'r', 'v', ':', 0};
    Require(memory.WriteBytes(buffer, port), "port");
    state.r[1] = buffer;
    Require(host.HandleSvc(0x2D, state, memory, context).Action == NativeA32HostAction::Resume,
            "srv open");
    call(state.r[1], {0x00050100U, 0x553A7366U, 0x00524553U, 7U});
    const uint32_t fs = word(3);
    const auto extPath = [&](uint32_t low) {
        Require(memory.Write32(buffer, 0U) && memory.Write32(buffer + 4U, low) &&
                    memory.Write32(buffer + 8U, 0U),
                "extdata path");
    };

    // Extdata compartilhado desconhecido: NotFound, sem criar nada.
    extPath(0xF000000AU);
    call(fs, {0x080C00C2U, 7U, 2U, 12U, (12U << 14U) | 2U, buffer});
    Require(word(0) == 0x080C00C0U && word(1) == 0xC8804478U && word(2) == 0U,
            "unknown shared extdata did not answer NotFound");

    extPath(0xF000000BU);
    call(fs, {0x080C00C2U, 7U, 2U, 12U, (12U << 14U) | 2U, buffer});
    const uint32_t archive = word(2);
    Require(word(0) == 0x080C00C0U && word(1) == 0U && archive != 0U,
            "Play Coins shared extdata did not open");
    const auto extdata = root / "extdata" / "0" / "00000000" / "f000000b";
    Require(std::filesystem::file_size(extdata / "gamecoin.dat") == 0x14U,
            "default gamecoin.dat was not created");

    // gamecoin.dat padrão pelo IPC: magic 0x4F00, 42 moedas, 2014-12-29.
    const std::array<uint8_t, 13> coinName{'/', 'g', 'a', 'm', 'e', 'c', 'o', 'i', 'n', '.', 'd', 'a', 't'};
    Require(memory.Fill(buffer, 16U, 0U) && memory.WriteBytes(buffer, coinName), "coin path");
    call(fs, {0x080201C2U, 0U, archive, 0U, 3U, 14U, 1U, 0U, (14U << 14U) | 2U, buffer});
    const uint32_t coins = word(3);
    Require(word(1) == 0U && coins != 0U, "gamecoin.dat open");
    call(coins, {0x080200C2U, 0U, 0U, 0x14U, (0x14U << 4U) | 0xCU, data});
    Require(word(1) == 0U && word(2) == 0x14U, "gamecoin.dat read");
    std::array<uint8_t, 0x14> coin{};
    Require(memory.ReadBytes(data, coin), "coin bytes");
    Require(coin[0] == 0x00U && coin[1] == 0x4FU && coin[2] == 0U && coin[3] == 0U &&
                coin[4] == 42U && coin[5] == 0U && coin[0x10] == 0xDEU && coin[0x11] == 0x07U &&
                coin[0x12] == 12U && coin[0x13] == 29U,
            "gamecoin.dat default layout");

    // CreateFile + Write num arquivo novo do extdata.
    const std::array<uint8_t, 7> testName{'/', 't', '.', 'b', 'i', 'n', 0};
    Require(memory.Fill(buffer, 16U, 0U) && memory.WriteBytes(buffer, testName), "test path");
    call(fs, {0x08080202U, 0U, archive, 0U, 3U, 7U, 0U, 4U, 0U, (7U << 14U) | 2U, buffer});
    Require(word(0) == 0x08080040U && word(1) == 0U, "extdata CreateFile");
    call(fs, {0x080201C2U, 0U, archive, 0U, 3U, 7U, 3U, 0U, (7U << 14U) | 2U, buffer});
    const uint32_t file = word(3);
    Require(word(1) == 0U && file != 0U, "extdata file open");
    const std::array<uint8_t, 4> payload{1, 2, 3, 4};
    Require(memory.WriteBytes(data, payload), "payload");
    call(file, {0x08030102U, 0U, 0U, 4U, 1U, (4U << 4U) | 0xAU, data});
    Require(word(1) == 0U && word(2) == 4U, "extdata write");
    {
        std::ifstream input(extdata / "t.bin", std::ios::binary);
        std::array<char, 4> disk{};
        Require(input.read(disk.data(), 4) && disk[0] == 1 && disk[3] == 4, "extdata write on disk");
    }

    // GetFormatInfo do extdata existente responde sucesso; o desconhecido, NotFound.
    extPath(0xF000000BU);
    call(fs, {0x084500C2U, 7U, 2U, 12U, (12U << 14U) | 2U, buffer});
    Require(word(0) == 0x08450140U && word(1) == 0U, "extdata GetFormatInfo");
    extPath(0xF000000AU);
    call(fs, {0x084500C2U, 7U, 2U, 12U, (12U << 14U) | 2U, buffer});
    Require(word(1) == 0xC8804478U, "unknown extdata GetFormatInfo");
    std::filesystem::remove_all(root);
}

// 05/11: o Super Mario 3D Land abria o archive NCCH 0x2345678A (Mii
// 0004009B00010202) e o host respondia unsupported-romfs-path -> err:f. Os
// system data archives substitutos (fonte compartilhada, Mii, países, NG
// word) são servidos sem arquivos do console, e sobrevivem ao savestate.
void TestSystemDataArchiveRomFs() {
    TestFs fs;
    FsFixture f(fs);
    const uint32_t archivePath = FsFixture::Buffer + 0x200U;
    const uint32_t filePath = FsFixture::Buffer + 0x240U;
    const auto open = [&](uint64_t titleId, bool exefs = false) {
        Require(f.Memory.Write64(archivePath, titleId, nullptr) &&
                    f.Memory.Write64(archivePath + 8U, 0U, nullptr) &&
                    f.Memory.Fill(filePath, 20U, 0U) &&
                    (!exefs || f.Memory.Write32(filePath + 8U, 2U)),
                "system archive paths");
        f.Call(f.Fs, {0x08030204U, 0U, 0x2345678AU, 2U, 16U, 2U, 20U, 1U, 0U,
                      (16U << 14U) | 2U, archivePath, (20U << 14U) | 2U, filePath});
        Require(f.State.r[0] == 0U && f.Word(0) == 0x08030042U, "system archive reply header");
        return f.Word(1) == 0U ? f.Word(3) : 0U;
    };
    // Mii (o pedido do Mario) e a fonte de todas as regiões.
    const uint32_t mii = open(0x0004009B00010202ULL);
    Require(mii != 0U, "Mii system archive not served");
    Require(f.Call(mii, {0x08040000U}).Action == NativeA32HostAction::Resume &&
                f.Word(1) == 0U && f.Word(2) == 73600U && f.Word(3) == 0U,
            "Mii RomFS size");
    f.Read(mii, 0U, 0x28U, (0x28U << 4U) | 0xCU);
    uint32_t header = 0U, metaOffset = 0U, dataOffset = 0U;
    Require(f.Word(1) == 0U && f.Word(2) == 0x28U &&
                f.Memory.Read32(FsFixture::Buffer, &header) && header == 0x28U &&
                f.Memory.Read32(FsFixture::Buffer + 0x1CU, &metaOffset) && metaOffset == 0x58U &&
                f.Memory.Read32(FsFixture::Buffer + 0x24U, &dataOffset) && dataOffset == 0x90U,
            "Mii RomFS level-3 header");
    for (const uint64_t font : {0x0004009B00014002ULL, 0x0004009B00014102ULL,
                                0x0004009B00014202ULL, 0x0004009B00014302ULL}) {
        const uint32_t handle = open(font);
        Require(handle != 0U, "shared font archive not served");
        // cbf_std.bcfnt.lz em +0xA0: LZ11 de 0x2FD4E6 bytes.
        f.Read(handle, 0xA0U, 4U, (4U << 4U) | 0xCU);
        uint32_t lz = 0U;
        Require(f.Word(2) == 4U && f.Memory.Read32(FsFixture::Buffer, &lz) && lz == 0x2FD4E611U,
                "shared font LZ11 header");
    }
    Require(open(0x0004009B00010402ULL) != 0U && open(0x000400DB00010302ULL) != 0U,
            "country list / NG word archives not served");
    // Título sem substituto e ExeFS: "não encontrado", sem prender a thread.
    Require(open(0x0004009B00010702ULL) == 0U && f.Word(1) == 0xC8804464U,
            "unknown system title result");
    Require(open(0x0004009B00010202ULL, true) == 0U && f.Word(1) == 0xC8804464U,
            "system archive ExeFS result");
    // Arquivo só de leitura.
    Require(f.Call(mii, {0x08030102U, 0U, 0U, 4U, 0U, (4U << 4U) | 0xAU, FsFixture::Buffer})
                    .Action == NativeA32HostAction::Resume && f.Word(1) == 0xC92044E6U,
            "system archive accepted a write");
    // Savestate: o handle continua lendo os mesmos bytes; título forjado falha.
    auto snapshot = f.Host.CaptureState();
    std::string error;
    Require(f.Host.RestoreState(snapshot, f.Process, &error), error);
    f.Read(mii, 0U, 4U, (4U << 4U) | 0xCU);
    Require(f.Word(1) == 0U && f.Word(2) == 4U && f.Memory.Read32(FsFixture::Buffer, &header) &&
                header == 0x28U,
            "restored system archive read");
    bool forged = false;
    for (auto& object : snapshot.at("kernel_objects")) {
        if (object.at("file_path") == "system_archive/0004009b00010202") {
            object["file_path"] = "system_archive/0004009b00010702";
            forged = true;
        }
    }
    Require(forged && !f.Host.RestoreState(snapshot, f.Process, &error),
            "restore accepted an unknown system archive");
}

void TestSharedFontRomFsAndRelocation() {
    // cbf_std.bcfnt.lz -> BCFNT, e o bloco do APT:GetSharedFont relocado.
    const auto archive = FindCtrSystemArchive(0x0004009B00014002ULL);
    Require(archive && archive->Name == "shared_font" && !archive->Substitute,
            "shared font archive lookup");
    Require(FindCtrSystemArchive(0x0004009B00014202ULL)->Substitute, "KOR font substitute flag");
    const auto lz = FindCtrRomFsRootFile(archive->RomFs, u"cbf_std.bcfnt.lz");
    Require(lz && lz->size() == 1089582U, "cbf_std.bcfnt.lz not found");
    Require(!FindCtrRomFsRootFile(archive->RomFs, u"cbf_ko-Hang-KR.bcfnt.lz"),
            "found a font file the substitute does not have");
    std::vector<uint8_t> bcfnt;
    Require(DecompressCtrLz11(*lz, &bcfnt) && bcfnt.size() == 0x2FD4E6U &&
                std::memcmp(bcfnt.data(), "CFNT", 4U) == 0,
            "BCFNT decompression");
    Require(!DecompressCtrLz11(lz->first(lz->size() / 2U), &bcfnt), "truncated LZ11 accepted");
    std::vector<uint8_t> memory;
    Require(BuildCtrSharedFontMemory(0x18000000U, &memory) && memory.size() == 0x332000U,
            "shared font memory");
    uint32_t value = 0U;
    std::memcpy(&value, memory.data(), 4U);
    Require(value == 2U && std::memcmp(memory.data() + 0x80U, "CFNU", 4U) == 0,
            "shared font header");
    // FINF está logo após o cabeçalho CFNT; seus ponteiros apontam para
    // seções dentro do bloco mapeado em 0x18000000.
    uint16_t cfntHeader = 0U;
    std::memcpy(&cfntHeader, memory.data() + 0x86U, 2U);
    const size_t finf = 0x80U + cfntHeader;
    Require(std::memcmp(memory.data() + finf, "FINF", 4U) == 0, "FINF position");
    const char* sections[] = {"TGLP", "CWDH", "CMAP"};
    for (int index = 0; index < 3; ++index) {
        std::memcpy(&value, memory.data() + finf + 0x10U + index * 4U, 4U);
        Require(value >= 0x18000008U && value < 0x18000000U + 0x332000U &&
                    std::memcmp(memory.data() + (value - 0x18000000U) - 8U, sections[index], 4U) == 0,
                "relocated FINF pointer");
    }
    // Relocar de novo para outra base desloca todos os ponteiros juntos.
    Require(RelocateCtrSharedFont(memory, 0x30000000U), "second relocation");
    std::memcpy(&value, memory.data() + finf + 0x10U, 4U);
    Require(value >= 0x30000008U && std::memcmp(memory.data() + (value - 0x30000000U) - 8U,
                                                "TGLP", 4U) == 0,
            "rebased FINF pointer");
}

int main() {
    TestFullHistoryIntentionalWait();
    TestFsFailuresAndRestore();
    TestFsRootedRoundTrip();
    TestMasterQuestUnlockPolicy();
    TestMasterQuestUnlockThroughFs();
    TestSaveDataFormatInfo();
    TestSharedExtDataPlayCoins();
    TestSystemDataArchiveRomFs();
    TestSharedFontRomFsAndRelocation();
    std::cout << "oot3d_native_a32_ctr_fs_tests: ok\n";
}
