// config/async_status_writes (oot3d_native_status_file.h): o heartbeat do
// boot-status.txt escrito por uma thread de serviço. O arquivo termina sempre
// com o texto mais novo; Post nunca espera o arquivo (escritor bloqueado no
// open de um FIFO); um WriteNow síncrono (etapa do boot, completed/failed)
// nunca é sobrescrito por um heartbeat postado antes; Stop escreve o que
// ainda espera.
//
// Portado de JohnUzoka/TriAevum-Switch a85cb85d.
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
        std::fprintf(stderr, "FALHA: %s\n", message);
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

// O heartbeat a cada 600 quadros: o arquivo é o último, todo post é escrito
// ou substituído.
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
        Require(ReadAll(path) == Heartbeat(600U * 2000U), "o arquivo não é o heartbeat mais novo");
        const auto stats = writer.Stats();
        Require(stats.Posted == 2000U, "posts não contados");
        Require(stats.Written + stats.Superseded == stats.Posted, "um post não foi escrito nem substituído");
        Require(stats.Written >= 1U && stats.SyncWrites == 0U, "nenhuma escrita contada");
        // Um texto mais curto substitui o arquivo (truncado, não sobrescrito no lugar).
        writer.Post("status=launching\n");
        writer.Flush();
        Require(ReadAll(path) == "status=launching\n", "um texto mais curto deixou o rabo do antigo");
    }
    std::filesystem::remove(path);
    std::printf("latest wins: ok\n");
}

// O estado final (WriteNow) nunca é sobrescrito por um heartbeat postado
// antes, esteja ele esperando ou sendo escrito.
void TestWriteNowWins() {
    const auto path = TempPath("final");
    for (int round = 0; round < 300; ++round) {
        std::filesystem::remove(path);
        StatusFileWriter writer(path);
        for (int post = 0; post < 1 + round % 7; ++post) writer.Post(Heartbeat(600U * (post + 1U)));
        if (round % 3 == 0) std::this_thread::yield();
        const std::string final = "status=failed\ndetail=round " + std::to_string(round) + '\n';
        Require(writer.WriteNow(final), "a escrita síncrona falhou");
        writer.Flush();
        writer.Stop();
        Require(ReadAll(path) == final, "um heartbeat postado antes sobrescreveu o estado final");
        const auto stats = writer.Stats();
        Require(stats.Written + stats.Superseded == stats.Posted, "contagem de posts depois do WriteNow");
        Require(stats.SyncWrites == 1U, "WriteNow não contado");
    }
    std::filesystem::remove(path);
    std::printf("write now wins: ok\n");
}

// Post volta enquanto a thread de serviço está presa na escrita no SD (aqui:
// open de um FIFO sem leitor bloqueia). O que a thread tinha é escrito, depois
// só o mais novo dos posts feitos no meio-tempo.
void TestPostNeverWaits() {
    const auto path = TempPath("fifo");
    std::filesystem::remove(path);
    Require(::mkfifo(path.c_str(), 0600) == 0, "mkfifo");
    StatusFileWriter writer(path);
    writer.Post(Heartbeat(600U));
    // Deixa a thread pegar o primeiro texto e bloquear no open.
    for (int spin = 0; spin < 200; ++spin) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto start = std::chrono::steady_clock::now();
    for (uint64_t frame = 1200U; frame <= 600U * 100U; frame += 600U) writer.Post(Heartbeat(frame));
    const double postMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    Require(postMs < 200.0, "Post esperou a escrita bloqueada");
    Require(writer.Stats().Written == 0U, "a escrita bloqueada terminou sem leitor");
    // As duas escritas podem chegar num open do FIFO ou em dois: lê até as duas chegarem.
    const std::string expected = Heartbeat(600U) + Heartbeat(600U * 100U);
    std::string received;
    std::thread reader([&] {
        while (received.size() < expected.size()) received += ReadAll(path);
    });
    writer.Flush();
    reader.join();
    Require(received == expected, "deve ser escrito o texto em andamento e depois só o post mais novo");
    const auto stats = writer.Stats();
    Require(stats.Written == 2U && stats.Superseded == 98U, "posts substituídos contados errado");
    writer.Stop();
    std::filesystem::remove(path);
    std::printf("post never waits: ok (99 posts em %.3f ms com o escritor bloqueado)\n", postMs);
}

// Stop escreve o texto que ainda espera; um post depois do Stop é escrito na hora.
void TestStop() {
    const auto path = TempPath("stop");
    std::filesystem::remove(path);
    StatusFileWriter writer(path);
    writer.Post(Heartbeat(600U));
    writer.Post(Heartbeat(1200U));
    writer.Stop();
    Require(ReadAll(path) == Heartbeat(1200U), "Stop perdeu o texto que esperava");
    writer.Post(Heartbeat(1800U));
    Require(ReadAll(path) == Heartbeat(1800U), "um post depois do Stop não foi escrito");
    Require(writer.Stats().SyncWrites == 1U, "um post depois do Stop é uma escrita síncrona");
    std::filesystem::remove(path);
    std::printf("stop: ok\n");
}

// Fora do Switch a chave lê TRIAEVUM_ASYNC_STATUS_WRITES (ausente = desligada).
void TestSwitch() {
    ::unsetenv("TRIAEVUM_ASYNC_STATUS_WRITES");
    Require(!ReadNativeAsyncStatusWrites(), "no desktop o padrão é desligado");
    ::setenv("TRIAEVUM_ASYNC_STATUS_WRITES", "1", 1);
    Require(ReadNativeAsyncStatusWrites(), "1 liga");
    ::setenv("TRIAEVUM_ASYNC_STATUS_WRITES", "0", 1);
    Require(!ReadNativeAsyncStatusWrites(), "0 desliga");
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
