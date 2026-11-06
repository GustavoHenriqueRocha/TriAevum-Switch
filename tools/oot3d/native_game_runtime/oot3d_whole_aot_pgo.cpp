// Profile collection of the instrumented whole-AOT build (see
// oot3d_whole_aot_gcov.h). Only does something with
// OOT3D_WHOLE_AOT_GCOV_GENERATE (OOT3D_WHOLE_AOT_PROFILE_MODE=generate, GCC).
#include "oot3d_whole_aot_gcov.h"

#if defined(OOT3D_WHOLE_AOT_GCOV_GENERATE) && defined(__SWITCH__)

#include <switch.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <vector>

extern "C" {  // GCC's gcov.h has no C++ guard of its own
#include <gcov.h>
}

// One pointer per instrumented object, in the section GNU ld brackets with
// these two symbols (-fprofile-info-section=gcov_info; a section referenced
// by __start_/__stop_ is kept by --gc-sections).
extern "C" const struct gcov_info* const __start_gcov_info[];
extern "C" const struct gcov_info* const __stop_gcov_info[];

namespace Oot3dNativeGame {
namespace {

std::mutex gDumpMutex;  // one dump at a time (the thread, the exit)
std::string gStream;    // this session's stream; empty = collection off
std::string gProblem;   // why it is off
std::mutex gThreadMutex;
std::condition_variable gWake;
bool gStop = false;
bool gDumpRequested = false;
std::thread gThread;
AppletHookCookie gHook{};

struct Stream {
    std::FILE* File = nullptr;
    bool Ok = true;
    uint64_t Bytes = 0;
    std::vector<std::unique_ptr<unsigned char[]>> Allocations;  // libgcov's scratch buffers
};

void Write(const void* data, unsigned length, void* argument) {
    auto* stream = static_cast<Stream*>(argument);
    if (stream->Ok && length != 0U && std::fwrite(data, 1, length, stream->File) != length)
        stream->Ok = false;
    stream->Bytes += length;
}

void Filename(const char* name, void* argument) {
    __gcov_filename_to_gcfn(name, Write, argument);
}

void* Allocate(unsigned length, void* argument) {
    auto* stream = static_cast<Stream*>(argument);
    stream->Allocations.push_back(std::make_unique<unsigned char[]>(length != 0U ? length : 1U));
    return stream->Allocations.back().get();
}

size_t ObjectCount() {
    const gcov_info* const* begin = __start_gcov_info;
    const gcov_info* const* end = __stop_gcov_info;
    __asm__("" : "+r"(begin), "+r"(end));  // the compiler must not reason about linker symbols
    return static_cast<size_t>(end - begin);
}

// Appends one line to pgo/dumps.txt: when, why, size and time of each dump
// (the logger may already be gone at exit).
void Note(const char* why, uint64_t bytes, double milliseconds, bool ok) {
    const std::string path = std::string(OOT3D_WHOLE_AOT_GCOV_DIR) + "/dumps.txt";
    if (std::FILE* file = std::fopen(path.c_str(), "a")) {
        std::fprintf(file, "%s %s bytes=%llu ms=%.0f%s\n", gStream.c_str(), why,
                     static_cast<unsigned long long>(bytes), milliseconds, ok ? "" : " FAILED");
        std::fclose(file);
    }
}

// The whole session so far into the .tmp, which then replaces the stream.
void DumpNow(const char* why) {
    std::lock_guard<std::mutex> lock(gDumpMutex);
    if (gStream.empty()) return;
    const u64 start = armGetSystemTick();
    const std::string temporary = gStream + ".tmp";
    Stream stream;
    stream.File = std::fopen(temporary.c_str(), "wb");
    if (stream.File == nullptr) {
        Note(why, 0, 0.0, false);
        return;
    }
    static std::vector<char> buffer(1U << 20U);  // one SD write per MB, not per record
    std::setvbuf(stream.File, buffer.data(), _IOFBF, buffer.size());
    const gcov_info* const* info = __start_gcov_info;
    const gcov_info* const* const end = __stop_gcov_info;
    __asm__("" : "+r"(info));
    for (; info != end && stream.Ok; ++info) {
        if (*info != nullptr) __gcov_info_to_gcda(*info, Filename, Write, Allocate, &stream);
    }
    const bool closed = std::fclose(stream.File) == 0;
    bool ok = stream.Ok && closed;
    if (ok) {
        // Horizon does not rename over an existing file: remove it first. A
        // cut between the two leaves only a .tmp, which the merge script
        // skips (it cannot tell it from a first dump cut short).
        std::remove(gStream.c_str());
        ok = std::rename(temporary.c_str(), gStream.c_str()) == 0;
    }
    Note(why, stream.Bytes,
         static_cast<double>(armTicksToNs(armGetSystemTick() - start)) / 1.0e6, ok);
}

void ThreadLoop() {
    std::unique_lock<std::mutex> lock(gThreadMutex);
    while (!gStop) {
        const bool woken = gWake.wait_for(lock, std::chrono::seconds(kOot3dWholeAotPgoPeriodSeconds),
                                          [] { return gStop || gDumpRequested; });
        if (gStop) break;
        const char* why = woken ? "focus_lost" : "periodic";
        gDumpRequested = false;
        lock.unlock();
        DumpNow(why);
        lock.lock();
    }
}

// Applet hook (game thread, inside SDL's appletMainLoop): only wakes the
// thread, the game thread never writes.
void OnAppletHook(AppletHookType hook, void*) {
    if (hook != AppletHookType_OnFocusState ||
        appletGetFocusState() == AppletFocusState_InFocus)
        return;
    {
        std::lock_guard<std::mutex> lock(gThreadMutex);
        gDumpRequested = true;
    }
    gWake.notify_one();
}

// atexit: every return from main and every exit() (not a HOME close, which
// kills the process: the focus-loss dump covers that).
void StopAtExit() {
    {
        std::lock_guard<std::mutex> lock(gThreadMutex);
        gStop = true;
    }
    gWake.notify_all();
    if (gThread.joinable()) gThread.join();
    DumpNow("exit");
}

bool Exists(const std::string& path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0;
}

} // namespace

void Oot3dWholeAotPgoStart() {
    static bool started = false;
    if (started) return;
    started = true;
    const std::string directory = OOT3D_WHOLE_AOT_GCOV_DIR;
    for (size_t slash = directory.find('/', 1);; slash = directory.find('/', slash + 1)) {
        const std::string prefix = directory.substr(0, slash);
        if (!prefix.empty() && prefix.back() != ':' &&  // "sdmc:" is the device
            mkdir(prefix.c_str(), 0777) != 0 && errno != EEXIST) {
            gProblem = "cannot create " + prefix;
            return;
        }
        if (slash == std::string::npos) break;
    }
    if (ObjectCount() == 0U) {
        gProblem = "no instrumented object in the gcov_info section";
        return;
    }
    const uint32_t session = Oot3dWholeAotPgoNextSession(directory, Exists);
    if (session == 0U) {
        gProblem = "no free session number in " + directory;
        return;
    }
    gStream = Oot3dWholeAotPgoStreamPath(directory, session);
    // A pthread: oot3d_switch_threads.cpp makes it a service thread (0x3B on
    // cores 1-2), off the game's core.
    gThread = std::thread(ThreadLoop);
    appletHook(&gHook, OnAppletHook, nullptr);
    std::atexit(StopAtExit);
}

std::string Oot3dWholeAotPgoDescribe() {
    if (gStream.empty()) return "pgo: instrumented build, counts NOT saved: " + gProblem;
    return "pgo: instrumented build, " + std::to_string(ObjectCount()) +
           " objects streamed to " + gStream + " every " +
           std::to_string(kOot3dWholeAotPgoPeriodSeconds) +
           " s, on focus loss and at exit (dump log: " OOT3D_WHOLE_AOT_GCOV_DIR "/dumps.txt)";
}

} // namespace Oot3dNativeGame

#else

namespace Oot3dNativeGame {

void Oot3dWholeAotPgoStart() {
}

std::string Oot3dWholeAotPgoDescribe() {
    return {};
}

} // namespace Oot3dNativeGame

#endif
