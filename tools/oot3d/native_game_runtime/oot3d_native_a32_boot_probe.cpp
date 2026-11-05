// Boot probe de um processo CTR: monta code.bin/ExHeader/RomFS pelo manifesto
// do processo (build_process_manifest.py), roda até a primeira espera e
// entrega VBlanks, e escreve o relatório JSON (SVCs, threads, PICA).
//
// Duas montagens:
//  - oot3d_native_a32_boot_probe: blocos A32 traduzidos do OoT3D
//    (oot3d_a32_generated, generate_aot.py);
//  - ctr_title_boot_probe (OOT3D_BOOT_PROBE_WHOLE_AOT): só o whole-AOT
//    (shards oot3d_whole_aot_shard_*.cpp de qualquer título), sem registro de
//    blocos. PC fora das funções traduzidas vira falha "MissingBlock" com o PC.
// O perfil de título (ctr_title_profile.h) vem do program id do ExHeader.
// Fronteira de quadro (ctr_frame_boundary.h): o provedor do perfil é o
// principal; o provedor GSP roda sempre (como sombra quando o perfil usa os
// PCs do OoT3D) para comparar contagem e ritmo. Sai em report.json
// ("frame_boundary") e no resumo do stderr.
#include "ctr_frame_boundary.h"
#include "ctr_title_profile.h"
#include "oot3d_native_a32_ctr_host.h"
#include "oot3d_native_a32_process_image.h"
#include "oot3d_native_pica_fragment_shader_gen.h"
#include "oot3d_native_pica_frontend.h"
#include "oot3d_native_pica_shader_gen.h"
#include "oot3d_native_pica_submission.h"
#include "oot3d_native_pica_vulkan_plan.h"

#if defined(OOT3D_BOOT_PROBE_WHOLE_AOT)
#include "oot3d_native_a32_memory.h"
#include "oot3d_native_compiled_functions.h"
#else
#include "oot3d_a32_generated.h"
#endif

#include <cstdio>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>
#ifdef OOT3D_NATIVE_HAS_SHADERC
#include <shaderc/shaderc.hpp>
#endif

namespace {

struct Arguments {
    std::filesystem::path Manifest;
    std::filesystem::path CodeBin;
    std::filesystem::path Output;
    std::filesystem::path SaveDataDirectory;
    uint32_t VblankCount = 2;
    // auto (program id do manifesto), oot3d ou generic.
    std::string TitleProfile = "auto";
    // --complete-gpu-work: o probe não tem GPU; sem isto, DisplayTransfer,
    // MemoryFill e P3D adiados para a GPU nunca concluem e o jogo para no
    // primeiro quadro esperando a interrupção (PPF/PSC/P3D). Ligado, antes de
    // cada VBlank o probe conclui o trabalho pendente (sem pixels: só as
    // interrupções, na ordem PSC, P3D, PPF) e deixa o jogo seguir, para
    // medir a fronteira de quadro. Os primeiros 512 draws ficam no relatório.
    bool CompleteGpuWork = false;
};

void PrintUsage() {
    std::cerr << "usage: oot3d_native_a32_boot_probe --manifest <process.json> "
                 "[--code-bin <code.bin>] [--output <report.json>] "
                 "[--save-data <directory>] [--vblanks <count>] "
                 "[--title-profile auto|oot3d|generic] "
                 "[--complete-gpu-work]\n";
}

std::optional<Arguments> ParseArguments(int argc, char** argv) {
    Arguments result;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--complete-gpu-work") {
            result.CompleteGpuWork = true;
            continue;
        }
        if (index + 1 >= argc) {
            return std::nullopt;
        }
        const std::filesystem::path value = argv[++index];
        if (option == "--manifest") {
            result.Manifest = value;
        } else if (option == "--code-bin") {
            result.CodeBin = value;
        } else if (option == "--output") {
            result.Output = value;
        } else if (option == "--title-profile") {
            result.TitleProfile = value.string();
            if (result.TitleProfile != "auto" &&
                result.TitleProfile != "oot3d" &&
                result.TitleProfile != "generic") {
                return std::nullopt;
            }
        } else if (option == "--save-data") {
            result.SaveDataDirectory = value;
        } else if (option == "--vblanks") {
            const auto count = std::stoul(value.string());
            if (count > 600U) {
                return std::nullopt;
            }
            result.VblankCount = static_cast<uint32_t>(count);
        } else {
            return std::nullopt;
        }
    }
    return result.Manifest.empty() ? std::nullopt
                                   : std::optional<Arguments>(result);
}

const char* RunKindName(Oot3dNativeGame::NativeA32ProcessRunKind kind) {
    switch (kind) {
    case Oot3dNativeGame::NativeA32ProcessRunKind::Yielded:
        return "yielded";
    case Oot3dNativeGame::NativeA32ProcessRunKind::Waiting:
        return "waiting";
    case Oot3dNativeGame::NativeA32ProcessRunKind::Terminated:
        return "terminated";
    case Oot3dNativeGame::NativeA32ProcessRunKind::Faulted:
        return "faulted";
    }
    return "unknown";
}

const char* ThreadStatusName(Oot3dNativeGame::NativeA32ThreadStatus status) {
    switch (status) {
    case Oot3dNativeGame::NativeA32ThreadStatus::Empty:
        return "empty";
    case Oot3dNativeGame::NativeA32ThreadStatus::Ready:
        return "ready";
    case Oot3dNativeGame::NativeA32ThreadStatus::Running:
        return "running";
    case Oot3dNativeGame::NativeA32ThreadStatus::Waiting:
        return "waiting";
    case Oot3dNativeGame::NativeA32ThreadStatus::Terminated:
        return "terminated";
    case Oot3dNativeGame::NativeA32ThreadStatus::Faulted:
        return "faulted";
    }
    return "unknown";
}

