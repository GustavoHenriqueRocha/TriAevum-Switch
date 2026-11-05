#pragma once

// Despejo periódico do estado do guest no stderr, para descobrir em que cada
// thread está esperando quando um jogo "trava" (tela parada, nenhum erro).
//
// Ligado por TRIAEVUM_GUEST_STATE_DUMP=<N> (a cada N VBlanks; 0, vazio ou
// texto inválido = desligado). Cada despejo imprime:
//
//   TRIAEVUM_GUEST_STATE vblank=V ticks=T svc_total=S svc_delta=D pad=0x...
//       threads=N ipc_counts=on gsp_shared=0x... (0 = GSP ainda sem mapa)
//   TRIAEVUM_GUEST_THREAD vblank=V id=I status=waiting prio=0x.. pc=0x.. lr=0x..
//       sp=0x.. last_svc_pc=0x.. last_svc="WaitSynchronizationN:any:..."
//   TRIAEVUM_GUEST_IPC vblank=V delta=D total=T name="SendSyncRequest:..."
//   TRIAEVUM_GUEST_MEM vblank=V addr=0x... words=0x........ 0x........ ...
//
// As linhas MEM vêm de TRIAEVUM_GUEST_STATE_MEM=<hex>:<hex>[,<hex>:<hex>]
// (endereço:tamanho em bytes, até 0x100 por faixa; palavras de 32 bits).
//
// As linhas IPC trazem só os comandos que mudaram desde o despejo anterior e
// exigem a contagem por nome no host (NativeA32CtrHostConfig::CollectIpcNameCounts
// ou ProfileRuntime); sem ela o cabeçalho diz ipc_counts=off.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Oot3dNativeGame {

class NativeA32Process;
class NativeA32CtrHostServices;

std::optional<uint32_t> ParseGuestStateDumpInterval(const char *text);

struct GuestMemoryRange {
    uint32_t Address = 0;
    uint32_t Size = 0;
};

// "41cfa0:40,41d218:4" -> faixas; vazio ou qualquer item inválido = nenhuma.
std::vector<GuestMemoryRange> ParseGuestMemoryRanges(const char *text);

class GuestStateDumper final {
  public:
    explicit GuestStateDumper(uint32_t intervalVblanks)
        : mInterval(intervalVblanks) {}

    void SetMemoryRanges(std::vector<GuestMemoryRange> ranges) {
        mMemoryRanges = std::move(ranges);
    }

    bool Due(uint64_t vblank) const noexcept {
        return mInterval != 0U && vblank != 0U && vblank % mInterval == 0U;
    }

    // Uma chamada = um despejo; guarda as contagens para o delta seguinte.
    std::string Format(const NativeA32Process &process,
                       const NativeA32CtrHostServices &host, uint64_t vblank,
                       uint32_t padButtons);

  private:
    uint32_t mInterval = 0;
    uint64_t mLastSvcTotal = 0;
    std::map<std::string, uint64_t> mLastIpcCounts;
    std::vector<GuestMemoryRange> mMemoryRanges;
};

} // namespace Oot3dNativeGame
