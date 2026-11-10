#pragma once

// Arquivo de status pequeno, substituído e não acrescentado (boot-status.txt),
// com o texto mais novo escrito por uma thread de serviço
// (config/async_status_writes; SWITCH_PORT.md "Heartbeat do boot-status.txt
// fora da thread do jogo").
//
// A thread do jogo reescrevia o boot-status.txt a cada 600 quadros do host:
// um open/truncate/write/close no SD dentro do quadro, 30-80 ms de thread do
// jogo sem tempo de guest (hitches.txt: todo present 600k-3). Post entrega o
// texto e volta; um texto ainda não escrito é trocado pelo mais novo (só o
// último estado importa). WriteNow escreve na thread que chama, depois da
// escrita em andamento, e descarta um texto postado que ainda espera: as
// etapas do boot e o estado final completed/failed nunca são sobrescritos
// por um heartbeat mais velho.
//
// Toda escrita do boot-status.txt do processo passa por
// SwitchBootStatusFile() (oot3d_native_a32_window.cpp, oot3d_native_game.cpp).
// Testes: oot3d_native_status_file_tests.
//
// Portado de JohnUzoka/TriAevum-Switch a85cb85d.

#include "oot3d_native_offload.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace Oot3dNativeGame {

struct StatusFileWriterStats {
    uint64_t Posted = 0U;            // textos entregues à thread de serviço
    uint64_t Written = 0U;           // desses, escritos (os outros foram substituídos)
    uint64_t Superseded = 0U;        // trocados por um texto mais novo antes da escrita
    uint64_t SyncWrites = 0U;        // chamadas de WriteNow
    uint64_t WriteNanoseconds = 0U;  // thread de serviço: open, write, close
    uint64_t SyncNanoseconds = 0U;   // WriteNow: espera da escrita em andamento + a própria
};

class StatusFileWriter final {
  public:
    explicit StatusFileWriter(std::string path) : mPath(std::move(path)) {}
    ~StatusFileWriter() { Stop(); }
    StatusFileWriter(const StatusFileWriter&) = delete;
    StatusFileWriter& operator=(const StatusFileWriter&) = delete;

    // Entrega o texto à thread de serviço; nunca espera o arquivo. Depois do
    // Stop escreve aqui mesmo (WriteNow).
    void Post(std::string text) {
        bool taken = false, notify = false;
        {
            std::lock_guard lock(mMutex);
            if (!mStopped) {
                if (!mThread.joinable()) mThread = std::thread([this] { Loop(); });
                if (mPending.has_value()) ++mSuperseded;
                mPending = std::move(text);
                ++mPosted;
                taken = true;
                notify = mWaiting;
            }
        }
        if (notify) mReady.notify_one();
        if (!taken) (void)WriteNow(text);
    }

    // Escreve nesta thread quando a escrita em andamento (se houver) termina;
    // um texto postado que ainda espera é descartado (este é mais novo).
    // False quando o arquivo não pôde ser escrito.
    bool WriteNow(std::string_view text) {
        const auto start = std::chrono::steady_clock::now();
        std::lock_guard file(mFileMutex);
        {
            std::lock_guard lock(mMutex);
            if (mPending.has_value()) {
                mPending.reset();
                ++mSuperseded;
                mIdle.notify_all();
            }
        }
        const bool written = WriteFile(mPath, text);
        mSyncWrites.fetch_add(1U, std::memory_order_relaxed);
        mSyncNanoseconds.fetch_add(NanosecondsSince(start), std::memory_order_relaxed);
        return written;
    }

    // Espera até não haver texto postado esperando nem sendo escrito.
    void Flush() {
        std::unique_lock lock(mMutex);
        mIdle.wait(lock, [&] { return !mPending.has_value() && !mWriting; });
    }

    // Escreve o texto postado que ainda espera e junta a thread (idempotente).
    void Stop() {
        {
            std::lock_guard lock(mMutex);
            mStopped = true;
        }
        mReady.notify_all();
        if (mThread.joinable()) mThread.join();
    }

