#include "ctr_dsp_audio_frames.h"
#include "ctr_frame_boundary.h"
#include "ctr_title_profile.h"
#include "ctr_library_natives.h"
#include "oot3d_native_a32_ctr_host.h"
#include "oot3d_native_a32_dsp_hle.h"
#include "oot3d_native_a32_guest_state_dump.h"
#include "oot3d_native_pica_frontend.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using namespace Oot3dNativeGame;
using oot3d::recomp::a32::GuestState;

[[noreturn]] void Fail(std::string_view message) {
    std::cerr << "oot3d_native_a32_ctr_host_tests: " << message << '\n';
    std::exit(1);
}

void Require(bool condition, std::string_view message) {
    if (!condition) {
        Fail(message);
    }
}

// Every test runs twice: with the SVC fast paths off and on
// (config/kernel_svc); both must give the same registers, events and state.
bool gFastSvc = false;

NativeA32CtrHostConfig MakeConfig() {
    NativeA32CtrHostConfig config;
    config.FastSvc = gFastSvc;
    config.ResourceLimitValues = {
        0x18, 0x04000000, 0x20, 0x20, 0x20,
        0x08, 0x08,       0x10, 0x02, 0,
    };
    config.ResourceCurrentValues[1] = 0x1000;
    config.ResourceCurrentValues[2] = 1;
    config.LinearHeapBaseAddress = 0x14000000;
    config.LinearHeapSize = 0x04000000;
    config.HeapBaseAddress = 0x08000000;
    config.HeapSize = 0x08000000;
    config.HeadphonesConnected = true;
    config.SoundOutputMode = 2;
    config.SystemLanguage = 4;
    config.SystemRegion = 2;
    return config;
}

void PushSvcEvent(detail::NativeA32CtrSvcEventHistory& history,
                  uint32_t immediate, uint32_t threadId, std::string name,
                  uint32_t eventDetail) {
    NativeA32CtrSvcEvent event;
    event.Immediate = immediate;
    event.Pc = eventDetail * 4U;
    event.ThreadId = threadId;
    event.Handled = true;
    event.Name = std::move(name);
    event.Detail = eventDetail;
    history.push_back(std::move(event));
}

void TestBoundedSvcEventHistory() {
    detail::NativeA32CtrSvcEventHistory history(false, true, 3U);
    constexpr std::string_view filesystemName =
        "SendSyncRequest:fs:USER:OpenFile";
    constexpr std::string_view ipcName = "SendSyncRequest:hid:USER";
    constexpr std::string_view romFsReadName =
        "SendSyncRequest:file:romfs:Read";
    constexpr std::string_view flushName =
        "SendSyncRequest:gsp::Gpu:FlushDataCache";

    for (uint32_t index = 0U; index < 80U; ++index) {
        PushSvcEvent(history, 0x32U, 0U, std::string(filesystemName), index);
    }
    for (uint32_t index = 0U; index < 150U; ++index) {
        PushSvcEvent(history, 0x32U, 0U, std::string(ipcName), 1000U + index);
    }
    for (uint32_t index = 0U; index < 5U; ++index) {
        PushSvcEvent(history, 0x32U, 0U, std::string(romFsReadName),
                     4000U + index);
        PushSvcEvent(history, 0x32U, 0U, std::string(flushName),
                     5000U + index);
    }
    PushSvcEvent(history, 0x28U, 1U, "thread-one-last", 3001U);
    PushSvcEvent(history, 0x28U, 2U, "thread-two-last", 3002U);
    for (uint32_t index = 0U; index < 40U; ++index) {
        PushSvcEvent(history, 0x28U, 0U, "GetSystemTick", 2000U + index);
    }

    Require(history.TotalEventCount() == 282U,
            "bounded SVC history lost the exact total event count");
    Require(!history.RetainsFullHistory(),
            "bounded SVC history unexpectedly retained the full trace");
    const auto& events = history.Events();
    constexpr size_t maximumBoundedEvents =
        detail::NativeA32CtrSvcEventHistory::RecentEventCapacity +
        detail::NativeA32CtrSvcEventHistory::FilesystemEventCapacity +
        detail::NativeA32CtrSvcEventHistory::IpcEventCapacity + 3U;
    Require(events.size() <= maximumBoundedEvents,
            "bounded SVC history exceeded its diagnostic capacity");
    Require(events.size() >=
                detail::NativeA32CtrSvcEventHistory::RecentEventCapacity,
            "bounded SVC history omitted recent events");

    const size_t recentStart =
        events.size() -
        detail::NativeA32CtrSvcEventHistory::RecentEventCapacity;
    for (size_t index = 0U;
         index < detail::NativeA32CtrSvcEventHistory::RecentEventCapacity;
         ++index) {
        Require(events[recentStart + index].Detail == 2008U + index,
                "bounded SVC history changed the recent-32 summary");
    }

    std::vector<uint32_t> filesystemDetails;
    std::vector<uint32_t> ipcDetails;
    std::map<uint32_t, uint32_t> lastDetailByThread;
    const auto isFilesystemEvent = [](const NativeA32CtrSvcEvent& event) {
        return event.Name.find("fs:USER") != std::string::npos ||
               event.Name.find("file:romfs") != std::string::npos ||
               event.Name.find("file:savedata") != std::string::npos;
    };
    const auto isIpcSummaryEvent = [](const NativeA32CtrSvcEvent& event) {
        return event.Immediate == 0x32U &&
               event.Name.find("file:romfs:Read") == std::string::npos &&
               event.Name.find("gsp::Gpu:FlushDataCache") ==
                   std::string::npos;
    };
    size_t firstFilesystemEvent = 0U;
    size_t filesystemEventCount = 0U;
    size_t firstIpcEvent = 0U;
    size_t ipcEventCount = 0U;
    for (size_t index = events.size(); index > 0U; --index) {
        const auto& event = events[index - 1U];
        if (filesystemEventCount < 64U && isFilesystemEvent(event)) {
            firstFilesystemEvent = index - 1U;
            ++filesystemEventCount;
        }
        if (ipcEventCount < 128U && isIpcSummaryEvent(event)) {
            firstIpcEvent = index - 1U;
            ++ipcEventCount;
        }
        if (filesystemEventCount == 64U && ipcEventCount == 128U) {
            break;
        }
    }
    for (size_t index = firstFilesystemEvent; index < events.size(); ++index) {
        if (isFilesystemEvent(events[index])) {
            filesystemDetails.push_back(events[index].Detail);
        }
    }
    for (size_t index = firstIpcEvent; index < events.size(); ++index) {
        if (isIpcSummaryEvent(events[index])) {
            ipcDetails.push_back(events[index].Detail);
        }
    }
    for (const auto& event : events) {
        lastDetailByThread[event.ThreadId] = event.Detail;
    }
    Require(filesystemDetails.size() == 64U,
            "bounded SVC history changed the filesystem-64 count");
    for (size_t index = 0U; index < 59U; ++index) {
        Require(filesystemDetails[index] == 21U + index,
                "bounded SVC history changed filesystem event ordering");
    }
    for (size_t index = 0U; index < 5U; ++index) {
        Require(filesystemDetails[59U + index] == 4000U + index,
                "bounded SVC history omitted recent RomFS reads");
    }
    Require(ipcDetails.size() == 128U,
            "bounded SVC history changed the filtered IPC-128 count");
    for (size_t index = 0U; index < ipcDetails.size(); ++index) {
        Require(ipcDetails[index] == 1022U + index,
                "bounded SVC history changed filtered IPC ordering");
    }
    Require(lastDetailByThread.size() == 3U &&
                lastDetailByThread[0U] == 2039U &&
                lastDetailByThread[1U] == 3001U &&
                lastDetailByThread[2U] == 3002U,
            "bounded SVC history changed the last event per thread");

    const auto& ipcCounts = history.IpcNameCounts();
    Require(ipcCounts.size() == 4U &&
                ipcCounts.at(std::string(filesystemName)) == 80U &&
                ipcCounts.at(std::string(ipcName)) == 150U &&
                ipcCounts.at(std::string(romFsReadName)) == 5U &&
                ipcCounts.at(std::string(flushName)) == 5U,
            "bounded SVC history changed exact profiling counters");

    detail::NativeA32CtrSvcEventHistory fullHistory(true, false, 1U);
    for (uint32_t index = 0U; index < 300U; ++index) {
        PushSvcEvent(fullHistory, 0x28U, 0U, "GetSystemTick", index);
    }
    const auto& fullEvents = fullHistory.Events();
    Require(fullHistory.RetainsFullHistory() &&
                fullHistory.TotalEventCount() == 300U &&
                fullEvents.size() == 300U && fullEvents.front().Detail == 0U &&
                fullEvents.back().Detail == 299U,
            "full SVC history opt-in did not retain the complete trace");
}

} // namespace

void TestGuestAccessWatch() {
    // Diagnostic watch: watched pages lose their fast entry so typed accesses
    // take the checked path. Values must be identical, only visibility changes.
    NativeA32Memory memory;
    std::string error;
    Require(memory.MapRegion(
                {"watch", 0x1C000000, 0x8000, true, false, {}}, &error),
            error);
    constexpr uint32_t watchedAddress = 0x1C001000;
    constexpr uint32_t outsideAddress = 0x1C006000;
    const std::array<uint8_t, 4> initial{0x78, 0x56, 0x34, 0x12};
    const std::array<uint8_t, 4> replacement{0xEF, 0xBE, 0xAD, 0xDE};
    Require(memory.WriteBytes(watchedAddress, initial), "seed watched bytes");
    Require(memory.WriteBytes(outsideAddress, initial), "seed outside bytes");

    uint32_t value = 0;
    Require(memory.ReadFast<uint32_t>(watchedAddress, &value) &&
                value == 0x12345678U,
            "fast read before watching");
    Require(memory.WatchCounters().TypedReads == 0U,
            "unwatched memory must not be counted");

    const std::array<std::pair<uint32_t, uint32_t>, 1> ranges{
        {{watchedAddress, watchedAddress + 0x100U}}};
    memory.SetRangeWatch(ranges);
    Require(memory.RangeWatchActive() && memory.WatchedRangeCount() == 1U,
            "watch must be active after SetRangeWatch");

    value = 0;
    Require(memory.ReadFast<uint32_t>(watchedAddress, &value) &&
                value == 0x12345678U,
            "watched typed read must return the same bytes");
    Require(memory.WatchCounters().TypedReads == 1U &&
                memory.WatchCounters().FirstReadAddress == watchedAddress &&
                memory.WatchCounters().FirstReadSize == 4U,
            "watched typed read must be counted once with its identity");

    Require(memory.ReadFast<uint32_t>(outsideAddress, &value) &&
                value == 0x12345678U,
            "read outside the watch still works");
    Require(memory.WatchCounters().TypedReads == 1U,
            "read outside the watch must not be counted");

    Require(memory.WriteFast<uint32_t>(watchedAddress, 0xDEADBEEFU),
            "watched typed write");
    Require(memory.WatchCounters().TypedWrites == 1U,
            "watched typed write must be counted");
    Require(memory.ReadFast<uint32_t>(watchedAddress, &value) &&
                value == 0xDEADBEEFU,
            "watched typed write must land");

    std::array<uint8_t, 4> bytes{};
    Require(memory.ReadBytes(watchedAddress, bytes) &&
                bytes == replacement,
            "watched byte read must return the bytes just written");
    Require(memory.WatchCounters().ByteReads == 1U,
            "watched byte read must be counted");
    Require(memory.GetReadPointer(watchedAddress, 4U) != nullptr &&
                memory.WatchCounters().PointerReads == 1U,
            "handing out a live pointer must be recorded");

    memory.SetRangeWatchSuppressed(true);
    Require(memory.WriteBytes(watchedAddress, initial), "suppressed byte write");
    Require(memory.WatchCounters().ByteWrites == 0U,
            "suppressed byte write must not be counted");
    memory.SetRangeWatchSuppressed(false);
    Require(memory.WriteBytes(watchedAddress, replacement),
            "counted byte write");
    Require(memory.WatchCounters().ByteWrites == 1U,
            "byte write must be counted when not suppressed");

    memory.ClearRangeWatch();
    Require(!memory.RangeWatchActive() && memory.WatchedRangeCount() == 0U,
            "watch must be cleared");
    const auto counters = memory.WatchCounters();
    Require(memory.ReadFast<uint32_t>(watchedAddress, &value) &&
                value == 0xDEADBEEFU,
            "typed read after clearing the watch");
    Require(memory.WatchCounters().TypedReads == counters.TypedReads,
            "cleared watch must stop counting");
}

void TestGpuMemoryWriteOwnership() {
    NativeA32Memory memory;
    std::string error;
    Require(memory.MapRegion({"copy", 0x1000, 0x3000, true, false, {}}, &error), error);
    std::vector<std::pair<uint32_t, size_t>> writes;
    memory.SetWriteObserver([&](uint32_t address, size_t size) {
        writes.emplace_back(address, size);
    });
    Require(memory.ObserveWrites(0x2000, 0x100), "observe GPU copy destination");
    Require(memory.WriteFast<uint32_t>(0x2000, 0U), "same-value typed write");
    Require(writes == std::vector<std::pair<uint32_t, size_t>>{{0x2000, 4}},
            "same-value typed writes must revoke exact GPU byte ownership");
    Require(memory.WriteFast<uint32_t>(0x3000, 1U) &&
                memory.WriteFast<uint16_t>(0x21FF, 1U), "unrelated writes");
    Require(writes.size() == 1, "writes outside GPU-owned range must not revoke it");
    Require(memory.WriteFast<uint32_t>(0x1FFE, 0U), "cross-page partial write");
    Require(writes.back() == std::pair<uint32_t, size_t>{0x1FFE, 4},
            "partial overlap must preserve its actual address and length");
    const std::array<uint8_t, 2> zero{};
    Require(memory.WriteBytes(0x2004, zero) &&
                memory.WriteHostBytes(0x2006, zero) &&
                memory.Fill(0x2008, 2, 0), "bulk writes");
    Require(writes.size() == 5 && writes[2].first == 0x2004 &&
                writes[3].first == 0x2006 && writes[4].first == 0x2008,
            "host and bulk writes must revoke ownership even without byte changes");
    {
        NativeA32Memory::RangeWatchSuppression materialization(memory);
        Require(memory.WriteBytes(0x2000, zero), "GPU materialization");
    }
    Require(writes.size() == 5, "GPU materialization must retain GPU ownership");
    uint64_t value = 0, token = 0;
    uint32_t fault = 0, previous = 0;
    Require(memory.LoadExclusive(0x2000, 4, &value, &token, &fault) &&
                memory.StoreExclusive(0x2000, 4, value, token, &fault) ==
                    oot3d::recomp::a32::ExclusiveStoreResult::Success &&
                memory.AtomicSwap(0x2000, 4, 0, &previous, &fault),
            "atomic writes");
    Require(writes.size() == 7, "atomic same-value stores must revoke ownership");
    Require(memory.GetWritePointer(0x2010, 4) != nullptr &&
                writes.back() == std::pair<uint32_t, size_t>{0x2010, 4} &&
                !memory.ObserveWrites(0x2010, 4),
            "escaped mutable pointers must prevent future GPU ownership");
    const auto saved = memory.CaptureState();
    Require(memory.RestoreState(saved, &error), error);
    Require(memory.ObserveWrites(0x2010, 4) &&
                memory.WriteFast<uint32_t>(0x2010, 0U),
            "restored memory must retain the observer but not old allocation pointers");
    Require(writes.size() == 9 && writes.back().first == 0x2010,
            "restored page traps must observe writes");
    memory.SetWriteObserver({});
    Require(memory.WriteFast<uint32_t>(0x2010, 0U) && writes.size() == 9,
            "removing observer must detach callbacks");
}

void TestDeferredDeviceMemory() {
    NativeA32Memory memory;
    std::string error;
    Require(memory.MapRegion({"device", 0x1000, 0x4000, true, false, {}}, &error), error);
    size_t materialized = 0;
    const auto publish = [&](uint32_t address, size_t size, uint8_t value) {
        const std::array ranges{std::pair<uint32_t, size_t>{address, size}};
        return memory.DeferWrites(ranges,
            [&, value](uint32_t, std::span<uint8_t> bytes) {
                materialized += bytes.size();
                std::fill(bytes.begin(), bytes.end(), value);
                return true;
            });
    };
    Require(publish(0x1FF0, 64, 0x31), "publish cross-page device write");
    Require(materialized == 0, "device completion must not fetch unobserved bytes");
    uint32_t word = 0;
    Require(memory.ReadFast<uint32_t>(0x1FFE, &word) && word == 0x31313131 &&
                materialized == 4, "unaligned cross-page read must fetch only its bytes");
    Require(memory.WriteFast<uint16_t>(0x2004, 0) &&
                memory.Fill(0x2008, 4, 0x72), "CPU partial overwrites");
    Require(publish(0x2010, 32, 0x54), "new producer partially supersedes old producer");
    std::array<uint8_t, 64> actual{};
    Require(memory.ReadBytes(0x1FF0, actual), "materialize surviving versions");
    std::array<uint8_t, 64> expected;
    expected.fill(0x31);
    expected[20] = expected[21] = 0;
    std::fill(expected.begin() + 24, expected.begin() + 28, 0x72);
    std::fill(expected.begin() + 32, expected.end(), 0x54);
    Require(actual == expected && materialized == 58,
            "CPU writes and newer device writes must replace only their exact bytes");
    Require(publish(0x2000, 8, 0x83), "publish for atomics");
    uint64_t value = 0, token = 0;
    uint32_t fault = 0, previous = 0;
    Require(memory.LoadExclusive(0x2000, 4, &value, &token, &fault) &&
                value == 0x83838383, "exclusive load observes device result");
    Require(publish(0x2000, 4, 0x94) &&
                memory.StoreExclusive(0x2000, 4, 1, token, &fault) ==
                    oot3d::recomp::a32::ExclusiveStoreResult::ReservationLost,
            "device publication invalidates the exclusive reservation");
    Require(memory.AtomicSwap(0x2000, 4, 0x12345678, &previous, &fault) &&
                previous == 0x94949494 &&
                memory.ReadFast<uint32_t>(0x2000, &word) && word == 0x12345678,
            "atomic swap must read the producer then preserve its replacement");
    Require(publish(0x3000, 16, 0xA5), "publish pointer-backed device result");
    const auto* pointer = memory.GetReadPointer(0x3000, 16);
    Require(pointer && pointer[15] == 0xA5 && !publish(0x3000, 16, 0xB6),
            "escaped read pointer must be coherent and prevent later deferral");
    Require(publish(0x4000, 16, 0xC7), "publish for snapshot");
    const auto snapshot = memory.CaptureState();
    NativeA32Memory restored;
    Require(restored.RestoreState(snapshot, &error), error);
    Require(restored.ReadFast<uint32_t>(0x4000, &word) && word == 0xC7C7C7C7,
            "savestate contains device bytes, not host callbacks or stale RAM");
    Require(publish(0x4010, 8, 0xD8), "publish for memory clone");
    const NativeA32Memory cloned(memory);
    Require(cloned.ReadFast<uint32_t>(0x4010, &word) && word == 0xD8D8D8D8,
            "copied memory owns a materialized independent image");
    Require(memory.RestoreState(snapshot, &error) && publish(0x3000, 16, 0xE9),
            "replacing memory releases pointers to the old allocation");
    Require(memory.GetWritePointer(0x3000, 16)[0] == 0xE9 &&
                !publish(0x3000, 16, 0xFA),
            "escaped writable pointers also materialize and exclude deferral");
}

void TestDeferredDeviceFailureAndOverwrite() {
    NativeA32Memory memory;
    std::string error;
    Require(memory.MapRegion({"device", 0x1000, 0x1000, true, false, {}}, &error), error);
    const std::array ranges{std::pair<uint32_t, size_t>{0x1100, 4}};
    Require(memory.DeferWrites(ranges, [](uint32_t, std::span<uint8_t>) { return false; }),
            "publish a producer that will fail on materialization");
    uint32_t value = 0xFFFFFFFF;
    Require(!memory.ReadFast<uint32_t>(0x1100, &value) && value == 0xFFFFFFFF,
            "failed producer must fault instead of exposing stale bytes");
    bool rejected = false;
    try { (void)memory.CaptureState(); } catch (const std::exception&) { rejected = true; }
    Require(rejected, "state capture must reject unresolved device output");
    Require(memory.WriteFast<uint32_t>(0x1100, 0x10203040) &&
                memory.ReadFast<uint32_t>(0x1100, &value) && value == 0x10203040,
            "a complete overwrite discards obsolete work without reading it");
}

// nn::fnd::DateTime::GetNow (EUR 004538D8) as the title computes it.
uint64_t GuestDateTimeNow(NativeA32Memory& memory, uint64_t tick) {
    uint32_t counter = 0;
    uint64_t date = 0, updateTick = 0, coefficient = 0;
    uint32_t faultAddress = 0;
    Require(memory.Read32(0x1FF81000U, &counter),
            "shared page counter is unreadable");
    const uint32_t base = 0x1FF81020U + (counter & 1U) * 0x20U;
    Require(memory.Read64(base, &date, &faultAddress) &&
                memory.Read64(base + 8U, &updateTick, &faultAddress) &&
                memory.Read64(base + 16U, &coefficient, &faultAddress) &&
                coefficient != 0U,
            "shared page DateTime slot is unreadable");
    return date + (tick - updateTick) * 1000U / coefficient;
}

