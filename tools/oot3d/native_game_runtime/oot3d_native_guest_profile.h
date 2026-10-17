#pragma once

// Perfil leve do código guest por função, para achar as funções que valem
// uma substituição nativa (oot3d_native_guest_overrides).
//
// Liga-se com um arquivo de configuração (no Switch,
// sdmc:/switch/oot3dre/config/guest_profile; no desktop, o caminho em
// TRIAEVUM_GUEST_PROFILE). Ligado, o callback de entrada de bloco passa a
// ser chamado em todos os blocos (modo "todos os blocos" que o runtime já
// suporta): ele conta a entrada do bloco, soma ao bloco anterior o tempo
// desde a última entrada (contador do sistema, 19,2 MHz no Switch) e repassa
// ao callback original só os PCs que eram ganchos, de modo que o jogo se
// comporta exatamente como sem o perfil. Desligado, nada muda no caminho
// quente.
//
// Saída periódica em logs/guest_profile.txt: histograma por função (entradas
// de bloco e tempo exclusivo), mais os blocos mais quentes. Os nomes vêm de
// config/guest_functions.txt (gerado por summarize_guest_profile.py
// --export-function-map); sem ele, a agregação usa só os PCs.
//
// Formato do arquivo de configuração: linhas "chave=valor", todas opcionais:
//   sample_every=4         mede 1 de cada N quadros do host (1 = todos); nos
//                          outros o callback original volta e o jogo roda na
//                          velocidade normal
//   dump_frames=900        quadros do host entre gravações (0 = nunca)
//   top_functions=250      linhas por função no relatório
//   top_blocks=400         linhas por bloco no relatório
//   window=0               1 = zera os contadores depois de cada gravação
//   output=<caminho>       arquivo do relatório
//   functions=<caminho>    mapa entrada->nome
//   max_gap_us=200         intervalo maior que isso não é atribuído a bloco
//                          (o guest saiu para o host: SVC, fim de quadro...)

#include "recomp/a32_runtime.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <span>
#include <string>
#include <vector>

namespace Oot3dNativeGame {

struct GuestProfileOptions {
    uint32_t SampleEveryFrames = 4U;
    uint32_t DumpEveryHostFrames = 900U;
    uint32_t TopFunctions = 250U;
    uint32_t TopBlocks = 400U;
    bool Window = false;
    uint32_t MaxGapMicroseconds = 200U;
    std::filesystem::path OutputPath;
    std::filesystem::path FunctionMapPath;
};

class GuestBlockProfiler {
public:
    // Lê o arquivo de configuração. false se ele não existe (perfil desligado).
    static bool LoadConfig(const std::filesystem::path& path,
                           const std::filesystem::path& defaultOutput,
                           const std::filesystem::path& defaultFunctionMap,
                           GuestProfileOptions* options);

    bool Start(const GuestProfileOptions& options, uint32_t codeBase,
               uint32_t codeSize);
    [[nodiscard]] bool Active() const noexcept { return mActive; }

    // Callback original e os PCs em que ele deve continuar sendo chamado.
    // forwardAll: o callback original já esperava todos os blocos.
    void Chain(oot3d::recomp::a32::BlockEntryCallback callback, void* user,
               std::span<const uint32_t> hooks, bool forwardAll);

    static void OnBlockEntry(uint32_t pc, oot3d::recomp::a32::GuestState& state,
                             oot3d::recomp::a32::MemoryBus& memory, void* user);

    // Para as substituições nativas: o perfil repassaria algum PC de
    // [first, last] ao callback original? (então elas recusam). Senão a
    // substituição roda e MarkNative conta uma entrada em `pc` e atribui a
    // ele o tempo até a próxima entrada de bloco (o tempo da versão nativa).
    [[nodiscard]] bool ForwardsRange(uint32_t first, uint32_t last) const noexcept;
    void MarkNative(uint32_t pc) noexcept;

    // Chamado uma vez por quadro do host; grava no intervalo configurado e
    // diz se o próximo quadro deve ser medido (callback do perfil instalado).
    bool OnHostFrame(uint64_t hostFrame);
    // O callback do perfil saiu ou voltou: não atribuir o intervalo.
    void Interrupt() noexcept { mLastSlot = UINT32_MAX; }
    bool WriteReport(uint64_t hostFrame);

    // Texto extra no fim do relatório (por exemplo, os contadores das
    // substituições nativas).
    void SetAppendix(std::function<void(std::ostream&)> appendix) {
        mAppendix = std::move(appendix);
    }

private:
    static uint64_t Ticks() noexcept;
    static uint64_t TicksPerSecond() noexcept;
    bool IsHook(uint32_t pc) const noexcept;
    void Note(uint32_t pc, uint64_t now) noexcept;
    void RegisterWithGuestOverrides() noexcept;
    void LoadFunctionMap();
    void Reset() noexcept;

    GuestProfileOptions mOptions;
    bool mActive = false;
    uint32_t mBase = 0U;
    uint32_t mSlots = 0U;
    std::vector<uint32_t> mCounts;
    std::vector<uint64_t> mTicks;
    uint64_t mTotalEntries = 0U;
    uint64_t mOutside = 0U;
    uint64_t mGaps = 0U;
    uint64_t mGapTicks = 0U;
    uint64_t mMaxGapTicks = 0U;
    uint64_t mLastTicks = 0U;
    uint32_t mLastSlot = UINT32_MAX;
    uint64_t mStartTicks = 0U;
    uint64_t mStartFrame = 0U;
    uint64_t mSampledFrames = 0U;
    bool mSampling = true;
    uint64_t mLastDumpFrame = 0U;
    bool mFrameSeen = false;

    oot3d::recomp::a32::BlockEntryCallback mCallback = nullptr;
    void* mCallbackUser = nullptr;
    bool mForwardAll = false;
    std::vector<uint32_t> mHooks;
    std::vector<uint64_t> mHookFilter;

    std::vector<std::pair<uint32_t, std::string>> mFunctions;
    bool mFunctionsLoaded = false;
    std::function<void(std::ostream&)> mAppendix;
};

} // namespace Oot3dNativeGame