    [[nodiscard]] StatusFileWriterStats Stats() const {
        StatusFileWriterStats stats;
        {
            std::lock_guard lock(mMutex);
            stats.Posted = mPosted;
            stats.Superseded = mSuperseded;
        }
        stats.Written = mWritten.load(std::memory_order_relaxed);
        stats.SyncWrites = mSyncWrites.load(std::memory_order_relaxed);
        stats.WriteNanoseconds = mWriteNanoseconds.load(std::memory_order_relaxed);
        stats.SyncNanoseconds = mSyncNanoseconds.load(std::memory_order_relaxed);
        return stats;
    }

    [[nodiscard]] const std::string& Path() const noexcept { return mPath; }

  private:
    static uint64_t NanosecondsSince(std::chrono::steady_clock::time_point start) {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - start)
                                         .count());
    }

    static bool WriteFile(const std::string& path, std::string_view text) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.close();
        return !output.fail();
    }

    // Ordem dos locks: mFileMutex, depois mMutex (aqui e no WriteNow). O texto
    // só sai de mPending com mFileMutex na mão, então um WriteNow posterior
    // nunca é sobrescrito por um texto tirado antes dele.
    void Loop() {
        std::unique_lock lock(mMutex);
        for (;;) {
            if (!mPending.has_value()) {
                if (mStopped) return;
                mWaiting = true;
                mReady.wait(lock, [&] { return mStopped || mPending.has_value(); });
                mWaiting = false;
                continue;
            }
            lock.unlock();
            {
                std::lock_guard file(mFileMutex);
                std::optional<std::string> text;
                {
                    std::lock_guard again(mMutex);
                    text.swap(mPending);  // um WriteNow pode ter descartado no meio-tempo
                    mWriting = text.has_value();
                }
                if (text.has_value()) {
                    const auto start = std::chrono::steady_clock::now();
                    (void)WriteFile(mPath, *text);
                    mWriteNanoseconds.fetch_add(NanosecondsSince(start), std::memory_order_relaxed);
                    mWritten.fetch_add(1U, std::memory_order_relaxed);
                }
            }
            lock.lock();
            mWriting = false;
            mIdle.notify_all();
        }
    }

    std::string mPath;
    std::mutex mFileMutex;
    mutable std::mutex mMutex;
    std::condition_variable mReady;
    std::condition_variable mIdle;
    std::optional<std::string> mPending;
    std::thread mThread;
    bool mWaiting = false;
    bool mWriting = false;
    bool mStopped = false;
    uint64_t mPosted = 0U;
    uint64_t mSuperseded = 0U;
    std::atomic<uint64_t> mWritten{0};
    std::atomic<uint64_t> mSyncWrites{0};
    std::atomic<uint64_t> mWriteNanoseconds{0};
    std::atomic<uint64_t> mSyncNanoseconds{0};
};

inline constexpr const char* kSwitchBootStatusPath = "sdmc:/switch/oot3dre/boot-status.txt";

// O boot-status.txt do processo (Switch): as etapas do boot, o heartbeat e o
// estado final passam todos por este único escritor.
inline StatusFileWriter& SwitchBootStatusFile() {
    static StatusFileWriter writer(kSwitchBootStatusPath);
    return writer;
}

// config/async_status_writes: o heartbeat do boot-status.txt (a cada 600
// quadros do host) escrito por uma thread de serviço. Switch: arquivo ausente
// = ligado, "0" = escrito na thread do jogo como antes. Desktop:
// TRIAEVUM_ASYNC_STATUS_WRITES=1 (desligado sem ela; lá nada posta).
inline bool ReadNativeAsyncStatusWrites() {
#if defined(__SWITCH__)
    std::ifstream input("sdmc:/switch/oot3dre/config/async_status_writes");
    if (!input) return true;
    std::string text;
    std::getline(input, text);
    return ParseOffloadSwitch(text);
#else
    const char* value = std::getenv("TRIAEVUM_ASYNC_STATUS_WRITES");
    return value != nullptr && std::string_view(value) != "" && ParseOffloadSwitch(value);
#endif
}

} // namespace Oot3dNativeGame
