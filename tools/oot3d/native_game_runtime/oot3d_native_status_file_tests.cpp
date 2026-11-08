// config/async_status_writes (oot3d_native_status_file.h): boot-status.txt's
// heartbeat written by a service thread. The file always ends as the newest
// text; Post never waits for the file (a writer blocked in open on a FIFO);
// a synchronous WriteNow (boot stage, completed/failed) is never overwritten
// by an older posted heartbeat; Stop writes what is still waiting.
#include "oot3d_native_status_file.h"

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace {

using namespace Oot3dNativeGame;

void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

std::string TempPath(const char* name) {
    return (std::filesystem::temp_directory_path() /
            (std::string("oot3d_status_file_") + name + "_" + std::to_string(::getpid())))
        .string();
}

std::string ReadAll(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    std::stringstream content;
    content << input.rdbuf();
    return content.str();
}

std::string Heartbeat(uint64_t frame) {
    return "status=launching\nstage=runtime_running\nhost_frames=" + std::to_string(frame) + '\n';
}

// The heartbeat every 600 frames: the file is the last one, every post is
// either written or superseded.
void TestLatestWins() {
    const auto path = TempPath("latest");
    std::filesystem::remove(path);
    {
        StatusFileWriter writer(path);
        for (uint64_t frame = 600U; frame <= 600U * 2000U; frame += 600U) {
            writer.Post(Heartbeat(frame));
            if (frame % 60000U == 0U) std::this_thread::yield();
        }
        writer.Flush();
        Require(ReadAll(path) == Heartbeat(600U * 2000U), "the file is not the newest heartbeat");
        const auto stats = writer.Stats();
        Require(stats.Posted == 2000U, "posts not counted");
        Require(stats.Written + stats.Superseded == stats.Posted, "a post was neither written nor superseded");
        Require(stats.Written >= 1U && stats.SyncWrites == 0U, "no write counted");
        // A shorter text replaces the file (truncated, not overwritten in place).
        writer.Post("status=launching\n");
        writer.Flush();
        Require(ReadAll(path) == "status=launching\n", "a shorter text left the old tail");
    }
    std::filesystem::remove(path);
    std::printf("latest wins: ok\n");
}

// The final state (WriteNow) is never overwritten by a heartbeat posted before it,
// whether that heartbeat is still waiting or being written.
void TestWriteNowWins() {
    const auto path = TempPath("final");
    for (int round = 0; round < 300; ++round) {
        std::filesystem::remove(path);
        StatusFileWriter writer(path);
        for (int post = 0; post < 1 + round % 7; ++post) writer.Post(Heartbeat(600U * (post + 1U)));
        if (round % 3 == 0) std::this_thread::yield();
        const std::string final = "status=failed\ndetail=round " + std::to_string(round) + '\n';
        Require(writer.WriteNow(final), "the synchronous write failed");
        writer.Flush();
        writer.Stop();
        Require(ReadAll(path) == final, "a heartbeat posted earlier overwrote the final state");
        const auto stats = writer.Stats();
        Require(stats.Written + stats.Superseded == stats.Posted, "post accounting after WriteNow");
        Require(stats.SyncWrites == 1U, "WriteNow not counted");
    }
    std::filesystem::remove(path);
    std::printf("write now wins: ok\n");
}

// Post returns while the service thread is stuck in the SD write (here: open
// of a FIFO without a reader blocks). What the thread had is written, then
// only the newest of the posts made meanwhile.
void TestPostNeverWaits() {
    const auto path = TempPath("fifo");
    std::filesystem::remove(path);
    Require(::mkfifo(path.c_str(), 0600) == 0, "mkfifo");
    StatusFileWriter writer(path);
    writer.Post(Heartbeat(600U));
    // Let the thread take the first text and block in open.
    for (int spin = 0; spin < 200; ++spin) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto start = std::chrono::steady_clock::now();
    for (uint64_t frame = 1200U; frame <= 600U * 100U; frame += 600U) writer.Post(Heartbeat(frame));
    const double postMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    Require(postMs < 200.0, "Post waited for the blocked write");
    Require(writer.Stats().Written == 0U, "the blocked write finished without a reader");
    // The two writes may reach one open of the FIFO or two: read until both are in.
    const std::string expected = Heartbeat(600U) + Heartbeat(600U * 100U);
    std::string received;
    std::thread reader([&] {
        while (received.size() < expected.size()) received += ReadAll(path);
    });
    writer.Flush();
    reader.join();
    Require(received == expected, "the in-flight text and then only the newest post must be written");
    const auto stats = writer.Stats();
    Require(stats.Written == 2U && stats.Superseded == 98U, "superseded posts miscounted");
    writer.Stop();
    std::filesystem::remove(path);
    std::printf("post never waits: ok (99 posts in %.3f ms with the writer blocked)\n", postMs);
}

// Stop writes the text still waiting; a post after Stop is written at once.
void TestStop() {
    const auto path = TempPath("stop");
    std::filesystem::remove(path);
    StatusFileWriter writer(path);
    writer.Post(Heartbeat(600U));
    writer.Post(Heartbeat(1200U));
    writer.Stop();
    Require(ReadAll(path) == Heartbeat(1200U), "Stop lost the waiting text");
    writer.Post(Heartbeat(1800U));
    Require(ReadAll(path) == Heartbeat(1800U), "a post after Stop was not written");
    Require(writer.Stats().SyncWrites == 1U, "a post after Stop is a synchronous write");
    std::filesystem::remove(path);
    std::printf("stop: ok\n");
}

// Off the Switch the switch reads TRIAEVUM_ASYNC_STATUS_WRITES (absent = off).
void TestSwitch() {
    ::unsetenv("TRIAEVUM_ASYNC_STATUS_WRITES");
    Require(!ReadNativeAsyncStatusWrites(), "desktop default is off");
    ::setenv("TRIAEVUM_ASYNC_STATUS_WRITES", "1", 1);
    Require(ReadNativeAsyncStatusWrites(), "1 turns it on");
    ::setenv("TRIAEVUM_ASYNC_STATUS_WRITES", "0", 1);
    Require(!ReadNativeAsyncStatusWrites(), "0 turns it off");
    ::unsetenv("TRIAEVUM_ASYNC_STATUS_WRITES");
    std::printf("switch: ok\n");
}

} // namespace

int main() {
    TestLatestWins();
    TestWriteNowWins();
    TestPostNeverWaits();
    TestStop();
    TestSwitch();
    std::printf("oot3d_native_status_file_tests: ok\n");
    return 0;
}
