#include "oot3d_native_a32_ctr_host.h"
#include "oot3d_native_master_quest_unlock.h"
#include "oot3d_native_a32_ctr_system_archives.h"
#include "oot3d_native_a32_ctr_save_safety.h"
#include "ctr_title_profile.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#if defined(__linux__)
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
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
    explicit FsFixture(NativeA32CtrFilesystem &fs) : FsFixture(Config(fs)) {}
    explicit FsFixture(NativeA32CtrHostConfig config) : Host(std::move(config)), Process(Registry, Host) {
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

// MK7 chamava err:f (erro fatal) logo no boot: fs:USER InitializeWithSdkVersion
// (0x0861) e SetPriority (0x0862) davam "não implementado". No Azahar
// (fs_user.cpp) os dois só respondem sucesso.
void TestFsSdkInitializeAndPriority() {
    TestFs fs;
    FsFixture f(fs);
    f.Call(f.Fs, {0x08610042U, 0x0F0001C8U, 0x20U, 0U});
    Require(f.State.r[0] == 0U && f.Word(0) == 0x08610040U && f.Word(1) == 0U,
            "InitializeWithSdkVersion was not served");
    f.Call(f.Fs, {0x08610042U, 0x0F0001C8U, 0x4002U, 0U});
    Require(f.State.r[0] == 0xD9001830U, "InitializeWithSdkVersion without PID descriptor");
    // GetPriority (0x0863) devolve o valor do SetPriority; sem SetPriority,
    // 0xFFFFFFFF como no Azahar. O valor vai no savestate.
    TestFs freshFs;
    FsFixture fresh(freshFs);
    fresh.Call(fresh.Fs, {0x08630000U});
    Require(fresh.State.r[0] == 0U && fresh.Word(0) == 0x08630080U &&
                fresh.Word(1) == 0U && fresh.Word(2) == 0xFFFFFFFFU,
            "GetPriority before SetPriority");
    f.Call(f.Fs, {0x08620040U, 0x10U});
    Require(f.State.r[0] == 0U && f.Word(0) == 0x08620040U && f.Word(1) == 0U,
            "SetPriority was not served");
    f.Call(f.Fs, {0x08630000U});
    Require(f.State.r[0] == 0U && f.Word(0) == 0x08630080U && f.Word(1) == 0U &&
                f.Word(2) == 0x10U,
            "GetPriority did not return the SetPriority value");
    const auto snapshot = f.Host.CaptureState();
    f.Call(f.Fs, {0x08620040U, 0x20U});
    std::string error;
    Require(f.Host.RestoreState(snapshot, f.Process, &error), error);
    f.Call(f.Fs, {0x08630000U});
    Require(f.Word(2) == 0x10U, "FS priority was not restored from the savestate");
    auto oldSnapshot = snapshot;
    oldSnapshot.erase("fs_priority");
    Require(f.Host.RestoreState(oldSnapshot, f.Process, &error), error);
    f.Call(f.Fs, {0x08630000U});
    Require(f.Word(2) == 0xFFFFFFFFU, "savestate without FS priority");
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

// config/unlock_master_quest: system.dat (34 bytes; +0x00 0xBE = MQ unlocked,
// +0x1F 0xEF = MQ mode, +0x20 CRC-16/ARC) comes out unlocked on read and the disk
// never receives that 0xBE.
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
        // The game sets the MQ mode (+0x1F) in RAM and rewrites the file.
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
        // Other save files pass through intact.
        fs.Storage.Bytes = disk;
        f.Call(f.Save, {0x080200C2U, 0U, 0U, 34U, (34U << 4U) | 0xCU, data});
        Require(f.Memory.ReadBytes(data, seen) && seen == disk, "save file patched");
    }
    SetMasterQuestUnlockOptionForTesting(std::nullopt);
}

#if !defined(_WIN32)
// A cwd this deep (24 x 201 characters) no longer fits PATH_MAX. POSIX only:
// Windows limits the current directory to MAX_PATH.
constexpr int kDeepLevels = 24;
const std::string kDeepName(200, 'd');

// Climbs into the deep tree under `base` and removes it from the bottom up: an
// absolute path through it is too long for remove_all.
void RemoveDeepTree(const std::filesystem::path &base) {
    namespace fs = std::filesystem;
    std::error_code error;
    if (!fs::is_directory(base, error)) return;
    fs::current_path(base);
    int depth = 0;
    for (; fs::is_directory(kDeepName, error); ++depth) fs::current_path(kDeepName);
    fs::remove_all("savedata", error);
    for (; depth > 0; --depth) {
        fs::current_path("..");
        fs::remove(kDeepName, error);
    }
    fs::current_path(base.parent_path());
    fs::remove_all(base, error);
}

// Erasing a file on the file select screen, on the product path (save root
// directory, no filesystem bridge). FileSelect_DeleteSlotFile (0x446FE4) queues
// an async file task of type 2 for "data:/save0N.bin"; its worker (0x41E7B8)
// mounts "data:", calls nn::fs::TryDeleteFile and commits. A failed delete goes
// to Result_ReportFatalToErrF, which asks srv for "err:f" and stops the game.
// The console's save root "sdmc:/switch/oot3dre/savedata" is relative to
// std::filesystem, so absolute() and canonical() put the cwd in front of it and
// name a path that does not exist. A cwd longer than PATH_MAX does the same on
// the host: relative paths open, the root's absolute form cannot be resolved.
void TestSlotEraseWithRelativeSaveRoot() {
    namespace fs = std::filesystem;
    const auto start = fs::current_path();
    const auto base = fs::temp_directory_path() / "oot3d_ctr_fs_slot_erase";
    RemoveDeepTree(base);
    fs::create_directories(base);
    fs::current_path(base);
    for (int level = 0; level < kDeepLevels; ++level) {
        fs::create_directory(kDeepName);
        fs::current_path(kDeepName);
    }
    auto config = MakeConfig();
    config.SaveDataDirectory = "savedata";
    config.RomFsImagePath = base / "romfs.bin"; // opened lazily, never read here
    config.RomFsImageSize = 4U;
    {
        FsFixture f(std::move(config));
        const auto writePath = [&](std::u16string_view path) {
            std::vector<uint8_t> bytes;
            for (const char16_t c : path) {
                bytes.push_back(static_cast<uint8_t>(c));
                bytes.push_back(static_cast<uint8_t>(c >> 8U));
            }
            bytes.insert(bytes.end(), {0U, 0U});
            Require(f.Memory.WriteBytes(FsFixture::Buffer, bytes), "slot path");
            return static_cast<uint32_t>(bytes.size());
        };
        const uint32_t data = FsFixture::Buffer + 0x80U;
        for (const auto slot : {u"/save00.bin", u"/save01.bin"}) {
            const uint32_t size = writePath(slot);
            f.Call(f.Fs, {0x08080202U, 0U, f.Archive, 0U, 4U, size, 0U, 16U, 0U, (size << 14U) | 2U,
                          FsFixture::Buffer});
            Require(f.Word(1) == 0U, "slot CreateFile");
            f.Call(f.Fs, {0x080201C2U, 0U, f.Archive, 0U, 4U, size, 2U, 0U, (size << 14U) | 2U,
                          FsFixture::Buffer});
            const uint32_t file = f.Word(3);
            Require(file && f.Word(1) == 0U, "slot OpenFile");
            Require(f.Memory.Fill(data, 16U, 0x5AU), "slot bytes");
            f.Call(file, {0x08030102U, 0U, 0U, 16U, 1U, (16U << 4U) | 0xAU, data});
            Require(f.Word(1) == 0U && f.Word(2) == 16U, "slot Write");
            f.Call(file, {0x08080000U});
            Require(f.Word(1) == 0U, "slot Close");
        }
        Require(fs::file_size("savedata/save00.bin") == 16U && fs::exists("savedata/save01.bin"),
                "slots on disk");
        const uint32_t size = writePath(u"/save00.bin");
        const auto erase = [&] {
            f.Call(f.Fs, {0x08040142U, 0U, f.Archive, 0U, 4U, size, (size << 14U) | 2U, FsFixture::Buffer});
            Require(f.Word(0) == 0x08040040U, "DeleteFile reply");
            return f.Word(1);
        };
        Require(erase() == 0U && !fs::exists("savedata/save00.bin") && fs::exists("savedata/save01.bin"),
                "erasing a file slot failed under a save root std::filesystem cannot make absolute");
        Require(erase() == 0xC8804470U, "erasing a missing slot did not report it missing");
        f.Call(f.Fs, {0x080D0144U, f.Archive, 0U, 0U, 1U, 1U, 0x1AU, data, 0x1CU, data + 4U});
        Require(f.Word(1) == 0U, "commit after the erase");
        f.Call(f.Fs, {0x080E0080U, f.Archive, 0U});
        Require(f.Word(1) == 0U, "unmount after the erase");
    }
    fs::current_path(start);
    RemoveDeepTree(base);
}
#endif
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