Oot3dNativeGame::NativeA32ProcessRunResult RunUntilGuestWait(
    Oot3dNativeGame::NativeA32Process& process) {
    auto result = process.Run();
    while (result.Kind ==
           Oot3dNativeGame::NativeA32ProcessRunKind::Yielded) {
        result = process.Run();
    }
    return result;
}

void AdvanceGuestClockTo(
    Oot3dNativeGame::NativeA32Process& process,
    Oot3dNativeGame::NativeA32CtrHostServices& hostServices,
    Oot3dNativeGame::NativeA32ProcessRunResult& result,
    uint64_t deadline) {
    if (hostServices.SystemTicks() > deadline) {
        throw std::runtime_error(
            "native CTR clock advanced beyond the probe deadline");
    }
    while (result.Kind !=
           Oot3dNativeGame::NativeA32ProcessRunKind::Terminated) {
        const auto wakeTick = hostServices.NextSleepWakeTick();
        if (!wakeTick.has_value() || *wakeTick > deadline) {
            break;
        }
        hostServices.AdvanceSystemTicks(*wakeTick - hostServices.SystemTicks());
        result = RunUntilGuestWait(process);
        if (result.Kind ==
            Oot3dNativeGame::NativeA32ProcessRunKind::Faulted) {
            return;
        }
    }
    if (hostServices.SystemTicks() < deadline) {
        hostServices.AdvanceSystemTicks(deadline - hostServices.SystemTicks());
    }
}

struct GpuWorkCompletionStats {
    uint64_t Rounds = 0;
    uint64_t Draws = 0;
    uint64_t MemoryFills = 0;
    uint64_t Completions = 0;
    uint64_t DisplayTransfers = 0;
    uint64_t InterruptsSignaled = 0;
};

// Conclui o trabalho que o frontend adiou para a GPU: só as interrupções que
// o jogo espera (a imagem não importa para o probe). true = sinalizou algo.
bool CompleteDeferredGpuWork(
    Oot3dNativeGame::Oot3dNativePicaSubmissionQueue& queue,
    Oot3dNativeGame::NativeA32CtrHostServices& hostServices,
    Oot3dNativeGame::NativeA32Memory& memory,
    std::vector<Oot3dNativeGame::Oot3dPicaDrawSubmission>& retainedDraws,
    GpuWorkCompletionStats& stats) {
    constexpr size_t kRetainedDraws = 512U;
    auto batch = queue.TakePendingBatch();
    if (batch.Empty()) {
        return false;
    }
    ++stats.Rounds;
    stats.Draws += batch.Draws.size();
    for (auto& draw : batch.Draws) {
        if (retainedDraws.size() >= kRetainedDraws) {
            break;
        }
        retainedDraws.push_back(std::move(draw));
    }
    bool signaled = false;
    const auto signal = [&](Oot3dNativeGame::Oot3dPicaInterruptId interrupt) {
        if (!hostServices.SignalPicaInterrupt(memory, interrupt)) {
            throw std::runtime_error(
                "deferred GPU completion could not enter the GSP relay queue");
        }
        ++stats.InterruptsSignaled;
        signaled = true;
    };
    for (const auto& fill : batch.MemoryFills) {
        ++stats.MemoryFills;
        if (fill.Interrupt.has_value()) {
            signal(*fill.Interrupt);
        }
    }
    for (const auto& completion : batch.Completions) {
        ++stats.Completions;
        signal(completion.Interrupt);
    }
    for (const auto& transfer : batch.DisplayTransfers) {
        ++stats.DisplayTransfers;
        if (transfer.SignalInterrupt &&
            transfer.Transfer.TextureCopyBytes == 0U) {
            signal(Oot3dNativeGame::Oot3dPicaInterruptId::Ppf);
        }
    }
    return signaled;
}

void WriteReport(const nlohmann::json& report,
                 const std::filesystem::path& output) {
    if (!output.empty()) {
        if (!output.parent_path().empty()) {
            std::filesystem::create_directories(output.parent_path());
        }
        std::ofstream stream(output);
        if (!stream) {
            throw std::runtime_error("cannot open boot probe output");
        }
        stream << report.dump(2) << '\n';
    }
    std::cout << report.dump(2) << '\n';
}

bool CompileVertexShader(const std::string& source, std::string& error) {
#ifdef OOT3D_NATIVE_HAS_SHADERC
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan,
                                 shaderc_env_version_vulkan_1_1);
    options.SetOptimizationLevel(shaderc_optimization_level_performance);
    const auto result = compiler.CompileGlslToSpv(
        source, shaderc_vertex_shader, "oot3d_native_pica.vert", options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        error = result.GetErrorMessage();
        return false;
    }
    return true;
#else
    error = "shaderc is unavailable in this build";
    return false;
#endif
}

bool CompileFragmentShader(const std::string& source, std::string& error) {
#ifdef OOT3D_NATIVE_HAS_SHADERC
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan,
                                 shaderc_env_version_vulkan_1_1);
    options.SetOptimizationLevel(shaderc_optimization_level_performance);
    const auto result = compiler.CompileGlslToSpv(
        source, shaderc_fragment_shader, "oot3d_native_pica.frag", options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        error = result.GetErrorMessage();
        return false;
    }
    return true;
#else
    error = "shaderc is unavailable in this build";
    return false;
#endif
}

