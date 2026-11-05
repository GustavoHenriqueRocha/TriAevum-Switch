#include "oot3d_native_a32_guest_state_dump.h"

#include "oot3d_native_a32_ctr_host.h"
#include "oot3d_native_a32_process.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace Oot3dNativeGame {
namespace {

const char *ThreadStatusName(NativeA32ThreadStatus status) {
    switch (status) {
    case NativeA32ThreadStatus::Empty:
        return "empty";
    case NativeA32ThreadStatus::Ready:
        return "ready";
    case NativeA32ThreadStatus::Running:
        return "running";
    case NativeA32ThreadStatus::Waiting:
        return "waiting";
    case NativeA32ThreadStatus::Terminated:
        return "terminated";
    case NativeA32ThreadStatus::Faulted:
        return "faulted";
    }
    return "unknown";
}

// Aspas e barras do nome do evento não quebram a linha.
std::string Quoted(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2U);
    out.push_back('"');
    for (const char c : text) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c == '\n' ? ' ' : c);
    }
    out.push_back('"');
    return out;
}

void Append(std::string &out, const char *format, auto... values) {
    char buffer[256];
    const int written = std::snprintf(buffer, sizeof(buffer), format, values...);
    if (written > 0)
        out.append(buffer, std::min<size_t>(static_cast<size_t>(written),
                                             sizeof(buffer) - 1U));
}

} // namespace

std::optional<uint32_t> ParseGuestStateDumpInterval(const char *text) {
    if (text == nullptr || *text == '\0') return std::nullopt;
    errno = 0;
    char *end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0UL ||
        value > 0xFFFFFFFFUL)
        return std::nullopt;
    return static_cast<uint32_t>(value);
}

std::vector<GuestMemoryRange> ParseGuestMemoryRanges(const char *text) {
    std::vector<GuestMemoryRange> ranges;
    if (text == nullptr) return ranges;
    std::string_view rest(text);
    while (!rest.empty()) {
        const size_t comma = rest.find(',');
        const std::string item(rest.substr(0, comma));
        rest = comma == std::string_view::npos ? std::string_view{}
                                               : rest.substr(comma + 1U);
        const size_t colon = item.find(':');
        if (colon == std::string::npos) return {};
        const std::string addressText = item.substr(0, colon);
        const std::string sizeText = item.substr(colon + 1U);
        char *end = nullptr;
        errno = 0;
        const unsigned long long address =
            std::strtoull(addressText.c_str(), &end, 16);
        if (errno != 0 || addressText.empty() || *end != '\0' ||
            address > 0xFFFFFFFFULL)
            return {};
        const unsigned long long size = std::strtoull(sizeText.c_str(), &end, 16);
        if (errno != 0 || sizeText.empty() || *end != '\0' || size == 0ULL ||
            size > 0x100ULL || address + size > 0x100000000ULL)
            return {};
        ranges.push_back({static_cast<uint32_t>(address),
                          static_cast<uint32_t>(size)});
    }
    return ranges;
}

std::string GuestStateDumper::Format(const NativeA32Process &process,
                                     const NativeA32CtrHostServices &host,
                                     uint64_t vblank, uint32_t padButtons) {
    std::string out;
    const uint64_t svcTotal = host.SvcEventCount();
    const auto &ipcCounts = host.IpcNameCounts();
    Append(out,
           "TRIAEVUM_GUEST_STATE vblank=%llu ticks=%llu svc_total=%llu "
           "svc_delta=%llu pad=0x%08x threads=%zu ipc_counts=%s "
           "gsp_shared=0x%08x\n",
           static_cast<unsigned long long>(vblank),
           static_cast<unsigned long long>(host.SystemTicks()),
           static_cast<unsigned long long>(svcTotal),
           static_cast<unsigned long long>(svcTotal - mLastSvcTotal),
           padButtons, process.ThreadCount(),
           host.CollectsIpcNameCounts() ? "on" : "off",
           host.GspSharedMemoryAddress().value_or(0U));
    mLastSvcTotal = svcTotal;

    // Último SVC de cada thread (o histórico guarda o último por thread mesmo
    // quando o anel de eventos recentes já o descartou).
    const auto &events = host.SvcEvents();
    std::vector<const NativeA32CtrSvcEvent *> lastByThread(process.ThreadCount(),
                                                           nullptr);
    for (const auto &event : events)
        if (event.ThreadId < lastByThread.size())
            lastByThread[event.ThreadId] = &event;

    for (uint32_t id = 0; id < process.ThreadCount(); ++id) {
        const auto *state = process.ThreadState(id);
        Append(out,
               "TRIAEVUM_GUEST_THREAD vblank=%llu id=%u status=%s prio=0x%02x "
               "pc=0x%08x lr=0x%08x sp=0x%08x",
               static_cast<unsigned long long>(vblank), id,
               ThreadStatusName(process.ThreadStatus(id)),
               process.ThreadPriority(id).value_or(0U),
               state != nullptr ? state->r[15] : 0U,
               state != nullptr ? state->r[14] : 0U,
               state != nullptr ? state->r[13] : 0U);
        if (const auto *event = lastByThread[id]; event != nullptr) {
            Append(out, " last_svc_pc=0x%08x last_svc=", event->Pc);
            out += Quoted(event->Name);
        }
        out.push_back('\n');
    }

    for (const auto &[name, total] : ipcCounts) {
        const auto previous = mLastIpcCounts.find(name);
        const uint64_t before =
            previous == mLastIpcCounts.end() ? 0U : previous->second;
        if (total == before) continue;
        Append(out, "TRIAEVUM_GUEST_IPC vblank=%llu delta=%llu total=%llu name=",
               static_cast<unsigned long long>(vblank),
               static_cast<unsigned long long>(total - before),
               static_cast<unsigned long long>(total));
        out += Quoted(name);
        out.push_back('\n');
    }
    mLastIpcCounts = ipcCounts;

    for (const auto &range : mMemoryRanges) {
        Append(out, "TRIAEVUM_GUEST_MEM vblank=%llu addr=0x%08x words=",
               static_cast<unsigned long long>(vblank), range.Address);
        for (uint32_t offset = 0; offset < range.Size; offset += 4U) {
            uint32_t value = 0;
            std::array<uint8_t, 4> bytes{};
            if (process.Memory().ReadBytes(range.Address + offset, bytes)) {
                std::memcpy(&value, bytes.data(), sizeof(value));
                Append(out, offset == 0U ? "0x%08x" : " 0x%08x", value);
            } else {
                out += offset == 0U ? "unmapped" : " unmapped";
            }
        }
        out.push_back('\n');
    }
    return out;
}

} // namespace Oot3dNativeGame