// ---------------------------------------------------------------------------
// 05/11: saves à prova de perda (docs/pesquisas/2026-11-05-saves-seguros.md).
// Backend de pasta (o do NRO do Switch: SaveDataDirectory sem Filesystem).
// Layout sintético que imita o save real do Zelda no Switch: save00.bin,
// save03.bin e save04.bin de 5340 bytes, system.dat de 34 bytes, sem .format.
namespace {
namespace fs = std::filesystem;
constexpr size_t kZeldaSaveSize = 5340U;

std::vector<uint8_t> ReadAll(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void WriteAll(const fs::path& path, const std::vector<uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Require(static_cast<bool>(output), "fixture write");
}
std::vector<uint8_t> Pattern(size_t size, uint8_t seed) {
    std::vector<uint8_t> bytes(size);
    for (size_t index = 0; index < size; ++index)
        bytes[index] = static_cast<uint8_t>(seed + index * 31U + (index >> 7U));
    return bytes;
}
// Arquivos (relativos, com '/') de uma árvore; vazio se não existe.
std::vector<std::string> ListTree(const fs::path& root) {
    std::vector<std::string> names;
    std::error_code error;
    if (!fs::exists(root, error)) return names;
    for (const auto& entry : fs::recursive_directory_iterator(root))
        names.push_back(entry.path().lexically_relative(root).generic_string() +
                        (entry.is_directory() ? "/" : ""));
    std::sort(names.begin(), names.end());
    return names;
}
struct ZeldaSave {
    std::vector<uint8_t> Save00 = Pattern(kZeldaSaveSize, 0x11U);
    std::vector<uint8_t> Save03 = Pattern(kZeldaSaveSize, 0x33U);
    std::vector<uint8_t> Save04 = Pattern(kZeldaSaveSize, 0x44U);
    std::vector<uint8_t> System = MakeSystemData(0U, 0U);
    void Write(const fs::path& directory) const {
        WriteAll(directory / "save00.bin", Save00);
        WriteAll(directory / "save03.bin", Save03);
        WriteAll(directory / "save04.bin", Save04);
        WriteAll(directory / "system.dat", System);
    }
};
fs::path FreshRoot(std::string_view name) {
    const auto root = fs::temp_directory_path() / ("triaevum_saves_seguros_" + std::string(name));
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}
// Gerações de backup (AAAAMMDD-HHMMSS[-n]) dentro de <save>-backups.
std::vector<fs::path> Generations(const fs::path& backups) {
    std::vector<fs::path> found;
    std::error_code error;
    if (!fs::is_directory(backups, error)) return found;
    for (const auto& entry : fs::directory_iterator(backups)) {
        const auto name = entry.path().filename().string();
        if (entry.is_directory() && name.size() >= 15U && name[8] == '-' &&
            std::all_of(name.begin(), name.begin() + 8, [](char c) { return c >= '0' && c <= '9'; }))
            found.push_back(entry.path());
    }
    std::sort(found.begin(), found.end());
    return found;
}
std::vector<fs::path> DirectoriesStartingWith(const fs::path& backups, std::string_view prefix) {
    std::vector<fs::path> found;
    std::error_code error;
    if (!fs::is_directory(backups, error)) return found;
    for (const auto& entry : fs::directory_iterator(backups))
        if (entry.is_directory() && entry.path().filename().string().starts_with(prefix))
            found.push_back(entry.path());
    std::sort(found.begin(), found.end());
    return found;
}

// fs:USER sobre uma pasta de save, pelo mesmo caminho de IPC do jogo.
struct DirFs {
    static constexpr uint32_t Tls = FsFixture::Tls, Cmd = FsFixture::Cmd, Path = FsFixture::Buffer;
    static constexpr uint32_t Data = 0x10000000U, DataSize = 0x4000U;
    static NativeA32CtrHostConfig Config(const fs::path& save) {
        auto config = MakeConfig();
        config.SaveDataDirectory = save;
        return config;
    }
    explicit DirFs(const fs::path& save) : Host(Config(save)), Process(Registry, Host) {
        std::string error;
        Require(Memory.MapRegion({"tls", Tls, 0x1000U, true, false, {}}, &error), error);
        Require(Memory.MapRegion({"data", Data, DataSize, true, false, {}}, &error), error);
        State.thread_pointer = Tls;
        const std::array<uint8_t, 5> port{'s', 'r', 'v', ':', 0};
        Require(Memory.WriteBytes(Path, port), "port");
        State.r[1] = Path;
        NativeA32HostContext context{Process, 0U};
        Require(Host.HandleSvc(0x2D, State, Memory, context).Action == NativeA32HostAction::Resume,
                "srv open");
        Call(State.r[1], {0x00050100U, 0x553A7366U, 0x00524553U, 7U});
        Fs = Word(3);
    }
    void Call(uint32_t handle, std::initializer_list<uint32_t> words) {
        uint32_t address = Cmd;
        for (const uint32_t word : words) {
            Require(Memory.Write32(address, word), "request");
            address += 4;
        }
        State.r[0] = handle;
        NativeA32HostContext context{Process, 0U};
        Require(Host.HandleSvc(0x32U, State, Memory, context).Action == NativeA32HostAction::Resume,
                "FS request did not resume");
    }
    uint32_t Word(unsigned index) {
        uint32_t value = 0U;
        Require(Memory.Read32(Cmd + index * 4U, &value), "reply word");
        return value;
    }
    uint32_t PutPath(std::string_view name) {  // ASCII com '/' inicial e NUL
        std::vector<uint8_t> bytes{'/'};
        bytes.insert(bytes.end(), name.begin(), name.end());
        bytes.push_back(0U);
        Require(Memory.Fill(Path, 0x40U, 0U) && Memory.WriteBytes(Path, bytes), "path");
        return static_cast<uint32_t>(bytes.size());
    }
    uint32_t OpenArchive() {
        Require(Memory.Fill(Path, 16U, 0U), "empty path");
        Call(Fs, {0x080C00C2U, 4U, 1U, 1U, 0x4002U, Path});
        Require(Word(1) == 0U && Word(2) != 0U, "OpenArchive(SaveData)");
        return Word(2);
    }
    uint32_t OpenFile(uint32_t archive, std::string_view name, uint32_t mode) {
        const uint32_t size = PutPath(name);
        Call(Fs, {0x080201C2U, 0U, archive, 0U, 3U, size, mode, 0U, (size << 14U) | 2U, Path});
        Require(Word(1) == 0U && Word(3) != 0U, "OpenFile");
        return Word(3);
    }
    uint32_t DeleteFile(uint32_t archive, std::string_view name) {
        const uint32_t size = PutPath(name);
        Call(Fs, {0x08040142U, 0U, archive, 0U, 3U, size, (size << 14U) | 2U, Path});
        return Word(1);
    }
    // Escreve em pedaços de 1 KiB, como um jogo que salva por partes.
    void Write(uint32_t file, uint64_t offset, const std::vector<uint8_t>& bytes) {
        Require(bytes.size() <= DataSize, "write too large");
        Require(Memory.WriteBytes(Data, bytes), "write payload");
        for (size_t done = 0; done < bytes.size(); done += 1024U) {
            const uint32_t length = static_cast<uint32_t>(std::min<size_t>(1024U, bytes.size() - done));
            const uint64_t at = offset + done;
            Call(file, {0x08030102U, static_cast<uint32_t>(at), static_cast<uint32_t>(at >> 32U), length, 1U,
                        (length << 4U) | 0xAU, Data + static_cast<uint32_t>(done)});
            Require(Word(1) == 0U && Word(2) == length, "Write");
        }
    }
    std::vector<uint8_t> Read(uint32_t file, uint64_t offset, uint32_t length) {
        Require(length <= DataSize, "read too large");
        Call(file, {0x080200C2U, static_cast<uint32_t>(offset), static_cast<uint32_t>(offset >> 32U), length,
                    (length << 4U) | 0xCU, Data});
        Require(Word(1) == 0U, "Read");
        std::vector<uint8_t> bytes(Word(2));
        Require(Memory.ReadBytes(Data, bytes), "read bytes");
        return bytes;
    }
    uint64_t Size(uint32_t file) {
        Call(file, {0x08040000U});
        Require(Word(1) == 0U, "GetSize");
        return Word(2) | (static_cast<uint64_t>(Word(3)) << 32U);
    }
    // Versões que devolvem o resultado sem exigir sucesso (disco cheio etc.).
    uint32_t TryWrite(uint32_t file, uint64_t at, const std::vector<uint8_t>& bytes) {
        Require(bytes.size() <= DataSize && Memory.WriteBytes(Data, bytes), "write payload");
        const uint32_t length = static_cast<uint32_t>(bytes.size());
        Call(file, {0x08030102U, static_cast<uint32_t>(at), static_cast<uint32_t>(at >> 32U), length, 1U,
                    (length << 4U) | 0xAU, Data});
        return Word(1);
    }
    uint32_t TrySetSize(uint32_t file, uint64_t size) {
        Call(file, {0x08050080U, static_cast<uint32_t>(size), static_cast<uint32_t>(size >> 32U)});
        return Word(1);
    }
    uint32_t TryFlush(uint32_t file) { Call(file, {0x08090000U}); return Word(1); }
    uint32_t TryClose(uint32_t file) { Call(file, {0x08080000U}); return Word(1); }
    uint32_t TryCommit(uint32_t archive) {
        Call(Fs, {0x080D0144U, archive, 0U, 0U, 1U, 1U, 0x1AU, Path + 0x40U, 0x1CU, Path + 0x44U});
        return Word(1);
    }
    // svcCloseHandle direto na sessão do arquivo, sem FSFILE_Close.
    void SvcClose(uint32_t handle) {
        State.r[0] = handle;
        NativeA32HostContext context{Process, 0U};
        Require(Host.HandleSvc(0x23U, State, Memory, context).Action == NativeA32HostAction::Resume &&
                    State.r[0] == 0U,
                "svcCloseHandle");
    }
    void Flush(uint32_t file) { Call(file, {0x08090000U}); Require(Word(1) == 0U, "Flush"); }
    void Close(uint32_t file) { Call(file, {0x08080000U}); Require(Word(1) == 0U, "Close"); }
    void Commit(uint32_t archive) {  // ControlArchive(commit), como o OoT3D faz ao salvar
        Call(Fs, {0x080D0144U, archive, 0U, 0U, 1U, 1U, 0x1AU, Path + 0x40U, 0x1CU, Path + 0x44U});
        Require(Word(1) == 0U, "ControlArchive");
    }
    uint32_t GetFormatInfo() {
        Require(Memory.Fill(Path, 16U, 0U), "empty path");
        Call(Fs, {0x084500C2U, 4U, 1U, 1U, 0x4002U, Path});
        return Word(1);
    }
    uint32_t FormatSaveData() {
        Require(Memory.Fill(Path, 16U, 0U), "empty path");
        Call(Fs, {0x084C0242U, 4U, 1U, 1U, 0x10U, 8U, 16U, 8U, 16U, 1U, 0x4002U, Path});
        return Word(1);
    }
    NativeA32CtrHostServices Host;
    const oot3d::recomp::a32::Registry Registry{};
    NativeA32Process Process;
    NativeA32Memory Memory;
    GuestState State{};
    uint32_t Fs = 0U;
};
} // namespace

// Compatibilidade: o save existente do Zelda (sem .format) abre e é lido byte
// a byte igual, e abrir/ler não cria nada dentro nem ao lado da pasta.
void TestLegacyZeldaSaveOpensUnchanged() {
    SetMasterQuestUnlockOptionForTesting(false);
    const auto root = FreshRoot("legado");
    const auto save = root / "savedata";
    const ZeldaSave zelda;
    zelda.Write(save);
    const auto before = ListTree(root);
    {
        DirFs f(save);
        const uint32_t archive = f.OpenArchive();
        const std::pair<const char*, const std::vector<uint8_t>*> files[] = {
            {"save00.bin", &zelda.Save00}, {"save03.bin", &zelda.Save03},
            {"save04.bin", &zelda.Save04}, {"system.dat", &zelda.System}};
        for (const auto& [name, bytes] : files) {
            const uint32_t file = f.OpenFile(archive, name, 1U);
            Require(f.Size(file) == bytes->size(), "legacy file size");
            Require(f.Read(file, 0U, static_cast<uint32_t>(bytes->size())) == *bytes,
                    "legacy file not read byte for byte");
            f.Close(file);
        }
        // O OoT3D abre os slots para leitura e escrita (modo 3) no menu.
        const uint32_t slot = f.OpenFile(archive, "save00.bin", 3U);
        Require(f.Read(slot, 0U, kZeldaSaveSize) == zelda.Save00, "mode 3 read");
        f.Close(slot);
    }
    Require(ListTree(root) == before, "opening/reading the legacy save changed files on disk");
    Require(ReadAll(save / "save00.bin") == zelda.Save00 && ReadAll(save / "system.dat") == zelda.System,
            "legacy bytes changed");
    fs::remove_all(root);
    SetMasterQuestUnlockOptionForTesting(std::nullopt);
}

// Escrita atômica: o jogo escreve por partes; até o Flush/Close/ControlArchive
// o save no disco continua o antigo, e uma queda no meio não deixa o arquivo
// meio novo meio velho. Durante a sessão o Read vê as escritas.
void TestSaveWriteIsAtomic() {
    SetMasterQuestUnlockOptionForTesting(false);
    const auto root = FreshRoot("atomico");
    const auto save = root / "savedata";
    const ZeldaSave zelda;
    zelda.Write(save);
    const auto fresh = Pattern(kZeldaSaveSize, 0xA0U);
    const std::vector<uint8_t> firstHalf(fresh.begin(), fresh.begin() + 2048);
    {
        DirFs f(save);
        const uint32_t archive = f.OpenArchive();
        const uint32_t file = f.OpenFile(archive, "save00.bin", 3U);
        f.Write(file, 0U, firstHalf);
        Require(ReadAll(save / "save00.bin") == zelda.Save00,
                "a partial write reached save00.bin before Flush/Close (not atomic)");
        auto expected = zelda.Save00;
        std::copy(firstHalf.begin(), firstHalf.end(), expected.begin());
        Require(f.Size(file) == kZeldaSaveSize, "size during the session");
        Require(f.Read(file, 0U, kZeldaSaveSize) == expected, "Read did not see the session writes");
        // Outro handle no mesmo arquivo também vê as escritas.
        const uint32_t other = f.OpenFile(archive, "save00.bin", 1U);
        Require(f.Read(other, 0U, kZeldaSaveSize) == expected, "second handle did not see the writes");
        // "Queda": o host morre sem Flush/Close.
    }
    Require(ReadAll(save / "save00.bin") == zelda.Save00, "crash left save00.bin changed");
    {
        DirFs f(save);
        const uint32_t archive = f.OpenArchive();
        // A recuperação tira o temporário de dentro do save (sem apagar: vai para os backups).
        Require(ListTree(save) == std::vector<std::string>{"save00.bin", "save03.bin", "save04.bin", "system.dat"},
                "journal files left inside the save after recovery");
        Require(DirectoriesStartingWith(save.parent_path() / "savedata-backups", "incompleto-").size() == 1U,
                "the interrupted write was not kept aside");
        const uint32_t file = f.OpenFile(archive, "save00.bin", 3U);
        Require(f.Read(file, 0U, kZeldaSaveSize) == zelda.Save00, "recovered save is not the old one");
        f.Write(file, 0U, fresh);
        f.Flush(file);
        Require(ReadAll(save / "save00.bin") == fresh, "Flush did not commit");
        // ControlArchive também confirma (sequência do OoT3D: escreve, flush, commit).
        const auto again = Pattern(kZeldaSaveSize, 0xB7U);
        f.Write(file, 0U, again);
        f.Commit(archive);
        Require(ReadAll(save / "save00.bin") == again, "ControlArchive did not commit");
        const auto last = Pattern(kZeldaSaveSize, 0xC9U);
        f.Write(file, 0U, last);
        f.Close(file);
        Require(ReadAll(save / "save00.bin") == last, "Close did not commit");
        Require(ListTree(save) == std::vector<std::string>{"save00.bin", "save03.bin", "save04.bin", "system.dat"},
                "journal files left inside the save after commit");
    }
    fs::remove_all(root);
    SetMasterQuestUnlockOptionForTesting(std::nullopt);
}

// Queda entre "temporário completo" e "trocar o arquivo": no Switch o rename
// não sobrescreve (EEXIST), então a troca é remove + rename; quem abre depois
// termina a troca com a versão completa.
void TestInterruptedCommitIsFinished() {
    SetMasterQuestUnlockOptionForTesting(false);
    const auto root = FreshRoot("troca");
    const auto save = root / "savedata";
    const ZeldaSave zelda;
    zelda.Write(save);
    const auto committed = Pattern(kZeldaSaveSize, 0x5AU);
    // (a) caiu depois de apagar o antigo e antes do rename final.
    fs::remove(save / "save00.bin");
    WriteAll(save / "save00.bin.triaevum-new", committed);
    // (b) caiu antes de apagar o antigo.
    WriteAll(save / "save03.bin.triaevum-new", committed);
    {
        DirFs f(save);
        const uint32_t archive = f.OpenArchive();
        Require(ReadAll(save / "save00.bin") == committed, "interrupted commit (old removed) was not finished");
        Require(ReadAll(save / "save03.bin") == committed, "interrupted commit (old kept) was not finished");
        Require(ListTree(save) == std::vector<std::string>{"save00.bin", "save03.bin", "save04.bin", "system.dat"},
                "commit leftovers inside the save");
        const uint32_t file = f.OpenFile(archive, "save00.bin", 1U);
        Require(f.Read(file, 0U, kZeldaSaveSize) == committed, "finished commit not visible");
    }
    fs::remove_all(root);
    SetMasterQuestUnlockOptionForTesting(std::nullopt);
}

// Backup rotativo: antes da primeira escrita de cada arquivo numa execução, a
// versão anterior vai para <save>-backups/<AAAAMMDD-HHMMSS>/.
void TestSaveBackupBeforeFirstWrite() {
    SetMasterQuestUnlockOptionForTesting(false);
    const auto root = FreshRoot("backup");
    const auto save = root / "savedata";
    const auto backups = root / "savedata-backups";
    const ZeldaSave zelda;
    zelda.Write(save);
    const auto first = Pattern(kZeldaSaveSize, 0x01U), second = Pattern(kZeldaSaveSize, 0x02U);
    {
        DirFs f(save);
        const uint32_t archive = f.OpenArchive();
        const uint32_t file = f.OpenFile(archive, "save00.bin", 3U);
        f.Write(file, 0U, first);
        f.Flush(file);
        f.Write(file, 0U, second);
        f.Close(file);
        const uint32_t system = f.OpenFile(archive, "system.dat", 3U);
        auto data = zelda.System;
        data[kSystemDataModeOffset] = 0xEFU;
        StoreSystemDataChecksum(data);
        f.Write(system, 0U, data);
        f.Close(system);
    }
    auto generations = Generations(backups);
    Require(generations.size() == 1U, "one backup generation per run");
    Require(ReadAll(generations[0] / "save00.bin") == zelda.Save00,
            "backup does not hold the version before the run");
    Require(ReadAll(generations[0] / "system.dat") == zelda.System, "system.dat backup");
    Require(!fs::exists(generations[0] / "save03.bin"), "untouched file was backed up");
    {
        DirFs f(save);
        const uint32_t archive = f.OpenArchive();
        const uint32_t file = f.OpenFile(archive, "save00.bin", 3U);
        f.Write(file, 0U, first);
        f.Close(file);
    }
    generations = Generations(backups);
    Require(generations.size() == 2U && ReadAll(generations[1] / "save00.bin") == second,
            "second run did not back up the first run's result");
    Require(ReadAll(generations[0] / "save00.bin") == zelda.Save00, "older generation changed");
    fs::remove_all(root);
    SetMasterQuestUnlockOptionForTesting(std::nullopt);
}

// Formatação sem perda: save legado (arquivos, sem .format) não responde
// NotFormatted, e FormatSaveData move a pasta para os backups antes de recriar.
void TestFormatNeverDeletesSave() {
    const auto root = FreshRoot("formato");
    const auto save = root / "savedata";
    const ZeldaSave zelda;
    zelda.Write(save);
    {
        DirFs f(save);
        Require(f.GetFormatInfo() == 0U, "legacy save answered NotFormatted (the game would format it)");
        Require(!fs::exists(save / ".format") && !fs::exists(root / "savedata.format"),
                "GetFormatInfo wrote a .format");
        Require(f.FormatSaveData() == 0U, "FormatSaveData");
        Require(fs::is_directory(save) && fs::is_empty(save), "format did not leave an empty save");
        const auto moved = DirectoriesStartingWith(root / "savedata-backups", "format-");
        Require(moved.size() == 1U && ReadAll(moved[0] / "save00.bin") == zelda.Save00 &&
                    ReadAll(moved[0] / "save04.bin") == zelda.Save04 &&
                    ReadAll(moved[0] / "system.dat") == zelda.System,
                "FormatSaveData deleted the save instead of moving it to the backups");
        Require(f.GetFormatInfo() == 0U && f.Word(2) == 0x10U * 512U && f.Word(4) == 16U,
                "formatted layout");
    }
    // Pasta vazia continua NotFormatted (como o Azahar).
    const auto empty = root / "vazio";
    fs::create_directories(empty);
    {
        DirFs f(empty);
        Require(f.GetFormatInfo() == 0xC8A04554U, "empty save must stay NotFormatted");
    }
    // --save-data com '/' no fim: o .format fica fora da pasta, sem barra.
    const auto slash = root / "barra";
    {
        DirFs f(fs::path(slash.string() + "/"));
        Require(f.FormatSaveData() == 0U, "format with trailing slash");
        Require(!fs::exists(slash / ".format") && fs::exists(root / "barra.format"),
                ".format must be beside the save folder, never inside it");
    }
    fs::remove_all(root);
}

// DeleteFile (Erase file do OoT3D): o arquivo apagado fica nos backups.
void TestDeleteFileKeepsBackup() {
    const auto root = FreshRoot("apagar");
    const auto save = root / "savedata";
    const ZeldaSave zelda;
    zelda.Write(save);
    {
        DirFs f(save);
        const uint32_t archive = f.OpenArchive();
        Require(f.DeleteFile(archive, "save03.bin") == 0U, "DeleteFile");
        Require(!fs::exists(save / "save03.bin"), "file not deleted");
        Require(f.DeleteFile(archive, "save03.bin") == 0xC8804470U, "second delete must be FileNotFound");
    }
    const auto generations = Generations(root / "savedata-backups");
    Require(generations.size() == 1U && ReadAll(generations[0] / "save03.bin") == zelda.Save03,
            "deleted save file was not kept in the backups");
    Require(ReadAll(save / "save00.bin") == zelda.Save00, "other slots changed");
    fs::remove_all(root);
}

// Savestate: arquivo de save aberto é guardado relativo à raiz do save
// (restaura com outra pasta, ex.: PC -> Switch), e arquivo de extdata aberto
// não impede o restore.
void TestSavestateSavePathsAreRelative() {
    SetMasterQuestUnlockOptionForTesting(false);
    const auto root = FreshRoot("savestate");
    const ZeldaSave zelda;
    zelda.Write(root / "a" / "savedata");
    zelda.Write(root / "b" / "savedata");
    WriteAll(root / "b" / "savedata" / "save00.bin", Pattern(kZeldaSaveSize, 0x77U));
    nlohmann::json snapshot;
    uint32_t file = 0U, archive = 0U;
    {
        DirFs f(root / "a" / "savedata");
        archive = f.OpenArchive();
        file = f.OpenFile(archive, "save00.bin", 3U);
        snapshot = f.Host.CaptureState();
    }
    bool relative = false;
    for (const auto& object : snapshot.at("kernel_objects"))
        relative = relative || object.at("file_path") == "save00.bin";
    Require(relative, "save file path was serialized as an absolute host path");
    {
        DirFs f(root / "b" / "savedata");
        std::string error;
        const bool restored = f.Host.RestoreState(snapshot, f.Process, &error);
        Require(restored, "restore with another save folder: " + error);
        Require(f.Read(file, 0U, kZeldaSaveSize) == Pattern(kZeldaSaveSize, 0x77U),
                "restored handle does not read the new folder");
    }
    fs::remove_all(root);
    SetMasterQuestUnlockOptionForTesting(std::nullopt);
}

// Restore de savestate com arquivo de extdata (Play Coins) aberto falhava:
// validFilePath só aceitava caminhos dentro do save.
void TestSavestateWithExtDataFileOpen() {
    const auto root = FreshRoot("savestate_extdata");
    {
        DirFs f(root / "a" / "savedata");
        f.OpenArchive();
        Require(f.Memory.Write32(DirFs::Path, 0U) && f.Memory.Write32(DirFs::Path + 4U, 0xF000000BU) &&
                    f.Memory.Write32(DirFs::Path + 8U, 0U),
                "extdata path");
        f.Call(f.Fs, {0x080C00C2U, 7U, 2U, 12U, (12U << 14U) | 2U, DirFs::Path});
        const uint32_t coins = f.Word(2);
        Require(f.Word(1) == 0U && coins != 0U, "Play Coins open");
        const uint32_t coin = f.OpenFile(coins, "gamecoin.dat", 1U);
        auto state = f.Host.CaptureState();
        std::string error;
        const bool restored = f.Host.RestoreState(state, f.Process, &error);
        Require(restored, "restore with an extdata file open: " + error);
        Require(f.Read(coin, 0U, 4U).size() == 4U, "restored extdata handle");
    }
    fs::remove_all(root);
    SetMasterQuestUnlockOptionForTesting(std::nullopt);
}

// Contenção do DeleteFile sem absolute/canonical: com raiz "sdmc:/..." o
// std::filesystem do devkitA64 trata o caminho como relativo e canonical
// concatenava o cwd (DeleteFile/Erase respondia CommandNotAllowed no Switch).
void TestArchiveContainmentLexical() {
    for (const fs::path root : {fs::path("sdmc:/switch/x/savedata"), fs::path("sdmc:/switch/x/savedata/")}) {
        Require(CtrArchivePathContained(root, "save03.bin") && CtrArchivePathContained(root, "sub/a.bin"),
                "file inside a sdmc:/ root rejected");
        Require(!CtrArchivePathContained(root, "../savedata-backups/a.bin") &&
                    !CtrArchivePathContained(root, "a/../../x.bin") && !CtrArchivePathContained(root, "") &&
                    !CtrArchivePathContained(root, ".") && !CtrArchivePathContained(root, "/etc/passwd"),
                "path escaping a sdmc:/ root accepted");
    }
    Require(CtrPathWithin("sdmc:/switch/x/savedata", "sdmc:/switch/x/savedata/save00.bin") &&
                !CtrPathWithin("sdmc:/switch/x/savedata", "sdmc:/switch/x/savedata-backups/a") &&
                !CtrPathWithin("sdmc:/switch/x/savedata", "sdmc:/switch/x/extdata/a"),
            "CtrPathWithin");
    // PC: subdiretório-symlink apontando para fora continua barrado.
    const auto root = FreshRoot("contencao");
    fs::create_directories(root / "save");
    fs::create_directories(root / "fora");
    std::error_code error;
    fs::create_directory_symlink(root / "fora", root / "save" / "link", error);
    if (!error)
        Require(!CtrArchivePathContained(root / "save", "link/a.bin"), "symlinked subdirectory accepted");
    // Ponta a ponta pelo IPC com raiz literal "sdmc:/switch/x/savedata" (no
    // PC é relativa ao cwd, que aponta para uma pasta temporária).
    const auto previous = fs::current_path();
    fs::current_path(root);
    const ZeldaSave zelda;
    zelda.Write("sdmc:/switch/x/savedata");
    {
        DirFs f(fs::path("sdmc:/switch/x/savedata"));
        const uint32_t archive = f.OpenArchive();
        Require(f.DeleteFile(archive, "save03.bin") == 0U, "DeleteFile with a sdmc:/ root");
        Require(f.DeleteFile(archive, "save03.bin") == 0xC8804470U, "missing file with a sdmc:/ root");
        const uint32_t file = f.OpenFile(archive, "save00.bin", 3U);
        f.Write(file, 0U, Pattern(kZeldaSaveSize, 0x99U));
        f.Close(file);
    }
    Require(!fs::exists("sdmc:/switch/x/savedata/save03.bin") &&
                ReadAll("sdmc:/switch/x/savedata/save00.bin") == Pattern(kZeldaSaveSize, 0x99U) &&
                Generations("sdmc:/switch/x/savedata-backups").size() == 1U,
            "sdmc:/ root end to end");
    fs::current_path(previous);
    fs::remove_all(root);
}

// Troca no Switch: o rename não sobrescreve (EEXIST); o commit faz remove + rename.
void TestHorizonRenameCommit() {
    SetMasterQuestUnlockOptionForTesting(false);
    SetCtrSaveRenameFailsOnExistingForTesting(true);
    const auto root = FreshRoot("horizon");
    const auto save = root / "savedata";
    const ZeldaSave zelda;
    zelda.Write(save);
    WriteAll(root / "origem.bin", {1, 2, 3});
    WriteAll(root / "destino.bin", {9});
    Require(ReplaceCtrSaveFile(root / "origem.bin", root / "destino.bin") &&
                ReadAll(root / "destino.bin") == std::vector<uint8_t>{1, 2, 3} && !fs::exists(root / "origem.bin"),
            "replace with Horizon rename semantics");
    const auto fresh = Pattern(kZeldaSaveSize, 0x3CU);
    {
        DirFs f(save);
        const uint32_t archive = f.OpenArchive();
        const uint32_t file = f.OpenFile(archive, "save04.bin", 3U);
        f.Write(file, 0U, fresh);
        f.Close(file);
    }
    Require(ReadAll(save / "save04.bin") == fresh, "commit with Horizon rename semantics");
    Require(ListTree(save) == std::vector<std::string>{"save00.bin", "save03.bin", "save04.bin", "system.dat"},
            "journal leftovers with Horizon rename semantics");
    SetCtrSaveRenameFailsOnExistingForTesting(false);
    fs::remove_all(root);
    SetMasterQuestUnlockOptionForTesting(std::nullopt);
}

// Rotação: ficam as últimas N gerações, mas a mais antiga com a única cópia
// de um arquivo nunca é apagada; format-* e incompleto-* não entram na conta.
void TestBackupPruneKeepsOnlyCopies() {
    const auto root = FreshRoot("rotacao");
    std::vector<std::string> names;
    for (int index = 0; index < 24; ++index) {
        char name[32];
        std::snprintf(name, sizeof(name), "202601%02d-120000", index + 1);
        names.push_back(name);
    }
    names.push_back("20260124-120000-2");  // mesma hora, segunda execução
    for (const auto& name : names) WriteAll(root / name / "save00.bin", {1});
    WriteAll(root / names[0] / "save04.bin", {4});  // única cópia
    WriteAll(root / "format-20250101-000000" / "save00.bin", {7});
    WriteAll(root / "incompleto-20250101-000000" / "save00.bin.triaevum-tmp", {8});
    PruneCtrSaveBackups(root, 20U);
    const auto generations = Generations(root);
    Require(generations.size() == 20U, "kept generation count");
    Require(fs::exists(root / names[0] / "save04.bin"), "oldest generation with the only copy was deleted");
    for (int index = 1; index <= 5; ++index)
        Require(!fs::exists(root / names[index]), "old generation not pruned");
    Require(fs::exists(root / names[6]) && fs::exists(root / "20260124-120000-2"), "recent generations pruned");
    Require(fs::exists(root / "format-20250101-000000") && fs::exists(root / "incompleto-20250101-000000"),
            "format/incompleto backups pruned");
    fs::remove_all(root);
}

// Pasta por título: o OoT3D fica em sdmc:/switch/oot3dre/savedata; nenhum
// outro título resolve para lá (o argv padrão do NRO passa esse caminho).
void TestTitleSaveLayout() {
    const fs::path oot3d("sdmc:/switch/oot3dre/savedata");
    for (const uint64_t id : {0x0004000000033400ULL, 0x0004000000033500ULL, 0x0004000000033600ULL}) {
        Require(IsOot3dTitleId(id) && CtrSwitchSaveDataDirectory(true, id) == oot3d, "OoT3D save path changed");
        Require(ResolveCtrTitleSaveDataDirectory(oot3d, true, id) == oot3d, "OoT3D save path redirected");
    }
    Require(CtrSwitchSaveDataDirectory(false, 0x0004000000053F00ULL) ==
                fs::path("sdmc:/switch/triaevum/0004000000053f00/savedata"),
            "generic title layout");
    for (const uint64_t id : {0x0004000000053F00ULL, 0ULL, 0x0004000000033500ULL, 0xFFFFFFFFFFFFFFFFULL}) {
        for (const fs::path requested : {oot3d, fs::path("sdmc:/switch/oot3dre/savedata/"),
                                         fs::path("sdmc:/switch/oot3dre/savedata/sub"),
                                         fs::path("sdmc:/switch/oot3dre"),
                                         fs::path("sdmc:/switch/oot3dre/./savedata")}) {
            const auto resolved = ResolveCtrTitleSaveDataDirectory(requested, false, id);
            Require(resolved == CtrSwitchSaveDataDirectory(false, id), "generic title kept the OoT3D folder");
            Require(!CtrPathWithin(oot3d, resolved) && CtrLexicalDirectory(resolved) != oot3d &&
                        !CtrPathWithin(resolved, oot3d),
                    "generic title resolves into the OoT3D save");
        }
    }
    Require(ResolveCtrTitleSaveDataDirectory("/home/x/save", false, 1U) == fs::path("/home/x/save") &&
                ResolveCtrTitleSaveDataDirectory("sdmc:/switch/oot3dre/savedata-mario", false, 1U) ==
                    fs::path("sdmc:/switch/oot3dre/savedata-mario"),
            "unrelated folders must not be redirected");
    Require(CtrSaveFormatPath("sdmc:/switch/oot3dre/savedata/") == fs::path("sdmc:/switch/oot3dre/savedata.format") &&
                CtrSaveBackupRoot("sdmc:/switch/oot3dre/savedata/") ==
                    fs::path("sdmc:/switch/oot3dre/savedata-backups"),
            "format/backup paths");
}

// ---------------------------------------------------------------------------
// 05/11 (rodada 2): pendências da verificação adversarial (S1..S8).
namespace {
#if defined(__linux__)
// "SD cheio": RLIMIT_FSIZE faz toda escrita além de bytes falhar (EFBIG).
struct FileSizeLimit {
    explicit FileSizeLimit(rlim_t bytes) {
        previousHandler = std::signal(SIGXFSZ, SIG_IGN);
        getrlimit(RLIMIT_FSIZE, &previous);
        rlimit limit = previous;
        limit.rlim_cur = bytes;
        setrlimit(RLIMIT_FSIZE, &limit);
    }
    ~FileSizeLimit() {
        setrlimit(RLIMIT_FSIZE, &previous);
        std::signal(SIGXFSZ, previousHandler);
    }
    rlimit previous{};
    void (*previousHandler)(int) = nullptr;
};
#endif
const std::vector<std::string> kZeldaFiles{"save00.bin", "save03.bin", "save04.bin", "system.dat"};
// Todos os arquivos regulares de todas as gerações AAAAMMDD-HHMMSS[-n].
std::vector<fs::path> BackupFiles(const fs::path& backups) {
    std::vector<fs::path> files;
    for (const auto& generation : Generations(backups))
        for (const auto& entry : fs::recursive_directory_iterator(generation))
            if (entry.is_regular_file()) files.push_back(entry.path());
    return files;
}
} // namespace

// S2: um Write (ou SetSize) que falha no meio, com o SD cheio, marca a
// transação como falha: Close, Flush e ControlArchive não instalam o
// temporário meio escrito; o save antigo fica e o temporário vai para
// <save>-backups/incompleto-*. A transação seguinte recomeça do save antigo.
void TestFailedWriteIsNotCommitted() {
#if defined(__linux__)
    SetMasterQuestUnlockOptionForTesting(false);
    const auto root = FreshRoot("escrita_falha");
    const auto save = root / "savedata";
    const auto backups = root / "savedata-backups";
    const ZeldaSave zelda;
    zelda.Write(save);
    {
        DirFs f(save);
        const uint32_t archive = f.OpenArchive();
        // (a) Write falha e o jogo fecha o arquivo.
        uint32_t file = f.OpenFile(archive, "save00.bin", 3U);
        Require(f.TryWrite(file, 0U, Pattern(1024U, 0xA1U)) == 0U, "first chunk");
        {
            FileSizeLimit full(kZeldaSaveSize);
            Require(f.TryWrite(file, 5000U, Pattern(1024U, 0xB2U)) != 0U, "write past the limit succeeded");
        }
        f.TryClose(file);
        Require(ReadAll(save / "save00.bin") == zelda.Save00,
                "Close committed a transaction whose Write failed (partial save installed)");
        Require(ListTree(save) == kZeldaFiles, "failed transaction left journal files inside the save");
        const auto aside = DirectoriesStartingWith(backups, "incompleto-");
        Require(aside.size() == 1U && fs::exists(aside[0] / "save00.bin.triaevum-tmp"),
                "failed transaction was not kept in incompleto-*");
        // A transação seguinte começa do save antigo, não do temporário com erro.
        file = f.OpenFile(archive, "save00.bin", 3U);
        Require(f.Read(file, 0U, kZeldaSaveSize) == zelda.Save00, "new transaction saw the failed writes");
        const auto fresh = Pattern(kZeldaSaveSize, 0x2EU);
        f.Write(file, 0U, fresh);
        f.Close(file);
        Require(ReadAll(save / "save00.bin") == fresh, "transaction after a failed one did not commit");
        // (b) Write falha e o jogo chama Flush: falha, nada instalado.
        file = f.OpenFile(archive, "save03.bin", 3U);
        f.Write(file, 0U, Pattern(1024U, 0x31U));
        {
            FileSizeLimit full(kZeldaSaveSize);
            Require(f.TryWrite(file, 5000U, Pattern(1024U, 0x32U)) != 0U, "write past the limit succeeded");
        }
        Require(f.TryFlush(file) != 0U, "Flush of a failed transaction answered success");
        Require(ReadAll(save / "save03.bin") == zelda.Save03, "Flush committed a failed transaction");
        f.TryClose(file);
        Require(ReadAll(save / "save03.bin") == zelda.Save03, "Close after Flush committed a failed transaction");
        // (c) SetSize falha e o jogo chama ControlArchive.
        file = f.OpenFile(archive, "save04.bin", 3U);
        f.Write(file, 0U, Pattern(512U, 0x41U));
        {
            FileSizeLimit full(kZeldaSaveSize);
            Require(f.TrySetSize(file, 2U * kZeldaSaveSize) != 0U, "SetSize past the limit succeeded");
        }
        Require(f.TryCommit(archive) != 0U, "ControlArchive with a failed transaction answered success");
        Require(ReadAll(save / "save04.bin") == zelda.Save04, "ControlArchive committed a failed transaction");
        f.TryClose(file);
        Require(ReadAll(save / "save04.bin") == zelda.Save04, "Close after ControlArchive committed it");
        Require(ListTree(save) == kZeldaFiles, "journal files left inside the save");
    }
    fs::remove_all(root);
    SetMasterQuestUnlockOptionForTesting(std::nullopt);
#endif
}

// S3: commit por handle. Close/Flush de um handle que não escreveu (só
// leitura) não confirma a escrita pela metade de outro handle; a transação de
// um arquivo é confirmada quando o último handle com escrita pendente fecha
// ou dá Flush (svcCloseHandle sem FSFILE_Close conta como Close), ou no
// ControlArchive (commit do archive, como no 3DS).
void TestReadOnlyCloseDoesNotCommit() {
    SetMasterQuestUnlockOptionForTesting(false);
    const auto root = FreshRoot("handle_leitura");
    const auto save = root / "savedata";
    const ZeldaSave zelda;
    zelda.Write(save);
    const auto fresh = Pattern(kZeldaSaveSize, 0x60U);
    const std::vector<uint8_t> firstHalf(fresh.begin(), fresh.begin() + 2048);
    const std::vector<uint8_t> secondHalf(fresh.begin() + 2048, fresh.end());
    const auto a = Pattern(kZeldaSaveSize, 0x70U), b = Pattern(1024U, 0x71U);
    {
        DirFs f(save);
        const uint32_t archive = f.OpenArchive();
        const uint32_t writer = f.OpenFile(archive, "save00.bin", 3U);
        f.Write(writer, 0U, firstHalf);
        const uint32_t reader = f.OpenFile(archive, "save00.bin", 1U);
        Require(f.Read(reader, 0U, 2048U) == firstHalf, "reader did not see the pending write");
        f.Flush(reader);
        f.Close(reader);
        Require(ReadAll(save / "save00.bin") == zelda.Save00,
                "Flush/Close of a read-only handle committed another handle's partial write");
        // Dois handles escrevendo: só o último a fechar confirma.
        const uint32_t other = f.OpenFile(archive, "save00.bin", 3U);
        f.Write(other, 2048U, secondHalf);
        f.Close(other);
        Require(ReadAll(save / "save00.bin") == zelda.Save00,
                "Close of one writer committed while another writer was still pending");
        f.Close(writer);
        Require(ReadAll(save / "save00.bin") == fresh, "last writer's Close did not commit");
        // svcCloseHandle sem FSFILE_Close: conta como Close do handle.
        const uint32_t closer = f.OpenFile(archive, "save03.bin", 3U);
        f.Write(closer, 0U, a);
        f.SvcClose(closer);
        Require(ReadAll(save / "save03.bin") == a, "svcCloseHandle of the writer did not commit");
        // Flush confirma; escrever de novo deixa o handle pendente outra vez.
        const uint32_t again = f.OpenFile(archive, "save04.bin", 3U);
        f.Write(again, 0U, a);
        f.Flush(again);
        Require(ReadAll(save / "save04.bin") == a, "writer Flush did not commit");
        f.Write(again, 0U, b);
        const uint32_t peek = f.OpenFile(archive, "save04.bin", 1U);
        f.Close(peek);
        Require(ReadAll(save / "save04.bin") == a, "read-only Close committed a partial write after a Flush");
        // Queda com o handle `again` pendente.
    }
    Require(ReadAll(save / "save04.bin") == a, "crash left save04.bin half written");
    {
        DirFs f(save);
        f.OpenArchive();
        Require(ListTree(save) == kZeldaFiles, "journal files left after the crash");
        Require(DirectoriesStartingWith(root / "savedata-backups", "incompleto-").size() == 1U,
                "pending write was not kept in incompleto-*");
    }
    fs::remove_all(root);
    SetMasterQuestUnlockOptionForTesting(std::nullopt);
}

// S4: a rotação ordena as gerações pela sequência de criação, não pelo nome
// com a data (o relógio do console pode voltar), e a geração recém-criada
// nunca é apagada pelo próprio prune.
void TestBackupPruneUsesSequence() {
    SetMasterQuestUnlockOptionForTesting(false);
    const auto root = FreshRoot("relogio");
    const auto save = root / "savedata";
    const auto backups = root / "savedata-backups";
    const ZeldaSave zelda;
    zelda.Write(save);
    // 20 gerações antigas com data no futuro (relógio adiantado na época).
    // Sementes 0x80+ para não coincidir com o save do fixture (0x11...).
    for (int index = 0; index < 20; ++index) {
        char name[32];
        std::snprintf(name, sizeof(name), "20270101-0000%02d", index);
        WriteAll(backups / name / "save00.bin", Pattern(kZeldaSaveSize, static_cast<uint8_t>(0x80 + index)));
    }
    const auto first = Pattern(kZeldaSaveSize, 0xD0U), second = Pattern(kZeldaSaveSize, 0xD1U);
    const auto holding = [&](const std::vector<uint8_t>& bytes) {
        std::vector<fs::path> found;
        for (const auto& generation : Generations(backups))
            if (ReadAll(generation / "save00.bin") == bytes) found.push_back(generation);
        return found;
    };
    for (const auto* bytes : {&first, &second}) {
        DirFs f(save);
        const uint32_t archive = f.OpenArchive();
        const uint32_t file = f.OpenFile(archive, "save00.bin", 3U);
        f.Write(file, 0U, *bytes);
        f.Close(file);
        Require(Generations(backups).size() == kCtrSaveBackupGenerations, "generation count after prune");
        Require(holding(bytes == &first ? zelda.Save00 : first).size() == 1U,
                "the backup made by this run was pruned (ordered by the date in the name)");
    }
    // A geração da primeira execução (mais nova que as 2027 pela sequência)
    // sobrevive à segunda; quem sai são as 2027 mais antigas.
    Require(holding(zelda.Save00).size() == 1U, "older run's generation pruned before the 2027 ones");
    Require(!fs::exists(backups / "20270101-000000") && !fs::exists(backups / "20270101-000001") &&
                fs::exists(backups / "20270101-000002"),
            "prune did not remove the oldest generations by sequence");
    // Direto: a geração protegida nunca sai, mesmo sendo a mais antiga.
    const auto direct = FreshRoot("relogio_direto");
    for (int index = 0; index < 3; ++index) {
        char name[32];
        std::snprintf(name, sizeof(name), "20260101-00000%d", index);
        WriteAll(direct / name / "save00.bin", {static_cast<uint8_t>(index)});
    }
    PruneCtrSaveBackups(direct, 1U, direct / "20260101-000000");
    Require(fs::exists(direct / "20260101-000000"), "protected generation was pruned");
    fs::remove_all(direct);
    fs::remove_all(root);
    SetMasterQuestUnlockOptionForTesting(std::nullopt);
}

// S1: a cópia de backup só conta depois de conferida (tamanho e checksum
// contra a origem). Cópia truncada (SD cheio) ou estragada é descartada e não
// aparece na geração com o nome do arquivo (o prune contaria como cópia).
void TestBackupIsVerified() {
    SetMasterQuestUnlockOptionForTesting(false);
    const auto root = FreshRoot("backup_conferido");
    const auto save = root / "savedata";
    const auto backups = root / "savedata-backups";
    const ZeldaSave zelda;
#if defined(__linux__)
    zelda.Write(save);
    {
        DirFs f(save);
        const uint32_t archive = f.OpenArchive();
        const uint32_t file = f.OpenFile(archive, "save00.bin", 3U);
        {
            FileSizeLimit full(4096U);
            Require(f.TryWrite(file, 0U, Pattern(1024U, 0xEEU)) != 0U, "write with a full SD succeeded");
        }
        for (const auto& copy : BackupFiles(backups))
            Require(copy.filename() == "save00.bin" && ReadAll(copy) == zelda.Save00,
                    "truncated backup copy kept in the generation (" + copy.string() + ")");
        Require(ListTree(save) == kZeldaFiles, "partial journal copy left inside the save");
        f.TryClose(file);
        Require(ReadAll(save / "save00.bin") == zelda.Save00, "save changed after the failed write");
    }
    fs::remove_all(root);
    fs::create_directories(root);
#endif
    // SD que grava errado (mesmo tamanho, um byte trocado).
    zelda.Write(save);
    SetCtrSaveCopyFaultForTesting([&](const fs::path& destination) {
        // Só a cópia de backup (a cópia de trabalho do diário fica no save).
        if (!CtrPathWithin(backups, destination) || !destination.filename().string().starts_with("save03.bin"))
            return;
        std::fstream stream(destination, std::ios::binary | std::ios::in | std::ios::out);
        stream.seekp(100);
        stream.put('\x5A');
    });
    const auto fresh = Pattern(kZeldaSaveSize, 0x3EU);
    {
        DirFs f(save);
        const uint32_t archive = f.OpenArchive();
        const uint32_t file = f.OpenFile(archive, "save03.bin", 3U);
        f.Write(file, 0U, fresh);
        f.Close(file);
    }
    SetCtrSaveCopyFaultForTesting(nullptr);
    Require(ReadAll(save / "save03.bin") == fresh, "write did not commit after a failed backup");
    for (const auto& copy : BackupFiles(backups))
        Require(ReadAll(copy) == zelda.Save03, "corrupted backup copy kept (" + copy.string() + ")");
#if defined(__linux__)
    // Queda REAL no meio da cópia de backup (cópia cortada em 100 bytes e
    // SIGKILL): a sobra nunca tem o nome do arquivo (o prune contaria como
    // cópia); só <nome>.triaevum-tmp, e o save fica intacto.
    fs::remove_all(root);
    zelda.Write(save);
    const pid_t child = fork();
    if (child == 0) {
        SetCtrSaveCopyFaultForTesting([&](const fs::path& destination) {
            if (!CtrPathWithin(backups, destination)) return;
            fs::resize_file(destination, 100U);
            ::kill(::getpid(), SIGKILL);
        });
        DirFs f(save);
        const uint32_t archive = f.OpenArchive();
        const uint32_t file = f.OpenFile(archive, "save00.bin", 3U);
        f.Write(file, 0U, fresh);
        std::_Exit(0);
    }
    int status = 0;
    waitpid(child, &status, 0);
    Require(WIFSIGNALED(status), "backup crash child did not die mid-copy");
    const auto leftovers = BackupFiles(backups);
    Require(!leftovers.empty(), "crash fixture left no partial backup copy");
    for (const auto& copy : leftovers)
        Require(copy.filename().string().ends_with(".triaevum-tmp"),
                "interrupted backup copy has the file's own name (" + copy.string() + ")");
    Require(ReadAll(save / "save00.bin") == zelda.Save00, "save changed by the crash during the backup");
#endif
    fs::remove_all(root);
    SetMasterQuestUnlockOptionForTesting(std::nullopt);
}

// S8: a recuperação (.triaevum-new / .triaevum-old) roda no primeiro acesso
// da execução, não só no OpenArchive: uma execução que começa pelo restore de
// savestate ou pelo GetFormatInfo acha o save trocado.
void TestRecoveryBeforeFirstAccess() {
    SetMasterQuestUnlockOptionForTesting(false);
    const auto root = FreshRoot("recupera_estado");
    const auto save = root / "savedata";
    const ZeldaSave zelda;
    zelda.Write(save);
    const auto committed = Pattern(kZeldaSaveSize, 0x8CU);
    nlohmann::json snapshot;
    uint32_t file = 0U;
    {
        DirFs f(save);
        const uint32_t archive = f.OpenArchive();
        file = f.OpenFile(archive, "save00.bin", 1U);
        snapshot = f.Host.CaptureState();
    }
    // Queda no meio da troca: o antigo já saiu, o novo está completo.
    fs::remove(save / "save00.bin");
    WriteAll(save / "save00.bin.triaevum-new", committed);
    {
        DirFs f(save);
        std::string error;
        const bool restored = f.Host.RestoreState(snapshot, f.Process, &error);
        Require(restored, "restore: " + error);
        f.Call(file, {0x080200C2U, 0U, 0U, kZeldaSaveSize, (kZeldaSaveSize << 4U) | 0xCU, DirFs::Data});
        Require(f.Word(1) == 0U && f.Word(2) == kZeldaSaveSize,
                "Read after a savestate restore did not find the interrupted commit");
        Require(ReadAll(save / "save00.bin") == committed && ListTree(save) == kZeldaFiles,
                "restore did not finish the interrupted commit");
    }
    // Primeiro acesso pelo GetFormatInfo (Mario 3D Land pergunta antes de abrir).
    fs::remove(save / "save03.bin");
    WriteAll(save / "save03.bin.triaevum-new", committed);
    {
        DirFs f(save);
        Require(f.GetFormatInfo() == 0U && f.Word(4) == 4U, "GetFormatInfo before recovery");
        Require(ReadAll(save / "save03.bin") == committed && ListTree(save) == kZeldaFiles,
                "GetFormatInfo did not finish the interrupted commit");
    }
    fs::remove_all(root);
    SetMasterQuestUnlockOptionForTesting(std::nullopt);
}

// Horizon: o rename não sobrescreve, então a troca precisa tirar o destino do
// lugar. Se o rename do novo falha depois disso, o save não pode sumir: o
// destino vira <nome>.triaevum-old e volta se o rename falhar; a recuperação
// conhece .triaevum-old; e escrita nova na mesma execução não estraga o
// .triaevum-new bom.
void TestRenameFailureNeverLosesSave() {
    SetMasterQuestUnlockOptionForTesting(false);
    SetCtrSaveRenameFailsOnExistingForTesting(true);
    const auto root = FreshRoot("rename_falha");
    const auto save = root / "savedata";
    const ZeldaSave zelda;
    zelda.Write(save);
    const auto fresh = Pattern(kZeldaSaveSize, 0x9AU), more = Pattern(512U, 0x9BU);
    bool failInstall = true, failRollback = false;
    SetCtrSaveRenameFaultForTesting([&](const fs::path& from, const fs::path& to) {
        const auto name = from.filename().string();
        if (failInstall && name.ends_with(".triaevum-new") && to.filename() == "save00.bin") return true;
        if (failRollback && name.ends_with(".triaevum-old") && to.filename() == "save00.bin") return true;
        return false;
    });
    {
        DirFs f(save);
        const uint32_t archive = f.OpenArchive();
        uint32_t file = f.OpenFile(archive, "save00.bin", 3U);
        f.Write(file, 0U, fresh);
        Require(f.TryClose(file) != 0U, "Close answered success with the commit rename failing");
        Require(fs::is_regular_file(save / "save00.bin"),
                "save00.bin vanished after a failed commit rename (Horizon)");
        Require(ReadAll(save / "save00.bin.triaevum-new") == fresh, "committed data lost");
        // Na mesma execução o jogo vê o que confirmou e não estraga o .new.
        file = f.OpenFile(archive, "save00.bin", 3U);
        Require(f.Read(file, 0U, kZeldaSaveSize) == fresh, "Read after a failed install");
        Require(f.TryWrite(file, 0U, more) != 0U || ReadAll(save / "save00.bin.triaevum-new") == fresh,
                "write after a failed install clobbered the committed .triaevum-new");
        failInstall = false;  // o SD voltou: a próxima escrita termina a troca
        f.Write(file, 0U, more);
        f.Close(file);
        auto expected = fresh;
        std::copy(more.begin(), more.end(), expected.begin());
        Require(ReadAll(save / "save00.bin") == expected, "write after recovering the install");
        Require(ListTree(save) == kZeldaFiles, "journal leftovers after recovering the install");
    }
    // Rename do novo e a volta do antigo falham: o save sai do lugar durante
    // a execução, mas a próxima execução instala o .triaevum-new.
    failInstall = failRollback = true;
    const auto last = Pattern(kZeldaSaveSize, 0x9CU);
    {
        DirFs f(save);
        const uint32_t archive = f.OpenArchive();
        const uint32_t file = f.OpenFile(archive, "save00.bin", 3U);
        f.Write(file, 0U, last);
        f.TryClose(file);
        Require(ReadAll(save / "save00.bin.triaevum-new") == last, "committed data lost (double failure)");
    }
    failInstall = failRollback = false;
    {
        DirFs f(save);
        f.OpenArchive();
        Require(ReadAll(save / "save00.bin") == last && ListTree(save) == kZeldaFiles,
                "recovery after a double rename failure");
    }
    // Só sobrou o .triaevum-old (destino tirado do lugar, sem .new): volta.
    fs::rename(save / "save04.bin", save / "save04.bin.triaevum-old");
    {
        DirFs f(save);
        f.OpenArchive();
        Require(ReadAll(save / "save04.bin") == zelda.Save04 && ListTree(save) == kZeldaFiles,
                "recovery did not put .triaevum-old back");
    }
    SetCtrSaveRenameFaultForTesting(nullptr);
    SetCtrSaveRenameFailsOnExistingForTesting(false);
    fs::remove_all(root);
    SetMasterQuestUnlockOptionForTesting(std::nullopt);
}

int main(int argc, char** argv) {
    // Sem argumento: todos. Com nomes: só esses (para ver um teste falhar sozinho).
    const std::vector<std::pair<std::string_view, void (*)()>> tests{
        {"TestFullHistoryIntentionalWait", TestFullHistoryIntentionalWait},
        {"TestFsSdkInitializeAndPriority", TestFsSdkInitializeAndPriority},
        {"TestFsFailuresAndRestore", TestFsFailuresAndRestore},
        {"TestFsRootedRoundTrip", TestFsRootedRoundTrip},
        {"TestMasterQuestUnlockPolicy", TestMasterQuestUnlockPolicy},
        {"TestMasterQuestUnlockThroughFs", TestMasterQuestUnlockThroughFs},
        {"TestSaveDataFormatInfo", TestSaveDataFormatInfo},
        {"TestSharedExtDataPlayCoins", TestSharedExtDataPlayCoins},
        {"TestSystemDataArchiveRomFs", TestSystemDataArchiveRomFs},
        {"TestSharedFontRomFsAndRelocation", TestSharedFontRomFsAndRelocation},
        {"TestLegacyZeldaSaveOpensUnchanged", TestLegacyZeldaSaveOpensUnchanged},
        {"TestSaveWriteIsAtomic", TestSaveWriteIsAtomic},
        {"TestInterruptedCommitIsFinished", TestInterruptedCommitIsFinished},
        {"TestSaveBackupBeforeFirstWrite", TestSaveBackupBeforeFirstWrite},
        {"TestFormatNeverDeletesSave", TestFormatNeverDeletesSave},
        {"TestDeleteFileKeepsBackup", TestDeleteFileKeepsBackup},
        {"TestSavestateSavePathsAreRelative", TestSavestateSavePathsAreRelative},
        {"TestSavestateWithExtDataFileOpen", TestSavestateWithExtDataFileOpen},
        {"TestArchiveContainmentLexical", TestArchiveContainmentLexical},
        {"TestHorizonRenameCommit", TestHorizonRenameCommit},
        {"TestBackupPruneKeepsOnlyCopies", TestBackupPruneKeepsOnlyCopies},
        {"TestTitleSaveLayout", TestTitleSaveLayout},
        {"TestFailedWriteIsNotCommitted", TestFailedWriteIsNotCommitted},
        {"TestReadOnlyCloseDoesNotCommit", TestReadOnlyCloseDoesNotCommit},
        {"TestBackupPruneUsesSequence", TestBackupPruneUsesSequence},
        {"TestBackupIsVerified", TestBackupIsVerified},
        {"TestRecoveryBeforeFirstAccess", TestRecoveryBeforeFirstAccess},
        {"TestRenameFailureNeverLosesSave", TestRenameFailureNeverLosesSave},
    };
    for (const auto& [name, test] : tests) {
        bool selected = argc <= 1;
        for (int index = 1; index < argc; ++index) selected = selected || name == argv[index];
        if (selected) test();
    }
    std::cout << "oot3d_native_a32_ctr_fs_tests: ok\n";
}