// Ganchos de entrada de bloco do provedor por PCs (registro A32) e, no
// whole-AOT, as entradas despachadas pelo callback abaixo.
void ObserveFrameBoundaryBlock(uint32_t pc, oot3d::recomp::a32::GuestState&,
                               oot3d::recomp::a32::MemoryBus&, void* user) {
    static_cast<Oot3dNativeGame::FrameBoundaryProvider*>(user)
        ->ObserveGuestBlock(pc);
}

nlohmann::json DescribeFrameBoundary(
    const Oot3dNativeGame::FrameBoundaryProvider& provider) {
    const auto& stats = provider.Stats();
    return {
        {"source", Oot3dNativeGame::FrameBoundarySourceName(provider.Source())},
        {"boundaries", stats.Boundaries},
        {"vblanks", stats.VBlanks},
        {"boundaries_per_vblank", stats.BoundariesPerVBlank()},
        {"first_boundary_vblank", stats.FirstBoundaryVBlank},
        {"last_boundary_vblank", stats.LastBoundaryVBlank},
        {"minimum_interval_vblanks", stats.MinimumIntervalVBlanks},
        {"maximum_interval_vblanks", stats.MaximumIntervalVBlanks},
        {"interval_histogram_1_2_3_4_5plus", stats.IntervalHistogram},
        {"top_screen_commits", stats.ScreenCommits[0]},
        {"bottom_screen_commits", stats.ScreenCommits[1]},
        {"set_buffer_swap_commits", stats.SetBufferSwapCommits},
        {"shared_memory_commits", stats.SharedMemoryCommits},
    };
}

#if defined(OOT3D_BOOT_PROBE_WHOLE_AOT)
struct WholeAotDispatch {
    bool ManualCompiledFunctions = false;
    Oot3dNativeGame::FrameBoundaryProvider* GuestPcFrameBoundary = nullptr;
};

// Sem registro de blocos: todo PC passa por aqui. Fora das funções
// traduzidas devolve false e o processo cai em MissingBlock (falha com PC).
bool ExecuteTitleWholeAot(uint32_t pc, oot3d::recomp::a32::GuestState& state,
                          oot3d::recomp::a32::MemoryBus& memory,
                          oot3d::recomp::a32::ExecutionResult* result,
                          uint32_t blockBudget, uint32_t* blocksConsumed,
                          void* user) {
    const auto* dispatch = static_cast<const WholeAotDispatch*>(user);
    if (dispatch != nullptr && dispatch->GuestPcFrameBoundary != nullptr) {
        dispatch->GuestPcFrameBoundary->ObserveGuestBlock(pc);
    }
    auto& nativeMemory = static_cast<Oot3dNativeGame::NativeA32Memory&>(memory);
    return Oot3dNativeGame::ExecuteOot3dCompiledFunction(
        pc, state, nativeMemory, result, blockBudget, blocksConsumed, nullptr,
        nullptr, nullptr, 0U, false,
        dispatch != nullptr && dispatch->ManualCompiledFunctions, true, 0U);
}
#endif