void TestSharedPageDateTime() {
    constexpr uint64_t kBase = 3517257600000ULL; // 2011-06-16 00:00
    constexpr uint64_t kTicksPerSecond = 268111856ULL;
    NativeA32Memory memory;
    std::string error;
    Require(memory.MapRegion(
                {"tls", 0x1FF82000, 0x1000, true, false, {}}, &error) &&
                memory.MapRegion({"ctr_shared_page", 0x1FF81000, 0x1000,
                                  false, false, {}},
                                 &error),
            error);
    NativeA32CtrHostConfig config = MakeConfig();
    config.FixedDateTimeMilliseconds = kBase;
    NativeA32CtrHostServices host(std::move(config));
    const oot3d::recomp::a32::Registry registry{};
    NativeA32Process process(registry, host);
    NativeA32HostContext context{process, 0};
    GuestState state{};
    state.thread_pointer = 0x1FF82000;

    host.AdvanceSystemTicks(kTicksPerSecond * 90U);
    Require(host.HandleSvc(0x28, state, memory, context).Action ==
                NativeA32HostAction::Resume,
            "GetSystemTick did not resume with the shared page mapped");
    uint32_t counter = 0;
    uint64_t coefficient = 0;
    uint32_t faultAddress = 0;
    Require(memory.Read32(0x1FF81000U, &counter) && counter == 1U &&
                memory.Read64(0x1FF81050U, &coefficient, &faultAddress) &&
                coefficient == kTicksPerSecond,
            "GetSystemTick did not publish the RTC in DateTime slot 1");
    Require(GuestDateTimeNow(memory, host.SystemTicks()) == kBase + 90000U,
            "shared page RTC does not match the fixed base");

    // Within an emulated second the guest extrapolates the same slot.
    host.AdvanceSystemTicks(kTicksPerSecond / 2U);
    Require(host.HandleSvc(0x28, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                memory.Read32(0x1FF81000U, &counter) && counter == 1U &&
                GuestDateTimeNow(memory, host.SystemTicks()) ==
                    kBase + 90500U,
            "shared page RTC was not extrapolated from its update tick");
    host.AdvanceSystemTicks(kTicksPerSecond);
    Require(host.HandleSvc(0x28, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                memory.Read32(0x1FF81000U, &counter) && counter == 2U &&
                GuestDateTimeNow(memory, host.SystemTicks()) ==
                    kBase + 91500U,
            "shared page RTC did not flip to slot 0 after a second");

    // Unset, the RTC follows the host's local wall clock.
    NativeA32Memory hostClockMemory;
    Require(hostClockMemory.MapRegion({"ctr_shared_page", 0x1FF81000,
                                       0x1000, false, false, {}},
                                      &error),
            error);
    NativeA32CtrHostServices hostClock(MakeConfig());
    Require(hostClock.RefreshSharedPageDateTime(hostClockMemory, true),
            "host clock RTC was not published");
    const std::time_t utc = std::time(nullptr);
#if defined(__linux__)
    std::tm local{};
    Require(localtime_r(&utc, &local) != nullptr, "localtime failed");
    const int64_t zoneMilliseconds = local.tm_gmtoff * 1000LL;
    const int64_t tolerance = 5000;
#else
    // Elsewhere the zone comes from the platform (the Switch time
    // service); bound it by the widest real UTC offsets.
    const int64_t zoneMilliseconds = 0;
    const int64_t tolerance = 15LL * 3600LL * 1000LL;
#endif
    const int64_t expected =
        (static_cast<int64_t>(utc) + 2209075200LL) * 1000 + zoneMilliseconds;
    const int64_t published = static_cast<int64_t>(
        GuestDateTimeNow(hostClockMemory, hostClock.SystemTicks()));
    Require(published > expected - tolerance &&
                published < expected + tolerance,
            "host clock RTC is not the local wall clock");
}


// --- Objetos de sincronização do guest (perfil de outro título) ------------
// Regressão do boot de títulos que não o OoT3D (Super Mario 3D Land):
// CreateMutex/CreateSemaphore/ReleaseSemaphore/CreateTimer/SetTimer/
// CancelTimer/ClearTimer e WaitSynchronization1/N com timeout faltavam no
// HLE e paravam o boot com TRIAEVUM_HOST_REQUEST_UNSERVED.
constexpr uint32_t kTestResultTimeout = 0x09401BFEU;
constexpr uint32_t kTestOutOfRange = 0xD8E007FDU;
constexpr uint32_t kTestInvalidCombination = 0xE0E01BEEU;

struct GuestThreads {
    NativeA32CtrHostServices& Host;
    NativeA32Process& Process;

    GuestState& State(uint32_t thread) {
        auto* state = Process.ThreadState(thread);
        Require(state != nullptr, "guest test thread does not exist");
        return *state;
    }

    NativeA32HostAction Svc(uint32_t thread, uint32_t immediate) {
        NativeA32HostContext context{Process, thread};
        return Host.HandleSvc(immediate, State(thread), Process.Memory(),
                              context)
            .Action;
    }

    // WaitSynchronization1(handle, timeout em ns; negativo = infinito).
    NativeA32HostAction Wait1(uint32_t thread, uint32_t handle,
                              int64_t timeout) {
        auto& state = State(thread);
        state.r[0] = handle;
        state.r[2] = static_cast<uint32_t>(static_cast<uint64_t>(timeout));
        state.r[3] =
            static_cast<uint32_t>(static_cast<uint64_t>(timeout) >> 32U);
        return Svc(thread, 0x24);
    }

    NativeA32HostAction WaitN(uint32_t thread, uint32_t handle,
                              int64_t timeout) {
        auto& state = State(thread);
        const uint32_t array = state.thread_pointer + 0x40U;
        Require(Process.Memory().Write32(array, handle),
                "cannot write wait handle array");
        state.r[0] = static_cast<uint32_t>(static_cast<uint64_t>(timeout));
        state.r[1] = array;
        state.r[2] = 1;
        state.r[3] = 0;
        state.r[4] =
            static_cast<uint32_t>(static_cast<uint64_t>(timeout) >> 32U);
        return Svc(thread, 0x25);
    }
};

void MakeGuestThreads(NativeA32Process& process) {
    std::string error;
    Require(process.MapRegion({"text", 0x00100000, 0x1000, false, true, {}},
                              &error) &&
                process.MapRegion(
                    {"stacks", 0x0FF00000, 0x00010000, true, false, {}},
                    &error),
            error);
    NativeA32PrimaryThreadConfig primary;
    primary.EntryAddress = 0x00100000;
    primary.StackBaseAddress = 0x0FF10000;
    primary.StackSize = 0x4000;
    primary.TlsBaseAddress = 0x1FF82000;
    primary.TlsSize = 0x1000;
    primary.ThreadPointer = 0x1FF82000;
    primary.InitialCpsr = 0x10;
    primary.Priority = 0x30;
    Require(process.CreatePrimaryThread(primary, &error), error);
    // Thread 1 tem prioridade maior (número menor) que a thread 2.
    Require(process.CreateThread({0x00100000, 1, 0x0FF08000, 0x10, 0, 0x20},
                                 &error) == 1U &&
                process.CreateThread(
                    {0x00100000, 2, 0x0FF0C000, 0x10, 0, 0x31}, &error) == 2U,
            "cannot create guest test threads: " + error);
}

// Espera num handle inválido devolve 0xD8E007F7 (InvalidHandle), como o
// svc.cpp do Azahar; o MK7 esperava num 0xFFFFFFFF e o host parava. O
// OoT3D (sem KernelWaitTimeouts) continua com a falha nomeada (UNSERVED).
void TestWaitOnInvalidHandle() {
    constexpr uint32_t kResultInvalidHandle = 0xD8E007F7U;
    NativeA32CtrHostConfig config = MakeConfig();
    config.KernelWaitTimeouts = true;
    NativeA32CtrHostServices host(std::move(config));
    const oot3d::recomp::a32::Registry registry{};
    NativeA32Process process(registry, host);
    MakeGuestThreads(process);
    GuestThreads guest{host, process};
    Require(guest.Wait1(0, 0xFFFFFFFFU, -1) == NativeA32HostAction::Resume &&
                guest.State(0).r[0] == kResultInvalidHandle,
            "WaitSynchronization1 on an invalid handle did not fail");
    Require(guest.WaitN(1, 0xFFFFFFFFU, -1) == NativeA32HostAction::Resume &&
                guest.State(1).r[0] == kResultInvalidHandle,
            "WaitSynchronizationN on an invalid handle did not fail");

    NativeA32CtrHostServices oot(MakeConfig());
    NativeA32Process ootProcess(registry, oot);
    MakeGuestThreads(ootProcess);
    GuestThreads ootGuest{oot, ootProcess};
    Require(ootGuest.WaitN(0, 0xFFFFFFFFU, -1) == NativeA32HostAction::Fault,
            "the OoT3D profile changed the invalid-handle wait");
}

void TestGuestMutexSemaphoreAndTimeouts() {
    NativeA32CtrHostConfig config = MakeConfig();
    config.KernelWaitTimeouts = true;
    NativeA32CtrHostServices host(std::move(config));
    const oot3d::recomp::a32::Registry registry{};
    NativeA32Process process(registry, host);
    MakeGuestThreads(process);
    GuestThreads guest{host, process};

    // Mutex criado travado pela thread 0; duas esperas infinitas.
    guest.State(0).r[1] = 1;
    Require(guest.Svc(0, 0x13) == NativeA32HostAction::Resume &&
                guest.State(0).r[0] == 0 && guest.State(0).r[1] != 0,
            "CreateMutex(locked) did not return a handle");
    const uint32_t mutex = guest.State(0).r[1];
    guest.State(1).r[1] = 0xABCD1234U;
    Require(guest.Wait1(1, mutex, -1) == NativeA32HostAction::Wait &&
                guest.Wait1(2, mutex, -1) == NativeA32HostAction::Wait,
            "a locked mutex did not park its waiters");
    guest.State(0).r[0] = mutex;
    Require(guest.Svc(0, 0x14) == NativeA32HostAction::Resume,
            "ReleaseMutex by the owner failed");
    Require(guest.State(1).r[0] == 0 && guest.State(1).r[1] == 0xABCD1234U &&
                guest.State(2).r[0] == mutex,
            "ReleaseMutex did not hand the mutex to the higher-priority "
            "waiter only (or WaitSynchronization1 wrote r1)");
    guest.State(1).r[0] = mutex;
    Require(guest.Svc(1, 0x14) == NativeA32HostAction::Resume &&
                guest.State(2).r[0] == 0,
            "the second mutex waiter was not woken");
    // Recursivo para o dono; soltar sem ser dono para o host (falha nomeada).
    Require(guest.Wait1(2, mutex, -1) == NativeA32HostAction::Resume,
            "the mutex owner could not lock recursively");
    guest.State(2).r[0] = mutex;
    Require(guest.Svc(2, 0x14) == NativeA32HostAction::Resume,
            "recursive ReleaseMutex failed");
    guest.State(2).r[0] = mutex;
    Require(guest.Svc(2, 0x14) == NativeA32HostAction::Resume,
            "final ReleaseMutex failed");
    guest.State(0).r[0] = mutex;
    Require(guest.Svc(0, 0x14) == NativeA32HostAction::Fault,
            "ReleaseMutex by a non-owner was not a named host fault");
    guest.State(0).r[1] = 0;
    Require(guest.Svc(0, 0x13) == NativeA32HostAction::Resume &&
                guest.Wait1(1, guest.State(0).r[1], 0) ==
                    NativeA32HostAction::Resume &&
                guest.State(1).r[0] == 0,
            "an unlocked mutex was not immediately acquired");

    // Semáforo: combinação inválida, espera, liberação, limite e consulta.
    guest.State(0).r[1] = 3;
    guest.State(0).r[2] = 2;
    Require(guest.Svc(0, 0x15) == NativeA32HostAction::Resume &&
                guest.State(0).r[0] == kTestInvalidCombination,
            "CreateSemaphore accepted initial > maximum");
    guest.State(0).r[1] = 0;
    guest.State(0).r[2] = 2;
    Require(guest.Svc(0, 0x15) == NativeA32HostAction::Resume &&
                guest.State(0).r[0] == 0,
            "CreateSemaphore failed");
    const uint32_t semaphore = guest.State(0).r[1];
    Require(guest.WaitN(1, semaphore, -1) == NativeA32HostAction::Wait,
            "an empty semaphore did not park its waiter");
    guest.State(0).r[1] = semaphore;
    guest.State(0).r[2] = 1;
    Require(guest.Svc(0, 0x16) == NativeA32HostAction::Resume &&
                guest.State(0).r[0] == 0 && guest.State(0).r[1] == 0 &&
                guest.State(1).r[0] == 0 && guest.State(1).r[1] == 0,
            "ReleaseSemaphore did not wake the waiter with index 0");
    guest.State(0).r[1] = semaphore;
    guest.State(0).r[2] = 3;
    Require(guest.Svc(0, 0x16) == NativeA32HostAction::Resume &&
                guest.State(0).r[0] == kTestOutOfRange,
            "ReleaseSemaphore exceeded the maximum count");
    guest.State(0).r[1] = semaphore;
    guest.State(0).r[2] = 2;
    Require(guest.Svc(0, 0x16) == NativeA32HostAction::Resume &&
                guest.State(0).r[0] == 0 && guest.State(0).r[1] == 0,
            "ReleaseSemaphore to the maximum failed");
    Require(guest.Wait1(0, semaphore, 0) == NativeA32HostAction::Resume &&
                guest.State(0).r[0] == 0 &&
                guest.Wait1(0, semaphore, 0) == NativeA32HostAction::Resume &&
                guest.State(0).r[0] == 0 &&
                guest.Wait1(0, semaphore, 0) == NativeA32HostAction::Resume &&
                guest.State(0).r[0] == kTestResultTimeout,
            "semaphore polls did not consume two counts then time out");

    // Espera com prazo: expira com 0x09401BFE (N também devolve r1 = -1).
    guest.State(0).r[1] = 0;
    Require(guest.Svc(0, 0x17) == NativeA32HostAction::Resume,
            "CreateEvent failed");
    const uint32_t event = guest.State(0).r[1];
    const uint64_t start = host.SystemTicks();
    constexpr uint64_t kTwoMillisecondTicks = 536224U; // ceil(2 ms)
    Require(guest.Wait1(1, event, 2'000'000) == NativeA32HostAction::Wait &&
                host.NextSleepWakeTick() == start + kTwoMillisecondTicks,
            "a timed wait did not publish its deadline");
    Require(guest.WaitN(2, event, 1'000'000) == NativeA32HostAction::Wait,
            "a timed WaitSynchronizationN did not park");
    host.AdvanceSystemTicks(268112U);
    Require(guest.State(2).r[0] == kTestResultTimeout &&
                guest.State(2).r[1] == 0xFFFFFFFFU &&
                guest.State(1).r[0] == event,
            "WaitSynchronizationN did not time out at its own deadline");
    host.AdvanceSystemTicks(kTwoMillisecondTicks - 268112U);
    Require(guest.State(1).r[0] == kTestResultTimeout &&
                !host.NextSleepWakeTick().has_value(),
            "WaitSynchronization1 did not time out at its deadline");
    // Sinalizado antes do prazo: o prazo some com a espera.
    Require(guest.Wait1(1, event, 5'000'000) == NativeA32HostAction::Wait,
            "second timed wait did not park");
    guest.State(0).r[0] = event;
    Require(guest.Svc(0, 0x18) == NativeA32HostAction::Resume &&
                guest.State(1).r[0] == 0 &&
                !host.NextSleepWakeTick().has_value(),
            "a signaled timed wait kept its deadline");
    guest.State(1).r[0] = 0x5A5A5A5AU;
    host.AdvanceSystemTicks(10U * 268112U);
    Require(guest.State(1).r[0] == 0x5A5A5A5AU,
            "an already satisfied wait timed out later");
}

void TestGuestKernelTimers() {
    NativeA32CtrHostConfig config = MakeConfig();
    config.KernelWaitTimeouts = true;
    NativeA32CtrHostServices host(std::move(config));
    const oot3d::recomp::a32::Registry registry{};
    NativeA32Process process(registry, host);
    MakeGuestThreads(process);
    GuestThreads guest{host, process};
    constexpr uint64_t kMillisecondTicks = 268112U; // ceil(1 ms)
    const auto createTimer = [&](uint32_t resetType) {
        guest.State(0).r[1] = resetType;
        Require(guest.Svc(0, 0x1A) == NativeA32HostAction::Resume &&
                    guest.State(0).r[0] == 0,
                "CreateTimer failed");
        return guest.State(0).r[1];
    };
    const auto setTimer = [&](uint32_t timer, int64_t initial,
                              int64_t interval) {
        auto& state = guest.State(0);
        state.r[0] = timer;
        state.r[2] = static_cast<uint32_t>(initial);
        state.r[3] = static_cast<uint32_t>(static_cast<uint64_t>(initial) >> 32U);
        state.r[1] = static_cast<uint32_t>(interval);
        state.r[4] =
            static_cast<uint32_t>(static_cast<uint64_t>(interval) >> 32U);
        return guest.Svc(0, 0x1B) == NativeA32HostAction::Resume &&
               state.r[0] == 0;
    };

    // One-shot: dispara no prazo exato e acorda a espera.
    const uint32_t oneShot = createTimer(0);
    Require(guest.Wait1(1, oneShot, -1) == NativeA32HostAction::Wait &&
                setTimer(oneShot, 1'000'000, 0) &&
                host.NextSleepWakeTick() ==
                    host.SystemTicks() + kMillisecondTicks,
            "SetTimer did not schedule the one-shot timer");
    host.AdvanceSystemTicks(kMillisecondTicks - 1U);
    Require(guest.State(1).r[0] == oneShot, "timer fired early");
    host.AdvanceSystemTicks(1U);
    Require(guest.State(1).r[0] == 0 && !host.NextSleepWakeTick().has_value(),
            "one-shot timer did not fire once at its deadline");
    guest.State(0).r[0] = oneShot;
    Require(guest.Wait1(0, oneShot, 0) == NativeA32HostAction::Resume &&
                guest.State(0).r[0] == kTestResultTimeout,
            "a consumed one-shot timer stayed signaled");

    // Periódico: sinaliza, rearma; CancelTimer para; savestate preserva.
    Require(setTimer(oneShot, 1'000'000, 1'000'000), "periodic SetTimer");
    host.AdvanceSystemTicks(kMillisecondTicks);
    Require(guest.Wait1(0, oneShot, 0) == NativeA32HostAction::Resume &&
                guest.State(0).r[0] == 0 &&
                host.NextSleepWakeTick() ==
                    host.SystemTicks() + kMillisecondTicks,
            "periodic timer did not signal and re-arm");
    const auto saved = host.CaptureState();
    const auto savedWake = host.NextSleepWakeTick();
    std::string error;
    guest.State(0).r[0] = oneShot;
    Require(guest.Svc(0, 0x1C) == NativeA32HostAction::Resume &&
                !host.NextSleepWakeTick().has_value(),
            "CancelTimer did not disarm the timer");
    Require(host.RestoreState(saved, process, &error) &&
                host.NextSleepWakeTick() == savedWake,
            "an armed kernel timer did not survive a savestate: " + error);
    guest.State(0).r[0] = oneShot;
    Require(guest.Svc(0, 0x1C) == NativeA32HostAction::Resume,
            "CancelTimer after restore failed");

    // Pulse: acorda todas as esperas e não fica sinalizado.
    const uint32_t pulse = createTimer(2);
    Require(guest.Wait1(1, pulse, -1) == NativeA32HostAction::Wait &&
                guest.Wait1(2, pulse, -1) == NativeA32HostAction::Wait &&
                setTimer(pulse, 0, 0) && guest.State(1).r[0] == 0 &&
                guest.State(2).r[0] == 0,
            "a pulse timer did not wake every waiter");
    Require(guest.Wait1(0, pulse, 0) == NativeA32HostAction::Resume &&
                guest.State(0).r[0] == kTestResultTimeout,
            "a pulse timer stayed signaled");

    // Sticky: fica sinalizado até ClearTimer.
    const uint32_t sticky = createTimer(1);
    Require(setTimer(sticky, 0, 0) &&
                guest.Wait1(0, sticky, 0) == NativeA32HostAction::Resume &&
                guest.State(0).r[0] == 0 &&
                guest.Wait1(0, sticky, 0) == NativeA32HostAction::Resume &&
                guest.State(0).r[0] == 0,
            "a sticky timer was consumed by a wait");
    guest.State(0).r[0] = sticky;
    Require(guest.Svc(0, 0x1D) == NativeA32HostAction::Resume &&
                guest.Wait1(0, sticky, 0) == NativeA32HostAction::Resume &&
                guest.State(0).r[0] == kTestResultTimeout,
            "ClearTimer did not reset a sticky timer");
    Require(!setTimer(sticky, -1, 0) &&
                guest.State(0).r[0] == kTestOutOfRange,
            "SetTimer accepted a negative initial delay");

    // Timer sem handle: CloseHandle o destrói (nada mais a acordar).
    Require(setTimer(sticky, 1'000'000, 0) &&
                host.NextSleepWakeTick().has_value(),
            "sticky timer did not re-arm");
    guest.State(0).r[0] = sticky;
    Require(guest.Svc(0, 0x23) == NativeA32HostAction::Resume &&
                !host.NextSleepWakeTick().has_value(),
            "a closed kernel timer kept its schedule");
}

void TestOot3dProfileWaitsStayInfinite() {
    // Perfil do OoT3D (defaults): timeout ignorado como sempre foi, e serviço
    // desconhecido continua sendo falha nomeada do host.
    NativeA32CtrHostServices host(MakeConfig());
    const oot3d::recomp::a32::Registry registry{};
    NativeA32Process process(registry, host);
    MakeGuestThreads(process);
    GuestThreads guest{host, process};
    guest.State(0).r[1] = 0;
    Require(guest.Svc(0, 0x17) == NativeA32HostAction::Resume,
            "CreateEvent failed");
    const uint32_t event = guest.State(0).r[1];
    Require(guest.Wait1(1, event, 0) == NativeA32HostAction::Wait &&
                guest.WaitN(2, event, 1'000'000) == NativeA32HostAction::Wait &&
                !host.NextSleepWakeTick().has_value(),
            "the OoT3D profile started honoring wait timeouts");
    Require(host.SvcEvents().back().Name.find(":timeout=") ==
                std::string::npos,
            "the OoT3D profile changed the WaitSynchronizationN event name");
}

uint32_t OpenSrv(GuestThreads& guest) {
    constexpr std::array<uint8_t, 5> portName{'s', 'r', 'v', ':', 0};
    Require(guest.Process.Memory().WriteBytes(0x0FF00100, portName),
            "cannot write srv port name");
    guest.State(0).r[1] = 0x0FF00100;
    Require(guest.Svc(0, 0x2D) == NativeA32HostAction::Resume,
            "ConnectToPort(srv:) failed");
    return guest.State(0).r[1];
}

NativeA32HostAction GetServiceHandle(GuestThreads& guest, uint32_t srv,
                                     std::string_view name,
                                     uint32_t* session) {
    auto& memory = guest.Process.Memory();
    std::array<uint8_t, 8> bytes{};
    std::copy(name.begin(), name.end(), bytes.begin());
    uint32_t low = 0;
    uint32_t high = 0;
    for (size_t index = 0; index < 4; ++index) {
        low |= static_cast<uint32_t>(bytes[index]) << (index * 8U);
        high |= static_cast<uint32_t>(bytes[index + 4U]) << (index * 8U);
    }
    Require(memory.Write32(0x1FF82080, 0x00050100) &&
                memory.Write32(0x1FF82084, low) &&
                memory.Write32(0x1FF82088, high) &&
                memory.Write32(0x1FF8208C, static_cast<uint32_t>(name.size())),
            "cannot write srv:GetServiceHandle");
    guest.State(0).r[0] = srv;
    const auto action = guest.Svc(0, 0x32);
    if (session != nullptr) {
        Require(memory.Read32(0x1FF8208C, session), "cannot read session");
    }
    return action;
}

void TestUnknownServicesAreLoggedStubs() {
    NativeA32CtrHostConfig config = MakeConfig();
    config.StubUnknownServices = true;
    NativeA32CtrHostServices host(std::move(config));
    const oot3d::recomp::a32::Registry registry{};
    NativeA32Process process(registry, host);
    MakeGuestThreads(process);
    GuestThreads guest{host, process};
    auto& memory = process.Memory();
    const uint32_t srv = OpenSrv(guest);
    uint32_t ptm = 0;
    Require(GetServiceHandle(guest, srv, "ptm:u", &ptm) ==
                    NativeA32HostAction::Resume &&
                ptm != 0 &&
                host.SvcEvents().back().Name ==
                    "SendSyncRequest:srv::GetServiceHandle:ptm:u:stub",
            "an unknown service did not get a logged stub session");
    Require(memory.Write32(0x1FF82080, 0x00050000) &&
                memory.Write32(0x1FF82088, 0xDEADBEEFU),
            "cannot write stub request");
    guest.State(0).r[0] = ptm;
    uint32_t header = 0;
    uint32_t result = 1;
    uint32_t cleared = 1;
    Require(guest.Svc(0, 0x32) == NativeA32HostAction::Resume &&
                guest.State(0).r[0] == 0 &&
                memory.Read32(0x1FF82080, &header) && header == 0x00050040 &&
                memory.Read32(0x1FF82084, &result) && result == 0 &&
                memory.Read32(0x1FF82088, &cleared) && cleared == 0 &&
                host.SvcEvents().back().Name.find(":stub:cmd=0x00000005") !=
                    std::string::npos,
            "a stub service request did not answer success with a clean "
            "response");
    // Comando sem HLE de um serviço conhecido também vira stub registrado.
    uint32_t apt = 0;
    Require(GetServiceHandle(guest, srv, "APT:U", &apt) ==
                NativeA32HostAction::Resume,
            "APT:U was not opened");
    Require(memory.Write32(0x1FF82080, 0x00FE0000), "cannot write APT stub");
    guest.State(0).r[0] = apt;
    Require(guest.Svc(0, 0x32) == NativeA32HostAction::Resume &&
                memory.Read32(0x1FF82080, &header) && header == 0x00FE0040,
            "an unhandled APT:U command was not stubbed");
    // srv: Subscribe etc. sem HLE também.
    Require(memory.Write32(0x1FF82080, 0x00090040) &&
                memory.Write32(0x1FF82084, 0x100),
            "cannot write srv:Subscribe");
    guest.State(0).r[0] = srv;
    Require(guest.Svc(0, 0x32) == NativeA32HostAction::Resume &&
                memory.Read32(0x1FF82080, &header) && header == 0x00090040,
            "srv:Subscribe was not stubbed");

    // Perfil OoT3D: serviço desconhecido continua falha nomeada.
    NativeA32CtrHostServices strictHost(MakeConfig());
    NativeA32Process strictProcess(registry, strictHost);
    MakeGuestThreads(strictProcess);
    GuestThreads strict{strictHost, strictProcess};
    const uint32_t strictSrv = OpenSrv(strict);
    Require(GetServiceHandle(strict, strictSrv, "ptm:u", nullptr) ==
                NativeA32HostAction::Fault,
            "the OoT3D profile stubbed an unknown service");
}

// cecd:u 0x000F (GetCecInfoEventHandle) e 0x0010 (GetChangeStateEventHandle)
// devolvem um evento (Azahar cecd.cpp). O stub zerado dava handle 0 e o MK7
// esperava nele junto com outro evento: WaitSynchronizationN parava o boot.
void TestCecdEventHandles() {
    NativeA32CtrHostConfig config = MakeConfig();
    config.StubUnknownServices = true;
    config.KernelWaitTimeouts = true;
    NativeA32CtrHostServices host(std::move(config));
    const oot3d::recomp::a32::Registry registry{};
    NativeA32Process process(registry, host);
    MakeGuestThreads(process);
    GuestThreads guest{host, process};
    auto& memory = process.Memory();
    const uint32_t srv = OpenSrv(guest);
    uint32_t cecd = 0;
    Require(GetServiceHandle(guest, srv, "cecd:u", &cecd) ==
                    NativeA32HostAction::Resume && cecd != 0,
            "cecd:u was not opened");
    for (const uint32_t command : {0x000FU, 0x0010U}) {
        Require(memory.Write32(0x1FF82080, command << 16U),
                "cannot write cecd:u request");
        guest.State(0).r[0] = cecd;
        uint32_t header = 0;
        uint32_t result = 1;
        uint32_t descriptor = 1;
        uint32_t handle = 0;
        Require(guest.Svc(0, 0x32) == NativeA32HostAction::Resume &&
                    memory.Read32(0x1FF82080, &header) &&
                    header == ((command << 16U) | 0x42U) &&
                    memory.Read32(0x1FF82084, &result) && result == 0 &&
                    memory.Read32(0x1FF82088, &descriptor) && descriptor == 0 &&
                    memory.Read32(0x1FF8208C, &handle) && handle != 0,
                "cecd:u did not return an event handle");
        Require(guest.Wait1(1, handle, 0) == NativeA32HostAction::Resume &&
                    guest.State(1).r[0] == kTestResultTimeout,
                "the cecd:u event is not a valid, unsignaled event");
    }
}

// O boot probe não entregava os quadros de áudio do DSP: a thread de som do
// MK7 esperava a interrupção do DSP para sempre (36f8cbf2). Uma thread espera
// o evento da pipe de áudio; passado um quadro, a entrega faz o mix, sinaliza
// o DSP e roda o guest, e a thread acorda.
void TestDspAudioFrameDeliveryWakesSoundThread() {
    NativeA32CtrHostServices host(MakeConfig());
    const oot3d::recomp::a32::Registry registry{};
    NativeA32Process process(registry, host);
    std::string error;
    // As duas regiões de memória compartilhada do DSP (0x1FF50000/0x1FF70000);
    // regiões só antes de criar as threads.
    Require(process.MapRegion(
                {"dsp", 0x1FF50000, 0x30000, true, false, {}}, &error),
            "cannot map DSP shared memory");
    MakeGuestThreads(process);
    GuestThreads guest{host, process};
    auto& memory = process.Memory();
    const uint32_t srv = OpenSrv(guest);
    uint32_t dsp = 0;
    Require(GetServiceHandle(guest, srv, "dsp::DSP", &dsp) ==
                    NativeA32HostAction::Resume && dsp != 0,
            "dsp::DSP was not opened");
    guest.State(0).r[1] = 0; // one-shot
    Require(guest.Svc(0, 0x17) == NativeA32HostAction::Resume,
            "cannot create the DSP pipe event");
    const uint32_t pipeEvent = guest.State(0).r[1];
    const auto dspRequest = [&](std::initializer_list<uint32_t> words) {
        uint32_t address = 0x1FF82080;
        for (const uint32_t word : words) {
            Require(memory.Write32(address, word), "cannot write DSP request");
            address += 4;
        }
        guest.State(0).r[0] = dsp;
        Require(guest.Svc(0, 0x32) == NativeA32HostAction::Resume,
                "DSP request did not resume");
    };
    dspRequest({0x00150082, 2, 2, 0, pipeEvent}); // RegisterInterruptEvents
    constexpr std::array<uint8_t, 4> initialize{0, 0, 0xA5, 0x5A};
    Require(memory.WriteBytes(0x0FF00200, initialize),
            "cannot write DSP initialize command");
    dspRequest({0x000D0082, 2, 4, 0x00010402, 0x0FF00200}); // WriteProcessPipe
    Require(guest.Wait1(1, pipeEvent, 0) == NativeA32HostAction::Resume &&
                guest.State(1).r[0] == 0,
            "DSP initialization did not signal the pipe event");
    Require(guest.Wait1(1, pipeEvent, -1) == NativeA32HostAction::Wait,
            "the sound thread did not wait for the next DSP frame");

    host.AdvanceSystemTicks(NativeA32CtrHostServices::DspAudioFrameTicks);
    NativeA32DspHle dspHle({});
    std::vector<int16_t> samples;
    uint32_t guestRuns = 0;
    const auto mix = [&] {
        Require(dspHle.ProcessFrame(memory, samples, &error),
                "DSP HLE mix failed");
    };
    const uint32_t delivered = DeliverPendingDspAudioFrames(
        host, host.TakePendingDspAudioFrames(), mix, [&guestRuns] {
            ++guestRuns;
            return true;
        });
    Require(delivered == 1U && guestRuns == 1U &&
                samples.size() == NativeA32DspHle::SamplesPerFrame * 2U,
            "the pending DSP audio frame was not mixed and delivered");
    Require(process.ThreadStatus(1) != NativeA32ThreadStatus::Waiting &&
                guest.State(1).r[0] == 0,
            "the DSP audio frame did not wake the sound thread");
    Require(DeliverPendingDspAudioFrames(host,
                                         host.TakePendingDspAudioFrames(),
                                         mix, [] { return true; }) == 0U,
            "a DSP audio frame was delivered twice");
}

// --- Rotinas da biblioteca do RVCT pelos bytes (ctr_library_natives.h) -------
// O Mario parava no boot da janela em MissingBlock 0x003A0D38: __aeabi_f2ulz
// chama o helper __fpl_* com uma palavra literal depois do BL, a descoberta
// marca o CFG como aberto e a função fica fora do whole-AOT.
void TestRvctLibraryNatives() {
    struct Case { uint32_t Bits; uint64_t Expected; };
    const Case cases[] = {
        {0x00000000U, 0U},                       // +0
        {0x3F000000U, 0U},                       // 0.5
        {0x3F800000U, 1U},                       // 1.0
        {0x3FC00000U, 1U},                       // 1.5 (trunca)
        {0x4F32D05EU, 3000000000ULL},            // 3e9 (> 2^31)
        {0x5F000000U, 0x8000000000000000ULL},    // 2^63
        {0x5F7FFFFFU, 0xFFFFFF0000000000ULL},    // maior float < 2^64
        {0x5F800000U, ~0ULL},                    // 2^64: satura (slot 3)
        {0x7F800000U, ~0ULL},                    // +inf
        {0x7FC00000U, 0U},                       // NaN (slot 0)
        {0xBF000000U, 0U},                       // -0.5
        {0xBF800000U, 0U},                       // -1.0 (slot 3: ~(-1))
        {0xFF800000U, 0U},                       // -inf
        {0x00000001U, 0U},                       // subnormal
    };
    for (const auto& item : cases) {
        Require(CtrAeabiF2ulz(item.Bits) == item.Expected,
                "__aeabi_f2ulz native result differs from the RVCT routine");
    }
    // Achada pelos bytes em qualquer endereço; um byte diferente não casa.
    std::vector<uint8_t> text(4U * 40U, 0U);
    std::memcpy(text.data() + 8U, kRvctAeabiF2ulzWords.data(),
                kRvctAeabiF2ulzWords.size() * 4U);
    std::vector<CtrLibraryNativeEntry> found;
    FindCtrLibraryNatives(0x00100000U, text, found);
    Require(found.size() == 1U && found[0].Pc == 0x00100008U &&
                FindCtrLibraryNative(found, 0x00100008U) != nullptr &&
                FindCtrLibraryNative(found, 0x0010000CU) == nullptr,
            "RVCT f2ulz signature was not found by its bytes");
    text[8U + 4U * 7U] ^= 1U;
    found.clear();
    FindCtrLibraryNatives(0x00100000U, text, found);
    Require(found.empty(), "a changed routine matched the f2ulz signature");
    struct Regs { uint32_t r[16]{}; } regs;
    regs.r[0] = 0x4F32D05EU;
    regs.r[14] = 0x002C6064U;
    Require(ExecuteCtrLibraryNative(CtrLibraryNative::AeabiF2ulz, regs) ==
                    0x002C6064U &&
                regs.r[0] == 3000000000U && regs.r[1] == 0U &&
                regs.r[15] == 0x002C6064U,
            "native f2ulz did not return r0:r1 to lr");

    // MK7 parava em MissingBlock 0x005E9F38: o __aeabi_f2lz (com sinal) usa o
    // mesmo helper e a mesma palavra 0x806DB6DB. Slot 0 = NaN -> 0; slot 3 =
    // ~(r0 asr 31) e eor 0x80000000 no alto: satura em INT64_MAX/INT64_MIN.
    const Case signedCases[] = {
        {0x00000000U, 0U},                       // +0
        {0x80000000U, 0U},                       // -0
        {0x3FC00000U, 1U},                       // 1.5 (trunca)
        {0xBFC00000U, ~0ULL},                    // -1.5 -> -1
        {0xCF32D05EU, 0ULL - 3000000000ULL},     // -3e9
        {0x5EFFFFFFU, 0x7FFFFF8000000000ULL},    // maior float < 2^63
        {0x5F000000U, 0x7FFFFFFFFFFFFFFFULL},    // 2^63: satura (slot 3)
        {0x7F800000U, 0x7FFFFFFFFFFFFFFFULL},    // +inf
        {0xDF000000U, 0x8000000000000000ULL},    // -2^63 exato (direto)
        {0xDF000001U, 0x8000000000000000ULL},    // < -2^63: satura
        {0xFF800000U, 0x8000000000000000ULL},    // -inf
        {0x7FC00000U, 0U},                       // NaN (slot 0)
        {0xFFC00000U, 0U},                       // NaN negativo
        {0x80000001U, 0U},                       // subnormal negativo
    };
    for (const auto& item : signedCases) {
        Require(CtrAeabiF2lz(item.Bits) == item.Expected,
                "__aeabi_f2lz native result differs from the RVCT routine");
    }
    std::vector<uint8_t> both(4U * 80U, 0U);
    std::memcpy(both.data() + 4U, kRvctAeabiF2lzWords.data(),
                kRvctAeabiF2lzWords.size() * 4U);
    std::memcpy(both.data() + 4U * 40U, kRvctAeabiF2ulzWords.data(),
                kRvctAeabiF2ulzWords.size() * 4U);
    found.clear();
    FindCtrLibraryNatives(0x00100000U, both, found);
    Require(found.size() == 2U && found[0].Pc == 0x00100004U &&
                found[0].Kind == CtrLibraryNative::AeabiF2lz &&
                found[1].Pc == 0x001000A0U &&
                found[1].Kind == CtrLibraryNative::AeabiF2ulz,
            "RVCT f2lz/f2ulz signatures were not told apart by their bytes");
    regs.r[0] = 0xCF32D05EU;
    regs.r[14] = 0x00553EE8U;
    const uint64_t minus3e9 = 0ULL - 3000000000ULL;
    Require(ExecuteCtrLibraryNative(CtrLibraryNative::AeabiF2lz, regs) ==
                    0x00553EE8U &&
                regs.r[0] == static_cast<uint32_t>(minus3e9) &&
                regs.r[1] == static_cast<uint32_t>(minus3e9 >> 32U),
            "native f2lz did not return r0:r1 to lr");
}

// --- Perfil de título no executável com janela ------------------------------
// oot3d_native_a32_window_setup.cpp ligava sempre os ganchos por PC do OoT3D
// (projeção, PlayState, composição PICA, UI, TopScreen), typed gameplay, mass
// e true AOT e os núcleos manuais: num outro code.bin esses PCs caem no meio
// de funções quaisquer. O perfil genérico desliga tudo isso; o do OoT3D (com
// ou sem title_id no manifesto) mantém tudo ligado.
void TestWindowProfileGatesOot3dHooks() {
    for (const std::optional<uint64_t> titleId :
         {std::optional<uint64_t>{}, std::optional<uint64_t>{0x0004000000033400ULL},
          std::optional<uint64_t>{0x0004000000033500ULL},
          std::optional<uint64_t>{0x0004000000033600ULL}}) {
        const auto profile = SelectCtrTitleProfile(titleId);
        Require(profile.Oot3dGuestHooks && profile.TopScreenUi &&
                    profile.ManualCompiledFunctions && profile.GuestOverrides,
                "the OoT3D window lost its guest hooks or TopScreen UI");
        // OoT3D presents its lower screen through its own UI lifecycle.
        Require(!profile.BottomScreenPictureInPicture,
                "the OoT3D window must not show the lower screen as picture-in-picture");
        Require(!profile.SubRectDisplayTransfers,
                "the OoT3D window must keep transfers from inside a target on the CPU path");
        Require(!profile.LcdScreenCrop,
                "the OoT3D window must present its whole display output (TopScreen 16:9)");
        // Mira por giroscópio no Switch: o sensor de seis eixos do console.
        Require(profile.SwitchConsoleMotion,
                "the OoT3D window lost the Switch six-axis sensor (gyro aiming)");
    }
    // Super Mario 3D Land (EUR) e um title id qualquer.
    for (const uint64_t titleId : {0x0004000000053F00ULL, 0x0004000000123400ULL}) {
        const auto profile = SelectCtrTitleProfile(titleId);
        Require(!profile.Oot3dGuestHooks && !profile.TopScreenUi &&
                    !profile.ManualCompiledFunctions && !profile.GuestOverrides &&
                    profile.StubUnknownServices && profile.KernelWaitTimeouts,
                "a generic title window kept OoT3D guest hooks");
        Require(profile.BottomScreenPictureInPicture,
                "a generic title window must show its lower screen (picture-in-picture)");
        Require(profile.SubRectDisplayTransfers,
                "a generic title window must transfer from inside a target on the GPU (SM3DL's lower screen)");
        Require(profile.LcdScreenCrop,
                "a generic title window must show only what the LCD shows (MK7: 400 of 416 columns, 240 of 256 pixels)");
        // Não testado nos outros títulos: o sensor do console fica desligado.
        Require(!profile.SwitchConsoleMotion,
                "a generic title window must not start the Switch six-axis sensor");
    }
}

// --- Fronteira de quadro (ctr_frame_boundary.h) ---------------------------
// O executável detectava o fim do quadro só por PCs do OoT3D (0x00417014...),
// que não existem em outro code.bin. O provedor genérico usa o commit de
// framebuffer no GSP apresentado no VBlank; o OoT3D continua nos PCs.
void TestFrameBoundaryProviderSelection() {
    for (const std::optional<uint64_t> titleId :
         {std::optional<uint64_t>{}, std::optional<uint64_t>{0x0004000000033400ULL},
          std::optional<uint64_t>{0x0004000000033500ULL},
          std::optional<uint64_t>{0x0004000000033600ULL}}) {
        const auto profile = SelectCtrTitleProfile(titleId);
        Require(profile.FrameBoundary == CtrFrameBoundarySource::Oot3dGuestPcs &&
                    MakeFrameBoundaryProvider(profile)->Source() ==
                        CtrFrameBoundarySource::Oot3dGuestPcs,
                "the OoT3D profile stopped using the guest-PC frame boundary");
    }
    // Super Mario 3D Land (EUR).
    const auto mario = SelectCtrTitleProfile(0x0004000000053F00ULL);
    Require(mario.FrameBoundary == CtrFrameBoundarySource::GspPresent &&
                MakeFrameBoundaryProvider(mario)->Source() ==
                    CtrFrameBoundarySource::GspPresent &&
                MakeFrameBoundaryProvider(mario)->GuestHookPcs().empty(),
            "a generic title did not get the GSP frame boundary");

    const auto oot = MakeFrameBoundaryProvider(Oot3dTitleProfile());
    const auto hooks = oot->GuestHookPcs();
    Require(hooks.size() == 4U && hooks[0] == 0x00417014U &&
                hooks[1] == 0x003FD27CU && hooks[2] == 0x003FD3A8U &&
                hooks[3] == 0x003FD3B8U,
            "the OoT3D frame boundary hooks changed");
}

void TestGspFrameBoundaryProvider() {
    GspPresentFrameBoundaryProvider gsp;
    // Sem commit: VBlank não fecha quadro (loading, espera de VBlank).
    Require(!gsp.ObserveVBlank() && !gsp.ObserveVBlank() &&
                gsp.Stats().Boundaries == 0U && gsp.Stats().VBlanks == 2U,
            "a VBlank without a framebuffer commit closed a frame");
    Require(!gsp.ObserveGuestBlock(0x00417014U),
            "the GSP provider reacted to an OoT3D guest PC");
    // Topo e baixo no mesmo VBlank: um quadro só.
    gsp.ObserveFramebufferCommit(0U, FrameBoundaryCommitKind::SharedMemoryInfo);
    gsp.ObserveFramebufferCommit(1U, FrameBoundaryCommitKind::SharedMemoryInfo);
    Require(gsp.ObserveVBlank() && gsp.Stats().Boundaries == 1U &&
                gsp.Stats().FirstBoundaryVBlank == 3U &&
                gsp.Stats().ScreenCommits[0] == 1U &&
                gsp.Stats().ScreenCommits[1] == 1U,
            "top and bottom commits of one VBlank were not one frame");
    // 60 quadros/s: um commit por VBlank.
    for (int frame = 0; frame < 4; ++frame) {
        gsp.ObserveFramebufferCommit(0U, FrameBoundaryCommitKind::SharedMemoryInfo);
        Require(gsp.ObserveVBlank(), "a 60 Hz commit did not close a frame");
    }
    // 30 quadros/s: um commit a cada dois VBlanks (SetBufferSwap direto).
    for (int frame = 0; frame < 3; ++frame) {
        Require(!gsp.ObserveVBlank(), "an idle VBlank closed a frame");
        gsp.ObserveFramebufferCommit(0U, FrameBoundaryCommitKind::SetBufferSwap);
        Require(gsp.ObserveVBlank(), "a 30 Hz commit did not close a frame");
    }
    const auto& stats = gsp.Stats();
    Require(stats.Boundaries == 8U && stats.VBlanks == 13U &&
                stats.MinimumIntervalVBlanks == 1U &&
                stats.MaximumIntervalVBlanks == 2U &&
                stats.IntervalHistogram[0] == 4U &&
                stats.IntervalHistogram[1] == 3U &&
                stats.SetBufferSwapCommits == 3U &&
                stats.SharedMemoryCommits == 6U &&
                stats.LastBoundaryVBlank == 13U,
            "the GSP frame boundary rhythm statistics are wrong");
    // Pausa longa (5+ VBlanks sem quadro) cai no último balde.
    for (int idle = 0; idle < 6; ++idle) {
        gsp.ObserveVBlank();
    }
    gsp.ObserveFramebufferCommit(0U, FrameBoundaryCommitKind::SetBufferSwap);
    Require(gsp.ObserveVBlank() && gsp.Stats().IntervalHistogram[4] == 1U &&
                gsp.Stats().MaximumIntervalVBlanks == 7U,
            "a long frame did not reach the 5+ interval bucket");
}

void TestOot3dFrameBoundaryMatchesGsp() {
    // Ritmo do Zelda (30 quadros/s): GameState_Update, limitador e o swap do
    // nngx a cada dois VBlanks. Os dois provedores precisam concordar.
    Oot3dGuestPcFrameBoundaryProvider oot;
    GspPresentFrameBoundaryProvider gsp;
    for (int frame = 0; frame < 30; ++frame) {
        Require(oot.ObserveGuestBlock(0x00417014U) &&
                    !oot.ObserveGuestBlock(0x003FD27CU) &&
                    !oot.ObserveGuestBlock(0x003FD3B8U) &&
                    !oot.ObserveGuestBlock(0x003FD3A8U),
                "only GameState_Update may close an OoT3D frame");
        gsp.ObserveFramebufferCommit(0U, FrameBoundaryCommitKind::SharedMemoryInfo);
        gsp.ObserveFramebufferCommit(1U, FrameBoundaryCommitKind::SharedMemoryInfo);
        for (int vblank = 0; vblank < 2; ++vblank) {
            Require(!oot.ObserveVBlank(), "an OoT3D VBlank closed a frame");
            gsp.ObserveVBlank();
        }
    }
    Require(oot.Stats().Boundaries == 30U && gsp.Stats().Boundaries == 30U &&
                oot.Stats().VBlanks == 60U && gsp.Stats().VBlanks == 60U &&
                oot.Stats().IntervalHistogram[1] == 29U &&
                gsp.Stats().IntervalHistogram[1] == 29U &&
                oot.Stats().BoundariesPerVBlank() == 0.5 &&
                gsp.Stats().BoundariesPerVBlank() == 0.5,
            "the OoT3D and GSP frame boundaries disagree on a 30 Hz frame");
}

void TestHostReportsGspFrameBoundaries() {
    GspPresentFrameBoundaryProvider frameBoundary;
    NativeA32CtrHostConfig config = MakeConfig();
    config.FrameBoundary = &frameBoundary;
    NativeA32CtrHostServices host(std::move(config));
    const oot3d::recomp::a32::Registry registry{};
    NativeA32Process process(registry, host);
    MakeGuestThreads(process);
    GuestThreads guest{host, process};
    auto& memory = process.Memory();
    const uint32_t srv = OpenSrv(guest);
    uint32_t gsp = 0;
    Require(GetServiceHandle(guest, srv, "gsp::Gpu", &gsp) ==
                    NativeA32HostAction::Resume &&
                gsp != 0,
            "gsp::Gpu was not opened");
    guest.State(0).r[1] = 0;
    Require(guest.Svc(0, 0x17) == NativeA32HostAction::Resume,
            "cannot create the GSP interrupt event");
    const uint32_t interruptEvent = guest.State(0).r[1];
    Require(memory.Write32(0x1FF82080, 0x00130042) &&
                memory.Write32(0x1FF82084, 1) &&
                memory.Write32(0x1FF82088, 0) &&
                memory.Write32(0x1FF8208C, interruptEvent),
            "cannot write GSP:RegisterInterruptRelayQueue");
    guest.State(0).r[0] = gsp;
    uint32_t sharedMemory = 0;
    Require(guest.Svc(0, 0x32) == NativeA32HostAction::Resume &&
                memory.Read32(0x1FF82090, &sharedMemory) && sharedMemory != 0,
            "GSP:RegisterInterruptRelayQueue failed");
    auto& state = guest.State(0);
    state.r[0] = sharedMemory;
    state.r[1] = 0x10000000;
    state.r[2] = 3;
    state.r[3] = 0x10000000;
    Require(guest.Svc(0, 0x1F) == NativeA32HostAction::Resume &&
                memory.IsWritable(0x10000000, 0x1000),
            "cannot map the GSP shared memory");

    // VBlank sem commit: nenhuma fronteira.
    Require(host.SignalVBlank(memory) && frameBoundary.Stats().VBlanks == 1U &&
                frameBoundary.Stats().Boundaries == 0U,
            "a VBlank without a framebuffer commit closed a frame");
    // nngx: framebuffer info "dirty" na shared memory (topo e baixo).
    const auto publish = [&](uint32_t screen) {
        const uint32_t update = 0x10000200U + screen * 0x40U;
        const uint32_t info = update + 4U + 0x1CU;
        Require(memory.Write8(update, 1) && memory.Write8(update + 1U, 1) &&
                    memory.Write32(info, 1) &&
                    memory.Write32(info + 4U, 0x14002000) &&
                    memory.Write32(info + 8U, 0x14002000) &&
                    memory.Write32(info + 12U, 720) &&
                    memory.Write32(info + 16U, 2) &&
                    memory.Write32(info + 20U, 1),
                "cannot publish a shared framebuffer update");
    };
    publish(0U);
    publish(1U);
    Require(host.SignalVBlank(memory) && frameBoundary.Stats().Boundaries == 1U &&
                frameBoundary.Stats().SharedMemoryCommits == 2U &&
                frameBoundary.Stats().LastBoundaryVBlank == 2U,
            "the shared-memory framebuffer commit did not close one frame");
    Require(host.SignalVBlank(memory) && frameBoundary.Stats().Boundaries == 1U,
            "an acknowledged framebuffer update closed a second frame");
    // libctru: gsp::Gpu SetBufferSwap, apresentado no VBlank seguinte.
    Require(memory.Write32(0x1FF82080, 0x00050200) &&
                memory.Write32(0x1FF82084, 0) &&
                memory.Write32(0x1FF82088, 0) &&
                memory.Write32(0x1FF8208C, 0x14001000) &&
                memory.Write32(0x1FF82090, 0x14001000) &&
                memory.Write32(0x1FF82094, 960) &&
                memory.Write32(0x1FF82098, 0) &&
                memory.Write32(0x1FF8209C, 0) &&
                memory.Write32(0x1FF820A0, 0),
            "cannot write GSP:SetBufferSwap");
    guest.State(0).r[0] = gsp;
    Require(guest.Svc(0, 0x32) == NativeA32HostAction::Resume &&
                frameBoundary.Stats().SetBufferSwapCommits == 1U &&
                frameBoundary.Stats().Boundaries == 1U,
            "GSP:SetBufferSwap was not seen as a framebuffer commit");
    Require(host.SignalVBlank(memory) && frameBoundary.Stats().Boundaries == 2U &&
                frameBoundary.Stats().MaximumIntervalVBlanks == 2U,
            "the SetBufferSwap commit did not close a frame at the VBlank");
}

// Super Mario 3D Land: CreateMemoryBlock de uma página do próprio .bss
// (depois de 3365 SVCs).
void TestCreateMemoryBlockOfGuestMemory() {
    NativeA32CtrHostServices host(MakeConfig());
    const oot3d::recomp::a32::Registry registry{};
    NativeA32Process process(registry, host);
    MakeGuestThreads(process);
    std::string error;
    Require(process.Memory().MapRegion(
                {"data", 0x00500000U, 0x2000, true, false, {}}, &error),
            error);
    GuestThreads guest{host, process};
    auto& state = guest.State(0);
    state.r[0] = 3;
    state.r[1] = 0x00501000U;
    state.r[2] = 0x1000;
    state.r[3] = 3;
    Require(guest.Svc(0, 0x1E) == NativeA32HostAction::Resume &&
                state.r[0] == 0 && state.r[1] != 0 &&
                host.SvcEvents().back().Handled &&
                host.SvcEvents().back().Detail == 0x00501000U,
            "CreateMemoryBlock did not wrap mapped guest memory");
    state.r[0] = state.r[1];
    Require(guest.Svc(0, 0x23) == NativeA32HostAction::Resume && state.r[0] == 0,
            "the CreateMemoryBlock handle cannot be closed");
    state.r[1] = 0;
    state.r[2] = 0x1000;
    Require(guest.Svc(0, 0x1E) != NativeA32HostAction::Resume,
            "CreateMemoryBlock accepted a kernel-allocated block");
    state.r[1] = 0x00600000U;
    Require(guest.Svc(0, 0x1E) != NativeA32HostAction::Resume,
            "CreateMemoryBlock accepted unmapped memory");
}

void TestThreadQueries() {
    NativeA32CtrHostServices host(MakeConfig());
    const oot3d::recomp::a32::Registry registry{};
    NativeA32Process process(registry, host);
    MakeGuestThreads(process);
    GuestThreads guest{host, process};
    guest.State(2).r[1] = 0xFFFF8000U;
    Require(guest.Svc(2, 0x0B) == NativeA32HostAction::Resume &&
                guest.State(2).r[0] == 0 && guest.State(2).r[1] == 0x31,
            "GetThreadPriority(current) is wrong");
    guest.State(1).r[1] = 0xFFFF8000U;
    Require(guest.Svc(1, 0x37) == NativeA32HostAction::Resume &&
                guest.State(1).r[1] == 0x101,
            "GetThreadId(current) is wrong");
    guest.State(0).r[1] = 0xFFFF8001U;
    Require(guest.Svc(0, 0x35) == NativeA32HostAction::Resume &&
                guest.State(0).r[1] == 0x28,
            "GetProcessId(current) is wrong");
    // Super Mario 3D Land parou aqui (SVC 0x0C) depois de 751 SVCs.
    guest.State(1).r[0] = 0xFFFF8000U;
    guest.State(1).r[1] = 0x2C;
    Require(guest.Svc(1, 0x0C) == NativeA32HostAction::Resume &&
                guest.State(1).r[0] == 0 &&
                process.ThreadPriority(1) == std::optional<uint32_t>(0x2C),
            "SetThreadPriority(current) did not change the priority");
    guest.State(1).r[0] = 0xFFFF8000U;
    guest.State(1).r[1] = 0x40;
    Require(guest.Svc(1, 0x0C) == NativeA32HostAction::Resume &&
                guest.State(1).r[0] == 0xE0E01BFDU &&
                process.ThreadPriority(1) == std::optional<uint32_t>(0x2C),
            "SetThreadPriority accepted a priority above 0x3F");
    Require(guest.Svc(0, 0x0D) == NativeA32HostAction::Fault &&
                host.SvcEvents().back().Name ==
                    "unimplemented:GetThreadAffinityMask",
            "an unimplemented SVC is not named in the host fault");
}

// APT:GetSharedFont sem arquivos do console: bloco de 0x332000 com a fonte
// substituta (CFNU, ponteiros relocados para 0x18000000), mapeado só leitura.
void TestAptGetSharedFont() {
    NativeA32CtrHostServices host(MakeConfig());
    const oot3d::recomp::a32::Registry registry{};
    NativeA32Process process(registry, host);
    MakeGuestThreads(process);
    GuestThreads guest{host, process};
    auto& memory = process.Memory();
    const uint32_t srv = OpenSrv(guest);
    uint32_t apt = 0;
    Require(GetServiceHandle(guest, srv, "APT:U", &apt) == NativeA32HostAction::Resume && apt != 0,
            "APT:U was not opened");
    const auto getSharedFont = [&](uint32_t* address, uint32_t* handle) {
        uint32_t header = 0, result = 1, descriptor = 1;
        Require(memory.Write32(0x1FF82080, 0x00440000U), "cannot write APT:GetSharedFont");
        guest.State(0).r[0] = apt;
        Require(guest.Svc(0, 0x32) == NativeA32HostAction::Resume &&
                    memory.Read32(0x1FF82080, &header) && header == 0x00440082U &&
                    memory.Read32(0x1FF82084, &result) && result == 0U &&
                    memory.Read32(0x1FF82088, address) &&
                    memory.Read32(0x1FF8208C, &descriptor) && descriptor == 0U &&
                    memory.Read32(0x1FF82090, handle) && *handle != 0U,
                "APT:GetSharedFont response is wrong");
    };
    uint32_t address = 0, handle = 0;
    getSharedFont(&address, &handle);
    Require(address == 0x18000000U, "shared font address");
    auto& state = guest.State(0);
    state.r[0] = handle;
    state.r[1] = address;
    state.r[2] = 1U;            // leitura
    state.r[3] = 0x10000000U;   // DONTCARE
    Require(guest.Svc(0, 0x1F) == NativeA32HostAction::Resume && state.r[0] == 0U,
            "MapMemoryBlock of the shared font failed");
    uint32_t status = 0, region = 0, size = 0, magic = 0, tglp = 0;
    Require(memory.Read32(address, &status) && status == 2U &&
                memory.Read32(address + 4U, &region) && region == 1U &&
                memory.Read32(address + 8U, &size) && size == 0x2FD4E6U &&
                memory.Read32(address + 0x80U, &magic) && magic == 0x554E4643U &&
                memory.IsMapped(address, 0x332000U) && !memory.IsWritable(address, 4U),
            "shared font block contents");
    // FINF logo após o cabeçalho CFNT (0x14); o ponteiro do TGLP é absoluto.
    uint32_t tglpMagic = 0;
    Require(memory.Read32(address + 0x80U + 0x14U + 0x10U, &tglp) &&
                tglp > address && tglp < address + 0x332000U &&
                memory.Read32(tglp - 8U, &tglpMagic) && tglpMagic == 0x504C4754U,
            "shared font TGLP pointer is not relocated");
    // Segunda chamada: o mesmo bloco (já mapeado), sem segunda cópia.
    uint32_t again = 0, secondHandle = 0;
    getSharedFont(&again, &secondHandle);
    state.r[0] = secondHandle;
    state.r[1] = 0x18400000U;
    state.r[2] = 1U;
    state.r[3] = 0x10000000U;
    Require(again == address && guest.Svc(0, 0x1F) != NativeA32HostAction::Resume,
            "a second shared font block was mapped");
}

// --- Applet de biblioteca no APT (seletor de Mii, 0x402) ------------------
// O SM3DL pré-carrega o seletor de Mii e espera GetAppletInfo dizer
// "registered && loaded"; o stub zerava a resposta e o jogo esperava para
// sempre. Semântica do Azahar (applet_manager.cpp, applets/mii_selector.cpp):
// Preload registra o HLE, Request recebe Response com o bloco de captura e
// StartLibraryApplet devolve WakeupByExit com o Mii padrão.
void TestAptLibraryAppletMiiSelector() {
    NativeA32CtrHostConfig config = MakeConfig();
    config.KernelWaitTimeouts = true;  // espera com timeout 0 = consulta
    NativeA32CtrHostServices host(std::move(config));
    const oot3d::recomp::a32::Registry registry{};
    NativeA32Process process(registry, host);
    MakeGuestThreads(process);
    GuestThreads guest{host, process};
    auto& memory = process.Memory();
    const uint32_t srv = OpenSrv(guest);
    uint32_t apt = 0;
    Require(GetServiceHandle(guest, srv, "APT:U", &apt) == NativeA32HostAction::Resume && apt != 0,
            "APT:U was not opened");
    constexpr uint32_t command = 0x1FF82080;
    constexpr uint32_t input = 0x0FF00400;
    constexpr uint32_t output = 0x0FF00800;
    const auto call = [&](std::vector<uint32_t> words) {
        for (size_t i = 0; i < words.size(); ++i) {
            Require(memory.Write32(command + static_cast<uint32_t>(i * 4U), words[i]),
                    "cannot write APT command");
        }
        // Buffer estático 0 da thread (saída de Receive/GlanceParameter).
        Require(memory.Write32(0x1FF82180, (0x1000U << 14U) | 2U) &&
                    memory.Write32(0x1FF82184, output),
                "cannot write static buffer table");
        guest.State(0).r[0] = apt;
        return guest.Svc(0, 0x32);
    };
    const auto word = [&](uint32_t index) {
        uint32_t value = 0;
        Require(memory.Read32(command + index * 4U, &value), "cannot read APT reply");
        return value;
    };
    // Initialize(0x300) e o Wakeup inicial, como o SDK faz no boot.
    Require(call({0x00020080U, 0x300U, 0U}) == NativeA32HostAction::Resume &&
                word(0) == 0x00020043U && word(1) == 0U,
            "APT:Initialize failed");
    const uint32_t parameterEvent = word(4);
    Require(guest.Wait1(0, parameterEvent, 0) == NativeA32HostAction::Resume &&
                guest.State(0).r[0] == 0U,
            "the parameter event was not signaled for the initial Wakeup");
    Require(call({0x000D0080U, 0x300U, 0x1000U}) == NativeA32HostAction::Resume &&
                word(1) == 0U && word(3) == 1U,
            "the initial Wakeup was not received");
    Require(guest.Wait1(0, parameterEvent, 0) == NativeA32HostAction::Resume &&
                guest.State(0).r[0] != 0U,
            "the parameter event stayed signaled after the Wakeup");
    // Sem parâmetro pendente, GlanceParameter é NoData (Azahar).
    Require(call({0x000E0080U, 0x300U, 0x1000U}) == NativeA32HostAction::Resume &&
                word(0) == 0x000E0040U && word(1) == 0xC8A0CFEFU,
            "GlanceParameter without a parameter is not NoData");

    // Antes do Preload o seletor não existe.
    Require(call({0x00060040U, 0x402U}) == NativeA32HostAction::Resume &&
                word(0) == 0x00060040U && word(1) == 0xC880CFFAU,
            "GetAppletInfo(0x402) before the preload is not NotFound");
    Require(call({0x00160040U, 0x402U}) == NativeA32HostAction::Resume &&
                word(0) == 0x00160040U && word(1) == 0U,
            "PreloadLibraryApplet(0x402) failed");
    Require(call({0x00160040U, 0x402U}) == NativeA32HostAction::Resume &&
                word(1) == 0xC8A0CFFCU,
            "a second PreloadLibraryApplet is not AlreadyExists");
    // EUR (SystemRegion 2): seletor de Mii 0x000400300000D102, NAND,
    // registrado, carregado, posição Library.
    Require(call({0x00060040U, 0x402U}) == NativeA32HostAction::Resume &&
                word(0) == 0x000601C0U && word(1) == 0U && word(2) == 0x0000D102U &&
                word(3) == 0x00040030U && word(4) == 0U && word(5) == 1U &&
                word(6) == 1U && word(7) == 1U,
            "GetAppletInfo(0x402) after the preload is not registered && loaded");
    // O SDK ainda chama PrepareToStart (AlreadyExists, como no Azahar) e
    // espera IsRegistered(0x402) antes do StartLibraryApplet.
    Require(call({0x00180040U, 0x402U}) == NativeA32HostAction::Resume &&
                word(1) == 0xC8A0CFFCU,
            "PrepareToStartLibraryApplet after the preload is not AlreadyExists");
    const auto isRegistered = [&](uint32_t appletId) {
        Require(call({0x00090040U, appletId}) == NativeA32HostAction::Resume &&
                    word(0) == 0x00090080U && word(1) == 0U,
                "APT:IsRegistered failed");
        return word(2);
    };
    Require(isRegistered(0x402U) == 1U && isRegistered(0x300U) == 1U &&
                isRegistered(0x401U) == 0U && isRegistered(0x406U) == 0U,
            "IsRegistered does not follow the applet slots");

    // Request com o CaptureBufferInfo -> Response com o bloco de captura.
    std::vector<uint8_t> capture(0x20, 0);
    capture[0] = 0x00; capture[1] = 0x80; capture[2] = 0x07;  // 0x78000 bytes
    Require(memory.WriteBytes(input, capture), "cannot write CaptureBufferInfo");
    Require(call({0x000C0104U, 0x300U, 0x402U, 2U, 0x20U, 0U, 0U, (0x20U << 14U) | 2U, input}) ==
                    NativeA32HostAction::Resume &&
                word(0) == 0x000C0040U && word(1) == 0U,
            "SendParameter(Request) to the Mii selector failed");
    Require(guest.Wait1(0, parameterEvent, 0) == NativeA32HostAction::Resume &&
                guest.State(0).r[0] == 0U,
            "the Response did not signal the parameter event");
    // Savestate com a Response pendente: consumir e restaurar a traz de volta.
    const auto saved = host.CaptureState();
    Require(call({0x000D0080U, 0x300U, 0x1000U}) == NativeA32HostAction::Resume &&
                word(1) == 0U && word(3) == 3U,
            "the Response was not received before the restore");
    std::string error;
    Require(host.RestoreState(saved, process, &error),
            "a savestate with a pending APT parameter did not restore: " + error);
    Require(call({0x000E0080U, 0x300U, 0x1000U}) == NativeA32HostAction::Resume &&
                word(0) == 0x000E0104U && word(1) == 0U && word(2) == 0x402U &&
                word(3) == 3U && word(4) == 0U && word(5) == 0x10U && word(6) != 0U,
            "GlanceParameter did not see the Response");
    Require(call({0x000D0080U, 0x300U, 0x1000U}) == NativeA32HostAction::Resume &&
                word(0) == 0x000D0104U && word(1) == 0U && word(2) == 0x402U &&
                word(3) == 3U && word(5) == 0x10U && word(6) != 0U,
            "ReceiveParameter did not return the Response");
    const uint32_t captureBlock = word(6);
    auto& state = guest.State(0);
    state.r[0] = captureBlock;
    state.r[1] = 0x1A000000U;
    state.r[2] = 3U;
    state.r[3] = 3U;  // bloco do heap: other_permissions RW, não DontCare (Azahar)
    Require(guest.Svc(0, 0x1F) == NativeA32HostAction::Resume && state.r[0] == 0U &&
                memory.IsWritable(0x1A000000U, 0x78000U),
            "the capture block from the Mii selector cannot be mapped");
    Require(call({0x000E0080U, 0x300U, 0x1000U}) == NativeA32HostAction::Resume &&
                word(1) == 0xC8A0CFEFU,
            "ReceiveParameter did not consume the Response");

    // O SM3DL pede sem captura: size 0 e formatos 0xFFFFFFFF (bytes reais do
    // jogo). O Azahar cria o bloco de tamanho 0 e responde do mesmo jeito.
    std::vector<uint8_t> noCapture(0x20, 0);
    std::fill(noCapture.begin() + 0x10, noCapture.begin() + 0x14, uint8_t{0xFF});
    std::fill(noCapture.begin() + 0x1C, noCapture.begin() + 0x20, uint8_t{0xFF});
    Require(memory.WriteBytes(input, noCapture), "cannot write the empty CaptureBufferInfo");
    Require(call({0x000C0104U, 0x300U, 0x402U, 2U, 0x20U, 0U, 0U, (0x20U << 14U) | 2U, input}) ==
                    NativeA32HostAction::Resume &&
                word(0) == 0x000C0040U && word(1) == 0U,
            "SendParameter(Request) without capture was refused");
    Require(call({0x000D0080U, 0x300U, 0x1000U}) == NativeA32HostAction::Resume &&
                word(1) == 0U && word(2) == 0x402U && word(3) == 3U && word(6) != 0U,
            "the Response without capture did not carry a block");
    // O SM3DL mapeia o bloco vazio no endereço 0: nada a mapear, sucesso.
    state.r[0] = word(6);
    state.r[1] = 0U;
    state.r[2] = 3U;
    state.r[3] = 3U;
    Require(guest.Svc(0, 0x1F) == NativeA32HostAction::Resume && state.r[0] == 0U,
            "MapMemoryBlock of the empty capture block failed");
    // ... e o desmapeia depois que o seletor fecha (UnmapMemoryBlock, svc 0x20).
    const uint32_t emptyBlock = state.r[0] == 0U ? word(6) : 0U;
    state.r[0] = emptyBlock;
    state.r[1] = 0U;
    Require(guest.Svc(0, 0x20) == NativeA32HostAction::Resume && state.r[0] == 0U,
            "UnmapMemoryBlock of the empty capture block failed");

    // StartLibraryApplet com o MiiConfig (0x104 bytes) -> WakeupByExit com o
    // MiiResult do Mii padrão do Azahar.
    Require(memory.WriteBytes(input, std::vector<uint8_t>(0x104, 0)), "cannot write MiiConfig");
    Require(call({0x001E0084U, 0x402U, 0x104U, 0U, 0U, (0x104U << 14U) | 2U, input}) ==
                    NativeA32HostAction::Resume &&
                word(0) == 0x001E0040U && word(1) == 0U,
            "StartLibraryApplet(0x402) failed");
    Require(guest.Wait1(0, parameterEvent, 0) == NativeA32HostAction::Resume &&
                guest.State(0).r[0] == 0U,
            "the Mii selector result did not signal the parameter event");
    Require(memory.WriteBytes(output, std::vector<uint8_t>(0x100, 0xAA)), "cannot clear output");
    Require(call({0x000D0080U, 0x300U, 0x1000U}) == NativeA32HostAction::Resume &&
                word(1) == 0U && word(2) == 0x402U && word(3) == 0xAU && word(4) == 0x84U &&
                word(6) == 0U && word(7) == ((0x1000U << 14U) | 2U) && word(8) == output,
            "ReceiveParameter did not return WakeupByExit with the MiiResult");
    std::vector<uint8_t> result(0x84);
    Require(memory.ReadBytes(output, result), "cannot read MiiResult");
    const std::vector<uint8_t> header{0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
    Require(std::equal(header.begin(), header.end(), result.begin()),
            "MiiResult return code / guest index is wrong");
    // MiiData: versão 3, system_id/mii_id big-endian, nome "Citra" UTF-16LE,
    // CRC-16/XMODEM dos 0x5E bytes em +0x5E (big-endian) = 0x056C.
    Require(result[0x0C] == 3U && result[0x10] == 0xD2U && result[0x18] == 0x98U &&
                result[0x26] == 'C' && result[0x27] == 0U && result[0x28] == 'i' &&
                result[0x0C + 0x5E] == 0x05U && result[0x0C + 0x5F] == 0x6CU &&
                result[0x6C] == 0U && result[0x83] == 0U,
            "MiiResult does not carry Azahar's standard Mii");
    // O seletor fechou: o slot de applet de biblioteca ficou livre.
    Require(call({0x00060040U, 0x402U}) == NativeA32HostAction::Resume &&
                word(1) == 0xC880CFFAU,
            "the Mii selector is still registered after closing");
    Require(isRegistered(0x402U) == 0U, "IsRegistered(0x402) is still true after closing");
    Require(call({0x00160040U, 0x402U}) == NativeA32HostAction::Resume && word(1) == 0U,
            "the Mii selector cannot be preloaded again after closing");
}

// --- fs:USER OpenDirectory e a sessão de diretório ------------------------
// Depois do seletor de Mii o SM3DL abre um diretório (0x080B0102) e caía no
// err:f com "comando sem suporte". Semântica do Azahar (SaveDataArchive::
// OpenDirectory, DiskDirectory::Read, entrada de 0x228 bytes); os arquivos do
// diário de saves (.triaevum-*) não aparecem e o tamanho é o do arquivo
// ativo do diário (o mesmo do GetSize).
void TestFsOpenDirectory() {
    const auto directory = std::filesystem::temp_directory_path() /
                           "oot3d_native_a32_ctr_host_test_directory";
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    const auto save = directory / "save";
    std::filesystem::create_directories(save / "zz");
    const auto writeFile = [](const std::filesystem::path& path, size_t size) {
        std::ofstream out(path, std::ios::binary);
        out << std::string(size, 'x');
        Require(out.good(), "cannot write a directory fixture");
    };
    writeFile(save / "b.bin", 5);
    writeFile(save / "LongFileName.dat", 3);
    writeFile(save / "c.sav", 7);
    NativeA32CtrHostConfig config = MakeConfig();
    config.SaveDataDirectory = save;
    NativeA32CtrHostServices host(std::move(config));
    const oot3d::recomp::a32::Registry registry{};
    NativeA32Process process(registry, host);
    MakeGuestThreads(process);
    GuestThreads guest{host, process};
    auto& memory = process.Memory();
    const uint32_t srv = OpenSrv(guest);
    uint32_t fs = 0;
    Require(GetServiceHandle(guest, srv, "fs:USER", &fs) == NativeA32HostAction::Resume && fs != 0,
            "fs:USER was not opened");
    constexpr uint32_t command = 0x1FF82080;
    const auto call = [&](uint32_t session, std::vector<uint32_t> words) {
        for (size_t i = 0; i < words.size(); ++i) {
            Require(memory.Write32(command + static_cast<uint32_t>(i * 4U), words[i]),
                    "cannot write FS command");
        }
        guest.State(0).r[0] = session;
        Require(guest.Svc(0, 0x32) == NativeA32HostAction::Resume, "an FS request did not resume");
    };
    const auto word = [&](uint32_t index) {
        uint32_t value = 0;
        Require(memory.Read32(command + index * 4U, &value), "cannot read FS reply");
        return value;
    };
    call(fs, {0x08010002U, 0x20U, 0U});
    Require(word(1) == 0U, "FS:Initialize failed");
    constexpr uint32_t pathAddress = 0x0FF00200;
    Require(memory.Write32(pathAddress, 0), "cannot write the empty path");
    call(fs, {0x080C00C2U, 4U, 1U, 1U, (1U << 14U) | 2U, pathAddress});
    Require(word(1) == 0U, "OpenArchive(SaveData) failed");
    const uint32_t archiveLow = word(2), archiveHigh = word(3);
    const auto writePath = [&](std::string_view path) {
        std::vector<uint8_t> bytes(path.begin(), path.end());
        bytes.push_back(0);
        Require(memory.WriteBytes(pathAddress, bytes), "cannot write an FS path");
        return static_cast<uint32_t>(bytes.size());
    };
    const auto openDirectory = [&](std::string_view path) {
        const uint32_t size = writePath(path);
        call(fs, {0x080B0102U, archiveLow, archiveHigh, 3U, size, (size << 14U) | 2U, pathAddress});
        return word(1);
    };
    // Diário no meio da execução (a recuperação já rodou no OpenArchive): uma
    // sobra .triaevum-old e uma escrita sem flush em b.bin.triaevum-tmp.
    writeFile(save / "b.bin.triaevum-old", 2);
    const uint32_t pathSize = writePath("/b.bin");
    call(fs, {0x080201C2U, 0U, archiveLow, archiveHigh, 3U, pathSize, 3U, 0U, (pathSize << 14U) | 2U,
              pathAddress});
    Require(word(0) == 0x08020042U && word(1) == 0U && word(3) != 0U, "OpenFile(/b.bin) failed");
    const uint32_t file = word(3);
    constexpr uint32_t data = 0x0FF00400;
    Require(memory.WriteBytes(data, std::vector<uint8_t>(4U, 0x55U)), "cannot write the file data");
    call(file, {0x08030102U, 5U, 0U, 4U, 0U, (4U << 4U) | 0xAU, data});
    Require(word(0) == 0x08030082U && word(1) == 0U && word(2) == 4U, "File:Write failed");
    Require(std::filesystem::exists(save / "b.bin.triaevum-tmp") &&
                std::filesystem::file_size(save / "b.bin") == 5U,
            "the pending write did not go through the save journal");
    Require(openDirectory("/nope") == 0xC8804471U, "OpenDirectory of a missing path is not PathNotFound");
    Require(openDirectory("/b.bin") == 0xE0C04702U,
            "OpenDirectory of a file is not UnexpectedFileOrDirectory");
    Require(openDirectory("/") == 0U && word(0) == 0x080B0042U && word(2) == 0x10U && word(3) != 0U,
            "OpenDirectory(/) of the save archive failed");
    const uint32_t session = word(3);

    constexpr uint32_t entries = 0x0FF00800;
    struct Entry {
        std::u16string Name;
        std::string ShortName;
        std::string Extension;
        bool Directory = false;
        bool Archive = false;
        uint64_t Size = 0;
    };
    const auto read = [&](uint32_t count) {
        Require(memory.WriteBytes(entries, std::vector<uint8_t>(count * 0x228U, 0xAAU)),
                "cannot clear the entry buffer");
        call(session, {0x08010042U, count, ((count * 0x228U) << 4U) | 0xCU, entries});
        Require(word(0) == 0x08010082U && word(1) == 0U, "Directory:Read failed");
        std::vector<Entry> result;
        for (uint32_t i = 0; i < word(2); ++i) {
            std::vector<uint8_t> raw(0x228U);
            Require(memory.ReadBytes(entries + i * 0x228U, raw), "cannot read an entry");
            Entry entry;
            for (size_t c = 0; c < 0x20CU; c += 2U) {
                const char16_t value = static_cast<char16_t>(raw[c] | (raw[c + 1U] << 8U));
                if (value == 0) break;
                entry.Name.push_back(value);
            }
            entry.ShortName.assign(reinterpret_cast<const char*>(&raw[0x20C]), 8);
            entry.Extension.assign(reinterpret_cast<const char*>(&raw[0x216]), 3);
            Require(raw[0x214] == 0U && raw[0x219] == 0U, "8.3 names are not NUL-terminated");
            entry.Directory = raw[0x21C] != 0U;
            entry.Archive = raw[0x21E] != 0U;
            std::memcpy(&entry.Size, &raw[0x220], sizeof(entry.Size));
            result.push_back(entry);
        }
        return result;
    };
    auto first = read(2);
    Require(first.size() == 2 && first[0].Name == u"LongFileName.dat" &&
                first[0].ShortName == "LONGFI~1" && first[0].Extension == "DAT" &&
                !first[0].Directory && first[0].Archive && first[0].Size == 3 &&
                first[1].Name == u"b.bin" && first[1].ShortName == "B       " &&
                first[1].Extension == "BIN" && first[1].Size == 9,
            "the first directory entries are wrong (b.bin has the size the game sees)");
    // Savestate no meio da listagem: volta do mesmo ponto.
    const auto saved = host.CaptureState();
    auto second = read(4);
    Require(second.size() == 2 && second[0].Name == u"c.sav" && second[0].Size == 7 &&
                second[1].Name == u"zz" && second[1].Directory && !second[1].Archive &&
                second[1].Extension == "   " && second[1].Size == 0,
            "the remaining directory entries are wrong (journal files must stay hidden)");
    Require(read(4).empty(), "a finished directory listing returned more entries");
    std::string error;
    Require(host.RestoreState(saved, process, &error),
            "a savestate with an open directory did not restore: " + error);
    auto again = read(4);
    Require(again.size() == 2 && again[0].Name == u"c.sav",
            "the restored directory listing did not resume at its cursor");
    call(session, {0x08020000U});
    Require(word(0) == 0x08020040U && word(1) == 0U, "Directory:Close failed");
    std::filesystem::remove_all(directory, ignored);
}

// fs:USER CreateDirectory (0x0809) no save: o MK7 cria as pastas do save no
// primeiro A depois do boot e caía no err:f com o pedido sem HLE. Códigos do
// Azahar (SaveDataArchive::CreateDirectory).
void TestFsCreateDirectory() {
    const auto directory = std::filesystem::temp_directory_path() /
                           "oot3d_native_a32_ctr_host_test_create_directory";
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    const auto save = directory / "save";
    std::filesystem::create_directories(save);
    {
        std::ofstream out(save / "f.bin", std::ios::binary);
        out << "x";
        Require(out.good(), "cannot write the file fixture");
    }
    NativeA32CtrHostConfig config = MakeConfig();
    config.SaveDataDirectory = save;
    NativeA32CtrHostServices host(std::move(config));
    const oot3d::recomp::a32::Registry registry{};
    NativeA32Process process(registry, host);
    MakeGuestThreads(process);
    GuestThreads guest{host, process};
    auto& memory = process.Memory();
    const uint32_t srv = OpenSrv(guest);
    uint32_t fs = 0;
    Require(GetServiceHandle(guest, srv, "fs:USER", &fs) == NativeA32HostAction::Resume && fs != 0,
            "fs:USER was not opened");
    constexpr uint32_t command = 0x1FF82080;
    const auto call = [&](std::vector<uint32_t> words) {
        for (size_t i = 0; i < words.size(); ++i) {
            Require(memory.Write32(command + static_cast<uint32_t>(i * 4U), words[i]),
                    "cannot write FS command");
        }
        guest.State(0).r[0] = fs;
        Require(guest.Svc(0, 0x32) == NativeA32HostAction::Resume, "an FS request did not resume");
    };
    const auto word = [&](uint32_t index) {
        uint32_t value = 0;
        Require(memory.Read32(command + index * 4U, &value), "cannot read FS reply");
        return value;
    };
    call({0x08010002U, 0x20U, 0U});
    Require(word(1) == 0U, "FS:Initialize failed");
    constexpr uint32_t pathAddress = 0x0FF00200;
    Require(memory.Write32(pathAddress, 0), "cannot write the empty path");
    call({0x080C00C2U, 4U, 1U, 1U, (1U << 14U) | 2U, pathAddress});
    Require(word(1) == 0U, "OpenArchive(SaveData) failed");
    const uint32_t archiveLow = word(2), archiveHigh = word(3);
    const auto createDirectory = [&](std::string_view path) {
        std::vector<uint8_t> bytes(path.begin(), path.end());
        bytes.push_back(0);
        Require(memory.WriteBytes(pathAddress, bytes), "cannot write an FS path");
        const auto size = static_cast<uint32_t>(bytes.size());
        call({0x08090182U, 0U, archiveLow, archiveHigh, 3U, size, 0U, (size << 14U) | 2U,
              pathAddress});
        Require(word(0) == 0x08090040U, "CreateDirectory reply header is wrong");
        return word(1);
    };
    Require(createDirectory("/d") == 0U && std::filesystem::is_directory(save / "d"),
            "CreateDirectory(/d) did not create the directory");
    Require(createDirectory("/d/e") == 0U && std::filesystem::is_directory(save / "d" / "e"),
            "CreateDirectory(/d/e) did not create the nested directory");
    Require(createDirectory("/d") == 0xC82044B9U,
            "CreateDirectory of an existing directory is not DirectoryAlreadyExists");
    Require(createDirectory("/f.bin") == 0xC82044B9U,
            "CreateDirectory over a file is not DirectoryAlreadyExists");
    Require(createDirectory("/nope/x") == 0xC8804471U && !std::filesystem::exists(save / "nope"),
            "CreateDirectory under a missing parent is not PathNotFound");
    Require(createDirectory("/f.bin/x") == 0xE0C04702U,
            "CreateDirectory under a file is not UnexpectedFileOrDirectory");
    Require(createDirectory("/../fora") == 0xE0E046BEU && !std::filesystem::exists(directory / "fora"),
            "CreateDirectory escaped the save archive");
    std::filesystem::remove_all(directory, ignored);
}

// --- Despejo periódico do estado do guest (TRIAEVUM_GUEST_STATE_DUMP) -------
// O Mario parava no logo sem erro; para saber em que cada thread espera, o
// despejo mostra PC/LR, o último SVC de cada thread e os IPC que mudaram.
void TestGuestStateDump() {
    Require(!ParseGuestStateDumpInterval(nullptr) &&
                !ParseGuestStateDumpInterval("") &&
                !ParseGuestStateDumpInterval("0") &&
                !ParseGuestStateDumpInterval("30x") &&
                !ParseGuestStateDumpInterval("-5") &&
                ParseGuestStateDumpInterval("60") == std::optional<uint32_t>(60),
            "TRIAEVUM_GUEST_STATE_DUMP interval parsing is wrong");
    GuestStateDumper off(0);
    GuestStateDumper every30(30);
    Require(!off.Due(30) && !every30.Due(0) && !every30.Due(29) &&
                every30.Due(30) && every30.Due(60),
            "the guest state dump is due at the wrong VBlanks");

    NativeA32CtrHostConfig config = MakeConfig();
    config.StubUnknownServices = true;
    config.CollectIpcNameCounts = true;
    NativeA32CtrHostServices host(std::move(config));
    const oot3d::recomp::a32::Registry registry{};
    NativeA32Process process(registry, host);
    MakeGuestThreads(process);
    GuestThreads guest{host, process};
    auto& memory = process.Memory();
    // Thread 1 para num mutex travado pela thread 0.
    guest.State(0).r[1] = 1;
    Require(guest.Svc(0, 0x13) == NativeA32HostAction::Resume,
            "CreateMutex failed");
    const uint32_t mutex = guest.State(0).r[1];
    guest.State(1).r[15] = 0x00123456U;
    guest.State(1).r[14] = 0x00ABCDEFU;
    Require(guest.Wait1(1, mutex, -1) == NativeA32HostAction::Wait,
            "the locked mutex did not park thread 1");
    // Thread 0 faz dois IPC iguais a um serviço stub.
    const uint32_t srv = OpenSrv(guest);
    uint32_t ptm = 0;
    Require(GetServiceHandle(guest, srv, "ptm:u", &ptm) ==
                NativeA32HostAction::Resume,
            "ptm:u stub was not opened");
    for (int i = 0; i < 2; ++i) {
        Require(memory.Write32(0x1FF82080, 0x00050000), "cannot write IPC");
        guest.State(0).r[0] = ptm;
        Require(guest.Svc(0, 0x32) == NativeA32HostAction::Resume,
                "stub IPC failed");
    }

    GuestStateDumper dumper(30);
    const std::string first = dumper.Format(process, host, 30, 0x00000001U);
    const auto has = [](const std::string& text, std::string_view needle) {
        return text.find(needle) != std::string::npos;
    };
    Require(has(first, "TRIAEVUM_GUEST_STATE vblank=30 ") &&
                has(first, " pad=0x00000001 threads=3 ipc_counts=on gsp_shared=0x00000000\n"),
            "the dump header is missing VBlank, pad or thread count:\n" + first);
    Require(has(first, "TRIAEVUM_GUEST_THREAD vblank=30 id=1 ") &&
                has(first, "pc=0x00123456 lr=0x00abcdef") &&
                has(first, "last_svc=\"WaitSynchronization1"),
            "the dump does not show where thread 1 waits:\n" + first);
    Require(has(first, "id=2 ") && has(first, "prio=0x31"),
            "the dump skips a thread or its priority:\n" + first);
    Require(has(first, "TRIAEVUM_GUEST_IPC vblank=30 delta=2 total=2 name=\""),
            "the dump does not count the repeated IPC:\n" + first);
    // Sem IPC novo, o despejo seguinte não repete as linhas IPC e o delta
    // de SVC é zero.
    const std::string second = dumper.Format(process, host, 60, 0U);
    Require(!has(second, "TRIAEVUM_GUEST_IPC") && has(second, "svc_delta=0 "),
            "the second dump repeated unchanged IPC counts:\n" + second);
    Require(memory.Write32(0x1FF82080, 0x00050000), "cannot write IPC");
    guest.State(0).r[0] = ptm;
    Require(guest.Svc(0, 0x32) == NativeA32HostAction::Resume, "stub IPC failed");
    const std::string third = dumper.Format(process, host, 90, 0U);
    Require(has(third, "TRIAEVUM_GUEST_IPC vblank=90 delta=1 total=3 "),
            "the third dump did not report the IPC delta:\n" + third);

    // Faixas de memória: palavras little-endian; endereço sem mapa = unmapped.
    Require(ParseGuestMemoryRanges("1ff82080:8,0:4").size() == 2U &&
                ParseGuestMemoryRanges("1ff82080").empty() &&
                ParseGuestMemoryRanges("1ff82080:0").empty() &&
                ParseGuestMemoryRanges("1ff82080:101").empty() &&
                ParseGuestMemoryRanges("zz:4").empty() &&
                ParseGuestMemoryRanges(nullptr).empty(),
            "TRIAEVUM_GUEST_STATE_MEM parsing is wrong");
    Require(memory.Write32(0x1FF82080, 0x11223344U) &&
                memory.Write32(0x1FF82084, 0xA0B0C0D0U),
            "cannot write the watched words");
    dumper.SetMemoryRanges(ParseGuestMemoryRanges("1ff82080:8,4:4"));
    const std::string fourth = dumper.Format(process, host, 120, 0U);
    Require(has(fourth, "TRIAEVUM_GUEST_MEM vblank=120 addr=0x1ff82080 "
                        "words=0x11223344 0xa0b0c0d0\n") &&
                has(fourth, "addr=0x00000004 words=unmapped\n"),
            "the dump does not show the watched guest memory:\n" + fourth);

    // Sem a contagem por nome o cabeçalho avisa.
    NativeA32CtrHostServices plain(MakeConfig());
    NativeA32Process plainProcess(registry, plain);
    MakeGuestThreads(plainProcess);
    GuestStateDumper plainDumper(1);
    Require(has(plainDumper.Format(plainProcess, plain, 1, 0U), "ipc_counts=off"),
            "the dump did not say IPC counts are off");
}

// --- Archive SelfNCCH (id 3): RomFS e seções do ExeFS do próprio título -----
// O Mario, depois de A na tela de título, abre o archive 3 com caminho vazio e
// lê o "icon" do ExeFS (SMDH, 0x36C0 bytes) para a caixa do StreetPass. O host
// respondia 0xE0E046BE (unsupported-archive-path) e o jogo parava em err:f.
// Comportamento conferido no Azahar (file_sys/archive_selfncch.cpp).
void TestSelfNcchArchive() {
    const auto directory = std::filesystem::temp_directory_path() /
                           "oot3d_native_a32_ctr_host_test_selfncch";
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::create_directories(directory / "exefs");
    std::vector<uint8_t> icon(0x36C0U);
    for (size_t i = 0; i < icon.size(); ++i) icon[i] = static_cast<uint8_t>(i * 7U + 3U);
    {
        std::ofstream out(directory / "exefs" / "icon", std::ios::binary);
        out.write(reinterpret_cast<const char*>(icon.data()),
                  static_cast<std::streamsize>(icon.size()));
        Require(out.good(), "cannot write the ExeFS icon fixture");
    }
    constexpr std::array<uint8_t, 12> romFsBytes{0x49, 0x56, 0x46, 0x43, 0xA0, 0xA1,
                                                 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7};
    {
        std::ofstream out(directory / "romfs.bin", std::ios::binary);
        out.write(reinterpret_cast<const char*>(romFsBytes.data()),
                  static_cast<std::streamsize>(romFsBytes.size()));
        Require(out.good(), "cannot write the RomFS fixture");
    }
    NativeA32CtrHostConfig config = MakeConfig();
    config.StubUnknownServices = true;
    config.ExeFsDirectory = directory / "exefs";
    // Como a janela: com pasta de save, a raiz vazia do SelfNCCH não coincide
    // com a do SaveData no savestate.
    config.SaveDataDirectory = directory / "save";
    config.RomFsImagePath = directory / "romfs.bin";
    config.RomFsImageOffset = 4;
    config.RomFsImageSize = romFsBytes.size() - 4U;
    NativeA32CtrHostServices host(std::move(config));
    const oot3d::recomp::a32::Registry registry{};
    NativeA32Process process(registry, host);
    MakeGuestThreads(process);
    GuestThreads guest{host, process};
    auto& memory = process.Memory();
    const uint32_t srv = OpenSrv(guest);
    uint32_t fs = 0;
    Require(GetServiceHandle(guest, srv, "fs:USER", &fs) == NativeA32HostAction::Resume &&
                fs != 0,
            "fs:USER was not opened");
    const auto call = [&](uint32_t session) {
        guest.State(0).r[0] = session;
        Require(guest.Svc(0, 0x32) == NativeA32HostAction::Resume,
                "an FS request did not resume");
        uint32_t result = 0xFFFFFFFFU;
        Require(memory.Read32(0x1FF82084, &result), "cannot read the FS result");
        return result;
    };
    Require(memory.Write32(0x1FF82080, 0x08010002) && memory.Write32(0x1FF82084, 0x20) &&
                memory.Write32(0x1FF82088, 0) && call(fs) == 0,
            "FS:Initialize failed");

    // OpenArchive(3, caminho vazio) como o Mario faz (pc 0x23E468).
    constexpr uint32_t pathAddress = 0x0FF00200;
    uint32_t faultAddress = 0;
    Require(memory.Write32(pathAddress, 0) && memory.Write32(0x1FF82080, 0x080C00C2) &&
                memory.Write32(0x1FF82084, 3) && memory.Write32(0x1FF82088, 1) &&
                memory.Write32(0x1FF8208C, 1) && memory.Write32(0x1FF82090, (1U << 14U) | 2U) &&
                memory.Write32(0x1FF82094, pathAddress),
            "cannot write FS:OpenArchive(SelfNCCH)");
    const uint32_t openArchiveResult = call(fs);
    uint32_t header = 0;
    uint64_t archive = 0;
    Require(openArchiveResult == 0 && memory.Read32(0x1FF82080, &header) &&
                header == 0x080C00C0 &&
                memory.Read64(0x1FF82088, &archive, &faultAddress) && archive != 0,
            "FS:OpenArchive(SelfNCCH, empty path) did not open the archive (result 0x" +
                [&] { std::ostringstream o; o << std::hex << openArchiveResult; return o.str(); }() + ")");

    // OpenFile(archive, caminho binário de 12 bytes {tipo, nome[8]}).
    const auto openFile = [&](uint32_t pathType, uint32_t type, std::string_view name,
                              uint32_t pathSize, uint32_t* session) {
        std::array<uint8_t, 12> path{};
        std::memcpy(path.data(), &type, 4);
        std::copy(name.begin(), name.end(), path.begin() + 4);
        Require(memory.WriteBytes(pathAddress, path) && memory.Write32(0x1FF82080, 0x080201C2) &&
                    memory.Write32(0x1FF82084, 0) &&
                    memory.Write64(0x1FF82088, archive, &faultAddress) &&
                    memory.Write32(0x1FF82090, pathType) && memory.Write32(0x1FF82094, pathSize) &&
                    memory.Write32(0x1FF82098, 1) && memory.Write32(0x1FF8209C, 0) &&
                    memory.Write32(0x1FF820A0, (pathSize << 14U) | 2U) &&
                    memory.Write32(0x1FF820A4, pathAddress),
                "cannot write FS:OpenFile(SelfNCCH)");
        const uint32_t result = call(fs);
        *session = 0;
        if (result == 0) Require(memory.Read32(0x1FF8208C, session), "cannot read file session");
        return result;
    };
    constexpr uint32_t readAddress = 0x0FF04000;
    const auto read = [&](uint32_t session, uint64_t offset, uint32_t length, uint32_t* returned) {
        Require(memory.Write32(0x1FF82080, 0x080200C2) &&
                    memory.Write64(0x1FF82084, offset, &faultAddress) &&
                    memory.Write32(0x1FF8208C, length) &&
                    memory.Write32(0x1FF82090, (length << 4U) | 0xCU) &&
                    memory.Write32(0x1FF82094, readAddress),
                "cannot write File:Read");
        const uint32_t result = call(session);
        if (returned != nullptr) Require(memory.Read32(0x1FF82088, returned), "cannot read count");
        return result;
    };
    uint32_t iconFile = 0;
    Require(openFile(2, 2, "icon", 12, &iconFile) == 0 && iconFile != 0,
            "SelfNCCH OpenFile(ExeFS icon) failed");
    Require(memory.Write32(0x1FF82080, 0x08040000) && call(iconFile) == 0,
            "ExeFS icon GetSize failed");
    uint64_t iconSize = 0;
    Require(memory.Read64(0x1FF82088, &iconSize, &faultAddress) && iconSize == icon.size(),
            "ExeFS icon GetSize is not the section size");
    uint32_t returned = 0;
    std::vector<uint8_t> readBack(icon.size());
    Require(read(iconFile, 0, static_cast<uint32_t>(icon.size()), &returned) == 0 &&
                returned == icon.size() && memory.ReadBytes(readAddress, readBack) &&
                readBack == icon,
            "ExeFS icon Read did not return the section bytes");
    // Azahar ExeFSSectionFile: leitura inteira de uma vez, do offset 0.
    Require(read(iconFile, 0, 0x100, nullptr) == 0xE0C046F9U,
            "a partial ExeFS read was not IncorrectExeFSReadSize");
    Require(read(iconFile, 4, static_cast<uint32_t>(icon.size()), nullptr) == 0xE0C046F8U,
            "an ExeFS read at a non-zero offset was not UnsupportedOpenFlags");

    uint32_t other = 0;
    Require(openFile(2, 2, "banner", 12, &other) == 0xC8804637U,
            "a missing ExeFS section was not ExeFSSectionNotFound");
    Require(openFile(2, 2, "bogus", 12, &other) == 0xE0E046BEU,
            "an unknown ExeFS section was not InvalidPath");
    Require(openFile(2, 1, "", 12, &other) == 0xD9004676U,
            "the .code section was not CommandNotAllowed");
    Require(openFile(2, 7, "", 12, &other) == 0xE0E046BEU,
            "an unknown SelfNCCH file type was not InvalidPath");
    Require(openFile(2, 2, "icon", 8, &other) == 0xE0E046BEU,
            "a short SelfNCCH binary path was not InvalidPath");
    Require(openFile(3, 2, "icon", 12, &other) == 0xE0E046BEU,
            "a non-binary SelfNCCH path was not InvalidPath");
    Require(openFile(2, 5, "", 12, &other) == 0xC8804464U,
            "the update RomFS (absent) was not RomFSNotFound");
    uint32_t romFsFile = 0;
    std::array<uint8_t, 4> romFsRead{};
    Require(openFile(2, 0, "", 12, &romFsFile) == 0 && romFsFile != 0 &&
                read(romFsFile, 2, 4, &returned) == 0 && returned == 4 &&
                memory.ReadBytes(readAddress, romFsRead) &&
                romFsRead == std::array<uint8_t, 4>{0xA2, 0xA3, 0xA4, 0xA5},
            "SelfNCCH OpenFile(RomFS) did not open the title RomFS view");

    // Savestate com o archive e a seção abertos: volta lendo os mesmos bytes.
    const auto saved = host.CaptureState();
    std::string error;
    Require(host.RestoreState(saved, process, &error),
            "a savestate with the SelfNCCH archive and ExeFS icon open did not restore: " +
                error);
    std::fill(readBack.begin(), readBack.end(), uint8_t{0});
    Require(memory.WriteBytes(readAddress, readBack) &&
                read(iconFile, 0, static_cast<uint32_t>(icon.size()), &returned) == 0 &&
                memory.ReadBytes(readAddress, readBack) && readBack == icon,
            "the restored ExeFS icon did not read the section bytes");
    uint32_t afterRestore = 0;
    Require(openFile(2, 2, "icon", 12, &afterRestore) == 0 && afterRestore != 0,
            "the restored SelfNCCH archive handle no longer opens files");
    std::filesystem::remove_all(directory, ignored);
}

// --- cfg:u GetConfig dos blocos padrão do Azahar (cfg_defaults.cpp) ---------
// Depois do A na tela de título o Mario lê o bloco 0x000D0000 (versão do EULA
// aceita) e compara com a do SMDH (1.0 = 0x100, 0x2523CC). O stub respondia
// sucesso sem escrever o buffer: versão < 0x100, o jogo pré-carregava o applet
// do EULA (0x406) e esperava para sempre por ele. Azahar: EULA 0x7F.0x7F.
void TestCfgAzaharDefaultBlocks() {
    NativeA32CtrHostConfig config = MakeConfig();
    config.StubUnknownServices = true;
    NativeA32CtrHostServices host(std::move(config));
    const oot3d::recomp::a32::Registry registry{};
    NativeA32Process process(registry, host);
    MakeGuestThreads(process);
    GuestThreads guest{host, process};
    auto& memory = process.Memory();
    const uint32_t srv = OpenSrv(guest);
    uint32_t cfg = 0;
    Require(GetServiceHandle(guest, srv, "cfg:u", &cfg) == NativeA32HostAction::Resume &&
                cfg != 0,
            "cfg:u was not opened");
    constexpr uint32_t output = 0x0FF00400;
    const auto getConfig = [&](uint32_t block, uint32_t size, std::vector<uint8_t>* bytes) {
        std::vector<uint8_t> garbage(size, 0xAAU);
        Require(memory.WriteBytes(output, garbage) &&
                    memory.Write32(0x1FF82080, 0x00010082) &&
                    memory.Write32(0x1FF82084, size) && memory.Write32(0x1FF82088, block) &&
                    memory.Write32(0x1FF8208C, (size << 4U) | 0xCU) &&
                    memory.Write32(0x1FF82090, output),
                "cannot write cfg:u GetConfig");
        guest.State(0).r[0] = cfg;
        Require(guest.Svc(0, 0x32) == NativeA32HostAction::Resume, "GetConfig did not resume");
        uint32_t header = 0, result = 1;
        bytes->assign(size, 0);
        Require(memory.Read32(0x1FF82080, &header) && memory.Read32(0x1FF82084, &result) &&
                    memory.ReadBytes(output, *bytes),
                "cannot read GetConfig reply");
        return header == 0x00010042U && result == 0U;
    };
    std::vector<uint8_t> bytes;
    Require(getConfig(0x000D0000U, 4, &bytes) &&
                bytes == std::vector<uint8_t>{0x7F, 0x7F, 0x00, 0x00},
            "cfg block 0x000D0000 (agreed EULA version) is not Azahar's 0x7F.0x7F");
    Require(getConfig(0x000C0000U, 0xC0, &bytes) &&
                bytes == std::vector<uint8_t>(0xC0, 0x00),
            "cfg block 0x000C0000 (parental restrictions) is not all zero");
    Require(getConfig(0x000C0001U, 0x14, &bytes) &&
                bytes == std::vector<uint8_t>(0x14, 0x00),
            "cfg block 0x000C0001 (COPPACS) is not all zero");
    Require(getConfig(0x00150002U, 4, &bytes) &&
                bytes == std::vector<uint8_t>{'L', '1', 0, 0},
            "cfg block 0x00150002 (server type) is not L1");
}

// svcCreateThread passes the requested processor (r4) to the guest-thread
// profile, next to the guest_thread: log line.
void TestCreateThreadProcessor() {
    const oot3d::recomp::a32::Registry registry{};
    NativeA32CtrHostServices host(MakeConfig());
    NativeA32Process process(registry, host);
    const std::array<uint8_t, 0x10> code{};
    std::string error;
    Require(process.MapRegion({"text", 0x00100000, code.size(), false, true, code},
                              &error) &&
                process.MapRegion({"data", 0x00200000, 0x1000, true, false, {}},
                                  &error) &&
                process.CreatePrimaryThread(
                    {0x00100000, 0x00300000, 0x1000, 0x00400000, 0x1000, 0, 0,
                     0x10U, 0x03C00010U, 0x30U},
                    &error),
            error);
    NativeA32HostContext context{process, 0};
    GuestState state{};
    state.r[0] = 0x18;        // priority
    state.r[1] = 0x00100000;  // entry
    state.r[2] = 7;           // argument
    state.r[3] = 0x00200400;  // stack top
    state.r[4] = 1;           // processor
    Require(host.HandleSvc(0x08, state, process.Memory(), context).Action ==
                    NativeA32HostAction::Resume &&
                state.r[0] == 0 && process.ThreadCount() == 2U,
            "CreateThread did not create the guest thread");
    Require(process.GuestThreadProfile().ProcessorIds[0] == -2 &&
                process.GuestThreadProfile().ProcessorIds[1] == 1,
            "CreateThread did not record the requested processor");
    state.r[0] = 0x18;
    state.r[1] = 0x00100000;  // r1 came back as the thread handle
    state.r[4] = static_cast<uint32_t>(-1);
    Require(host.HandleSvc(0x08, state, process.Memory(), context).Action ==
                    NativeA32HostAction::Resume &&
                process.GuestThreadProfile().ProcessorIds[2] == -1,
            "CreateThread did not record processor -1 as signed");
}

// config/kernel_svc: the same random SVC sequence (ArbitrateAddress,
// SleepThread, WaitSynchronizationN, events, handles created and closed
// between lookups) on two hosts, fast paths off and on, must leave the same
// results, registers, guest memory, kernel state and SVC event history after
// every call.
void TestFastSvcEquivalence() {
    struct Side {
        explicit Side(bool fast) : host([fast] {
                                       auto config = MakeConfig();
                                       config.FastSvc = fast;
                                       return config;
                                   }()),
                                   process(registry, host) {
            const std::array<uint8_t, 0x10> code{};
            std::string error;
            Require(process.MapRegion({"text", 0x00100000, code.size(), false, true, code},
                                      &error) &&
                        process.MapRegion({"data", 0x00200000, 0x1000, true, false, {}},
                                          &error) &&
                        process.CreatePrimaryThread({0x00100000, 0x00300000, 0x1000, 0x00400000,
                                                     0x1000, 0, 0, 0x10U, 0x03C00010U, 0x30U},
                                                    &error),
                    error);
        }
        oot3d::recomp::a32::Registry registry{};
        NativeA32CtrHostServices host;
        NativeA32Process process;
    };
    Side slow(false);
    Side fast(true);
    uint64_t random = 0x9E3779B97F4A7C15ULL;
    const auto next = [&random](uint32_t bound) {
        random ^= random << 13U;
        random ^= random >> 7U;
        random ^= random << 17U;
        return static_cast<uint32_t>(random % bound);
    };
    std::vector<uint32_t> handles;  // the same values on both sides
    const auto call = [&](uint32_t immediate, GuestState state, uint32_t threadId,
                          const char* what) {
        GuestState slowState = state;
        GuestState fastState = state;
        NativeA32HostContext slowContext{slow.process, threadId};
        NativeA32HostContext fastContext{fast.process, threadId};
        const auto a = slow.host.HandleSvc(immediate, slowState, slow.process.Memory(),
                                           slowContext);
        const auto b = fast.host.HandleSvc(immediate, fastState, fast.process.Memory(),
                                           fastContext);
        const std::string where = std::string(what) + " (fast SVC equivalence)";
        Require(a.Action == b.Action && a.ResumePc == b.ResumePc && a.Detail == b.Detail &&
                    a.Error == b.Error,
                "result differs: " + where);
        Require(slowState.r == fastState.r && slowState.cpsr == fastState.cpsr,
                "registers differ: " + where);
        Require(slow.process.StateFingerprint() == fast.process.StateFingerprint(),
                "guest state differs: " + where);
        Require(slow.host.CaptureState().dump() == fast.host.CaptureState().dump(),
                "kernel state differs: " + where);
        const auto& slowEvents = slow.host.SvcEvents();
        const auto& fastEvents = fast.host.SvcEvents();
        Require(slowEvents.size() == fastEvents.size(), "event count differs: " + where);
        for (size_t index = 0; index < slowEvents.size(); ++index) {
            const auto& x = slowEvents[index];
            const auto& y = fastEvents[index];
            Require(x.Immediate == y.Immediate && x.Pc == y.Pc && x.ThreadId == y.ThreadId &&
                        x.Handled == y.Handled && x.Detail == y.Detail && x.Name == y.Name &&
                        y.NameKind == NativeA32CtrSvcEventNameKind::Text,
                    "event differs: " + where + ": " + x.Name + " / " + y.Name);
        }
        return slowState;
    };
    // Two more threads (ids 1 and 2), an arbiter and a few events.
    for (uint32_t thread = 0; thread < 2U; ++thread) {
        GuestState state{};
        state.r[0] = 0x20U + thread;
        state.r[1] = 0x00100000;
        state.r[3] = 0x00200800;
        state.r[4] = static_cast<uint32_t>(-2);
        call(0x08, state, 0, "CreateThread");
    }
    handles.push_back(call(0x21, GuestState{}, 0, "CreateAddressArbiter").r[1]);
    const uint32_t arbiter = handles.back();
    const auto createEvent = [&] {
        GuestState state{};
        state.r[0] = 0xFFFFFFFFU;  // success writes 0; a refused call leaves it
        state.r[1] = next(2U);     // one-shot or sticky
        const GuestState result = call(0x17, state, 0, "CreateEvent");
        if (result.r[0] == 0U) handles.push_back(result.r[1]);
    };
    for (int count = 0; count < 4; ++count) createEvent();
    const auto anyHandle = [&] {
        const uint32_t pick = next(10U);
        if (pick == 0U) return 0xFFFF8001U;          // the current thread: no record
        if (pick == 1U) return 0x12345678U;          // never created
        return handles[next(static_cast<uint32_t>(handles.size()))];
    };
    for (int step = 0; step < 4000; ++step) {
        const uint32_t threadId = next(3U);
        GuestState state{};
        state.r[15] = 0x00100000U + 4U * next(4U);
        switch (next(9U)) {
        case 0:
        case 1: {  // ArbitrateAddress: mostly valid, sometimes not
            state.r[0] = next(8U) == 0U ? anyHandle() : arbiter;
            state.r[1] = next(16U) == 0U ? 0x00500000U : 0x00200000U + 4U * next(8U);
            state.r[2] = next(6U);
            state.r[3] = static_cast<uint32_t>(static_cast<int32_t>(next(5U)) - 2);
            call(0x22, state, threadId, "ArbitrateAddress");
            break;
        }
        case 2: {  // the arbitrated words move
            const uint32_t address = 0x00200000U + 4U * next(8U);
            const uint32_t value = static_cast<uint32_t>(static_cast<int32_t>(next(5U)) - 2);
            Require(slow.process.Memory().Write32(address, value) &&
                        fast.process.Memory().Write32(address, value),
                    "data write");
            break;
        }
        case 3: {  // SleepThread: timed, infinite, yield, negative
            static constexpr int64_t kSleeps[] = {1000, 1'000'000'000, -1, 0, -7,
                                                  INT64_MAX};
            const auto nanoseconds = static_cast<uint64_t>(kSleeps[next(6U)]);
            state.r[0] = static_cast<uint32_t>(nanoseconds);
            state.r[1] = static_cast<uint32_t>(nanoseconds >> 32U);
            call(0x0A, state, threadId, "SleepThread");
            break;
        }
        case 4: {  // WaitSynchronizationN on 0-3 handles
            const uint32_t count = next(4U);
            for (uint32_t index = 0; index < count; ++index) {
                const uint32_t handle = anyHandle();
                Require(slow.process.Memory().Write32(0x00200100U + 4U * index, handle) &&
                            fast.process.Memory().Write32(0x00200100U + 4U * index, handle),
                        "handle list write");
            }
            state.r[1] = next(16U) == 0U ? 0x00500000U : 0x00200100U;
            state.r[2] = next(32U) == 0U ? 0x41U : count;
            state.r[3] = next(2U);
            call(0x25, state, threadId, "WaitSynchronizationN");
            break;
        }
        case 5:
        case 6:  // SignalEvent / ClearEvent
            state.r[0] = anyHandle();
            call(next(2U) == 0U ? 0x18 : 0x19, state, threadId, "Signal/ClearEvent");
            break;
        case 7: {  // the handle table changes under the lookup cache
            if (next(2U) == 0U && handles.size() > 2U) {
                const size_t index = 1U + next(static_cast<uint32_t>(handles.size() - 1U));
                state.r[0] = handles[index];
                call(0x23, state, 0, "CloseHandle");
                handles.erase(handles.begin() + static_cast<std::ptrdiff_t>(index));
            } else if (handles.size() < 12U) {
                createEvent();
            }
            break;
        }
        default:
            slow.host.AdvanceSystemTicks(1U + next(400000U));
            fast.host.AdvanceSystemTicks(slow.host.SystemTicks() - fast.host.SystemTicks());
            break;
        }
    }
    std::cout << "oot3d_native_a32_ctr_host_tests: fast SVC equivalence ok ("
              << slow.host.SvcEventCount() << " SVCs)\n";
}

void RunAllTests() {
    TestCreateThreadProcessor();
    TestCfgAzaharDefaultBlocks();
    TestSelfNcchArchive();
    TestGuestStateDump();
    TestRvctLibraryNatives();
    TestWindowProfileGatesOot3dHooks();
    TestFrameBoundaryProviderSelection();
    TestGspFrameBoundaryProvider();
    TestOot3dFrameBoundaryMatchesGsp();
    TestHostReportsGspFrameBoundaries();
    TestSharedPageDateTime();
    TestGuestMutexSemaphoreAndTimeouts();
    TestWaitOnInvalidHandle();
    TestGuestKernelTimers();
    TestOot3dProfileWaitsStayInfinite();
    TestUnknownServicesAreLoggedStubs();
    TestCecdEventHandles();
    TestDspAudioFrameDeliveryWakesSoundThread();
    TestThreadQueries();
    TestCreateMemoryBlockOfGuestMemory();
    TestAptGetSharedFont();
    TestAptLibraryAppletMiiSelector();
    TestFsOpenDirectory();
    TestFsCreateDirectory();
    TestDeferredDeviceMemory();
    TestDeferredDeviceFailureAndOverwrite();
    TestGpuMemoryWriteOwnership();
    TestGuestAccessWatch();
    TestBoundedSvcEventHistory();
    const std::filesystem::path romFsPath =
        std::filesystem::temp_directory_path() /
        "oot3d_native_a32_ctr_host_test.romfs";
    const std::filesystem::path saveDataPath =
        std::filesystem::temp_directory_path() /
        "oot3d_native_a32_ctr_host_test_savedata";
    std::error_code cleanupError;
    std::filesystem::remove_all(saveDataPath, cleanupError);
    // Save journal backups (<save>-backups, next to the directory).
    std::filesystem::remove_all(saveDataPath.string() + "-backups", cleanupError);
    constexpr std::array<uint8_t, 16> romFsBytes{
        0x49, 0x56, 0x46, 0x43, 0x00, 0x01, 0x02, 0x03,
        0x10, 0x11, 0x12, 0x13, 0x20, 0x21, 0x22, 0x23,
    };
    {
        std::ofstream output(romFsPath, std::ios::binary | std::ios::trunc);
        Require(output.good(), "cannot create temporary RomFS image");
        output.write(reinterpret_cast<const char*>(romFsBytes.data()),
                     static_cast<std::streamsize>(romFsBytes.size()));
        Require(output.good(), "cannot write temporary RomFS image");
    }
    NativeA32Memory memory;
    std::string error;
    Require(memory.MapRegion(
                {"tls", 0x1FF82000, 0x1000, true, false, {}}, &error),
            error);
    Oot3dNativePicaFrontend picaFrontend;
    NativeA32CtrHostConfig config = MakeConfig();
    config.RomFsImagePath = romFsPath;
    config.RomFsImageOffset = 4;
    config.RomFsImageSize = romFsBytes.size() - config.RomFsImageOffset;
    config.PicaFrontend = &picaFrontend;
    config.SaveDataDirectory = saveDataPath;
    config.ProfileRuntime = true;
    NativeA32CtrHostServices host(std::move(config));
    const oot3d::recomp::a32::Registry registry{};
    NativeA32Process process(registry, host);
    NativeA32HostContext context{process, 0};
    GuestState state{};
    state.thread_pointer = 0x1FF82000;

    Require(host.HandleSvc(0x21, state, memory, context).Action ==
                NativeA32HostAction::Resume &&
                state.r[0] == 0 && state.r[1] == 1,
            "CreateAddressArbiter did not return the first typed handle");

    state.r[1] = 0xFFFF8001;
    Require(host.HandleSvc(0x38, state, memory, context).Action ==
                NativeA32HostAction::Resume,
            "GetResourceLimit rejected the current process");
    const uint32_t resourceLimit = state.r[1];
    Require(memory.Write32(0x1FF82000, 1), "cannot write resource name");
    state.r[0] = 0x1FF82008;
    state.r[1] = resourceLimit;
    state.r[2] = 0x1FF82000;
    state.r[3] = 1;
    Require(host.HandleSvc(0x3A, state, memory, context).Action ==
                NativeA32HostAction::Resume,
            "GetResourceLimitCurrentValues failed");
    uint64_t currentCommit = 0;
    uint32_t faultAddress = 0;
    Require(memory.Read64(0x1FF82008, &currentCommit, &faultAddress) &&
                currentCommit == 0x1000,
            "resource current value was not written as a 64-bit value");

    state.r[0] = 0x10003;
    state.r[1] = 0;
    state.r[2] = 0;
    state.r[3] = 0x2000;
    state.r[4] = 3;
    Require(host.HandleSvc(0x01, state, memory, context).Action ==
                NativeA32HostAction::Resume &&
                state.r[1] == 0x14000000 &&
                memory.IsMapped(0x14000000, 0x2000),
            "linear ControlMemory commit was not mapped");

    constexpr std::array<uint8_t, 5> portName{'s', 'r', 'v', ':', 0};
    Require(memory.WriteBytes(0x1FF82100, portName),
            "cannot write named port");
    state.r[1] = 0x1FF82100;
    Require(host.HandleSvc(0x2D, state, memory, context).Action ==
                NativeA32HostAction::Resume,
            "ConnectToPort did not open srv:");
    const uint32_t srvSession = state.r[1];
    Require(memory.Write32(0x1FF82080, 0x00010002),
            "cannot write srv request");
    state.r[0] = srvSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                NativeA32HostAction::Resume,
            "srv:RegisterClient did not resume");
    uint32_t response = 0;
    Require(memory.Read32(0x1FF82080, &response) && response == 0x00010040,
            "srv:RegisterClient response header is wrong");

    Require(memory.Write32(0x1FF82080, 0x00050100) &&
                memory.Write32(0x1FF82084, 0x3A545041) &&
                memory.Write32(0x1FF82088, 0x00000055) &&
                memory.Write32(0x1FF8208C, 5),
            "cannot write srv:GetServiceHandle request");
    state.r[0] = srvSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                NativeA32HostAction::Resume,
            "srv:GetServiceHandle did not open APT:U");
    uint32_t aptSession = 0;
    Require(memory.Read32(0x1FF82080, &response) &&
                response == 0x00050042 &&
                memory.Read32(0x1FF8208C, &aptSession),
            "srv:GetServiceHandle APT:U response is wrong");

    Require(memory.Write32(0x1FF82080, 0x00050100) &&
                memory.Write32(0x1FF82084, 0x553A7366) &&
                memory.Write32(0x1FF82088, 0x00524553) &&
                memory.Write32(0x1FF8208C, 7),
            "cannot write srv:GetServiceHandle fs:USER request");
    state.r[0] = srvSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                NativeA32HostAction::Resume,
            "srv:GetServiceHandle did not open fs:USER");
    uint32_t fsSession = 0;
    Require(memory.Read32(0x1FF8208C, &fsSession) && fsSession != 0,
            "srv:GetServiceHandle fs:USER response has no session");
    Require(memory.Write32(0x1FF82080, 0x08010002) &&
                memory.Write32(0x1FF82084, 0x20) &&
                memory.Write32(0x1FF82088, 0),
            "cannot write FS:Initialize request");
    state.r[0] = fsSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                NativeA32HostAction::Resume &&
                memory.Read32(0x1FF82080, &response) &&
                response == 0x08010040,
            "FS:Initialize response is wrong");

    Require(memory.Fill(0x1FF82120, 1, 0) &&
                memory.Write32(0x1FF82080, 0x080C00C2) &&
                memory.Write32(0x1FF82084, 4) &&
                memory.Write32(0x1FF82088, 1) &&
                memory.Write32(0x1FF8208C, 1) &&
                memory.Write32(0x1FF82090, 0x4002) &&
                memory.Write32(0x1FF82094, 0x1FF82120),
            "cannot write FS:OpenArchive SaveData request");
    state.r[0] = fsSession;
    uint64_t saveArchive = 0;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                memory.Read32(0x1FF82080, &response) &&
                response == 0x080C00C0 &&
                memory.Read64(0x1FF82088, &saveArchive, &faultAddress) &&
                saveArchive != 0 &&
                std::filesystem::is_directory(saveDataPath),
            "FS:OpenArchive did not mount persistent OOT3D SaveData");

    constexpr std::array<uint8_t, 20> saveFilePath{
        '/', 0, 't', 0, 'e', 0, 's', 0, 't', 0,
        '.', 0, 'd', 0, 'a', 0, 't', 0, 0, 0,
    };
    constexpr std::array<uint8_t, 4> saveFileBytes{1, 2, 3, 4};
    {
        std::ofstream saveFile(saveDataPath / "test.dat",
                               std::ios::binary | std::ios::trunc);
        saveFile.write(
            reinterpret_cast<const char*>(saveFileBytes.data()),
            static_cast<std::streamsize>(saveFileBytes.size()));
        Require(saveFile.good(), "cannot create SaveData file fixture");
    }
    Require(memory.WriteBytes(0x1FF82140, saveFilePath) &&
                memory.Write32(0x1FF82080, 0x080201C2) &&
                memory.Write32(0x1FF82084, 0) &&
                memory.Write64(0x1FF82088, saveArchive, &faultAddress) &&
                memory.Write32(0x1FF82090, 4) &&
                memory.Write32(0x1FF82094, saveFilePath.size()) &&
                memory.Write32(0x1FF82098, 3) &&
                memory.Write32(0x1FF8209C, 0) &&
                memory.Write32(0x1FF820A0,
                               (saveFilePath.size() << 14U) | 2U) &&
                memory.Write32(0x1FF820A4, 0x1FF82140),
            "cannot write FS:OpenFile SaveData request");
    state.r[0] = fsSession;
    uint32_t saveFileSession = 0;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                memory.Read32(0x1FF82080, &response) &&
                response == 0x08020042 &&
                memory.Read32(0x1FF82084, &response) && response == 0 &&
                memory.Read32(0x1FF8208C, &saveFileSession) &&
                saveFileSession != 0,
            "FS:OpenFile did not open a confined SaveData path");
    Require(memory.Write32(0x1FF82080, 0x08040000),
            "cannot write SaveData File:GetSize request");
    state.r[0] = saveFileSession;
    uint64_t saveFileSize = 0;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                memory.Read64(0x1FF82088, &saveFileSize, &faultAddress) &&
                saveFileSize == saveFileBytes.size(),
            "SaveData File:GetSize did not return the host file size");
    constexpr uint32_t writeAddress = 0x1FF82240;
    constexpr std::array<uint8_t, 3> writeBytes{9, 8, 7};
    Require(memory.WriteBytes(writeAddress, writeBytes) &&
                memory.Write32(0x1FF82080, 0x08030102) &&
                memory.Write32(0x1FF82084, 1) &&
                memory.Write32(0x1FF82088, 0) &&
                memory.Write32(0x1FF8208C, writeBytes.size()) &&
                memory.Write32(0x1FF82090, 1) &&
                memory.Write32(0x1FF82094,
                               (writeBytes.size() << 4U) | 0xAU) &&
                memory.Write32(0x1FF82098, writeAddress),
            "cannot write SaveData File:Write request");
    state.r[0] = saveFileSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                memory.Read32(0x1FF82080, &response) &&
                response == 0x08030082 &&
                memory.Read32(0x1FF82088, &response) &&
                response == writeBytes.size(),
            "SaveData File:Write did not persist the mapped payload");
    // 11/05 (loss-proof saves): the directory backend is no longer
    // write-through. Write and SetSize go to <file>.triaevum-tmp and only
    // swap the file on Flush/Close/ControlArchive, like the 3DS transactional
    // save; a crash midway no longer leaves the save half new. The same
    // byte and size assertions hold at the commit point (Flush).
    const auto persisted = [&]() {
        std::ifstream input(saveDataPath / "test.dat", std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(input)),
                                    std::istreambuf_iterator<char>());
    };
    const auto beforeCommit = persisted();
    Require(memory.Write32(0x1FF82080, 0x08050080) &&
                memory.Write64(0x1FF82084, 6, &faultAddress),
            "cannot write SaveData File:SetSize request");
    state.r[0] = saveFileSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                memory.Read32(0x1FF82080, &response) &&
                response == 0x08050040,
            "SaveData File:SetSize did not resume");
    Require(persisted() == beforeCommit,
            "SaveData File:Write/SetSize reached the host file before Flush");
    Require(memory.Write32(0x1FF82080, 0x08090000),
            "cannot write SaveData File:Flush request");
    state.r[0] = saveFileSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                memory.Read32(0x1FF82080, &response) &&
                response == 0x08090040,
            "SaveData File:Flush did not resume");
    const auto committed = persisted();
    Require(committed.size() >= 4U &&
                std::equal(committed.begin(), committed.begin() + 4,
                           std::array<uint8_t, 4>{1, 9, 8, 7}.begin()),
            "SaveData File:Write changed the wrong host byte range");
    Require(std::filesystem::file_size(saveDataPath / "test.dat") == 6,
            "SaveData File:SetSize did not resize the host file");
    Require(memory.Write32(0x1FF82080, 0x08080000),
            "cannot write SaveData File:Close request");
    state.r[0] = saveFileSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                NativeA32HostAction::Resume,
            "SaveData File:Close did not resume");

    constexpr std::array<uint8_t, 18> newSaveFilePath{
        '/', 0, 'n', 0, 'e', 0, 'w', 0, '.', 0,
        'd', 0, 'a', 0, 't', 0, 0, 0,
    };
    Require(memory.WriteBytes(0x1FF82170, newSaveFilePath) &&
                memory.Write32(0x1FF82080, 0x080201C2) &&
                memory.Write32(0x1FF82084, 0) &&
                memory.Write64(0x1FF82088, saveArchive, &faultAddress) &&
                memory.Write32(0x1FF82090, 4) &&
                memory.Write32(0x1FF82094, newSaveFilePath.size()) &&
                memory.Write32(0x1FF82098, 2) &&
                memory.Write32(0x1FF8209C, 0) &&
                memory.Write32(0x1FF820A0,
                               (newSaveFilePath.size() << 14U) | 2U) &&
                memory.Write32(0x1FF820A4, 0x1FF82170),
            "cannot write writable missing SaveData OpenFile request");
    state.r[0] = fsSession;
    uint32_t newSaveFileSession = 0;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                memory.Read32(0x1FF82084, &response) && response == 0 &&
                memory.Read32(0x1FF8208C, &newSaveFileSession) &&
                newSaveFileSession != 0 &&
                std::filesystem::is_regular_file(saveDataPath / "new.dat"),
            "writable SaveData open did not establish its persistent file");
    Require(memory.Write32(0x1FF82080, 0x08080000),
            "cannot close newly established SaveData file");
    state.r[0] = newSaveFileSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                NativeA32HostAction::Resume,
            "newly established SaveData file did not close");

    const auto deleteSave = [&](uint64_t archive, uint32_t descriptor) {
        Require(memory.WriteBytes(0x1FF82170, newSaveFilePath) &&
                    memory.Write32(0x1FF82080, 0x08040142) &&
                    memory.Write32(0x1FF82084, 0) &&
                    memory.Write64(0x1FF82088, archive, &faultAddress) &&
                    memory.Write32(0x1FF82090, 4) &&
                    memory.Write32(0x1FF82094, newSaveFilePath.size()) &&
                    memory.Write32(0x1FF82098, descriptor) &&
                    memory.Write32(0x1FF8209C, 0x1FF82170),
                "cannot write DeleteFile request");
        state.r[0] = fsSession;
        Require(host.HandleSvc(0x32, state, memory, context).Action == NativeA32HostAction::Resume &&
                    memory.Read32(0x1FF82080, &response) && response == 0x08040040 &&
                    memory.Read32(0x1FF82084, &response),
                "DeleteFile left its client waiting instead of replying");
        return response;
    };
    const uint32_t deleteDescriptor = (newSaveFilePath.size() << 14U) | 2U;
    Require(deleteSave(saveArchive + 1U, deleteDescriptor) == 0xC8804465U &&
                std::filesystem::exists(saveDataPath / "new.dat"), "invalid archive deleted a save");
    Require(deleteSave(saveArchive, 2U) == 0xE0E046BEU &&
                std::filesystem::exists(saveDataPath / "new.dat"), "malformed deletion changed a save");
    Require(deleteSave(saveArchive, deleteDescriptor) == 0U &&
                !std::filesystem::exists(saveDataPath / "new.dat") &&
                std::filesystem::exists(saveDataPath / "test.dat"), "DeleteFile removed the wrong file");
    Require(deleteSave(saveArchive, deleteDescriptor) == 0xC8804470U,
            "repeated DeleteFile did not report missing file");

    Require(memory.Write8(0x1FF82190, 0x5A) &&
                memory.Write8(0x1FF82194, 0xFF) &&
                memory.Write32(0x1FF82080, 0x080D0144) &&
                memory.Write64(0x1FF82084, saveArchive, &faultAddress) &&
                memory.Write32(0x1FF8208C, 0) &&
                memory.Write32(0x1FF82090, 1) &&
                memory.Write32(0x1FF82094, 1) &&
                memory.Write32(0x1FF82098, 0x1A) &&
                memory.Write32(0x1FF8209C, 0x1FF82190) &&
                memory.Write32(0x1FF820A0, 0x1C) &&
                memory.Write32(0x1FF820A4, 0x1FF82194),
            "cannot write FS:ControlArchive request");
    state.r[0] = fsSession;
    uint8_t controlOutput = 0xFF;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                memory.Read32(0x1FF82080, &response) &&
                response == 0x080D0040 &&
                memory.Read32(0x1FF82084, &response) && response == 0 &&
                memory.Read8(0x1FF82194, &controlOutput) &&
                controlOutput == 0,
            "FS:ControlArchive did not complete the mapped-buffer action");

    Require(memory.Write32(0x1FF82080, 0x080E0080) &&
                memory.Write64(0x1FF82084, saveArchive, &faultAddress),
            "cannot write FS:CloseArchive request");
    state.r[0] = fsSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                memory.Read32(0x1FF82080, &response) &&
                response == 0x080E0040 &&
                memory.Read32(0x1FF82084, &response) && response == 0,
            "FS:CloseArchive did not close the SaveData handle");
    Require(memory.Write32(0x1FF82080, 0x080E0080) &&
                memory.Write64(0x1FF82084, saveArchive, &faultAddress),
            "cannot rewrite FS:CloseArchive request");
    state.r[0] = fsSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                memory.Read32(0x1FF82084, &response) &&
                response == 0xC8804465,
            "FS:CloseArchive did not reject a stale archive handle");

    Require(memory.Fill(0x1FF82140, 1, 0) &&
                memory.Fill(0x1FF82150, 12, 0) &&
                memory.Write32(0x1FF82080, 0x08030204) &&
                memory.Write32(0x1FF82084, 0) &&
                memory.Write32(0x1FF82088, 3) &&
                memory.Write32(0x1FF8208C, 1) &&
                memory.Write32(0x1FF82090, 1) &&
                memory.Write32(0x1FF82094, 2) &&
                memory.Write32(0x1FF82098, 12) &&
                memory.Write32(0x1FF8209C, 1) &&
                memory.Write32(0x1FF820A0, 0) &&
                memory.Write32(0x1FF820A4, 0x4802) &&
                memory.Write32(0x1FF820A8, 0x1FF82140) &&
                memory.Write32(0x1FF820AC, 0x30002) &&
                memory.Write32(0x1FF820B0, 0x1FF82150),
            "cannot write FS:OpenFileDirectly request");
    state.r[0] = fsSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                NativeA32HostAction::Resume,
            "FS:OpenFileDirectly did not open the original RomFS image");
    uint32_t romFsFile = 0;
    Require(memory.Read32(0x1FF82080, &response) &&
                response == 0x08030042 &&
                memory.Read32(0x1FF8208C, &romFsFile) && romFsFile != 0,
            "FS:OpenFileDirectly response is wrong");

    constexpr uint32_t readAddress = 0x1FF82200;
    constexpr uint32_t readOffset = 3;
    constexpr uint32_t readSize = 7;
    Require(memory.Fill(readAddress, readSize, 0xFF) &&
                memory.Write32(0x1FF82080, 0x080200C2) &&
                memory.Write32(0x1FF82084, readOffset) &&
                memory.Write32(0x1FF82088, 0) &&
                memory.Write32(0x1FF8208C, readSize) &&
                memory.Write32(0x1FF82090, (readSize << 4U) | 0xCU) &&
                memory.Write32(0x1FF82094, readAddress),
            "cannot write File:Read request");
    state.r[0] = romFsFile;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                NativeA32HostAction::Resume,
            "File:Read did not resume");
    std::array<uint8_t, readSize> readBytes{};
    Require(memory.Read32(0x1FF82080, &response) &&
                response == 0x08020082 &&
                memory.ReadBytes(readAddress, readBytes) &&
                std::equal(readBytes.begin(), readBytes.end(),
                           romFsBytes.begin() + config.RomFsImageOffset +
                               readOffset),
            "File:Read did not copy the requested RomFS range");

    auto romFsProfile = host.RuntimeProfile();
    Require(romFsProfile.RomFsStreamOpenCalls == 1U &&
                romFsProfile.RomFsReadCalls == 1U &&
                romFsProfile.RomFsReadBytes == readSize,
            "the first RomFS read did not establish one persistent stream");

    constexpr uint32_t secondReadOffset = 1;
    constexpr uint32_t secondReadSize = 3;
    Require(memory.Fill(readAddress, secondReadSize, 0xFF) &&
                memory.Write32(0x1FF82080, 0x080200C2) &&
                memory.Write64(0x1FF82084, secondReadOffset,
                               &faultAddress) &&
                memory.Write32(0x1FF8208C, secondReadSize) &&
                memory.Write32(0x1FF82090,
                               (secondReadSize << 4U) | 0xCU) &&
                memory.Write32(0x1FF82094, readAddress),
            "cannot write the second File:Read request");
    state.r[0] = romFsFile;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                NativeA32HostAction::Resume,
            "the second File:Read did not resume");
    std::array<uint8_t, secondReadSize> secondReadBytes{};
    Require(memory.Read32(0x1FF82088, &response) &&
                response == secondReadSize &&
                memory.ReadBytes(readAddress, secondReadBytes) &&
                std::equal(
                    secondReadBytes.begin(), secondReadBytes.end(),
                    romFsBytes.begin() + config.RomFsImageOffset +
                        secondReadOffset),
            "the persistent RomFS stream did not honor a backward offset");
    romFsProfile = host.RuntimeProfile();
    Require(romFsProfile.RomFsStreamOpenCalls == 1U &&
                romFsProfile.RomFsReadCalls == 2U &&
                romFsProfile.RomFsReadBytes ==
                    readSize + secondReadSize,
            "the second RomFS read reopened its host stream");

    Require(memory.Write32(0x1FF82080, 0x080200C2) &&
                memory.Write64(0x1FF82084, config.RomFsImageSize,
                               &faultAddress) &&
                memory.Write32(0x1FF8208C, 0) &&
                memory.Write32(0x1FF82090, 0xCU) &&
                memory.Write32(0x1FF82094, readAddress),
            "cannot write the zero-length EOF File:Read request");
    state.r[0] = romFsFile;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                memory.Read32(0x1FF82088, &response) && response == 0,
            "zero-length RomFS read at EOF changed behavior");
    romFsProfile = host.RuntimeProfile();
    Require(romFsProfile.RomFsStreamOpenCalls == 1U &&
                romFsProfile.RomFsReadCalls == 3U &&
                romFsProfile.RomFsReadBytes ==
                    readSize + secondReadSize,
            "zero-length EOF read did not reuse the persistent stream");

    constexpr std::array<uint8_t, 2> invalidReadSentinel{0xA5, 0x5A};
    Require(memory.WriteBytes(readAddress, invalidReadSentinel) &&
                memory.Write32(0x1FF82080, 0x080200C2) &&
                memory.Write64(0x1FF82084,
                               config.RomFsImageSize - 1U,
                               &faultAddress) &&
                memory.Write32(0x1FF8208C,
                               invalidReadSentinel.size()) &&
                memory.Write32(
                    0x1FF82090,
                    (invalidReadSentinel.size() << 4U) | 0xCU) &&
                memory.Write32(0x1FF82094, readAddress),
            "cannot write the over-EOF File:Read request");
    state.r[0] = romFsFile;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                NativeA32HostAction::Resume,
            "over-EOF RomFS read did not return a short read");
    std::array<uint8_t, invalidReadSentinel.size()> invalidReadBytes{};
    Require(memory.ReadBytes(readAddress, invalidReadBytes) &&
                invalidReadBytes[0] == romFsBytes.back() && invalidReadBytes[1] == invalidReadSentinel[1] &&
                memory.Read32(0x1FF82088, &response) && response == 1U &&
                host.RuntimeProfile().RomFsReadCalls == 4U,
            "short read changed bytes past EOF");

    Require(memory.Write32(0x1FF82080, 0x08080000),
            "cannot write File:Close request");
    state.r[0] = romFsFile;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                NativeA32HostAction::Resume &&
                memory.Read32(0x1FF82080, &response) &&
                response == 0x08080040,
            "File:Close response is wrong");

    constexpr uint32_t appletAttributes = 0x300U;
    Require(memory.Write32(0x1FF82080, 0x00010040) &&
                memory.Write32(0x1FF82084, appletAttributes),
            "cannot write APT:GetLockHandle request");
    state.r[0] = aptSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                NativeA32HostAction::Resume,
            "APT:GetLockHandle did not resume");
    uint32_t correctedAttributes = 0;
    uint32_t appletState = 1;
    uint32_t descriptor = 0;
    uint32_t lockHandle = 0;
    Require(memory.Read32(0x1FF82080, &response) &&
                response == 0x000100C2 &&
                memory.Read32(0x1FF82088, &correctedAttributes) &&
                correctedAttributes == appletAttributes &&
                memory.Read32(0x1FF8208C, &appletState) &&
                appletState == 0 &&
                memory.Read32(0x1FF82090, &descriptor) &&
                descriptor == 0 &&
                memory.Read32(0x1FF82094, &lockHandle) && lockHandle != 0,
            "APT:GetLockHandle response payload is wrong");
    Require(memory.Write32(0x1FF82100, lockHandle),
            "cannot write APT lock wait array");
    state.r[1] = 0x1FF82100;
    state.r[2] = 1;
    state.r[3] = 0;
    state.r[4] = 0xFFFFFFFFU;
    state.r[5] = 0x7FFFFFFFU;
    Require(host.HandleSvc(0x25, state, memory, context).Action ==
                NativeA32HostAction::Resume &&
                state.r[0] == 0 && state.r[1] == 0,
            "WaitSynchronizationN did not acquire the available APT lock");
    {
        // Event names are built without streams; they must keep the text
        // the stream formatting produced.
        std::ostringstream expected;
        expected << "WaitSynchronizationN:any:0x" << std::hex << std::setw(8)
                 << std::setfill('0') << lockHandle << "=";
        const std::string& name = host.SvcEvents().back().Name;
        Require(name.rfind(expected.str(), 0) == 0 &&
                    name.size() > expected.str().size(),
                "WaitSynchronizationN event name changed format");
        const std::array<uint32_t, 4> saved{
            state.r[0], state.r[1], state.r[2], state.r[3]};
        state.r[0] = 0xFFFFFFF0U;  // no such arbiter: report a host fault
        state.r[1] = 0x0000ABCDU;
        state.r[2] = 3U;
        state.r[3] = static_cast<uint32_t>(-5);
        Require(host.HandleSvc(0x22, state, memory, context).Action ==
                    NativeA32HostAction::Fault,
                "ArbitrateAddress on an invalid handle did not fault");
        std::ostringstream arbitrate;
        arbitrate << "ArbitrateAddress:address=0x" << std::hex << std::setw(8)
                  << std::setfill('0') << 0x0000ABCDU << ":type=" << std::dec
                  << 3U << ":value=" << -5;
        Require(host.SvcEvents().back().Name == arbitrate.str(),
                "ArbitrateAddress event name changed format");
        std::copy(saved.begin(), saved.end(), state.r.begin());
    }

    Require(memory.Write32(0x1FF82080, 0x00020080) &&
                memory.Write32(0x1FF82084, 0x300) &&
                memory.Write32(0x1FF82088, 0),
            "cannot write APT:Initialize request");
    state.r[0] = aptSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                NativeA32HostAction::Resume,
            "APT:Initialize did not resume");
    uint32_t notificationEvent = 0;
    uint32_t parameterEvent = 0;
    Require(memory.Read32(0x1FF82080, &response) &&
                response == 0x00020043 &&
                memory.Read32(0x1FF82088, &descriptor) &&
                descriptor == 0x04000000 &&
                memory.Read32(0x1FF8208C, &notificationEvent) &&
                memory.Read32(0x1FF82090, &parameterEvent) &&
                notificationEvent != 0 && parameterEvent != 0,
            "APT:Initialize event response is wrong");
    Require(memory.Write32(0x1FF82100, notificationEvent) &&
                memory.Write32(0x1FF82104, parameterEvent),
            "cannot write APT event wait array");
    state.r[1] = 0x1FF82100;
    state.r[2] = 2;
    state.r[3] = 0;
    Require(host.HandleSvc(0x25, state, memory, context).Action ==
                NativeA32HostAction::Resume && state.r[1] == 1,
            "APT wakeup parameter event was not initially signaled");

    constexpr uint32_t transition = 0x62;
    Require(memory.Write32(0x1FF82140, transition) &&
                memory.Fill(0x1FF82150, 1, 0xFF) &&
                memory.Write32(0x1FF82000 + 0x180, 0x4002) &&
                memory.Write32(0x1FF82000 + 0x184, 0x1FF82150) &&
                memory.Write32(0x1FF82080, 0x004B00C2) &&
                memory.Write32(0x1FF82084, 7) &&
                memory.Write32(0x1FF82088, 4) &&
                memory.Write32(0x1FF8208C, 1) &&
                memory.Write32(0x1FF82090, 0x10402) &&
                memory.Write32(0x1FF82094, 0x1FF82140),
            "cannot write APT:UnlockTransition request");
    state.r[0] = aptSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                NativeA32HostAction::Resume,
            "APT:UnlockTransition did not resume");
    uint8_t transitionOutput = 0xFF;
    Require(memory.Read32(0x1FF82080, &response) &&
                response == 0x004B0082 &&
                memory.Read8(0x1FF82150, &transitionOutput) &&
                transitionOutput == 0,
            "APT:UnlockTransition response is wrong");

    Require(memory.Write32(0x1FF82080, 0x00050100) &&
                memory.Write32(0x1FF82084, 0x3A707367) &&
                memory.Write32(0x1FF82088, 0x7570473A) &&
                memory.Write32(0x1FF8208C, 8),
            "cannot write srv:GetServiceHandle gsp::Gpu request");
    state.r[0] = srvSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                NativeA32HostAction::Resume,
            "srv:GetServiceHandle did not open gsp::Gpu");
    uint32_t gspSession = 0;
    Require(memory.Read32(0x1FF8208C, &gspSession) && gspSession != 0,
            "srv:GetServiceHandle gsp::Gpu response has no session");

    Require(memory.Write32(0x1FF82080, 0x00080082) &&
                memory.Write32(0x1FF82084, 0x14000100) &&
                memory.Write32(0x1FF82088, 0x100) &&
                memory.Write32(0x1FF8208C, 0) &&
                memory.Write32(0x1FF82090, 0xFFFF8001),
            "cannot write GSP:FlushDataCache request");
    state.r[0] = gspSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                memory.Read32(0x1FF82080, &response) &&
                response == 0x00080040,
            "GSP:FlushDataCache rejected coherent guest memory");

    Require(memory.Write32(0x1FF82080, 0x001E0080) &&
                memory.Write32(0x1FF82084, 0x19) &&
                memory.Write32(0x1FF82088, 0x1A),
            "cannot write GSP:SetInternalPriorities request");
    state.r[0] = gspSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                memory.Read32(0x1FF82080, &response) &&
                response == 0x001E0040 && host.GspPriority() == 0x19 &&
                host.GspPriorityWithRights() == 0x1A,
            "GSP:SetInternalPriorities did not preserve the native values");

    Require(memory.Write32(0x1FF82080, 0x00050200) &&
                memory.Write32(0x1FF82084, 0) &&
                memory.Write32(0x1FF82088, 0) &&
                memory.Write32(0x1FF8208C, 0x14001000) &&
                memory.Write32(0x1FF82090, 0x14001000) &&
                memory.Write32(0x1FF82094, 960) &&
                memory.Write32(0x1FF82098, 0) &&
                memory.Write32(0x1FF8209C, 0) &&
                memory.Write32(0x1FF820A0, 0),
            "cannot write GSP:SetBufferSwap request");
    state.r[0] = gspSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                memory.Read32(0x1FF82080, &response) &&
                response == 0x00050040 &&
                host.TopFramebuffer().has_value() &&
                host.TopFramebuffer()->AddressLeft == 0x14001000 &&
                host.TopFramebuffer()->Stride == 960,
            "GSP:SetBufferSwap did not select the native top framebuffer");
    Require(memory.Write32(0x1FF82080, 0x00050200) &&
                memory.Write32(0x1FF82084, 1) &&
                memory.Write32(0x1FF82088, 0) &&
                memory.Write32(0x1FF8208C, 0x14003000) &&
                memory.Write32(0x1FF82090, 0x14003000) &&
                memory.Write32(0x1FF82094, 720) &&
                memory.Write32(0x1FF82098, 0) &&
                memory.Write32(0x1FF8209C, 0) &&
                memory.Write32(0x1FF820A0, 0),
            "cannot write bottom GSP:SetBufferSwap request");
    state.r[0] = gspSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                host.BottomFramebuffer().has_value() &&
                host.BottomFramebuffer()->AddressLeft == 0x14003000 &&
                host.BottomFramebuffer()->Stride == 720,
            "GSP:SetBufferSwap did not preserve the native bottom framebuffer");

    Require(memory.Write32(0x1FF82080, 0x00050100) &&
                memory.Write32(0x1FF82084, 0x3A707364) &&
                memory.Write32(0x1FF82088, 0x5053443A) &&
                memory.Write32(0x1FF8208C, 8),
            "cannot write srv:GetServiceHandle dsp::DSP request");
    state.r[0] = srvSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                NativeA32HostAction::Resume,
            "srv:GetServiceHandle did not open dsp::DSP");
    uint32_t dspSession = 0;
    Require(memory.Read32(0x1FF8208C, &dspSession) && dspSession != 0,
            "srv:GetServiceHandle dsp::DSP response has no session");

    Require(memory.Write32(0x1FF82080, 0x00050100) &&
                memory.Write32(0x1FF82084, 0x3A723279) &&
                memory.Write32(0x1FF82088, 0x75) &&
                memory.Write32(0x1FF8208C, 5),
            "cannot write Y2R service request");
    state.r[0] = srvSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action == NativeA32HostAction::Resume,
            "Y2R service missing");
    uint32_t y2rSession = 0;
    Require(memory.Read32(0x1FF8208C, &y2rSession) && y2rSession,
            "Y2R handle missing");
    const auto y2rRequest = [&](uint16_t command, std::initializer_list<uint32_t> args) {
        Require(memory.Write32(0x1FF82080, uint32_t(command) << 16 | uint32_t(args.size()) << 6),
                "Y2R request header");
        uint32_t address = 0x1FF82084;
        for (const auto argument : args) {
            Require(memory.Write32(address, argument), "Y2R request argument");
            address += 4;
        }
        state.r[0] = y2rSession;
        Require(host.HandleSvc(0x32, state, memory, context).Action == NativeA32HostAction::Resume,
                "Y2R request did not resume");
        uint32_t result = ~0U;
        Require(memory.Read32(0x1FF82084, &result), "Y2R result absent");
        return result;
    };
    Require(y2rRequest(0x2B, {}) == 0, "Y2R initialize failed");
    Require(y2rRequest(0x0F, {}) == 0, "Y2R completion event failed");
    uint32_t y2rEvent = 0;
    Require(memory.Read32(0x1FF82080, &response) && response == 0x000F0042 &&
                memory.Read32(0x1FF8208C, &y2rEvent) && y2rEvent,
            "Y2R event IPC shape incorrect");
    Require(y2rRequest(0x1A, {8}) == 0 && y2rRequest(0x1C, {8}) == 0 &&
                y2rRequest(0x20, {0}) == 0 && y2rRequest(0x22, {255}) == 0,
            "Y2R configuration failed");
    std::array<uint8_t, 128> neutralYuv;
    neutralYuv.fill(128);
    Require(memory.WriteBytes(0x14001000, neutralYuv), "Y2R source memory");
    Require(y2rRequest(0x10, {0x14001000, 64, 8, 0}) == 0 &&
                y2rRequest(0x11, {0x14001040, 32, 4, 0}) == 0 &&
                y2rRequest(0x12, {0x14001060, 32, 4, 0}) == 0 &&
                y2rRequest(0x18, {0x14001100, 256, 32, 0}) == 0 &&
                y2rRequest(0x26, {}) == 0,
            "Y2R native conversion failed");
    uint32_t pixel = 0;
    Require(memory.Read32(0x14001100, &pixel) && pixel == 0x808180FF,
            "Y2R fixed-point conversion or output byte order incorrect");
    Require(memory.Write32(0x14001200, y2rEvent), "Y2R wait handle");
    state.r[1] = 0x14001200; state.r[2] = 1; state.r[3] = 0;
    Require(host.HandleSvc(0x25, state, memory, context).Action == NativeA32HostAction::Resume &&
                state.r[0] == 0,
            "Y2R completed transfer did not signal event");
    Require(y2rRequest(0x1A, {7}) != 0, "Y2R accepted malformed width");
    Require(y2rRequest(0x18, {0, 256, 32, 0}) == 0 && y2rRequest(0x26, {}) != 0,
            "Y2R silently completed invalid output DMA");

    constexpr uint32_t dspComponentAddress = 0x14000100;
    constexpr std::array<uint8_t, 12> dspComponent{
        0x44, 0x53, 0x50, 0x31, 0x02, 0x03,
        0x05, 0x07, 0x0B, 0x0D, 0x11, 0x13,
    };
    constexpr uint16_t dspProgramMask = 0x00FF;
    constexpr uint16_t dspDataMask = 0x0003;
    constexpr uint32_t dspComponentDescriptor =
        (static_cast<uint32_t>(dspComponent.size()) << 4U) | 0xAU;
    Require(memory.WriteBytes(dspComponentAddress, dspComponent) &&
                memory.Write32(0x1FF82080, 0x001100C2) &&
                memory.Write32(0x1FF82084,
                               static_cast<uint32_t>(dspComponent.size())) &&
                memory.Write32(0x1FF82088, dspProgramMask) &&
                memory.Write32(0x1FF8208C, dspDataMask) &&
                memory.Write32(0x1FF82090, dspComponentDescriptor) &&
                memory.Write32(0x1FF82094, dspComponentAddress),
            "cannot write DSP:LoadComponent request");
    state.r[0] = dspSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                NativeA32HostAction::Resume,
            "DSP:LoadComponent did not resume");
    uint32_t dspLoaded = 0;
    uint32_t returnedDspDescriptor = 0;
    uint32_t returnedDspAddress = 0;
    Require(memory.Read32(0x1FF82080, &response) &&
                response == 0x00110082 &&
                memory.Read32(0x1FF82088, &dspLoaded) && dspLoaded == 1 &&
                memory.Read32(0x1FF8208C, &returnedDspDescriptor) &&
                returnedDspDescriptor == dspComponentDescriptor &&
                memory.Read32(0x1FF82090, &returnedDspAddress) &&
                returnedDspAddress == dspComponentAddress &&
                host.DspComponent() == std::vector<uint8_t>(
                                           dspComponent.begin(),
                                           dspComponent.end()) &&
                host.DspProgramMask() == dspProgramMask &&
                host.DspDataMask() == dspDataMask,
            "DSP:LoadComponent did not preserve or return its native payload");

    state.r[1] = 1;
    Require(host.HandleSvc(0x17, state, memory, context).Action ==
                NativeA32HostAction::Resume,
            "cannot create DSP pipe interrupt event");
    const uint32_t dspPipeEvent = state.r[1];
    Require(memory.Write32(0x1FF82080, 0x00150082) &&
                memory.Write32(0x1FF82084, 2) &&
                memory.Write32(0x1FF82088, 2) &&
                memory.Write32(0x1FF8208C, 0) &&
                memory.Write32(0x1FF82090, dspPipeEvent),
            "cannot write DSP:RegisterInterruptEvents request");
    state.r[0] = dspSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                memory.Read32(0x1FF82080, &response) &&
                response == 0x00150040,
            "DSP:RegisterInterruptEvents did not preserve the guest event");
    Require(host.SignalDspInterrupt(2, 2),
            "registered DSP pipe interrupt could not be signaled");
    Require(memory.Write32(0x1FF82100, dspPipeEvent),
            "cannot write DSP event wait array");
    state.r[1] = 0x1FF82100;
    state.r[2] = 1;
    state.r[3] = 0;
    Require(host.HandleSvc(0x25, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                state.r[0] == 0 && state.r[1] == 0,
            "signaled DSP pipe event was not observable by the guest");

    Require(memory.Write32(0x1FF82080, 0x00160000),
            "cannot write DSP:GetSemaphoreEventHandle request");
    state.r[0] = dspSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                memory.Read32(0x1FF82080, &response) &&
                response == 0x00160042 &&
                memory.Read32(0x1FF82088, &descriptor) && descriptor == 0,
            "DSP:GetSemaphoreEventHandle response is wrong");
    uint32_t dspSemaphoreEvent = 0;
    Require(memory.Read32(0x1FF8208C, &dspSemaphoreEvent) &&
                dspSemaphoreEvent != 0,
            "DSP:GetSemaphoreEventHandle returned no event");

    constexpr uint16_t dspSemaphoreMask = 0x2000;
    Require(memory.Write32(0x1FF82080, 0x00170040) &&
                memory.Write32(0x1FF82084, dspSemaphoreMask),
            "cannot write DSP:SetSemaphoreMask request");
    state.r[0] = dspSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                memory.Read32(0x1FF82080, &response) &&
                response == 0x00170040 &&
                host.DspSemaphoreMask() == dspSemaphoreMask,
            "DSP:SetSemaphoreMask did not preserve the native mask");

    constexpr uint32_t dspPipeCommandAddress = 0x1FF82180;
    constexpr std::array<uint8_t, 4> dspInitializeCommand{0, 0, 0xA5,
                                                         0x5A};
    Require(memory.WriteBytes(dspPipeCommandAddress, dspInitializeCommand) &&
                memory.Write32(0x1FF82080, 0x000D0082) &&
                memory.Write32(0x1FF82084, 2) &&
                memory.Write32(0x1FF82088, 4) &&
                memory.Write32(0x1FF8208C, 0x00010402) &&
                memory.Write32(0x1FF82090, dspPipeCommandAddress),
            "cannot write DSP:WriteProcessPipe request");
    state.r[0] = dspSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                memory.Read32(0x1FF82080, &response) &&
                response == 0x000D0040,
            "DSP:WriteProcessPipe did not initialize the audio pipe");
    Require(memory.Write32(0x1FF82100, dspPipeEvent),
            "cannot rewrite DSP pipe event wait array");
    state.r[1] = 0x1FF82100;
    state.r[2] = 1;
    state.r[3] = 0;
    Require(host.HandleSvc(0x25, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                state.r[0] == 0 && state.r[1] == 0,
            "DSP audio initialization did not signal its pipe event");

    host.AdvanceSystemTicks(
        NativeA32CtrHostServices::DspAudioFrameTicks - 1U);
    Require(host.TakePendingDspAudioFrames() == 0U,
            "DSP audio tick fired before one native frame elapsed");
    host.AdvanceSystemTicks(1U);
    Require(host.TakePendingDspAudioFrames() == 1U &&
                host.SignalDspAudioFrame(),
            "DSP audio tick did not follow the native frame cadence");
    Require(memory.Write32(0x1FF82100, dspPipeEvent),
            "cannot rewrite periodic DSP event wait array");
    state.r[1] = 0x1FF82100;
    state.r[2] = 1;
    state.r[3] = 0;
    // config/kernel_retake: the waits that list the audio interrupt (this
    // event resets manually, as the game's: a wait leaves it signaled), and
    // no waiter on it.
    {
        oot3d::recomp::a32::GuestState single = state;
        single.r[0] = dspPipeEvent;
        oot3d::recomp::a32::GuestState tooMany = state;
        tooMany.r[2] = 0x41U;
        oot3d::recomp::a32::GuestState other = state;
        other.r[0] = dspSemaphoreEvent;
        Require(host.SvcWaitsOnDspAudioInterrupt(0x25U, state, memory) &&
                    host.SvcWaitsOnDspAudioInterrupt(0x24U, single, memory) &&
                    !host.SvcWaitsOnDspAudioInterrupt(0x24U, other, memory) &&
                    !host.SvcWaitsOnDspAudioInterrupt(0x25U, tooMany, memory) &&
                    !host.SvcWaitsOnDspAudioInterrupt(0x18U, single, memory) &&
                    !host.DspAudioInterruptHasCore0Waiter(),
                "the DSP audio interrupt's waits were not told apart");
    }
    Require(host.HandleSvc(0x25, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                state.r[0] == 0 && state.r[1] == 0,
            "periodic DSP audio frame did not signal its pipe event");
    host.AdvanceSystemTicks(
        NativeA32CtrHostServices::DspAudioFrameTicks * 2U);
    Require(host.TakePendingDspAudioFrames() == 2U,
            "DSP audio cadence did not preserve multiple elapsed frames");

    constexpr uint16_t dspSemaphoreValue = 0x4000;
    Require(memory.Write32(0x1FF82080, 0x00070040) &&
                memory.Write32(0x1FF82084, dspSemaphoreValue),
            "cannot write DSP:SetSemaphore request");
    state.r[0] = dspSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                memory.Read32(0x1FF82080, &response) &&
                response == 0x00070040 &&
                host.DspSemaphoreValue() == dspSemaphoreValue,
            "DSP:SetSemaphore did not preserve the native value");

    constexpr uint32_t dspPipeReadAddress = 0x1FF82200;
    constexpr uint32_t dspPipeReadCapacity = 0x40;
    Require(memory.Fill(dspPipeReadAddress, dspPipeReadCapacity, 0xFF) &&
                memory.Write32(0x1FF82000 + 0x180,
                               (dspPipeReadCapacity << 14U) | 2U) &&
                memory.Write32(0x1FF82000 + 0x184,
                               dspPipeReadAddress) &&
                memory.Write32(0x1FF82080, 0x001000C0) &&
                memory.Write32(0x1FF82084, 2) &&
                memory.Write32(0x1FF82088, 0) &&
                memory.Write32(0x1FF8208C, 2),
            "cannot write DSP:ReadPipeIfPossible request");
    state.r[0] = dspSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                NativeA32HostAction::Resume,
            "DSP:ReadPipeIfPossible did not resume");
    uint32_t dspPipeReturnedSize = 0;
    uint16_t dspStructCount = 0;
    Require(memory.Read32(0x1FF82080, &response) &&
                response == 0x00100082 &&
                memory.Read32(0x1FF82088, &dspPipeReturnedSize) &&
                dspPipeReturnedSize == 2 &&
                memory.Read16(dspPipeReadAddress, &dspStructCount) &&
                dspStructCount == 15,
            "DSP audio pipe did not return its native structure count");

    Require(memory.MapRegion({"dsp_ram", 0x1FF00000, 0x80000, true,
                              false, {}},
                             &error),
            error);
    Require(memory.Write32(0x1FF82080, 0x000C0040) &&
                memory.Write32(0x1FF82084, 0xBFFF),
            "cannot write DSP:ConvertProcessAddress request");
    state.r[0] = dspSession;
    uint32_t convertedDspAddress = 0;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                memory.Read32(0x1FF82080, &response) &&
                response == 0x000C0080 &&
                memory.Read32(0x1FF82088, &convertedDspAddress) &&
                convertedDspAddress == 0x1FF57FFE,
            "DSP:ConvertProcessAddress did not expose the DSP RAM view");

    // The ending's audio flushes and invalidates DSP-shared buffers (#104).
    for (const auto [request, expected] :
         {std::pair{0x00130082U, 0x00130040U}, std::pair{0x00140082U, 0x00140040U}}) {
        Require(memory.Write32(0x1FF82080, request) &&
                    memory.Write32(0x1FF82084, 0x1FF00000) &&
                    memory.Write32(0x1FF82088, 0x100) &&
                    memory.Write32(0x1FF8208C, 0) &&
                    memory.Write32(0x1FF82090, 0xFFFF8001),
                "cannot write DSP data-cache request");
        state.r[0] = dspSession;
        Require(host.HandleSvc(0x32, state, memory, context).Action ==
                        NativeA32HostAction::Resume &&
                    memory.Read32(0x1FF82080, &response) &&
                    response == expected &&
                    memory.Read32(0x1FF82084, &response) && response == 0,
                "DSP data-cache request rejected coherent guest memory");
    }

    Require(memory.Write32(0x1FF82080, 0x001F0000),
            "cannot write DSP:GetHeadphoneStatus request");
    state.r[0] = dspSession;
    uint32_t headphonesConnected = 0;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                memory.Read32(0x1FF82080, &response) &&
                response == 0x001F0080 &&
                memory.Read32(0x1FF82088, &headphonesConnected) &&
                headphonesConnected == 1,
            "DSP:GetHeadphoneStatus ignored the host audio route");

    Require(memory.Write32(0x1FF82080, 0x00050100) &&
                memory.Write32(0x1FF82084, 0x3A676663) &&
                memory.Write32(0x1FF82088, 0x00000075) &&
                memory.Write32(0x1FF8208C, 5),
            "cannot write srv:GetServiceHandle cfg:u request");
    state.r[0] = srvSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                NativeA32HostAction::Resume,
            "srv:GetServiceHandle did not open cfg:u");
    uint32_t cfgSession = 0;
    Require(memory.Read32(0x1FF8208C, &cfgSession) && cfgSession != 0,
            "srv:GetServiceHandle cfg:u response has no session");
    constexpr uint32_t configValueAddress = 0x1FF821F0;
    const auto readConfigByte = [&](uint32_t blockId, uint8_t expected) {
        Require(memory.Write8(configValueAddress, 0xFF) &&
                    memory.Write32(0x1FF82080, 0x00010082) &&
                    memory.Write32(0x1FF82084, 1) &&
                    memory.Write32(0x1FF82088, blockId) &&
                    memory.Write32(0x1FF8208C, 0x1C) &&
                    memory.Write32(0x1FF82090, configValueAddress),
                "cannot write CFG:GetConfig request");
        state.r[0] = cfgSession;
        Require(host.HandleSvc(0x32, state, memory, context).Action ==
                    NativeA32HostAction::Resume,
                "CFG:GetConfig did not resume");
        uint8_t value = 0xFF;
        Require(memory.Read32(0x1FF82080, &response) &&
                    response == 0x00010042 &&
                    memory.Read8(configValueAddress, &value) &&
                    value == expected,
                "CFG:GetConfig returned the wrong host setting");
    };
    readConfigByte(0x00070001, 2);
    readConfigByte(0x000A0002, 4);
    Require(memory.Write32(0x1FF82080, 0x00020000),
            "cannot write CFG:GetRegion request");
    state.r[0] = cfgSession;
    uint32_t systemRegion = 0;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                memory.Read32(0x1FF82080, &response) &&
                response == 0x00020080 &&
                memory.Read32(0x1FF82088, &systemRegion) &&
                systemRegion == 2,
            "CFG:GetRegion ignored the host region");
    Require(memory.Write32(0x1FF82080, 0x00010082) &&
                memory.Write32(0x1FF82084, 32) &&
                memory.Write32(0x1FF82088, 0x00050005) &&
                memory.Write32(0x1FF8208C, 0x20C) &&
                memory.Write32(0x1FF82090, 0x14000500),
            "cannot write CFG stereo camera request");
    state.r[0] = cfgSession;
    uint32_t stereoCameraFirstValue = 0;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                memory.Read32(0x14000500, &stereoCameraFirstValue) &&
                stereoCameraFirstValue == 0x42780000,
            "CFG stereo camera calibration was not exposed structurally");

    Require(memory.Write32(0x1FF82080, 0x00050100) &&
                memory.Write32(0x1FF82084, 0x3A646968) &&
                memory.Write32(0x1FF82088, 0x52455355) &&
                memory.Write32(0x1FF8208C, 8),
            "cannot write srv:GetServiceHandle hid:USER request");
    state.r[0] = srvSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                NativeA32HostAction::Resume,
            "srv:GetServiceHandle did not open hid:USER");
    uint32_t hidSession = 0;
    Require(memory.Read32(0x1FF8208C, &hidSession) && hidSession != 0,
            "srv:GetServiceHandle hid:USER response has no session");
    Require(memory.Write32(0x1FF82080, 0x000A0000),
            "cannot write HID:GetIPCHandles request");
    state.r[0] = hidSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                NativeA32HostAction::Resume,
            "HID:GetIPCHandles did not resume");
    std::array<uint32_t, 6> hidHandles{};
    bool hidHandlesReadable = true;
    for (size_t index = 0; index < hidHandles.size(); ++index) {
        hidHandlesReadable =
            hidHandlesReadable &&
            memory.Read32(0x1FF8208C +
                              static_cast<uint32_t>(index * 4U),
                          &hidHandles[index]);
    }
    Require(memory.Read32(0x1FF82080, &response) &&
                response == 0x000A0047 &&
                memory.Read32(0x1FF82088, &descriptor) &&
                descriptor == 0x14000000 && hidHandlesReadable &&
                std::all_of(hidHandles.begin(), hidHandles.end(),
                            [](uint32_t handle) { return handle != 0; }),
            "HID:GetIPCHandles response is incomplete");
    state.r[0] = hidHandles[0];
    state.r[1] = 0x10002000;
    state.r[2] = 1;
    state.r[3] = 0x10000000;
    Require(host.HandleSvc(0x1F, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                memory.IsMapped(0x10002000, 0x1000),
            "HID shared memory could not be mapped read-only");

    NativeA32HidState hidState;
    hidState.Buttons =
        NativeA32HidButtonMask(NativeA32HidButton::A) |
        NativeA32HidButtonMask(NativeA32HidButton::Start);
    hidState.CirclePadX = 154;
    hidState.CirclePadY = -154;
    hidState.TouchX = 319;
    hidState.TouchY = 239;
    hidState.TouchPressed = true;
    const auto firstHidUpdate =
        host.AdvanceHidToCurrentTick(memory, hidState);
    constexpr uint32_t hidExpectedButtons =
        (1U << 0U) | (1U << 3U) | (1U << 28U) | (1U << 31U);
    uint32_t hidPadIndex = UINT32_MAX;
    uint32_t hidCurrentButtons = 0;
    uint32_t hidEntryButtons = 0;
    uint32_t hidAdditions = 0;
    uint32_t hidRemovals = UINT32_MAX;
    uint16_t hidCircleX = 0;
    uint16_t hidCircleY = 0;
    uint32_t hidTouchPressed = 0;
    Require(firstHidUpdate.Status ==
                    NativeA32CtrHidUpdateStatus::Updated &&
                firstHidUpdate.SamplesWritten == 1U &&
                firstHidUpdate.EventsSignaled &&
                memory.Read32(0x10002010, &hidPadIndex) &&
                hidPadIndex == 0U &&
                memory.Read32(0x1000201C, &hidCurrentButtons) &&
                hidCurrentButtons == hidExpectedButtons &&
                memory.Read32(0x10002028, &hidEntryButtons) &&
                hidEntryButtons == hidExpectedButtons &&
                memory.Read32(0x1000202C, &hidAdditions) &&
                hidAdditions == hidExpectedButtons &&
                memory.Read32(0x10002030, &hidRemovals) &&
                hidRemovals == 0U &&
                memory.Read16(0x10002034, &hidCircleX) &&
                static_cast<int16_t>(hidCircleX) == 154 &&
                memory.Read16(0x10002036, &hidCircleY) &&
                static_cast<int16_t>(hidCircleY) == -154 &&
                memory.Read32(0x100020CC, &hidTouchPressed) &&
                hidTouchPressed == 1U,
            "native HID producer did not populate the CTR pad/touch ABI");
    Require(memory.Write32(0x1FF82100, hidHandles[1]),
            "cannot write HID pad event wait array");
    state.r[1] = 0x1FF82100;
    state.r[2] = 1;
    state.r[3] = 0;
    Require(host.HandleSvc(0x25, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                state.r[0] == 0U && state.r[1] == 0U,
            "native HID producer did not signal the guest pad event");

    host.AdvanceSystemTicks(NativeA32CtrHostServices::HidPadUpdateTicks);
    const auto secondHidUpdate =
        host.AdvanceHidToCurrentTick(memory, NativeA32HidState{});
    Require(secondHidUpdate.Status ==
                    NativeA32CtrHidUpdateStatus::Updated &&
                secondHidUpdate.SamplesWritten == 1U &&
                memory.Read32(0x10002010, &hidPadIndex) &&
                hidPadIndex == 1U &&
                memory.Read32(0x10002038, &hidEntryButtons) &&
                hidEntryButtons == 0U &&
                memory.Read32(0x1000203C, &hidAdditions) &&
                hidAdditions == 0U &&
                memory.Read32(0x10002040, &hidRemovals) &&
                hidRemovals == hidExpectedButtons,
            "native HID producer did not preserve cadence or release edges");
    const auto hidProfile = host.HidRuntimeProfile();
    Require(hidProfile.SharedMemoryMapped &&
                hidProfile.SamplesWritten == 2U &&
                hidProfile.EventBatchesSignaled == 2U &&
                hidProfile.LastRemovals == hidExpectedButtons &&
                hidProfile.NextPadIndex == 2U,
            "native HID runtime profile lost producer state");

    Require(memory.Write32(0x1FF82080, 0x00110000),
            "cannot write HID:EnableAccelerometer request");
    state.r[0] = hidSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                host.HidAccelerometerEnabled(),
            "HID accelerometer was not enabled");
    Require(memory.Write32(0x1FF82080, 0x00130000),
            "cannot write HID:EnableGyroscopeLow request");
    state.r[0] = hidSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                host.HidGyroscopeEnabled(),
            "HID gyroscope was not enabled");
    NativeA32HidState motionState;
    motionState.Accelerometer = {0.5F, -1.0F, 0.25F};
    motionState.AccelerometerValid = true;
    motionState.GyroscopeDegreesPerSecond = {10.0F, -20.0F, 30.0F};
    motionState.GyroscopeValid = true;
    const auto motionUpdate =
        host.AdvanceHidToCurrentTick(memory, motionState);
    uint32_t accelerometerIndex = UINT32_MAX;
    uint32_t gyroscopeIndex = UINT32_MAX;
    std::array<uint16_t, 3> accelerometerEntry{};
    std::array<uint16_t, 3> accelerometerRaw{};
    std::array<uint16_t, 3> gyroscopeEntry{};
    std::array<uint16_t, 3> gyroscopeRaw{};
    const auto readVector = [&](uint32_t address,
                                std::array<uint16_t, 3>& output) {
        return memory.Read16(address, &output[0]) &&
               memory.Read16(address + 2U, &output[1]) &&
               memory.Read16(address + 4U, &output[2]);
    };
    Require(
        motionUpdate.Status == NativeA32CtrHidUpdateStatus::Updated &&
            motionUpdate.SamplesWritten == 0U &&
            motionUpdate.AccelerometerSamplesWritten == 1U &&
            motionUpdate.GyroscopeSamplesWritten == 1U &&
            memory.Read32(0x10002118, &accelerometerIndex) &&
            accelerometerIndex == 0U &&
            memory.Read32(0x10002168, &gyroscopeIndex) &&
            gyroscopeIndex == 0U &&
            readVector(0x10002120, accelerometerRaw) &&
            static_cast<int16_t>(accelerometerRaw[0]) == -512 &&
            static_cast<int16_t>(accelerometerRaw[1]) == -256 &&
            static_cast<int16_t>(accelerometerRaw[2]) == -1024 &&
            readVector(0x10002128, accelerometerEntry) &&
            static_cast<int16_t>(accelerometerEntry[0]) == 256 &&
            static_cast<int16_t>(accelerometerEntry[1]) == -512 &&
            static_cast<int16_t>(accelerometerEntry[2]) == 128 &&
            readVector(0x10002170, gyroscopeRaw) &&
            static_cast<int16_t>(gyroscopeRaw[0]) == 144 &&
            static_cast<int16_t>(gyroscopeRaw[1]) == 431 &&
            static_cast<int16_t>(gyroscopeRaw[2]) == 288 &&
            readVector(0x10002178, gyroscopeEntry) &&
            static_cast<int16_t>(gyroscopeEntry[0]) == 144 &&
            static_cast<int16_t>(gyroscopeEntry[1]) == -288 &&
            static_cast<int16_t>(gyroscopeEntry[2]) == 431,
        "native HID producer did not populate CTR motion rings");
    const auto motionProfile = host.HidRuntimeProfile();
    Require(motionProfile.AccelerometerSamplesWritten == 1U &&
                motionProfile.GyroscopeSamplesWritten == 1U &&
                motionProfile.NextAccelerometerIndex == 1U &&
                motionProfile.NextGyroscopeIndex == 1U,
            "native HID motion profile lost producer state");
    // Consumer locations from libctru hidScanInput (word 86 + header 8),
    // independent of producer constants. Exercise both rings through wrap.
    host.AdvanceSystemTicks(NativeA32CtrHostServices::HidGyroscopeUpdateTicks * 40U);
    Require(host.AdvanceHidToCurrentTick(memory, motionState).Status ==
                NativeA32CtrHidUpdateStatus::Updated,
            "motion rings did not advance");
    for (uint32_t index = 0; index < 8U; ++index) {
        Require(readVector(0x10002128U + index * 6U, accelerometerEntry) &&
                    static_cast<int16_t>(accelerometerEntry[0]) == 256 &&
                    static_cast<int16_t>(accelerometerEntry[1]) == -512 &&
                    static_cast<int16_t>(accelerometerEntry[2]) == 128,
                "gyro timestamps overwrote the accelerometer tail");
    }
    for (uint32_t index = 0; index < 32U; ++index) {
        Require(readVector(0x10002178U + index * 6U, gyroscopeEntry) &&
                    static_cast<int16_t>(gyroscopeEntry[0]) == 144 &&
                    static_cast<int16_t>(gyroscopeEntry[1]) == -288 &&
                    static_cast<int16_t>(gyroscopeEntry[2]) == 431,
                "libctru-compatible consumer read shifted gyro data");
    }
    Require(memory.Write32(0x1FF82080, 0x00160000),
            "cannot write HID gyro calibration request");
    state.r[0] = hidSession;
    uint16_t gyroPositiveUnit = 0;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                memory.Read32(0x1FF82080, &response) &&
                response == 0x00160180 &&
                memory.Read16(0x1FF8208A, &gyroPositiveUnit) &&
                gyroPositiveUnit == 6700,
            "HID gyro calibration response is wrong");

    Require(memory.Write32(0x1FF82080, 0x00050100) &&
                memory.Write32(0x1FF82084, 0x3A6D646E) &&
                memory.Write32(0x1FF82088, 0x00000075) &&
                memory.Write32(0x1FF8208C, 5),
            "cannot write srv:GetServiceHandle ndm:u request");
    state.r[0] = srvSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                NativeA32HostAction::Resume,
            "srv:GetServiceHandle did not open ndm:u");
    uint32_t ndmSession = 0;
    Require(memory.Read32(0x1FF8208C, &ndmSession) && ndmSession != 0,
            "srv:GetServiceHandle ndm:u response has no session");
    Require(memory.Write32(0x1FF82080, 0x00080040) &&
                memory.Write32(0x1FF82084, 1),
            "cannot write NDM:SuspendScheduler request");
    state.r[0] = ndmSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                memory.Read32(0x1FF82080, &response) &&
                response == 0x00080040 && host.NdmSchedulerSuspended() &&
                host.NdmSchedulerRunsInBackground(),
            "NDM:SuspendScheduler did not preserve its native state");
    Require(memory.Write32(0x1FF82080, 0x00090000),
            "cannot write NDM:ResumeScheduler request");
    state.r[0] = ndmSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                !host.NdmSchedulerSuspended() &&
                !host.NdmSchedulerRunsInBackground(),
            "NDM:ResumeScheduler did not restore its native state");

    Require(memory.Write32(0x1FF82080, 0x00160042) &&
                memory.Write32(0x1FF82084, 0) &&
                memory.Write32(0x1FF82088, 0) &&
                memory.Write32(0x1FF8208C, 0xFFFF8001),
            "cannot write GSP:AcquireRight request");
    state.r[0] = gspSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                NativeA32HostAction::Resume &&
                memory.Read32(0x1FF82080, &response) &&
                response == 0x00160040,
            "GSP:AcquireRight response is wrong");

    state.r[1] = 0;
    Require(host.HandleSvc(0x17, state, memory, context).Action ==
                NativeA32HostAction::Resume,
            "cannot create GSP interrupt event");
    const uint32_t gspInterruptEvent = state.r[1];
    Require(memory.Write32(0x1FF82080, 0x00130042) &&
                memory.Write32(0x1FF82084, 1) &&
                memory.Write32(0x1FF82088, 0) &&
                memory.Write32(0x1FF8208C, gspInterruptEvent),
            "cannot write GSP:RegisterInterruptRelayQueue request");
    state.r[0] = gspSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                NativeA32HostAction::Resume,
            "GSP:RegisterInterruptRelayQueue did not resume");
    uint32_t gspSharedMemory = 0;
    Require(memory.Read32(0x1FF82080, &response) &&
                response == 0x00130082 &&
                memory.Read32(0x1FF82084, &descriptor) &&
                descriptor == 0x2A07 &&
                memory.Read32(0x1FF82090, &gspSharedMemory) &&
                gspSharedMemory != 0,
            "GSP:RegisterInterruptRelayQueue response is wrong");

    Require(memory.Write32(0x1FF82080, 0x000B0040) &&
                memory.Write32(0x1FF82084, 0),
            "cannot write GSP:SetLcdForceBlack request");
    state.r[0] = gspSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                memory.Read32(0x1FF82080, &response) &&
                response == 0x000B0040 && !host.LcdForceBlack(),
            "GSP:SetLcdForceBlack did not expose native LCD state");

    state.r[0] = gspSharedMemory;
    state.r[1] = 0x10000000;
    state.r[2] = 3;
    state.r[3] = 0x10000000;
    Require(host.HandleSvc(0x1F, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                memory.IsMapped(0x10000000, 0x1000) &&
                memory.IsWritable(0x10000000, 0x1000),
            "MapMemoryBlock did not map writable GSP shared memory");
    constexpr uint32_t topFramebufferUpdate = 0x10000200;
    constexpr uint32_t topFramebufferInfo1 = topFramebufferUpdate + 0x20;
    Require(memory.Write8(topFramebufferUpdate, 1) &&
                memory.Write8(topFramebufferUpdate + 1U, 1) &&
                memory.Write32(topFramebufferInfo1, 1) &&
                memory.Write32(topFramebufferInfo1 + 4U, 0x14002000) &&
                memory.Write32(topFramebufferInfo1 + 8U, 0x14002000) &&
                memory.Write32(topFramebufferInfo1 + 12U, 720) &&
                memory.Write32(topFramebufferInfo1 + 16U, 2) &&
                memory.Write32(topFramebufferInfo1 + 20U, 1) &&
                memory.Write32(topFramebufferInfo1 + 24U, 0),
            "cannot write native GSP shared framebuffer update");
    uint8_t gspInterruptCount = 0;
    uint8_t topVBlankInterrupt = 0;
    uint8_t bottomVBlankInterrupt = 0;
    Require(host.SignalVBlank(memory) &&
                memory.Read8(0x10000001, &gspInterruptCount) &&
                memory.Read8(0x1000000C, &topVBlankInterrupt) &&
                memory.Read8(0x1000000D, &bottomVBlankInterrupt) &&
                gspInterruptCount == 2 && topVBlankInterrupt == 2 &&
                bottomVBlankInterrupt == 3 &&
                host.TopFramebuffer().has_value() &&
                host.TopFramebuffer()->AddressLeft == 0x14002000 &&
                host.TopFramebuffer()->BufferIndex == 1,
            "VBlank did not apply the framebuffer update and enter the relay queue");
    uint8_t framebufferDirty = 1;
    Require(memory.Read8(topFramebufferUpdate + 1U, &framebufferDirty) &&
                framebufferDirty == 0,
            "VBlank did not acknowledge the shared framebuffer update");
    uint8_t p3dInterrupt = 0;
    Require(host.SignalPicaInterrupt(
                memory, Oot3dNativeGame::Oot3dPicaInterruptId::P3d) &&
                memory.Read8(0x1000000E, &p3dInterrupt) &&
                memory.Read8(0x10000001, &gspInterruptCount) &&
                gspInterruptCount == 3 && p3dInterrupt == 5,
            "P3D completion did not enter the native GSP relay queue");

    Require(memory.Write8(0x10000001, 0x20) &&
                memory.Write32(0x10000004, 0) &&
                memory.Write32(0x10000008, 0) &&
                host.SignalVBlank(memory),
            "cannot exercise the native PDC relay threshold");
    uint32_t missedPdc0 = 0;
    uint32_t missedPdc1 = 0;
    Require(memory.Read8(0x10000001, &gspInterruptCount) &&
                memory.Read32(0x10000004, &missedPdc0) &&
                memory.Read32(0x10000008, &missedPdc1) &&
                gspInterruptCount == 0x20 && missedPdc0 == 1 &&
                missedPdc1 == 1,
            "PDC interrupts did not use the native missed-event counters");
    Require(memory.Write8(0x10000001, 0) &&
                memory.Write8(0x10000003, 1) &&
                memory.Write32(0x10000004, 0) &&
                memory.Write32(0x10000008, 0) &&
                host.SignalVBlank(memory) &&
                memory.Read8(0x10000001, &gspInterruptCount) &&
                memory.Read32(0x10000004, &missedPdc0) &&
                memory.Read32(0x10000008, &missedPdc1) &&
                gspInterruptCount == 0 && missedPdc0 == 0 &&
                missedPdc1 == 0 && memory.Write8(0x10000003, 0) &&
                memory.Write8(0x10000001, 3),
            "GSP ignore-PDC configuration was not honored");

    constexpr uint32_t dmaSource = 0x14000100;
    constexpr uint32_t dmaDestination = 0x14000200;
    constexpr std::array<uint8_t, 8> dmaSourceBytes{
        0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE};
    constexpr uint32_t gspCommandQueue = 0x10000800;
    constexpr uint32_t gspCommand = gspCommandQueue + 0x20;
    Require(memory.WriteBytes(dmaSource, dmaSourceBytes) &&
                memory.Fill(dmaDestination, dmaSourceBytes.size(), 0) &&
                memory.Write32(gspCommandQueue, 0x00000100) &&
                memory.Write32(gspCommand, 0x01000100) &&
                memory.Write32(gspCommand + 4U, dmaSource) &&
                memory.Write32(gspCommand + 8U, dmaDestination) &&
                memory.Write32(gspCommand + 12U,
                               static_cast<uint32_t>(dmaSourceBytes.size())) &&
                memory.Write32(0x1FF82080, 0x000C0000),
            "cannot prepare native GSP DMA command");
    state.r[0] = gspSession;
    Require(host.HandleSvc(0x32, state, memory, context).Action ==
                NativeA32HostAction::Resume,
            "GSP DMA command did not resume");
    std::array<uint8_t, dmaSourceBytes.size()> dmaDestinationBytes{};
    uint32_t processedQueueHeader = 0;
    uint8_t dmaInterrupt = 0;
    Require(memory.ReadBytes(dmaDestination, dmaDestinationBytes) &&
                dmaDestinationBytes == dmaSourceBytes &&
                memory.Read32(gspCommandQueue, &processedQueueHeader) &&
                processedQueueHeader == 1 &&
                memory.Read8(0x1000000F, &dmaInterrupt) &&
                memory.Read8(0x10000001, &gspInterruptCount) &&
                gspInterruptCount == 4 && dmaInterrupt == 6,
            "GSP DMA did not copy memory and signal native completion");

    const uint64_t ticksBeforeClockAdvance = host.SystemTicks();
    constexpr uint64_t clockAdvance = 0x123456789ABCDEF0ULL;
    const uint64_t expectedSystemTicks = ticksBeforeClockAdvance + clockAdvance;
    host.AdvanceSystemTicks(clockAdvance);
    Require(host.HandleSvc(0x28, state, memory, context).Action ==
                NativeA32HostAction::Resume &&
                state.r[0] == static_cast<uint32_t>(expectedSystemTicks) &&
                state.r[1] ==
                    static_cast<uint32_t>(expectedSystemTicks >> 32U) &&
                host.SystemTicks() == expectedSystemTicks,
            "GetSystemTick did not expose the host-controlled CTR clock");

    const uint64_t ticksBeforeSleep = host.SystemTicks();
    state.r[0] = 100000;
    state.r[1] = 0;
    constexpr uint64_t sleepTicks = 26812;
    Require(host.HandleSvc(0x0A, state, memory, context).Action ==
                    NativeA32HostAction::Wait &&
                host.SystemTicks() == ticksBeforeSleep &&
                host.NextSleepWakeTick() == ticksBeforeSleep + sleepTicks &&
                host.PendingSleepCount() == 1,
            "SleepThread did not schedule an independent CTR timer");
    host.AdvanceSystemTicks(sleepTicks - 1U);
    Require(host.PendingSleepCount() == 1 &&
                host.NextSleepWakeTick() == ticksBeforeSleep + sleepTicks,
            "SleepThread woke before its CTR timer deadline");
    host.AdvanceSystemTicks(1U);
    Require(host.PendingSleepCount() == 0 &&
                !host.NextSleepWakeTick().has_value() &&
                host.SystemTicks() == ticksBeforeSleep + sleepTicks,
            "SleepThread timer did not expire at its CTR deadline");
    state.r[0] = 0;
    state.r[1] = 0;
    Require(host.HandleSvc(0x0A, state, memory, context).Action ==
                    NativeA32HostAction::Resume &&
                host.SystemTicks() == ticksBeforeSleep + sleepTicks,
            "zero-duration SleepThread changed the CTR clock");

    const uint64_t ticksBeforeSynchronousSleep = host.SystemTicks();
    state.r[0] = 100000;
    state.r[1] = 0;
    const auto synchronousSleep =
        host.HandleSvc(0x0A, state, memory, context);
    error.clear();
    Require(
        synchronousSleep.Action == NativeA32HostAction::Wait &&
            host.CompleteSynchronousWait(
                0x0A, synchronousSleep, state, memory, context, &error) &&
            error.empty() &&
            host.SystemTicks() ==
                ticksBeforeSynchronousSleep + sleepTicks &&
            host.PendingSleepCount() == 0U,
        "finite synchronous SleepThread did not advance to its deadline");

    const uint64_t ticksBeforeInfiniteSleep = host.SystemTicks();
    state.r[0] = 0xFFFFFFFFU;
    state.r[1] = 0xFFFFFFFFU;
    const auto infiniteSleep =
        host.HandleSvc(0x0A, state, memory, context);
    error.clear();
    Require(
        infiniteSleep.Action == NativeA32HostAction::Wait &&
            !host.CompleteSynchronousWait(
                0x0A, infiniteSleep, state, memory, context, &error) &&
            !error.empty() &&
            host.SystemTicks() == ticksBeforeInfiniteSleep &&
            host.PendingSleepCount() == 0U,
        "infinite synchronous SleepThread did not remain scheduler-owned");

    const uint64_t savedSystemTicks = host.SystemTicks();
    const auto savedTopFramebuffer = host.TopFramebuffer();
    const auto hostStateBytes =
        nlohmann::json::to_msgpack(host.CaptureState());
    host.AdvanceSystemTicks(777U);
    Require(host.RestoreState(
                nlohmann::json::from_msgpack(hostStateBytes), process,
                &error) &&
                host.SystemTicks() == savedSystemTicks &&
                host.TopFramebuffer().has_value() ==
                    savedTopFramebuffer.has_value() &&
                (!savedTopFramebuffer.has_value() ||
                 host.TopFramebuffer()->AddressLeft ==
                     savedTopFramebuffer->AddressLeft),
            "CTR host state did not survive binary round-trip");
    Require(host.CaptureState().at("y2r") ==
                nlohmann::json::from_msgpack(hostStateBytes).at("y2r"),
            "Y2R configuration did not survive binary round-trip");

    auto crossPlatformHostState = host.CaptureState();
    bool foundRomFsObject = false;
    for (auto& object : crossPlatformHostState["kernel_objects"]) {
        if (object.at("type").get<std::string>() ==
            "client_session:file:romfs") {
            object["file_path"] =
                R"(Z:\foreign-host\extracted-game\romfs.bin)";
            foundRomFsObject = true;
        }
    }
    error.clear();
    Require(foundRomFsObject &&
                host.RestoreState(crossPlatformHostState, process, &error),
            "CTR host state did not safely rebind a foreign RomFS path");
    bool reboundRomFsObject = false;
    const auto reboundHostState = host.CaptureState();
    for (const auto& object : reboundHostState["kernel_objects"]) {
        if (object.at("type").get<std::string>() ==
            "client_session:file:romfs") {
            reboundRomFsObject =
                object.at("file_path").get<std::string>() ==
                romFsPath.string();
        }
    }
    Require(reboundRomFsObject,
            "restored RomFS object retained its foreign host path");

    auto incompatibleHostState = host.CaptureState();
    bool foundSaveDataObject = false;
    for (auto& object : incompatibleHostState["kernel_objects"]) {
        if (object.at("type").get<std::string>() ==
            "client_session:file:savedata") {
            object["file_path"] =
                (std::filesystem::temp_directory_path() /
                 "oot3d_native_a32_ctr_host_outside_savedata.dat")
                    .string();
            foundSaveDataObject = true;
            break;
        }
    }
    error.clear();
    Require(foundSaveDataObject &&
                !host.RestoreState(incompatibleHostState, process, &error) &&
                !error.empty(),
            "RomFS path rebinding weakened SaveData path confinement");

    NativeA32DspHle dspHle({});
    const auto dspStateBytes =
        nlohmann::json::to_msgpack(dspHle.CaptureState());
    Require(dspHle.RestoreState(
                nlohmann::json::from_msgpack(dspStateBytes), &error),
            "DSP HLE state did not survive binary round-trip");
    auto incompatibleDspState =
        nlohmann::json::from_msgpack(dspStateBytes);
    incompatibleDspState["sources"][0]["source_id"] = 1;
    Require(!dspHle.RestoreState(incompatibleDspState, &error),
            "DSP HLE state accepted an incompatible source identity");

    std::error_code removeError;
    std::filesystem::remove(romFsPath, removeError);
    std::filesystem::remove_all(saveDataPath, removeError);
    std::filesystem::remove_all(saveDataPath.string() + "-backups", removeError);

    std::cout << "oot3d_native_a32_ctr_host_tests: ok (fast SVC "
              << (gFastSvc ? "on" : "off") << ")\n";
}

int main() {
    for (const bool fastSvc : {false, true}) {
        gFastSvc = fastSvc;
        RunAllTests();
    }
    TestFastSvcEquivalence();
    std::cout << "oot3d_native_a32_ctr_host_tests: ok\n";
    return 0;
}
