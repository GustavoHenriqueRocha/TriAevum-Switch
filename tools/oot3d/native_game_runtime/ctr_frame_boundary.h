#pragma once

// Fronteira de quadro do jogo (onde termina um frame do título), separada de
// COMO ela é detectada.
//
// Duas fontes:
//  - Oot3dGuestPcs: a de sempre do Zelda. GameState_Update (0x00417014) fecha
//    o quadro de simulação e adianta o NativeGameplayClock; os adaptadores do
//    limitador de quadros (0x003FD27C/0x003FD3A8/0x003FD3B8, contador de vsync
//    0x0054CC3C) continuam presos a ela. Só vale para o code.bin do OoT3D.
//  - GspPresent: genérica, para qualquer título. Um quadro termina quando o
//    jogo PUBLICA um framebuffer novo e o VBlank seguinte o apresenta. A
//    publicação chega ao host por um de dois caminhos, ambos tratados pelo
//    host GSP (oot3d_native_a32_ctr_host_gsp.cpp / _ctr_host.cpp):
//      * nngx/SDK da Nintendo (OoT3D, Super Mario 3D Land...): o jogo escreve
//        o framebuffer info na shared memory do GSP (0x200 + (2*thread +
//        tela)*0x40) e liga o bit "dirty"; o GSP consome no PDC (VBlank) da
//        tela (ApplyGspFramebufferUpdate);
//      * libctru/homebrew e alguns títulos: gsp::Gpu SetBufferSwap direto.
//    O provedor marca "commit pendente" em qualquer um dos dois e, no VBlank,
//    se havia commit, conta UMA fronteira (topo e baixo no mesmo VBlank são o
//    mesmo quadro). Assim a fronteira fica alinhada ao vsync em que o quadro
//    aparece, como no console.
//
// Por que não outros sinais:
//  - esperar o evento de VBlank: jogos a 30 Hz esperam 2 VBlanks por quadro,
//    jogos com thread de render esperam em outra thread, e loading/menus
//    esperam sem apresentar nada; contar esperas não é contar quadros;
//  - TriggerCmdReqQueue/FlushDataCache: vários por quadro (o Mario faz ~5,6
//    TriggerCmdReqQueue por VBlank no boot) e sem marca de "fim";
//  - DisplayTransfer (PPF): é a cópia do render para o framebuffer, mas há
//    jogos que fazem várias (tela de baixo, capturas, efeitos) por quadro.
//    O commit do framebuffer é o único sinal que é, por definição, 1 por
//    quadro apresentado e não depende de endereço nenhum do code.bin.
//
// O perfil de título (ctr_title_profile.h) escolhe a fonte: OoT3D usa os PCs
// (comportamento byte a byte igual), qualquer outro título usa o GSP.

#include "ctr_title_profile.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace Oot3dNativeGame {

// PCs do OoT3D que a fonte Oot3dGuestPcs precisa observar na entrada de
// bloco. Iguais aos de oot3d_native_frame_rate.h (repetidos aqui para o
// provedor não puxar o NativeFrameRatePolicy inteiro; o teste compara).
inline constexpr uint32_t kOot3dFrameBoundaryGameStateUpdatePc = 0x00417014U;
inline constexpr uint32_t kOot3dFrameBoundaryPacingBeginPc = 0x003FD27CU;
inline constexpr uint32_t kOot3dFrameBoundaryPacingDecisionPc = 0x003FD3A8U;
inline constexpr uint32_t kOot3dFrameBoundaryPacingCommitPc = 0x003FD3B8U;

enum class FrameBoundaryCommitKind : uint8_t {
    // gsp::Gpu SetBufferSwap (IPC).
    SetBufferSwap,
    // Framebuffer info "dirty" na shared memory do GSP, consumido no PDC.
    SharedMemoryInfo,
};

constexpr const char* FrameBoundarySourceName(
    CtrFrameBoundarySource source) noexcept {
    switch (source) {
    case CtrFrameBoundarySource::Oot3dGuestPcs:
        return "oot3d_guest_pcs";
    case CtrFrameBoundarySource::GspPresent:
        return "gsp_present";
    }
    return "unknown";
}

struct FrameBoundaryStats {
    // Fronteiras de quadro detectadas.
    uint64_t Boundaries = 0;
    // VBlanks entregues ao jogo enquanto o provedor observava.
    uint64_t VBlanks = 0;
    // Commits de framebuffer por tela (0 = topo, 1 = baixo) e por caminho.
    std::array<uint64_t, 2> ScreenCommits{};
    uint64_t SetBufferSwapCommits = 0;
    uint64_t SharedMemoryCommits = 0;
    // VBlank (1 = primeiro) em que caiu a primeira/última fronteira; 0 = nenhuma.
    uint64_t FirstBoundaryVBlank = 0;
    uint64_t LastBoundaryVBlank = 0;
    // Distância, em VBlanks, entre fronteiras consecutivas (ritmo do jogo:
    // 1 = 60 quadros/s, 2 = 30 quadros/s). Histograma: [1], [2], [3], [4], [5+].
    uint64_t MinimumIntervalVBlanks = 0;
    uint64_t MaximumIntervalVBlanks = 0;
    std::array<uint64_t, 5> IntervalHistogram{};

    // Quadros por VBlank entre a primeira e a última fronteira (0 sem duas).
    [[nodiscard]] double BoundariesPerVBlank() const noexcept {
        if (Boundaries < 2U || LastBoundaryVBlank <= FirstBoundaryVBlank) {
            return 0.0;
        }
        return static_cast<double>(Boundaries - 1U) /
               static_cast<double>(LastBoundaryVBlank - FirstBoundaryVBlank);
    }
};