Oot3dNativeGame::CtrTitleProfile SelectProfile(
    const Arguments& arguments,
    const Oot3dNativeGame::NativeA32ProcessImageManifest& manifest) {
    if (arguments.TitleProfile == "oot3d") {
        return Oot3dNativeGame::Oot3dTitleProfile(manifest.TitleId);
    }
    if (arguments.TitleProfile == "generic") {
        return Oot3dNativeGame::GenericTitleProfile(manifest.TitleId);
    }
    return Oot3dNativeGame::SelectCtrTitleProfile(manifest.TitleId);
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto arguments = ParseArguments(argc, argv);
        if (!arguments.has_value()) {
            PrintUsage();
            return 64;
        }

        std::string error;
        const auto manifest =
            Oot3dNativeGame::LoadNativeA32ProcessImageManifest(
                arguments->Manifest, &error);
        if (!manifest.has_value()) {
            throw std::runtime_error(error);
        }

        const auto profile = SelectProfile(*arguments, *manifest);
        std::fprintf(stderr,
                     "[boot-probe] processo \"%s\" title_id=%016llx perfil=%s"
                     " (nucleos manuais do OoT3D: %s, servicos stub: %s, "
                     "timeouts do kernel: %s)\n",
                     manifest->ProcessName.c_str(),
                     static_cast<unsigned long long>(
                         manifest->TitleId.value_or(0U)),
                     std::string(profile.Name).c_str(),
                     profile.ManualCompiledFunctions ? "sim" : "nao",
                     profile.StubUnknownServices ? "sim" : "nao",
                     profile.KernelWaitTimeouts ? "sim" : "nao");
#if !defined(OOT3D_BOOT_PROBE_WHOLE_AOT)
        if (profile.Kind != Oot3dNativeGame::CtrTitleKind::Oot3d) {
            throw std::runtime_error(
                "this probe links the OoT3D A32 block registry; use "
                "ctr_title_boot_probe (whole-AOT shards) for another title");
        }
#endif

        Oot3dNativeGame::Oot3dNativePicaFrontend picaFrontend;
        // Campo a campo: a inicialização posicional antiga quebrou quando
        // Filesystem/RomFsContentPath entraram antes de PicaFrontend.
        Oot3dNativeGame::NativeA32CtrHostConfig hostConfig;
        hostConfig.ResourceLimitValues = manifest->ResourceLimitValues;
        hostConfig.ResourceCurrentValues = manifest->ResourceCurrentValues;
        hostConfig.LinearHeapBaseAddress = manifest->LinearHeapBaseAddress;
        hostConfig.LinearHeapSize = manifest->LinearHeapSize;
        hostConfig.HeapBaseAddress = manifest->HeapBaseAddress;
        hostConfig.HeapSize = manifest->HeapSize;
        hostConfig.RomFsImagePath = manifest->RomFsImagePath;
        hostConfig.RomFsImageOffset = manifest->RomFsImageOffset;
        hostConfig.RomFsImageSize = manifest->RomFsImageSize;
        hostConfig.PicaFrontend = &picaFrontend;
        hostConfig.SaveDataDirectory =
            arguments->SaveDataDirectory.empty()
                ? arguments->Output.parent_path() /
                      (std::string(profile.Name) + "_native_savedata")
                : arguments->SaveDataDirectory;
        // The probe serializes every SVC boundary, unlike the product runtime
        // which only needs bounded diagnostic summaries.
        hostConfig.RetainFullSvcHistory = true;
        // Probe traces must not depend on the host clock: anchor the CTR
        // RTC at 2011-06-16 00:00 (title release) plus the virtual clock.
        hostConfig.FixedDateTimeMilliseconds = profile.ProbeDateTimeMilliseconds;
        hostConfig.StubUnknownServices = profile.StubUnknownServices;
        hostConfig.KernelWaitTimeouts = profile.KernelWaitTimeouts;
        hostConfig.MasterQuestUnlock = profile.MasterQuestUnlock;
        // Fronteira de quadro: a do perfil e, sempre, a do GSP.
        auto frameBoundary = Oot3dNativeGame::MakeFrameBoundaryProvider(profile);
        std::unique_ptr<Oot3dNativeGame::FrameBoundaryProvider>
            gspShadowFrameBoundary;
        if (frameBoundary->Source() !=
            Oot3dNativeGame::CtrFrameBoundarySource::GspPresent) {
            gspShadowFrameBoundary = Oot3dNativeGame::MakeFrameBoundaryProvider(
                Oot3dNativeGame::CtrFrameBoundarySource::GspPresent);
        }
        Oot3dNativeGame::FrameBoundaryProvider& gspFrameBoundary =
            gspShadowFrameBoundary ? *gspShadowFrameBoundary : *frameBoundary;
        hostConfig.FrameBoundary = &gspFrameBoundary;
        Oot3dNativeGame::NativeA32CtrHostServices hostServices(
            std::move(hostConfig));
#if defined(OOT3D_BOOT_PROBE_WHOLE_AOT)
        if (!Oot3dNativeGame::Oot3dWholeAotAvailable()) {
            throw std::runtime_error(
                "ctr_title_boot_probe was built without whole-AOT functions");
        }
        Oot3dNativeGame::SetOot3dGuestOverridesActive(profile.GuestOverrides);
        static const oot3d::recomp::a32::Registry emptyRegistry{};
        WholeAotDispatch wholeAotDispatch{
            profile.ManualCompiledFunctions,
            frameBoundary->GuestHookPcs().empty() ? nullptr
                                                   : frameBoundary.get()};
        Oot3dNativeGame::NativeA32Process process(emptyRegistry, hostServices);
        process.SetNativeBlockCallback(&ExecuteTitleWholeAot,
                                       &wholeAotDispatch);
#else
        Oot3dNativeGame::NativeA32Process process(
            oot3d::recomp::GetA32GeneratedRegistry(), hostServices);
#endif
        if (!frameBoundary->GuestHookPcs().empty()) {
            const auto hookPcs = frameBoundary->GuestHookPcs();
            process.SetBlockEntryCallback(
                &ObserveFrameBoundaryBlock, frameBoundary.get(),
                std::vector<uint32_t>(hookPcs.begin(), hookPcs.end()));
        }
        if (!Oot3dNativeGame::MountNativeA32ProcessImage(
                process, *manifest, arguments->CodeBin, &error)) {
            throw std::runtime_error(error);
        }
        std::vector<Oot3dNativeGame::Oot3dPicaPhysicalMemoryRegion>
            picaMemoryRegions{{0x20000000U,
                               manifest->LinearHeapBaseAddress,
                               manifest->LinearHeapSize}};
        for (const auto& region : manifest->SystemRegions) {
            if (region.Name == "ctr_vram") {
                picaMemoryRegions.push_back(
                    {0x18000000U, region.Address, region.MappedSize});
                break;
            }
        }
        Oot3dNativeGame::Oot3dNativePicaSubmissionQueue submissionQueue(
            Oot3dNativeGame::Oot3dPicaPhysicalMemoryView(
                process.Memory(), std::move(picaMemoryRegions)));
        picaFrontend.SetPacketSink(&submissionQueue);

        auto result = RunUntilGuestWait(process);
        std::vector<Oot3dNativeGame::Oot3dPicaDrawSubmission> retainedDraws;
        GpuWorkCompletionStats gpuWork;
        const auto completeGpuWork = [&]() {
            // Concluir pode liberar mais trabalho (o jogo submete a próxima
            // lista ao ver a P3D): algumas rodadas por VBlank.
            for (uint32_t round = 0;
                 arguments->CompleteGpuWork && round < 16U &&
                 result.Kind !=
                     Oot3dNativeGame::NativeA32ProcessRunKind::Faulted &&
                 CompleteDeferredGpuWork(submissionQueue, hostServices,
                                         process.Memory(), retainedDraws,
                                         gpuWork);
                 ++round) {
                result = RunUntilGuestWait(process);
            }
        };
        completeGpuWork();
        uint32_t vblanksDelivered = 0;
        uint64_t refreshTickRemainder = 0;
        uint64_t nextVblankTick = 0;
        constexpr uint64_t kCtrArm11TicksPerSecond = 268111856ULL;
        constexpr uint64_t kDisplayRefreshRate = 60ULL;
        while (vblanksDelivered < arguments->VblankCount &&
               result.Kind ==
                   Oot3dNativeGame::NativeA32ProcessRunKind::Waiting) {
            refreshTickRemainder += kCtrArm11TicksPerSecond;
            nextVblankTick += refreshTickRemainder / kDisplayRefreshRate;
            refreshTickRemainder %= kDisplayRefreshRate;
            AdvanceGuestClockTo(process, hostServices, result,
                                nextVblankTick);
            if (result.Kind ==
                Oot3dNativeGame::NativeA32ProcessRunKind::Faulted) {
                break;
            }
            completeGpuWork();
            if (!hostServices.SignalVBlank(process.Memory())) {
                break;
            }
            if (gspShadowFrameBoundary) {
                // O provedor por PCs também conta os VBlanks (ritmo).
                frameBoundary->ObserveVBlank();
            }
            ++vblanksDelivered;
            result = RunUntilGuestWait(process);
            completeGpuWork();
        }
        const auto& state = process.PrimaryThreadState();
        nlohmann::json registers = nlohmann::json::array();
        for (uint32_t value : state.r) {
            registers.push_back(value);
        }
        nlohmann::json threads = nlohmann::json::array();
        nlohmann::json ipcCommandBuffers = nlohmann::json::array();
        nlohmann::json ipcStaticBuffers = nlohmann::json::array();
        std::vector<uint32_t> memoryCandidates;
        for (uint32_t threadId = 0; threadId < process.ThreadCount();
             ++threadId) {
            const auto* threadState = process.ThreadState(threadId);
            if (threadState == nullptr) {
                continue;
            }
            nlohmann::json threadRegisters = nlohmann::json::array();
            for (uint32_t value : threadState->r) {
                threadRegisters.push_back(value);
                memoryCandidates.push_back(value);
            }
            nlohmann::json stackWords = nlohmann::json::array();
            const uint32_t stackAddress = threadState->r[13];
            for (uint32_t offset = 0; offset < 0x80U; offset += 4U) {
                uint32_t value = 0;
                if (!process.Memory().Read32(stackAddress + offset, &value)) {
                    break;
                }
                stackWords.push_back(value);
                memoryCandidates.push_back(value);
            }
            threads.push_back({
                {"id", threadId},
                {"status", ThreadStatusName(process.ThreadStatus(threadId))},
                {"priority", process.ThreadPriority(threadId).value_or(0)},
                {"registers", std::move(threadRegisters)},
                {"cpsr", threadState->cpsr},
                {"fpscr", threadState->fpscr},
                {"thread_pointer", threadState->thread_pointer},
                {"stack_address", stackAddress},
                {"stack_words", std::move(stackWords)},
            });
            const uint32_t commandBufferAddress =
                threadState->thread_pointer + 0x80U;
            nlohmann::json commandWords = nlohmann::json::array();
            for (uint32_t offset = 0; offset < 0x100U; offset += 4U) {
                uint32_t value = 0;
                if (!process.Memory().Read32(commandBufferAddress + offset,
                                             &value)) {
                    break;
                }
                commandWords.push_back(value);
            }
            ipcCommandBuffers.push_back({
                {"thread_id", threadId},
                {"address", commandBufferAddress},
                {"words", std::move(commandWords)},
            });
            const uint32_t staticBufferAddress =
                threadState->thread_pointer + 0x180U;
            nlohmann::json staticBufferWords = nlohmann::json::array();
            for (uint32_t offset = 0; offset < 0x80U; offset += 4U) {
                uint32_t value = 0;
                if (!process.Memory().Read32(staticBufferAddress + offset,
                                             &value)) {
                    break;
                }
                staticBufferWords.push_back(value);
            }
            ipcStaticBuffers.push_back({
                {"thread_id", threadId},
                {"address", staticBufferAddress},
                {"words", std::move(staticBufferWords)},
            });
        }
        nlohmann::json memorySnapshots = nlohmann::json::array();
        std::set<uint32_t> capturedAddresses;
        for (size_t index = 0;
             index < memoryCandidates.size() && memorySnapshots.size() < 512U;
             ++index) {
            const uint32_t address = memoryCandidates[index] & ~3U;
            if (!capturedAddresses.insert(address).second ||
                !process.Memory().IsMapped(address, 0x100U)) {
                continue;
            }
            nlohmann::json words = nlohmann::json::array();
            for (uint32_t offset = 0; offset < 0x100U; offset += 4U) {
                uint32_t value = 0;
                if (!process.Memory().Read32(address + offset, &value)) {
                    break;
                }
                words.push_back(value);
                memoryCandidates.push_back(value);
            }
            memorySnapshots.push_back({
                {"address", address},
                {"words", std::move(words)},
            });
        }
        nlohmann::json svcEvents = nlohmann::json::array();
        for (const auto& event : hostServices.SvcEvents()) {
            svcEvents.push_back({
                {"immediate", event.Immediate},
                {"pc", event.Pc},
                {"thread_id", event.ThreadId},
                {"handled", event.Handled},
                {"name", event.Name},
                {"detail", event.Detail},
            });
        }
        const auto lastSvc = hostServices.SvcEvents().empty()
                                 ? Oot3dNativeGame::NativeA32CtrSvcEvent{}
                                 : hostServices.SvcEvents().back();
        const std::string boundaryKind =
            hostServices.FallbackReason() !=
                    oot3d::recomp::a32::FallbackReason::None
                ? "fallback"
                : (!hostServices.SvcEvents().empty() ? "svc" : "dispatch");
        nlohmann::json gspCommands = nlohmann::json::array();
        for (const auto& command : picaFrontend.PendingGspCommands()) {
            gspCommands.push_back({
                {"control", command.Control},
                {"id", command.Control & 0xFFU},
                {"parameters", command.Parameters},
            });
        }
        nlohmann::json hardwareWrites = nlohmann::json::array();
        for (const auto& write : picaFrontend.PendingWrites()) {
            hardwareWrites.push_back({
                {"offset", write.Offset},
                {"physical_address", write.PhysicalAddress},
                {"value", write.Value},
            });
        }
        nlohmann::json drawPackets = nlohmann::json::array();
        for (const auto& draw : picaFrontend.PendingDrawPackets()) {
            nlohmann::json registers = nlohmann::json::array();
            for (size_t registerId = 0;
                 registerId < draw.Registers.size(); ++registerId) {
                if (draw.Registers[registerId] != 0U) {
                    registers.push_back(
                        {registerId, draw.Registers[registerId]});
                }
            }
            drawPackets.push_back({
                {"command_list_address", draw.CommandListAddress},
                {"command_list_offset_words", draw.CommandListOffsetWords},
                {"indexed", draw.Indexed},
                {"nonzero_registers", std::move(registers)},
            });
        }
        nlohmann::json drawSubmissions = nlohmann::json::array();
        std::unordered_map<uint64_t, std::pair<bool, std::string>>
            compiledVertexShaders;
        std::unordered_map<uint64_t, std::pair<bool, std::string>>
            compiledFragmentShaders;
        const auto pendingDraws = submissionQueue.PendingDraws();
        retainedDraws.insert(retainedDraws.end(), pendingDraws.begin(),
                             pendingDraws.end());
        for (const auto& submission : retainedDraws) {
            nlohmann::json resources = nlohmann::json::array();
            for (const auto& resource : submission.Resources) {
                resources.push_back({
                    {"kind", static_cast<uint32_t>(resource.Kind)},
                    {"slot", resource.Slot},
                    {"physical_address", resource.PhysicalAddress},
                    {"first_element", resource.FirstElement},
                    {"byte_count", resource.ResolvedBytes().size()},
                });
            }
            nlohmann::json defaultAttributes = nlohmann::json::array();
            for (size_t attribute = 0;
                 attribute < submission.State.VertexInput.AttributeCount;
                 ++attribute) {
                if (!submission.State.VertexInput.Attributes[attribute].Default) {
                    continue;
                }
                defaultAttributes.push_back({
                    {"attribute", attribute},
                    {"input_register",
                     submission.State.ShaderInterface
                         .InputRegisterByAttribute[attribute]},
                    {"value", submission.Packet.DefaultAttributes[attribute]},
                });
            }
            Oot3dNativeGame::Oot3dPicaGeneratedVertexShader vertexShader;
            std::string shaderError;
            const bool vertexShaderGenerated =
                Oot3dNativeGame::GenerateOot3dPicaVertexShader(
                    submission.Packet, submission.State, vertexShader,
                    &shaderError);
            bool vertexShaderCacheHit = false;
            bool vertexShaderCompiled = false;
            std::string shaderCompileError;
            if (vertexShaderGenerated) {
                const auto cached =
                    compiledVertexShaders.find(vertexShader.StateKey);
                if (cached != compiledVertexShaders.end()) {
                    vertexShaderCacheHit = true;
                    vertexShaderCompiled = cached->second.first;
                    shaderCompileError = cached->second.second;
                } else {
                    vertexShaderCompiled = CompileVertexShader(
                        vertexShader.Source, shaderCompileError);
                    compiledVertexShaders.emplace(
                        vertexShader.StateKey,
                        std::pair{vertexShaderCompiled, shaderCompileError});
                }
            }
            Oot3dNativeGame::Oot3dPicaGeneratedFragmentShader fragmentShader;
            std::string fragmentShaderError;
            const bool fragmentShaderGenerated =
                Oot3dNativeGame::GenerateOot3dPicaFragmentShader(
                    submission.Packet, submission.State, fragmentShader,
                    &fragmentShaderError);
            bool fragmentShaderCacheHit = false;
            bool fragmentShaderCompiled = false;
            std::string fragmentShaderCompileError;
            if (fragmentShaderGenerated) {
                const auto cached =
                    compiledFragmentShaders.find(fragmentShader.StateKey);
                if (cached != compiledFragmentShaders.end()) {
                    fragmentShaderCacheHit = true;
                    fragmentShaderCompiled = cached->second.first;
                    fragmentShaderCompileError = cached->second.second;
                } else {
                    fragmentShaderCompiled = CompileFragmentShader(
                        fragmentShader.Source, fragmentShaderCompileError);
                    compiledFragmentShaders.emplace(
                        fragmentShader.StateKey,
                        std::pair{fragmentShaderCompiled,
                                  fragmentShaderCompileError});
                }
            }
            Oot3dNativeGame::Oot3dPicaVulkanDrawPlan vulkanPlan;
            std::string vulkanPlanError;
            const bool vulkanPlanBuilt =
                Oot3dNativeGame::BuildOot3dPicaVulkanDrawPlan(
                    submission, vulkanPlan, &vulkanPlanError);
            drawSubmissions.push_back({
                {"id", submission.Id},
                {"minimum_vertex_index", submission.MinimumVertexIndex},
                {"maximum_vertex_index", submission.MaximumVertexIndex},
                {"vertex_count", submission.State.VertexInput.VertexCount},
                {"indexed", submission.State.VertexInput.Indexed},
                {"vertex_program_words",
                 submission.Packet.VertexShader.ProgramWordCount},
                {"vertex_swizzle_words",
                 submission.Packet.VertexShader.SwizzleWordCount},
                {"vertex_shader_generated", vertexShaderGenerated},
                {"vertex_shader_error", shaderError},
                {"vertex_shader_state_key", vertexShader.StateKey},
                {"vertex_shader_source_bytes", vertexShader.Source.size()},
                {"vertex_shader_compiled", vertexShaderCompiled},
                {"vertex_shader_cache_hit", vertexShaderCacheHit},
                {"vertex_shader_compile_error", shaderCompileError},
                {"fragment_shader_generated", fragmentShaderGenerated},
                {"fragment_shader_error", fragmentShaderError},
                {"fragment_shader_state_key", fragmentShader.StateKey},
                {"fragment_shader_source_bytes", fragmentShader.Source.size()},
                {"fragment_shader_compiled", fragmentShaderCompiled},
                {"fragment_shader_cache_hit", fragmentShaderCacheHit},
                {"fragment_shader_compile_error", fragmentShaderCompileError},
                {"vulkan_plan_built", vulkanPlanBuilt},
                {"vulkan_plan_error", vulkanPlanError},
                {"vulkan_vertex_bindings", vulkanPlan.VertexBindings.size()},
                {"vulkan_vertex_attributes",
                 vulkanPlan.VertexAttributes.size()},
                {"vulkan_index_bytes", vulkanPlan.ResolvedIndexBytes().size()},
                {"vulkan_base_vertex", vulkanPlan.BaseVertex},
                {"vulkan_texture_bindings", vulkanPlan.Textures.size()},
                {"pipeline_state",
                 {{"topology",
                   static_cast<uint32_t>(submission.State.Topology)},
                  {"cull_mode",
                   static_cast<uint32_t>(submission.State.CullMode)},
                  {"viewport",
                   {{"half_width", submission.State.Viewport.HalfWidth},
                    {"half_height", submission.State.Viewport.HalfHeight},
                    {"corner_x", submission.State.Viewport.CornerX},
                    {"corner_y", submission.State.Viewport.CornerY},
                    {"depth_range", submission.State.Viewport.DepthRange},
                    {"near_plane", submission.State.Viewport.NearPlane},
                    {"z_buffering", submission.State.Viewport.ZBuffering}}},
                  {"scissor",
                   {{"mode",
                     static_cast<uint32_t>(submission.State.Scissor.Mode)},
                    {"x1", submission.State.Scissor.X1},
                    {"y1", submission.State.Scissor.Y1},
                    {"x2", submission.State.Scissor.X2},
                    {"y2", submission.State.Scissor.Y2}}},
                  {"color_write_mask",
                   submission.State.OutputMerger.ColorWriteMask},
                  {"fragment_operation_mode",
                   submission.State.OutputMerger.FragmentOperationMode},
                  {"logic_operation",
                   static_cast<uint32_t>(
                       submission.State.OutputMerger.LogicOperation)},
                  {"blend_enabled",
                   submission.State.OutputMerger.Blend.Enabled},
                  {"depth_test_enabled",
                   submission.State.OutputMerger.Depth.TestEnabled},
                  {"depth_write_enabled",
                   submission.State.OutputMerger.Depth.WriteEnabled},
                  {"depth_compare",
                   static_cast<uint32_t>(
                       submission.State.OutputMerger.Depth.Compare)},
                  {"stencil_enabled",
                   submission.State.OutputMerger.Stencil.Enabled}}},
                {"default_attributes", std::move(defaultAttributes)},
                {"resources", std::move(resources)},
            });
        }
#if defined(OOT3D_BOOT_PROBE_WHOLE_AOT)
        const auto wholeAotStats =
            Oot3dNativeGame::GetOot3dCompiledFunctionStats();
        const nlohmann::json wholeAot = {
            {"entry_points",
             Oot3dNativeGame::Oot3dCompiledFunctionEntryPoints(true, false)
                 .size()},
            {"calls", wholeAotStats.WholeAotCalls},
            {"svc_exits", wholeAotStats.WholeAotSvcExits},
            {"block_limit_exits", wholeAotStats.WholeAotBlockLimitExits},
            {"memory_faults", wholeAotStats.WholeAotMemoryFaults},
            {"unsupported_exits", wholeAotStats.WholeAotUnsupportedExits},
            {"manual_compiled_calls", wholeAotStats.Calls},
        };
#else
        const nlohmann::json wholeAot = nullptr;
#endif
        nlohmann::json frameBoundaryReport = DescribeFrameBoundary(*frameBoundary);
        frameBoundaryReport["gsp_shadow"] =
            gspShadowFrameBoundary
                ? DescribeFrameBoundary(*gspShadowFrameBoundary)
                : nlohmann::json(nullptr);
        {
            const auto& stats = frameBoundary->Stats();
            const auto& gspStats = gspFrameBoundary.Stats();
            std::fprintf(
                stderr,
                "[boot-probe] fronteiras de quadro (%s): %llu em %llu vblanks "
                "(%.3f/vblank, intervalo %llu..%llu vblanks, histograma "
                "1/2/3/4/5+ = %llu/%llu/%llu/%llu/%llu); commits GSP "
                "topo=%llu baixo=%llu (SetBufferSwap=%llu, shared mem=%llu)",
                Oot3dNativeGame::FrameBoundarySourceName(
                    frameBoundary->Source()),
                static_cast<unsigned long long>(stats.Boundaries),
                static_cast<unsigned long long>(stats.VBlanks),
                stats.BoundariesPerVBlank(),
                static_cast<unsigned long long>(stats.MinimumIntervalVBlanks),
                static_cast<unsigned long long>(stats.MaximumIntervalVBlanks),
                static_cast<unsigned long long>(stats.IntervalHistogram[0]),
                static_cast<unsigned long long>(stats.IntervalHistogram[1]),
                static_cast<unsigned long long>(stats.IntervalHistogram[2]),
                static_cast<unsigned long long>(stats.IntervalHistogram[3]),
                static_cast<unsigned long long>(stats.IntervalHistogram[4]),
                static_cast<unsigned long long>(gspStats.ScreenCommits[0]),
                static_cast<unsigned long long>(gspStats.ScreenCommits[1]),
                static_cast<unsigned long long>(gspStats.SetBufferSwapCommits),
                static_cast<unsigned long long>(gspStats.SharedMemoryCommits));
            if (gspShadowFrameBoundary) {
                std::fprintf(stderr,
                             "; sombra gsp_present: %llu fronteiras "
                             "(%.3f/vblank)",
                             static_cast<unsigned long long>(
                                 gspStats.Boundaries),
                             gspStats.BoundariesPerVBlank());
            }
            std::fprintf(stderr, "\n");
        }
        std::fprintf(stderr,
                     "[boot-probe] fim: run=%s vblanks=%u/%u svcs=%llu "
                     "ultimo_svc=\"%s\" pc=0x%08x%s%s\n",
                     RunKindName(result.Kind), vblanksDelivered,
                     arguments->VblankCount,
                     static_cast<unsigned long long>(
                         hostServices.SvcEventCount()),
                     lastSvc.Name.c_str(),
                     boundaryKind == "fallback" ? hostServices.FallbackPc()
                                                : lastSvc.Pc,
                     result.Error.empty() ? "" : " erro=",
                     result.Error.c_str());
        const nlohmann::json report = {
            {"format", "oot3d_native_a32_boot_probe_v1"},
            {"title",
             {{"title_id", manifest->TitleId.has_value()
                               ? nlohmann::json(*manifest->TitleId)
                               : nlohmann::json(nullptr)},
              {"profile", std::string(profile.Name)},
              {"manual_compiled_functions", profile.ManualCompiledFunctions},
              {"stub_unknown_services", profile.StubUnknownServices},
              {"kernel_wait_timeouts", profile.KernelWaitTimeouts}}},
            {"whole_aot", wholeAot},
            {"frame_boundary", std::move(frameBoundaryReport)},
            {"gpu_work_completion",
             arguments->CompleteGpuWork
                 ? nlohmann::json{{"rounds", gpuWork.Rounds},
                                  {"draws", gpuWork.Draws},
                                  {"memory_fills", gpuWork.MemoryFills},
                                  {"completions", gpuWork.Completions},
                                  {"display_transfers",
                                   gpuWork.DisplayTransfers},
                                  {"interrupts_signaled",
                                   gpuWork.InterruptsSignaled}}
                 : nlohmann::json(nullptr)},
            {"process",
             {{"name", manifest->ProcessName},
              {"entrypoint", manifest->EntryAddress},
              {"code_bin_sha256", manifest->CodeBinSha256},
              {"mapped_regions", process.Memory().RegionCount()}}},
            {"pica_frontend",
             {{"hardware_register_writes",
              picaFrontend.HardwareWriteCount()},
              {"hardware_register_write_records", std::move(hardwareWrites)},
              {"gsp_commands", std::move(gspCommands)},
              {"pica_register_writes",
               picaFrontend.PendingRegisterWrites().size()},
              {"draw_packets", picaFrontend.PendingDrawPackets().size()},
              {"draw_packet_records", std::move(drawPackets)},
              {"draw_submissions", std::move(drawSubmissions)}}},
            {"run",
             {{"kind", RunKindName(result.Kind)},
              {"error", result.Error},
              {"exit_kind", static_cast<uint32_t>(result.Exit.kind)},
              {"exit_pc", result.Exit.pc},
              {"exit_detail", result.Exit.detail},
              {"host_transitions", result.HostTransitions},
              {"vblanks_requested", arguments->VblankCount},
              {"vblanks_delivered", vblanksDelivered},
              {"system_ticks", hostServices.SystemTicks()}}},
            {"boundary",
             {{"kind", boundaryKind},
              {"pc", boundaryKind == "fallback" ? hostServices.FallbackPc()
                                                  : lastSvc.Pc},
              {"svc_immediate", lastSvc.Immediate},
              {"svc_name", lastSvc.Name},
              {"svc_handled", lastSvc.Handled},
              {"fallback_reason",
               static_cast<uint32_t>(hostServices.FallbackReason())},
              {"raw_operation", hostServices.FallbackRawOperation()}}},
            {"svc_events", svcEvents},
            {"threads", std::move(threads)},
            {"ipc_command_buffers", std::move(ipcCommandBuffers)},
            {"ipc_static_buffers", std::move(ipcStaticBuffers)},
            {"memory_snapshots", std::move(memorySnapshots)},
            {"state",
             {{"registers", registers},
              {"cpsr", state.cpsr},
              {"fpscr", state.fpscr},
              {"thread_pointer", state.thread_pointer}}},
        };
        WriteReport(report, arguments->Output);
        return result.Kind ==
                       Oot3dNativeGame::NativeA32ProcessRunKind::Waiting &&
                   boundaryKind == "svc"
                   ? 0
                   : 2;
    } catch (const std::exception& exception) {
        std::cerr << "oot3d_native_a32_boot_probe: " << exception.what()
                  << '\n';
        return 1;
    }
}
