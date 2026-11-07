// Guest block profile (config/guest_profile, oot3d_native_guest_profile.h):
// the reports leave the game thread. The writer thread writes what the
// game thread's report used to (cumulative and window modes), a dump while it
// is still writing waits for a later frame without blocking the game thread,
// and the session's end writes the last counters.
#include "oot3d_native_compiled_functions.h"
#include "oot3d_native_guest_profile.h"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__unix__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Oot3dNativeGame {
// The profile's link to the native overrides: none in this test.
void RegisterOot3dGuestOverrideProfileHook(
    oot3d::recomp::a32::BlockEntryCallback,
    bool (*)(const void*, uint32_t, uint32_t) noexcept,
    void (*)(void*, uint32_t) noexcept) noexcept {}
} // namespace Oot3dNativeGame

namespace {

using Oot3dNativeGame::GuestBlockProfiler;
using Oot3dNativeGame::GuestProfileOptions;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kBase = 0x00001000U;

[[noreturn]] void Fail(std::string_view message) {
    std::cerr << "oot3d_native_guest_profile_tests: " << message << '\n';
    std::exit(1);
}

void Require(bool condition, std::string_view message) {
    if (!condition) Fail(message);
}

std::filesystem::path TempPath(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

GuestProfileOptions Options(const std::filesystem::path& output, uint32_t dumpFrames,
                            bool window) {
    GuestProfileOptions options;
    options.SampleEveryFrames = 1U;
    options.DumpEveryHostFrames = dumpFrames;
    options.Window = window;
    options.OutputPath = output;
    options.FunctionMapPath = TempPath("oot3d_guest_profile_test_no_map.txt");
    return options;
}

void Enter(GuestBlockProfiler& profiler, uint32_t pc, uint32_t times) {
    for (uint32_t index = 0; index < times; ++index) profiler.MarkNative(pc);
}

void WaitForReports(const GuestBlockProfiler& profiler, uint64_t reports) {
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    while (profiler.ReportsWritten() < reports) {
        Require(Clock::now() < deadline, "the writer did not write the report");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

struct Report {
    uint64_t BlockEntries = 0U;
    uint64_t SampledFrames = 0U;
    std::map<uint32_t, uint64_t> Entries;  // block pc -> entries
};

Report Parse(const std::string& text) {
    Report report;
    std::istringstream lines(text);
    std::string line;
    bool blocks = false;
    while (std::getline(lines, line)) {
        if (line.starts_with("# sampled_frames=")) {
            Require(std::sscanf(line.c_str(), "# sampled_frames=%" SCNu64, &report.SampledFrames) ==
                        1,
                    "header: sampled_frames");
            const auto entries = line.find("block_entries=");
            Require(entries != std::string::npos &&
                        std::sscanf(line.c_str() + entries, "block_entries=%" SCNu64,
                                    &report.BlockEntries) == 1,
                    "header: block_entries");
        } else if (line.starts_with("# blocks:")) {
            blocks = true;
        } else if (blocks && line.starts_with("0x")) {
            unsigned pc = 0U;
            double percent = 0.0;
            double ms = 0.0;
            uint64_t entries = 0U;
            Require(std::sscanf(line.c_str(), "0x%X %lf%% %lf %" SCNu64, &pc, &percent, &ms,
                                &entries) == 4,
                    "block line: " + line);
            report.Entries[pc] = entries;
        }
    }
    Require(text.starts_with("# triaevum guest_profile v1\n"), "report header");
    return report;
}

Report ReadReport(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return Parse(text.str());
}

// window=0: each report holds every entry since the start; window=1: only
// those since the previous report. The last counters go out at the end.
void TestReports(bool window) {
    const auto output = TempPath(window ? "oot3d_guest_profile_test_window.txt"
                                        : "oot3d_guest_profile_test_total.txt");
    std::filesystem::remove(output);
    GuestBlockProfiler profiler;
    Require(profiler.Start(Options(output, 2U, window), kBase, 0x100U), "start");
    profiler.SetAppendix([](std::ostream& out) { out << "# appendix line\n"; });
    profiler.OnHostFrame(0U);
    Enter(profiler, kBase, 3U);
    Enter(profiler, kBase + 0x10U, 1U);
    profiler.OnHostFrame(1U);
    Enter(profiler, kBase, 2U);
    profiler.OnHostFrame(2U);  // report 1
    WaitForReports(profiler, 1U);
    const Report first = ReadReport(output);
    Require(first.BlockEntries == 6U && first.Entries.at(kBase) == 5U &&
                first.Entries.at(kBase + 0x10U) == 1U && first.SampledFrames == 3U,
            "first report: the entries of frames 0-2");
    {
        std::ifstream file(output);
        std::stringstream text;
        text << file.rdbuf();
        Require(text.str().ends_with("\n# appendix line\n"),
                "the appendix closes the report, after a blank line");
    }

    Enter(profiler, kBase + 0x10U, 4U);
    Enter(profiler, kBase + 0x20U, 1U);
    profiler.OnHostFrame(3U);
    profiler.OnHostFrame(4U);  // report 2
    WaitForReports(profiler, 2U);
    const Report second = ReadReport(output);
    if (window) {
        Require(second.BlockEntries == 5U && !second.Entries.contains(kBase) &&
                    second.Entries.at(kBase + 0x10U) == 4U &&
                    second.Entries.at(kBase + 0x20U) == 1U && second.SampledFrames == 2U,
                "window report: only the entries since the previous report");
    } else {
        Require(second.BlockEntries == 11U && second.Entries.at(kBase) == 5U &&
                    second.Entries.at(kBase + 0x10U) == 5U &&
                    second.Entries.at(kBase + 0x20U) == 1U && second.SampledFrames == 5U,
                "cumulative report: every entry since the start");
    }

    Enter(profiler, kBase + 0x30U, 7U);
    profiler.OnHostFrame(5U);
    Require(profiler.FinishReports(), "the last report was written");
    Require(profiler.ReportsWritten() == 3U, "the session's end wrote the last counters");
    const Report last = ReadReport(output);
    Require(last.Entries.at(kBase + 0x30U) == 7U &&
                last.Entries.contains(kBase) == !window,
            "the last report");
    std::filesystem::remove(output);
}

#if defined(__unix__)
// A report the writer cannot finish (its file a FIFO nobody reads yet) must
// not hold the game thread: the next dump waits for a later frame.
void TestWriterNeverBlocksTheGameThread() {
    const auto fifo = TempPath("oot3d_guest_profile_test.fifo");
    std::filesystem::remove(fifo);
    Require(mkfifo(fifo.c_str(), 0600) == 0, "mkfifo");
    GuestBlockProfiler profiler;
    Require(profiler.Start(Options(fifo, 1U, false), kBase, 0x100U), "start");
    profiler.OnHostFrame(0U);
    Enter(profiler, kBase, 1U);
    profiler.OnHostFrame(1U);  // the writer now blocks opening the FIFO
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto start = Clock::now();
    Enter(profiler, kBase, 1U);
    profiler.OnHostFrame(2U);
    const auto elapsed = Clock::now() - start;
    Require(elapsed < std::chrono::milliseconds(20),
            "a dump during a stuck write held the game thread");
    Require(profiler.ReportsPostponed() == 1U && profiler.ReportsWritten() == 0U,
            "the dump waits for a later frame");

    std::atomic<bool> stop{false};
    std::atomic<bool> done{false};
    std::vector<std::string> reports;
    std::thread reader([&] {
        while (!stop.load()) {
            const int fd = open(fifo.c_str(), O_RDONLY);
            if (fd < 0) break;
            std::string text;
            char buffer[4096];
            ssize_t got = 0;
            while ((got = read(fd, buffer, sizeof(buffer))) > 0) {
                text.append(buffer, static_cast<size_t>(got));
            }
            close(fd);
            if (!text.empty()) reports.push_back(std::move(text));
        }
        done = true;
    });
    WaitForReports(profiler, 1U);
    profiler.OnHostFrame(3U);  // the postponed dump goes out now
    Require(profiler.FinishReports() && profiler.ReportsWritten() == 2U,
            "every report written");
    stop = true;
    while (!done.load()) {
        // Releases a reader in open() (none there yet: this open fails, and
        // the loop tries again).
        const int fd = open(fifo.c_str(), O_WRONLY | O_NONBLOCK);
        if (fd >= 0) close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    reader.join();
    Require(reports.size() == 2U, "the reader saw both reports");
    Require(Parse(reports[0]).BlockEntries == 1U && Parse(reports[1]).BlockEntries == 2U,
            "the first report has frame 0-1's entry, the second all of them");
    std::filesystem::remove(fifo);
}
#endif

} // namespace

int main() {
    TestReports(false);
    TestReports(true);
#if defined(__unix__)
    TestWriterNeverBlocksTheGameThread();
#endif
    std::cout << "oot3d_native_guest_profile_tests: ok\n";
    return 0;
}