class FrameBoundaryProvider {
  public:
    virtual ~FrameBoundaryProvider() = default;

    [[nodiscard]] virtual CtrFrameBoundarySource Source() const noexcept = 0;
    // PCs que precisam de gancho de entrada de bloco (vazio no GSP).
    [[nodiscard]] virtual std::span<const uint32_t>
    GuestHookPcs() const noexcept = 0;
    // Entrada de um bloco ganchado. true = este PC fecha um quadro.
    virtual bool ObserveGuestBlock(uint32_t pc) noexcept = 0;
    // O jogo publicou um framebuffer (tela 0 = topo, 1 = baixo).
    virtual void ObserveFramebufferCommit(uint32_t screen,
                                          FrameBoundaryCommitKind kind) noexcept {
        if (screen < mStats.ScreenCommits.size()) {
            ++mStats.ScreenCommits[screen];
        }
        if (kind == FrameBoundaryCommitKind::SetBufferSwap) {
            ++mStats.SetBufferSwapCommits;
        } else {
            ++mStats.SharedMemoryCommits;
        }
    }
    // Um VBlank (PDC0 + PDC1) foi entregue. true = fronteira neste VBlank.
    virtual bool ObserveVBlank() noexcept {
        ++mStats.VBlanks;
        return false;
    }

    [[nodiscard]] const FrameBoundaryStats& Stats() const noexcept {
        return mStats;
    }

  protected:
    void RecordBoundary() noexcept {
        ++mStats.Boundaries;
        const uint64_t vblank = mStats.VBlanks;
        if (mStats.Boundaries == 1U) {
            mStats.FirstBoundaryVBlank = vblank;
        } else if (vblank > mStats.LastBoundaryVBlank) {
            const uint64_t interval = vblank - mStats.LastBoundaryVBlank;
            if (mStats.MinimumIntervalVBlanks == 0U ||
                interval < mStats.MinimumIntervalVBlanks) {
                mStats.MinimumIntervalVBlanks = interval;
            }
            if (interval > mStats.MaximumIntervalVBlanks) {
                mStats.MaximumIntervalVBlanks = interval;
            }
            const uint64_t bucket =
                interval >= mStats.IntervalHistogram.size()
                    ? mStats.IntervalHistogram.size() - 1U
                    : interval - 1U;
            ++mStats.IntervalHistogram[bucket];
        }
        mStats.LastBoundaryVBlank = vblank;
    }

    FrameBoundaryStats mStats;
};

// OoT3D: a fronteira é a entrada de GameState_Update. Os PCs do limitador
// entram na lista de ganchos porque a política de frame rate do Zelda os usa
// (oot3d_native_a32_projection_policy.cpp); eles não fecham quadro.
class Oot3dGuestPcFrameBoundaryProvider final : public FrameBoundaryProvider {
  public:
    static constexpr std::array<uint32_t, 4> kHookPcs{
        kOot3dFrameBoundaryGameStateUpdatePc,
        kOot3dFrameBoundaryPacingBeginPc,
        kOot3dFrameBoundaryPacingDecisionPc,
        kOot3dFrameBoundaryPacingCommitPc,
    };

    [[nodiscard]] CtrFrameBoundarySource Source() const noexcept override {
        return CtrFrameBoundarySource::Oot3dGuestPcs;
    }
    [[nodiscard]] std::span<const uint32_t>
    GuestHookPcs() const noexcept override {
        return kHookPcs;
    }
    bool ObserveGuestBlock(uint32_t pc) noexcept override {
        if (pc != kOot3dFrameBoundaryGameStateUpdatePc) {
            return false;
        }
        RecordBoundary();
        return true;
    }
};

// Genérica: commit de framebuffer apresentado no VBlank seguinte.
class GspPresentFrameBoundaryProvider final : public FrameBoundaryProvider {
  public:
    [[nodiscard]] CtrFrameBoundarySource Source() const noexcept override {
        return CtrFrameBoundarySource::GspPresent;
    }
    [[nodiscard]] std::span<const uint32_t>
    GuestHookPcs() const noexcept override {
        return {};
    }
    bool ObserveGuestBlock(uint32_t) noexcept override { return false; }
    void ObserveFramebufferCommit(uint32_t screen,
                                  FrameBoundaryCommitKind kind) noexcept override {
        FrameBoundaryProvider::ObserveFramebufferCommit(screen, kind);
        mCommitPending = true;
    }
    bool ObserveVBlank() noexcept override {
        FrameBoundaryProvider::ObserveVBlank();
        if (!mCommitPending) {
            return false;
        }
        mCommitPending = false;
        RecordBoundary();
        return true;
    }

  private:
    bool mCommitPending = false;
};

inline std::unique_ptr<FrameBoundaryProvider>
MakeFrameBoundaryProvider(CtrFrameBoundarySource source) {
    if (source == CtrFrameBoundarySource::GspPresent) {
        return std::make_unique<GspPresentFrameBoundaryProvider>();
    }
    return std::make_unique<Oot3dGuestPcFrameBoundaryProvider>();
}

inline std::unique_ptr<FrameBoundaryProvider>
MakeFrameBoundaryProvider(const CtrTitleProfile& profile) {
    return MakeFrameBoundaryProvider(profile.FrameBoundary);
}

} // namespace Oot3dNativeGame
