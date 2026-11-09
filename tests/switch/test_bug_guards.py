#!/usr/bin/env python3
"""Guardas de regressão dos bugs já corrigidos no port do Switch.

Cada guarda confere no código-fonte que a correção de um bug visto no console
continua no lugar. São checagens estáticas (rodam em segundos, sem build): elas
não provam que o código está certo, mas falham se um merge do upstream, um
refactor ou um conflito resolvido às pressas apagar a correção. O teste de
comportamento de cada bug, quando existe, fica no alvo citado em `test`.

Uso: python3 tests/switch/test_bug_guards.py [--list]
"""
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


@dataclass
class Guard:
    bug: str          # o que se via no console
    path: str         # arquivo onde a correção mora
    pattern: str      # regex que precisa existir (re.S)
    test: str = ""    # teste de comportamento, quando houver
    forbid: str = ""  # regex que NÃO pode existir no mesmo arquivo


GUARDS = [
    # --- 08/11: "cannot replace TopScreen UI config: File exists" ao salvar uma opção ---
    # O FS do Horizon recusa rename() por cima de arquivo existente (EEXIST); o gravador do
    # TopScreen (e o do cache) só tinham o rename POSIX. Uma troca para todos os gravadores
    # (ship/config/FileReplace.h): arquivo antigo para <arquivo>.bak, novo no lugar, backup
    # apagado; um save interrompido é recuperado do .bak ao carregar.
    Guard("salvar uma opção do TopScreen falhava no Switch: 'cannot replace TopScreen UI config: File exists'",
          "tools/oot3d/ui_topscreen/oot3d_top_screen_config.cpp",
          r'Ship::RestoreInterruptedReplace\(path\);\s+std::ifstream stream\(path.*?'
          r'#else\s+.*?Ship::ReplaceFileWithTemporary\(temporary, path\);',
          "oot3d_config_file_replace_tests (TestTopScreenConfig)",
          forbid=r'std::filesystem::rename\('),
    Guard("troca de arquivo no Switch: EEXIST sobre arquivo existente tira o antigo do lugar, põe o novo, "
          "devolve o antigo se isso falhar; um arquivo que sumiu volta do backup",
          "runtime/three_ds_recomp/include/ship/config/FileReplace.h",
          r'std::filesystem::rename\(temporary, target, error\);\s+if \(error != std::errc::file_exists\).*?'
          r'std::filesystem::rename\(target, backup, error\);.*?'
          r'std::filesystem::rename\(temporary, target, error\);\s+if \(error\) \{.*?'
          r'std::filesystem::rename\(backup, target, restore\);.*?'
          r'RestoreInterruptedReplace\(.*?if \(std::filesystem::exists\(target, error\) \|\| error\)\s+\{\s+return true;'
          r'\s+\}\s+std::filesystem::rename\(backup, target, error\);',
          "oot3d_config_file_replace_tests (TestReplace, TestReplaceFailures, TestRecovery)"),
    Guard("troca de arquivo no Switch: arquivos de cache do renderer são substituídos, não recusados com EEXIST",
          "runtime/three_ds_recomp/src/fast/renderer/cache_file.cpp",
          r'#else\s+.*?Ship::ReplaceFileWithTemporary\(temporary, path\)',
          "oot3d_config_file_replace_tests (TestCacheFileWriter)",
          forbid=r'std::filesystem::rename\('),
    Guard("troca de arquivo no Switch: a inicialização recupera um save interrompido do TopScreen/controle antes de checar os arquivos",
          "tools/oot3d/native_game_runtime/oot3d_native_game_bootstrap.cpp",
          r'Ship::RestoreInterruptedReplace\(launch\.TopScreenConfigPath\);\s+'
          r'if \(!std::filesystem::is_regular_file\(launch\.TopScreenConfigPath\)\).*?'
          r'Ship::RestoreInterruptedReplace\(launch\.ControlConfigPath\);\s+'
          r'if \(std::filesystem::is_regular_file\(launch\.ControlConfigPath\)\)'),
    Guard("troca de arquivo no Switch: o arquivo de configurações volta do backup antes de ser lido",
          "runtime/three_ds_recomp/src/ship/config/Config.cpp",
          r'void Config::Reload\(\) \{\s+if \(mPath != "None"\) \{\s+RestoreInterruptedReplace\(mPath\);'),
    Guard("troca de arquivo no Switch: developer-tools.json volta do backup antes de ser lido",
          "tools/oot3d/native_game_runtime/oot3d_dev_warp.cpp",
          r'Ship::RestoreInterruptedReplace\(mStorage\);\s+std::error_code exists;'),
    Guard("troca de arquivo no Switch: o arquivo de idioma volta do backup antes de ser lido",
          "tools/oot3d/native_game_runtime/oot3d_game_language.cpp",
          r'Ship::RestoreInterruptedReplace\(mPath\);\s+if \(std::filesystem::exists\(mPath\)\)'),
    Guard("troca de arquivo no Switch: as preferências de savestate voltam do backup antes de serem lidas",
          "tools/oot3d/native_game_runtime/oot3d_savestate_library.cpp",
          r'Ship::RestoreInterruptedReplace\(mPreferencesPath\);\s+std::ifstream input\(mPreferencesPath'),
    Guard("troca de arquivo no Switch: perfis de ambiente voltam do backup antes de serem lidos",
          "tools/oot3d/native_game_runtime/oot3d_scene_environment_codec.cpp",
          r'Ship::RestoreInterruptedReplace\(path\);\s+std::error_code error;\s+'
          r'file\.Exists = std::filesystem::exists\(path, error\);'),
    # --- 04/11: mapa da dungeon (pausa, TopScreen) preso no andar em que abre ---
    # 0x00442F38 (PauseDungeonMap_HandleCursorInput) é o cursor de andares por
    # D-pad/analógico (Cima/Baixo mudam o andar via SetSelectedFloor); o toque é
    # 0x004435B0. O port do TopScreen fazia o 0x00442F38 retornar na hora.
    Guard("mapa da dungeon na pausa não trocava de andar com o analógico (TopScreen pulava 0x00442F38)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_candidate_dispatch.cpp",
          r'PauseDungeonMap_HandleCursorInput \(0x00442F38\) is not intercepted',
          forbid=r'kPauseDungeonMapCursorInput|0x00442F38U'),
    Guard("mapa da dungeon: 0x00442F38 não pode voltar às listas de blocos do TopScreen",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'kPauseLowerScreenBlock,\s+kPauseUiNativeUpdateCall,',
          forbid=r'kPauseDungeonMapCursorInput|0x00442F38'),
    # --- 04/11: "cannot replace native control config" ao salvar controle no menu de dev ---
    # O FS do Horizon não renomeia por cima de arquivo existente (EEXIST). Desde 08/11 a
    # troca é a de ship/config/FileReplace.h (backup + recuperação), testada no host.
    Guard("salvar controle no menu de dev falhava no Switch (rename por cima de oot3d_controls.json dá EEXIST)",
          "tools/oot3d/native_game_runtime/oot3d_native_control_config.cpp",
          r'Ship::RestoreInterruptedReplace\(path\);.*?#else\s+.*?Ship::ReplaceFileWithTemporary\(temporary, path\);',
          "oot3d_config_file_replace_tests (TestControlConfig)",
          forbid=r'std::filesystem::rename\('),
    Guard("warp de dev não salvava developer-tools.json no Switch (sobrava o .tmp)",
          "tools/oot3d/native_game_runtime/oot3d_dev_warp.cpp",
          r'Ship::WriteConfigFileAtomically\(mStorage,',
          forbid=r'std::filesystem::rename\(temporary, mStorage'),
    Guard("preferências dos savestates não salvavam no Switch (rename por cima dá EEXIST)",
          "tools/oot3d/native_game_runtime/oot3d_savestate_library.cpp",
          r'Ship::WriteConfigFileAtomically\(mPreferencesPath,',
          forbid=r'std::filesystem::rename\('),
    Guard("savestate não sobrescrevia o arquivo existente no Switch (EEXIST)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_savestate.cpp",
          r'#if defined\(__SWITCH__\).*?renameError == std::errc::file_exists.*?std::filesystem::remove\(destination'),
    # --- 26/10: menu de dev sem botões no deko3d ---
    Guard("menu de dev (L3+R3) abria no deko3d mas não recebia o controle (sem backend SDL do ImGui)",
          "runtime/three_ds_recomp/src/fast/Fast3dGui.cpp",
          r'void Fast3dGui::ImGuiWMNewFrame\(\) \{\s+if \(Deko3dRendererSelected\(\)\) \{.*?FeedDeko3dGamepad\(io\);.*?return;'),
    # --- 24/10: backend deko3d (branch deko3d, docs/DEKO3D_PROGRESSO.md) ---
    Guard("[decisão] deko3d só com config/renderer = deko3d; sem o arquivo o GL de sempre",
          "runtime/three_ds_recomp/src/fast/Fast3dWindow.cpp",
          r'if \(Deko3dRendererSelected\(\)\) \{\s+mRenderingApi = CreateDeko3dRenderingApi\(\);\s+\}\s+'
          r'if \(mRenderingApi == nullptr\) \{\s+mRenderingApi = new GfxRenderingAPIOGL\(\);',
          "oot3d_deko3d_backend_tests (TestRendererConfigSelection)"),
    Guard("[decisão] deko3d: sem janela SDL (o SDL do Switch põe uma superfície EGL na janela nativa do swapchain)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_sdl2.cpp",
          r'if \(Deko3dRendererSelected\(\)\) \{.*?mNoWindow = true;.*?return;'),
    Guard("deko3d: glFlush (config/gl_pacing_flush) sem contexto GL chamaria ponteiro nulo",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'if \(gFlushBeforePacing && !Fast::Deko3dRendererSelected\(\)\)\s+glFlush\(\);'),
    Guard("deko3d: contexto GL não é movido para a render thread (não existe)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'if \(openGlWorker && !Fast::Deko3dRendererSelected\(\)\) \{'),
    Guard("deko3d: ImGui sem backend SDL/OpenGL3 (ponteiros GL nulos, ImplSDL2 sem janela)",
          "runtime/three_ds_recomp/src/fast/Fast3dGui.cpp",
          r'void Fast3dGui::ImGuiRenderDrawData\(ImDrawData\* data\) \{\s+if \(Deko3dRendererSelected\(\)\) \{\s+return;'
          r'.*?void Fast3dGui::ImGuiWMNewFrame|void Fast3dGui::ImGuiWMNewFrame\(\) \{\s+if \(Deko3dRendererSelected\(\)\)'),
    Guard("deko3d: profiler GL (config/gpu_timing) chamaria glGenQueries nulo",
          "runtime/three_ds_recomp/src/fast/backends/gl_frame_profiler.cpp",
          r'if \(Deko3dRendererSelected\(\)\) return;\s+const auto config = ReadConfig\(\);'),
    Guard("deko3d: dkCmdBufClear volta à memória do quadro anterior: o slot alimenta o próprio pedaço",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_device.cpp",
          r'dkCmdBufClear\(mCmd\);\s+dkCmdBufAddMemory\(mCmd, mCommandMemory, mSlot \* kCommandSliceBytes'),
    Guard("[decisão] deko3d: draws levam o programa paramétrico (uber) e nada compila na thread de render",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_api.h",
          r'bool UsesAlwaysAvailablePrograms\(\) const override \{\s+return true;'),
    Guard("deko3d: estágio sem DKSH no pacote é lembrado (sem retraduzir a cada draw)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d.cpp",
          r'return cache\.Get\(identity, \[&\]\(\) -> const DkShader\* \{.*?TranslatePicaShaderToOpenGl43',
          "oot3d_deko3d_backend_tests (TestStageCacheResolvesOnce)"),
    Guard("deko3d: especializado ausente cai no uber antes de pular o draw",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d.cpp",
          r'if \(specialized == nullptr\) \{\s+uber = ResolvePicaStage\(Deko3d::ShaderStage::Fragment, draw\.UberFragmentShaderSource',
          "oot3d_deko3d_backend_tests (TestProgramPick)"),
    Guard("deko3d: textura liberada com o GPU ainda lendo (despejo só depois da espera do fence do slot)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d.cpp",
          r'void GfxRenderingAPIDeko3d::EnsureFrame\(\) \{\s+mDevice\.BeginFrame\(\);.*?mResources\.BeginFrame\(mResourcesFrame\);'),
    Guard("deko3d: textura que não decodifica não é decodificada de novo a cada draw",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_pica_resources.cpp",
          r'did not decode".*?return rememberFallback\(\);'),
    Guard("deko3d: atributo de vértice ausente lê (0,0,0,1) do buffer constante, como array GL desligado",
          "runtime/three_ds_recomp/include/fast/backends/gfx_deko3d_pica_plan.h",
          r'PicaAttributeKind Kind = PicaAttributeKind::Constant;.*?uint8_t Binding = kPicaConstantVertexBinding;',
          "oot3d_deko3d_backend_tests (TestVertexPlan)"),
    # --- 24/10: deko3d pacote 5 (alvos, estado, draw, transfer, scanout) ---
    Guard("deko3d: atributo constante (0,0,0,1) nunca era alocado (endereço inválido em todo atributo ausente)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_pica_resources.cpp",
          r'mConstantGpu = dkMemBlockGetGpuAddr\(mConstant\);'),
    Guard("deko3d: projeção alargada como no GL (senão o frustum vira 16:9 e o alvo fica 5:3 esticado)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'GetName\(\)\) == "OpenGL" \|\|\s*std::string_view\(apiPointer->GetName\(\)\) == "deko3d"'),
    Guard("deko3d: display transfer com barreira antes (fragmentos na memória) e depois (cache de textura) do blit",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_pica_draw.cpp",
          r'dkCmdBufBarrier\(cmd, DkBarrier_Full, 0\);.*?dkCmdBufBlitImage.*?dkCmdBufBarrier\(cmd, DkBarrier_Full, DkInvalidateFlags_Image\);'),
    Guard("[decisão] deko3d: sentido de face invertido em relação ao GL (origem em cima, como o Vulkan); config/deko3d_cull_invert desfaz",
          "runtime/three_ds_recomp/include/fast/backends/gfx_deko3d_pica_pipeline.h",
          r'plan\.FrontCcw = invertWinding \? glCcw : !glCcw;',
          "oot3d_deko3d_backend_tests (TestCullPlan)"),
    Guard("deko3d: operação lógica PICA tem outra ordem que a do deko3d (Set=4 no PICA, 15 no deko3d)",
          "runtime/three_ds_recomp/include/fast/backends/gfx_deko3d_pica_pipeline.h",
          r'case L::Set: return 15U;',
          "oot3d_deko3d_backend_tests (TestPipelineEnums)"),
    Guard("deko3d: scanout inverte v (origem em cima) para a tela não sair de cabeça para baixo",
          "tools/oot3d/deko3d_scanout/scanout_vsh.glsl",
          r'pica_uv = vec2\(position\.x \* 0\.5 \+ 0\.5, 0\.5 - position\.y \* 0\.5\);'),
    Guard("boot-status ficava em host_frames=1/guest_frames=1 sem cache de geometria (deko3d): parecia guest travado",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'runFrameCount == 1U \|\| gSwitchPicaGeometryCacheObserved \|\|\s*runFrameCount % 600U == 0U'),
    Guard("deko3d.txt não dizia de qual NRO vinha nem se o PICA chegava ao backend (log do p5 parecia o do p3)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_pica_draw.cpp",
          r'pica: first draw.*?pica: first %s.*?pica: first render-target clear.*?pica: first memory fill',
          "oot3d_deko3d_backend_tests (TestLogSchedule)"),
    Guard("deko3d p5/p5b: crash (Data Abort) no 1º draw — views do alvo apontavam para o DkImage da cópia local na pilha",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_pica_targets.cpp",
          r'auto& stored = mTargets\.emplace\(key, target\)\.first->second;.*?'
          r'dkImageViewDefaults\(&stored\.ColorView, &stored\.Color\);\s+'
          r'dkImageViewDefaults\(&stored\.DepthView, &stored\.DepthStencil\);',
          forbid=r'dkImageViewDefaults\(&target\.(ColorView|DepthView)'),
    Guard("deko3d p5c: jogo fechava ao entrar no gameplay ('renderer does not support the native UI sampler contract')",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_ui.cpp",
          r'bool GfxRenderingAPIDeko3d::SetNativeSamplerParameters\(int sampler, const GfxNativeSamplerState& state\) \{'
          r'.*?found->second\.Sampler = UiSamplerKey\(state\);\s+\}\s+return true;',
          "oot3d_deko3d_backend_tests (TestUiPlan)"),
    Guard("deko3d: sem ImGui na janela não havia medidor de FPS (config/fps_counter) — cópia desenhada na thread de render",
          "runtime/three_ds_recomp/src/fast/Fast3dGui.cpp",
          r'if \(Deko3dRendererSelected\(\)\) \{.*?mRendererRouting\.Deferred\(\s*'
          r'\[copy = CloneImGuiDrawData\(data\)\] \{ Deko3dRenderImGuiDrawData\(copy\.get\(\)\); \}\);',
          forbid=r'ImGui is not drawn on deko3d yet'),
    Guard("deko3d p6: UI de cabeça para baixo e cena girada 180° (NDC -1 na última linha); céu sumia e a cena arrastava",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_pica_draw.cpp",
          r'ApplyDkViewport\(cmd, static_cast<float>\(viewport\.X\).*?'
          r'ApplyDkViewport\(cmd, static_cast<float>\(placement\.Viewport\.X\)',
          "oot3d_deko3d_backend_tests (TestUiPlan: ClipYSwizzleNegates)",
          forbid=r'const DkViewport \w+ = \{'),
    Guard("deko3d p6b: tela preta — viewport de altura negativa vira retângulo de recorte inválido no deko3d 0.5.0",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_api.h",
          r'void ApplyDkViewport\(.*?dkCmdBufSetViewports\(cmd, 0, &viewport, 1\);.*?DkSwizzle_NegativeY.*?'
          r'dkCmdBufSetViewportSwizzles\(cmd, 0, &swizzle, 1\);',
          forbid=r'FlipViewportRows|\{x, y \+ height'),
    Guard("deko3d p6f: GPU rejeitava a lista ao sair da casa do Link — shaders com 4 KiB de scratch, fila com 2 KiB",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_device.cpp",
          r'queueMaker\.perWarpScratchMemorySize = kPerWarpScratchBytes;',
          "oot3d_deko3d_backend_tests (TestDkshScratch)"),
    Guard("deko3d p6k: crash ao sair da casa do Link — BindVtxBuffers do deko3d escreve além do bloco de comandos",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_device.cpp",
          r'slot\.CommandUsed \+ want \+ kCommandGuardBytes <= kCommandSliceBytes\).*?'
          r'slot\.CommandUsed \+= want \+ kCommandGuardBytes;.*?size - kCommandGuardBytes\);.*?'
          r'slot\.CommandUsed = kCommandChunkBytes \+ kCommandGuardBytes;',
          "oot3d_deko3d_backend_tests (TestCommandGuard)"),
    Guard("deko3d p6f: shader com mais scratch que a fila é recusado, não derruba a GPU",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_shaders.cpp",
          r'DkshPerWarpScratchBytes\(dksh\);\s+if \(!scratch\.has_value\(\) \|\| \*scratch > kPerWarpScratchBytes\)'),
    Guard("deko3d p6m: GPU ms incluía a espera do acquire (14,5 ms × 8,5 do GL) — carimbos em volta do acquire",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_device.cpp",
          r'kTimestampSlotBytes \+ kTimestampBytes\);\s+\}\s+Submit\(\);.*?'
          r'dkQueueAcquireImage\(mQueue, mSwapchain\);.*?TimestampPipelineTop.*?2U \* kTimestampBytes\);'),
    Guard("deko3d p6m: governador e DRS sem tempo de GPU no deko3d (só CPU) — Deko3dTagFrame/TakeGpuFrame",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'Fast::Deko3dTagFrame\(kind, costCpuMs\);.*?timer\.Poll\(onGpuResult\);.*?Fast::Deko3dTakeGpuFrame\(frame\)'),
    Guard("deko3d p6m: DRS — imagem de exibição na escala da alocação, conteúdo na escala desenhada (sem realocar)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_pica_draw.cpp",
          r'source->Extent\.Height, source->Alloc\.Scale,\s+source->Alloc\.ScaleY\);',
          "oot3d_deko3d_backend_tests (DRS: conteúdo menor que a alocação)"),
    Guard("deko3d p6p: present no fio próprio sem esperar o anterior → nwindow perde o slot atual (dequeue antes do queue)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_device.cpp",
          r'WaitPresent\(\);(?:(?!dkQueueAcquireImage|QueuePresent).){0,800}?'
          r'mWindowSlot = dkQueueAcquireImage\(mQueue, mSwapchain\);'),
    Guard("deko3d p6p: janela comprimida com present no fio próprio (só dkQueuePresentImage descomprime)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_device.cpp",
          r'if \(!mPresentThreaded\) \{\s+layoutMaker\.flags \|= DkImageFlags_HwCompression;'),
    Guard("p6w: IPC do sys-clk na thread do jogo travou 4,4 s — SwitchClockPerfSummary só lê o cache",
          "tools/oot3d/native_game_runtime/oot3d_switch_clocks.cpp",
          r'std::string SwitchClockPerfSummary\(\) \{(?:(?!serviceDispatch|ReadClocks\(\)).)*?return text \+'),
    Guard("p6x: std::thread::detach() lança 'Function not implemented' no Switch (crash no primeiro perf)",
          "tools/oot3d/native_game_runtime/oot3d_switch_clocks.cpp",
          r'\A(?:(?!\.detach\(\)).)*\Z'),
    Guard("p6x: std::thread::detach() lança no Switch — gravação de áudio de depuração",
          "runtime/three_ds_recomp/src/ship/audio/SDLAudioPlayer.cpp",
          r'\A(?:(?!\.detach\(\)).)*\Z'),
    Guard("deko3d p6w: mailbox — o render não pode reusar a imagem que espera cópia ou está sendo copiada",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_device.cpp",
          r'while \(image == mMailboxPending \|\| image == mMailboxCopying\)'),
    Guard("deko3d p6w: mailbox — dequeue e queue da janela só na thread de present (nwindow empresta 1 imagem por vez)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_device.cpp",
          r'void Device::MailboxLoop\(\).*?dkSwapchainAcquireImage\(mSwapchain, &slot, &dequeued\);'
          r'.*?dkQueueWaitFence\(mPresentQueue, &dequeued\);.*?dkQueueWaitFence\(mPresentQueue, &drawn\);'
          r'.*?dkQueuePresentImage\(mPresentQueue, mSwapchain, slot\);'),
    Guard("deko3d p6w: mailbox — a GPU do render espera a cópia anterior sair da imagem antes de desenhar",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_device.cpp",
          r'if \(wait\) \{\s+dkQueueWaitFence\(mQueue, &copied\);'),
    Guard("deko3d p6q: deko3d_swapchain_images fora de 2-4 estouraria os arrays de imagens da janela",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_device.cpp",
          r'count >= 2U && count <= kMaxSwapchainImages\)'),
    # --- 24/10: picos fora de carregamento (worktree picos-logica) ---
    Guard("[decisão] som/DSP noutro núcleo descartado: resolveria 3 de 95 picos de guest (tour20+22)",
          "docs/GUIA_PORT_3DS_SWITCH.md",
          r'resolveria \*\*3 de 95\*\* picos de guest — \*\*descartado\*\*'),
    Guard("spikes.txt acumula sessões: análise de picos só da última (tour20 tinha 428 linhas do tour19)",
          "tools/oot3d/spike_report.py",
          r'if line\.startswith\("session_start"\):\s+rows = \[\]',
          "test_perf_tools.py (picos: só a última sessão)"),
    Guard("[decisão] early_frame_slot é opt-in (arquivo; conteúdo 0 desliga)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r"earlyFrameSlot = value\.empty\(\) \|\| value\[0\] != '0';"),
    Guard("early_frame_slot: slot devolvido quando o pacer começa a esperar (anúncio do prazo)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'PublishPresentationDeadline\(slot\.Serial, deadline\);\s+slot\.Session->renderStream\.ReleaseFrameSlot\(\);',
          "oot3d_native_render_command_stream_tests (TestEarlyFrameSlotRelease), oot3d_frame_spike_tests"),
    Guard("[decisão] early_frame_slot sem relógio pelo prazo do pacer piora imagens/s (refresh 0/2 alternado)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'PublishedPresentationDeadline\(presentationFrameCount - 2U\)',
          "oot3d_native_cadence_governor_tests (TestEarlyFrameSlotAbsorbsLongSteps)"),
    Guard("[decisão] frames_in_flight=2 segue ignorado (heap corrompido em 21 s, tour3b)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'frames_in_flight ignorado.*?SetMaxFramesInFlight\(1U\);'),
    Guard("Pacer anuncia o prazo também atrasado/desligado (senão o slot só volta depois do swap)",
          "tools/oot3d/native_game_runtime/oot3d_native_presentation_pacer.cpp",
          r'if \(!mEnabled\) \{\s+announce\(std::chrono::steady_clock::now\(\)\);',
          "oot3d_frame_spike_tests (TestPacerAnnouncesDeadlineBeforeWait)"),
    Guard("[decisão] varredura do heap com gpu_timing = pico de 16-21 ms a cada 20 janelas (artefato, documentado)",
          "docs/GUIA_PORT_3DS_SWITCH.md",
          r'varredura do heap \(a cada 20 janelas\) custa 16-21 ms'),
    # --- 24/10: "piscada" de textura/objeto ao girar a câmera rápido (só com fps60) ---
    Guard("Piscada ao girar a câmera (fps60): quadro de passo (alpha 0) desenhava o conjunto de draws do quadro novo",
          "tools/oot3d/native_game_runtime/oot3d_native_pica_visual_frame.cpp",
          r'if \(mImpl->Options\.ExactStep && alpha <= 0\.0F\) \{.*?return ViewOot3dPicaVisualFrame\(previous, output\);',
          test="oot3d_native_pica_visual_cull_tests"),
    Guard("Piscada ao girar a câmera (fps60): intermediário sem os draws que o jogo cortou / com os novos na câmera errada",
          "tools/oot3d/native_game_runtime/oot3d_native_pica_visual_frame.cpp",
          r'ApplyVisualUnmatchedCarry\(previous, current, mImpl->Carry, alpha, output\);',
          test="oot3d_native_pica_visual_cull_tests"),
    Guard("Correção da piscada ligada por padrão (config/interp_exact_step e interp_carry_unmatched = 0 desligam)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'transitionOptions\.ExactStep = true;\s*transitionOptions\.CarryUnmatched = true;.*?preparedVisualTransition\.SetOptions'),
    Guard("[decisão] piscada não é o DRS: o fill de profundidade limpa o anexo inteiro (sem scissor) e a escala só muda 1x por FrameSerial",
          "runtime/three_ds_recomp/src/fast/backends/gfx_opengl_pica.cpp",
          r'glBindFramebuffer\(GL_FRAMEBUFFER, target\.Framebuffer\);\s*glDisable\(GL_SCISSOR_TEST\);\s*GLbitfield mask = 0U;'),
    Guard("[decisão] piscada não é o cache de texturas: chave por conteúdo (hash), entrada descartada é decodificada de novo dos bytes do plano",
          "runtime/three_ds_recomp/src/fast/backends/gfx_opengl_pica.cpp",
          r'struct PicaGlTextureKey \{\s*uint64_t ContentHash = 0U;'),
    Guard("[decisão] piscada não é o hold: intermediário descartado reapresenta o último quadro apresentado inteiro",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'if \(present\) \{\s*capturedTemporalSample = temporalSample;\s*lastPresentedVisual\.emplace\(sample\.TopTransfer, renderTargetNamespace\);'),
    # --- 23/10: decisões medidas no console (não refazer) e bugs de análise ---
    Guard("[decisão] pacer_vsync só com o arquivo config/pacer_vsync (tour22: refresh perdido 3,7→4,1, interp 75→69)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'pacerVsyncConfigured = pacerVsyncFile\.good\(\);'),
    Guard("[decisão] ETC2 desligado sem config/gl_etc2=1 (nvc0/Mesa 20.1 sem ETC2 no GM20B: decodifica na CPU)",
          "runtime/three_ds_recomp/include/fast/oot3d/pica_etc1_transcode.h",
          r'config/gl_etc2.*?return false;\s*\}\s*return !text\.empty\(\) && text\[0\] == \'1\';'),
    Guard("[decisão] governador conta a espera do acquire (pgo5j sem ela: 58,52 fps, otimista demais)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'bool gCountRenderWait = true;'),
    Guard("[decisão] junção de draws do aurora/Dusklight descartada (~1% dos draws juntáveis; a medição inflava o render 3-4 ms)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_opengl_pica.cpp",
          r'kGlCompressedRgb8Etc2', forbid=r'step_mergeable|merge_stats'),
    Guard("[decisão] registro da junção de draws descartada no guia",
          "docs/GUIA_PORT_3DS_SWITCH.md",
          r'Junção de draws \(técnica do aurora/Dusklight\): \*\*medida e descartada'),
    Guard("DRS decidia e nunca aplicava (pgo5n, drs_applied=0): aplica também no primeiro draw do quadro",
          "runtime/three_ds_recomp/include/fast/backends/gl_pica_dynamic_scale.h",
          r'drs_applied stayed 0.*?first draw'),
    Guard("Evento de vsync falha com 0x1272 (vi desc 9): relógio cai na reserva do acquire",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'!vsyncStarted && vsyncClock\.StartAcquireFallback\(\)', test="oot3d_frame_spike_tests"),
    Guard("perf.txt acumula execuções: relatório por área usa só a última sessão",
          "tools/oot3d/lab_tour_report.py",
          r'if h < prev: wins = \[\]'),
    Guard("lab_tour_report quebrava com acks sem quadro",
          "tools/oot3d/lab_tour_report.py",
          r'if "frame" in a'),
    Guard("Médias de perf.txt misturando sessões (tour22 contado com o tour21: 48 em vez de 4,1)",
          "tools/oot3d/perf_window_summary.py",
          r'if previous is not None and frames < previous:\s*windows = \[\]',
          test="tests/switch/test_perf_tools.py"),
    Guard("Perfil de picos saturado lido como perfil de picos (tour21: 97% dos quadros acima de 14 ms)",
          "tools/oot3d/perf_window_summary.py",
          r'def guest_spike_saturation', test="tests/switch/test_perf_tools.py"),
    Guard("[decisão] perfil de picos do guest só com config/guest_spike_ms (deixa o jogo lento)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'config/guest_spike_ms; desligado por padrão'),
    Guard("[decisão] efeitos de áudio 0x4A0338/0x4A022C já são nativos (não reescrever de novo)",
          "tools/oot3d/native_game_runtime/oot3d_native_guest_overrides.cpp",
          r'\{0x004A0338U, "AudioEffect_ProcessStereoReverb"\}'),
    Guard("Tempo por thread guest só com gpu_timing/thread_timing (mediu o som: ~1,2 ms/quadro, tour23)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'SetThreadTimingEnabled\(\s*std::ifstream\("sdmc:/switch/oot3dre/config/gpu_timing"\)'),
    Guard("config/audio_deferred \"0\" sem quebra de linha era lido como ligado (good() falso no EOF)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'const bool present = audioDeferredFile\.good\(\);.*?deferDspAudio = !present',
          forbid=r'deferDspAudio = !audioDeferredFile\.good\(\)'),
    Guard("Áudio adiado: quadros DSP pendentes entregues no fim do RunFrame (nada fica para trás)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'frameCount \+= advanceGuest \? 1U : 0U;\s*\}\s*if \(deferredDspAudioFrames != 0U\)'),
    Guard("Áudio adiado depois da apresentação atrasava o quadro seguinte (tour24): vai na espera do render",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'deferredDspAudioFrames != 0U && renderStream\.FrameSlotsFull\(\)',
          test="oot3d_native_render_command_stream_tests"),
    Guard("Crash na abertura com config/gpu_timing (pgo5e): cntvct_el0 dá Data Abort no EL0 do Horizon",
          "runtime/three_ds_recomp/include/fast/backends/gl_pica_draw_phases.h",
          r'mrs %0, cntpct_el0',
          forbid=r'mrs %0, cntvct_el0'),
    Guard("Código gerado com -ffast-math diverge da emulação VFP exata",
          "CMake/oot3d/native_runtime.cmake",
          r'NintendoSwitch.*?oot3d_native_whole_aot PRIVATE\s+-fno-fast-math -ffp-contract=off'),
    Guard("std::thread::detach() lança no Horizon (pthread_detach = ENOSYS)",
          "tools/oot3d/native_demo_host/oot3d_demo_host_context.cpp",
          r'pthread_detach, so\s+// std::thread::detach\(\) always throws'),
    Guard("Polígono gigante na mira em 1ª pessoa (GL_DEPTH_CLAMP nos draws PICA)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_opengl_pica_state_cache.h",
          r'Before a PICA draw: true means glDisable\(GL_DEPTH_CLAMP\)',
          "gfx_opengl_pica_near_clip_tests (EGL)"),
    Guard("HUD sumia: VAO do PICA vazava para a UI legada (divisor dos atributos)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_opengl_pica.cpp",
          r'glVertexAttribDivisor\('),
    Guard("Game Over sem o prompt de continuar (dispatch 0x00300544 duplicado)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_candidate_dispatch.cpp",
          r'TopScreenRendererVisibilityCallDefersToGameOverDraw\(',
          "config/debug_force_game_over no console"),
    Guard("Congelamento: mallinfo() segura o lock do malloc e varre heap corrompido",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'__malloc_current_mallinfo.*?config/heap_stats',
          "oot3d_hang_watchdog_tests"),
    Guard("Config.cpp 'unable to atomically save' (rename sobre arquivo existente)",
          "runtime/three_ds_recomp/src/ship/config/ConfigPersistence.cpp",
          r'#else\s+.*?ReplaceFileWithTemporary\(temporary, target\);',
          "config_atomic_save_tests, oot3d_config_file_replace_tests (TestShipConfigWriter)",
          forbid=r'std::filesystem::rename\('),
    Guard("Pacing adaptive travava em 30 FPS (padrão precisa ser legacy)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'config/fps60_mode: legacy \(default\)',
          "oot3d_native_cadence_governor_tests"),
    Guard("Log rotacionado ao abrir (upstream #72) estragou leitura do .o3tu no SD",
          "runtime/three_ds_recomp/src/ship/Context.cpp",
          r'__SWITCH__.*?kRotateOnOpen = false',
          "tests/switch/eden_smoke.sh (status=failed + md5 do SD)"),
    Guard("Contador de FPS (e todo ImGui) invisível no Switch: ImGui sem contexto GL na thread do jogo",
          "runtime/three_ds_recomp/src/fast/Fast3dGui.cpp",
          r'OpenGlContextCurrent\(\)\) \{\s+(?:FlushDeferredWindowClear\(\);\s+)?ImGui_ImplOpenGL3_RenderDrawData\(data\);\s+\} else if \(mRendererRouting\.Deferred',
          "Eden com config/fps_counter (ver o canto superior esquerdo)"),
    Guard("Engasgos ao entrar em área nova: texturas decodificadas na render thread",
          "runtime/three_ds_recomp/src/fast/backends/gfx_opengl_pica.cpp",
          r'SharedPicaPredecodeCache\(\)\.Take\(',
          "oot3d_pica_texture_predecode_tests + logs/hitches.txt (cause=texturas)"),
    Guard("Pré-decodificação de texturas desligada no worker de planos",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'picaPlanBuilder\.SetPredecodeTextures\(predecodeTextures\)',
          "oot3d_pica_texture_predecode_tests"),
    Guard("Menu de debug (L3+R3) inacessível no Switch: janela de ajustes só existe no Vulkan",
          "runtime/three_ds_recomp/src/fast/Fast3dGui.cpp",
          r'__SWITCH__.*?AddGuiWindow\(std::make_shared<Oot3dDebugMenuWindow>\(\)\)',
          "oot3d_debug_menu_tests"),
    Guard("L3+R3 não abria o menu no Switch: atalho estava só no module_host (fora do .nro)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_input.cpp",
          r'__SWITCH__.*?pollingState\.DebugMenuCombo\.Update\(debugCombo\)\) Fast::Oot3d::ToggleDebugMenu\(\);\s+\}\s+if \(Fast::Oot3d::DebugMenuOpen\(\)\) \{\s+hostGuiVisible = true;',
          "Eden: F+G (L3+R3) abre o menu de debug"),
    Guard("Jogo recebia os botões com o menu de debug aberto",
          "tools/oot3d/native_game_runtime/triaevum_oot3d_input_backend.cpp",
          r'if \(Fast::Oot3d::DebugMenuOpen\(\)\) \{\s+const auto sequence = mState\.sampleSequence;\s+mState = \{\};',
          "oot3d_debug_menu_tests"),
    Guard("Atalhos de teclado (F5/F8, F11, F2, teclas do jogo) ativos no Switch: console é só controle",
          "runtime/three_ds_recomp/src/fast/Fast3dWindow.cpp",
          r'IsKeyDown\(int32_t scancode\) const \{\s+#if defined\(__SWITCH__\)\s+\(void\)scancode;\s+return false;'
          r'.*?KeyUp\(int32_t scancode\) \{\s+#if defined\(__SWITCH__\).*?return true;'
          r'.*?KeyDown\(int32_t scancode\) \{\s+#if defined\(__SWITCH__\).*?return true;',
          "Eden: F5/F8 não fazem nada; menu L3+R3 continua abrindo"),
    Guard("Menu de debug do Switch mostrava atalhos de PC (F5/F8, F6, F2, teclado/mouse)",
          "tools/oot3d/native_game_runtime/oot3d_savestate_panel.cpp",
          r'TRIAEVUM_KEY_HINT\("Save state", "Save state \(F5\)"\)',
          "grep de F[0-9] nas abas: só dentro de TRIAEVUM_KEY_HINT ou #else de __SWITCH__"),
    Guard("Ring de vértices sem UNSYNCHRONIZED: nouveau (Mesa 20.1) cria staging a cada map (nfsu2-sw PERF_NOTES)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_opengl_pica.cpp",
          r'WritePicaGlStream.*?glMapBufferRange\(.*?GL_MAP_UNSYNCHRONIZED_BIT',
          "nouveau_buffer_transfer_map: GART + UNSYNCHRONIZED devolve o map direto"),
    Guard("tour14: carimbo GPU 'antes do acquire' emitido depois do glFlush (buraco descontado ~0)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_opengl.cpp",
          r'void GfxRenderingAPIOGL::FlushDeferredWindowClear\(\).*?gWindowAcquireHook\(false\);.*?glFlush\(\);.*?ClearFramebufferNow',
          "oot3d_native_gpu_frame_timer_tests"),
    Guard("tour14: espera do acquire contada como custo da render thread (intermediários descartados)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'NativeRenderThreadBusyMs\(cpuMs, waitMs\).*?timer\.EndFrame\(kind, costCpuMs\)',
          "oot3d_native_cadence_governor_tests (TestRenderWaitIsNotCost)"),
    Guard("tour15: par medido de 29-33 ms mantido (margem 0) e a área perdeu 3-5 FPS (custo sem a espera)",
          "tools/oot3d/native_game_runtime/oot3d_native_interpolation_pacing.h",
          r'kExcludedWaitMarginMs = 5\.0',
          "oot3d_native_cadence_governor_tests (TestMarginKeepsTheRefresh)"),
    Guard("tour16 (pgo5k): padrões do pacing voltam aos do pgo5i (espera contada, margem 0, sem glFlush antes do pacer)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'bool gCountRenderWait = true;.*?bool gFlushBeforePacing = false;'
          r'.*?!std::ifstream\("sdmc:/switch/oot3dre/config/interp_cpu_exclude_wait"\)'
          r'.*?std::ifstream\("sdmc:/switch/oot3dre/config/gl_pacing_flush"\)\.good\(\)',
          "oot3d_native_interpolation_pacing_tests (margem padrão 0)"),
    Guard("tour16 (pgo5k): carimbo do acquire na ordem do pgo5i salvo config/gl_acquire_stamp_before_flush",
          "runtime/three_ds_recomp/src/fast/backends/gfx_opengl.cpp",
          r'bool gAcquireStampBeforeFlush = false;.*?if \(!gAcquireStampBeforeFlush && gWindowAcquireHook != nullptr\)'),
    Guard("ETC2 no GM20B passa pelo fallback do st/mesa (malloc por mip + decodificação na render thread): desligado por padrão",
          "runtime/three_ds_recomp/include/fast/oot3d/pica_etc1_transcode.h",
          r'if \(!file \|\| !std::getline\(file, text\)\) \{\s+return false;\s+\}\s+return !text\.empty\(\) && text\[0\] == \'1\';',
          "nvc0_screen_is_format_supported: ETC/ASTC só na classe 0xA297 (GK20A)"),
    Guard("tour16: Data Abort em st_FreeTextureImageBuffer sob TrimPicaGlResidency (heap corrompido)",
          "runtime/three_ds_recomp/tests/gfx_opengl_pica_residency_tests.cpp",
          r'TEST\(PicaGlResidency, ReleasesEachNameOnceAndNeverAPinnedOne\)',
          "gfx_opengl_pica_residency_tests (ASan)"),
    Guard("tour19 (pgo5n): DRS desceu até 2,25 mas drs_applied=0 (o alvo da cena nunca recebe clear de cor)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_opengl_pica.cpp",
          r'if \(ApplyPicaGlTargetScaleAtDraw\(target, targetKey\)\)',
          "oot3d_native_dynamic_resolution_tests (TestBackendAppliesAtFirstDraw)"),
    Guard("tour19 (pgo5n): FrameSerial avança a cada passo (sem ele a escala nunca muda no primeiro draw)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'\+\+Fast::PicaGlDynamicScale::gState\.FrameSerial;'),
    Guard("tour19 (pgo5n): DRS cego à GPU oculta (par 26 ms < 28,3 com o governador descartando)",
          "tools/oot3d/native_game_runtime/oot3d_native_dynamic_resolution.h",
          r'double HiddenGpuMs = 3\.0;.*?pairMs \+= std::max\(0\.0, mConfig\.HiddenGpuMs\);',
          "oot3d_native_dynamic_resolution_tests (TestTour19WaterTempleHiddenGpu)"),
    Guard("tour19 (pgo5n): relógio de vsync falhou (vsync_rc=0x1272) e o pacer_vsync ficou sem fonte",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'if \(!vsyncStarted && vsyncClock\.StartAcquireFallback\(\)\)',
          "oot3d_frame_spike_tests (TestAcquireFallbackFindsVsync)"),
    Guard("tour19 (pgo5n): o acquire que bloqueou alimenta o relógio reserva",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'clock->ObserveAcquire\(nowNs, waitMs\);'),
    Guard("SELECT parou de abrir Salvar/Sair (upstream #80)",
          "tools/oot3d/native_game_runtime/oot3d_top_screen_pause_transaction.cpp",
          r'.',
          "oot3d_top_screen_pause_transaction_tests"),
    # --- 26/10: picos de 14-18 ms a cada 1800 quadros (log do deko3d no render thread) ---
    Guard("deko3d: Log() gravava logs/deko3d.txt (fopen/fclose no SD) no render thread a cada 1800 quadros",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_device.cpp",
          r'void Log\(const char\* format, \.\.\.\) \{.*?gLogQueue\.emplace_back\(line\);',
          "oot3d_frame_spike_tests (TestDeko3dPresentFieldsAtTheEnd)",
          forbid=r'void Log\(const char\* format, \.\.\.\) \{\s+std::lock_guard lock\(gLogMutex\);\s+FILE\*'),
    # --- 26/10: heap corrompido (tours 16-20: chunk 0x50 com bk trocado = uso após free) ---
    Guard("heap corrompido: alocador com páginas de guarda precisa embrulhar todo o malloc do newlib",
          "CMake/oot3d/game.cmake",
          r'oot3d_switch_guard_alloc\.cpp.*?-Wl,--wrap=_malloc_r -Wl,--wrap=_memalign_r -Wl,--wrap=_free_r '
          r'-Wl,--wrap=_realloc_r\s+-Wl,--wrap=_calloc_r -Wl,--wrap=_malloc_usable_size_r',
          "oot3d_switch_guard_alloc_tests"),
    Guard("guard_alloc: calloc/realloc reais leem o cabeçalho do chunk; com bloco de guarda seria lixo",
          "tools/oot3d/native_game_runtime/oot3d_switch_guard_alloc.cpp",
          r'tBypass = true;\s+void\* const pointer = __real__calloc_r.*?'
          r'tBypass = true;\s+void\* const moved = __real__realloc_r'),
    Guard("guard_alloc: TLS só depois de ligado (a libnx chama o malloc antes de tudo)",
          "tools/oot3d/native_game_runtime/oot3d_switch_guard_alloc.cpp",
          r'if \(gEnabled\.load\(std::memory_order_relaxed\) && InRange\(gConfig, size\) && !tBypass\)'),
    Guard("guard_alloc: malloc() do devkitA64 manda n >= 32 para _memalign_r(32, n); sem ele nada ganha guarda",
          "tools/oot3d/native_game_runtime/oot3d_switch_guard_alloc.cpp",
          r'void\* __wrap__memalign_r\(struct _reent\* reent, size_t alignment, size_t size\) \{\s+'
          r'if \(gEnabled\.load\(std::memory_order_relaxed\) && alignment <= kAlignment',
          "oot3d_switch_guard_alloc_tests (TestLayout)"),
    Guard("guard_alloc: página de folga depois do objeto (cabeçalho do chunk seguinte do newlib)",
          "tools/oot3d/native_game_runtime/oot3d_switch_guard_alloc.h",
          r'kBlockPages = 2U;'),
    Guard("guard_alloc: guarda desde o boot = boot sem memória ('Not enough space'); só liga na sessão",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window.cpp",
          r'Oot3dNativeGame::GuardAlloc::Arm\(\);\s+// Hang watchdog'),
    Guard("guard_alloc modo página: 32768 páginas fechadas esgotaram o kernel (áudio SDL/FS falharam no 1º quadro)",
          "tools/oot3d/native_game_runtime/oot3d_switch_guard_alloc.cpp",
          r'if \(parsed\.GuardMode == Mode::Page && parsed\.QuarantineBlocks > kMaxPageQuarantine\)',
          "oot3d_switch_guard_alloc_tests (TestParse)"),
    Guard("guard_alloc canário: free lia ptr-256 direto e caiu em página não mapeada (p6t, exceção no __wrap__free_r)",
          "tools/oot3d/native_game_runtime/oot3d_switch_guard_alloc.cpp",
          r'reinterpret_cast<const uint64_t\*>\(address\)\[-1\] != kCanaryTailMagic\) return nullptr;\s+'
          r'auto\* header = reinterpret_cast<CanaryHeader\*>\(address - sizeof\(CanaryHeader\)\);'),
    Guard("guard_alloc canário: encher o objeto com 0xFD no free travou o console (leitura após free); só cópia",
          "tools/oot3d/native_game_runtime/oot3d_switch_guard_alloc.cpp",
          r'std::memcpy\(header->Snapshot, header \+ 1, header->Block\.Size\);',
          "oot3d_switch_guard_alloc_tests (TestCanary)", forbid=r'kCanaryFill'),
    Guard("guard_alloc: teto de blocos vivos (sem ele o heap acaba)",
          "tools/oot3d/native_game_runtime/oot3d_switch_guard_alloc.cpp",
          r'if \(gLive\.load\(std::memory_order_relaxed\) >= gConfig\.MaxLive\) \{'),
    Guard("handler de exceção grava logs/hang.txt sem alocar e devolve (crash report do Atmosphère continua)",
          "tools/oot3d/native_game_runtime/oot3d_switch_guard_alloc.cpp",
          r'void __libnx_exception_handler\(ThreadExceptionDump\* context\) \{\s+HandleException\(context\);\s+\}'),
    Guard("deko3d.pack previsto dos CMB (~7,5 MiB) lido byte a byte atrasava o boot",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_shaders.cpp",
          r'std::ios::binary \| std::ios::ate\);.*?file\.read\(reinterpret_cast<char\*>\(bytes\.data\(\)\)',
          forbid=r'std::vector<uint8_t> bytes\(\(std::istreambuf_iterator'),
    Guard("uber (paramétrico) de 91-154 KiB: loop do TEV desenrolado pelo uam",
          "runtime/three_ds_recomp/src/fast/renderer3ds/pica_tev_program.cpp",
          r'int stage_count = program\.control\.z == 0u \? 6 : int\(program\.control\.z\);\s+'
          r'for \(int stage = 0; stage < stage_count; \+\+stage\)',
          "pica_tev_gpu_tests (estágios PREVIOUS no fim)", forbid=r'for \(int stage = 0; stage < 6; \+\+stage\)'),
    Guard("uber embutia o avaliador de proctex mesmo com a unidade desligada",
          "tools/oot3d/native_pica_frontend/oot3d_native_pica_fragment_shader_gen.cpp",
          r'if \(procTexEnabled\)\s+source << Fast::Renderer3ds::PicaProcTexCode'),
    # --- 05/11: outro título (Super Mario 3D Land) no mesmo runtime ---
    # Os núcleos manuais (PicaCommandWriter 0x00307BD8, memcpy 0x00371738...) e o
    # desbloqueio do Master Quest estão presos a endereços/arquivos do OoT3D; num
    # outro code.bin corromperiam o processo. O perfil vem do program id.
    Guard("perfil genérico de título religou núcleos manuais/MQ do OoT3D (corromperia outro code.bin)",
          "tools/oot3d/native_game_runtime/ctr_title_profile.h",
          r'GenericTitleProfile.*?ManualCompiledFunctions = false;.*?GuestOverrides = false;'
          r'.*?MasterQuestUnlock = false;',
          "oot3d_native_a32_ctr_host_tests"),
    Guard("manifesto sem title_id (OoT3D antigo) deixou de escolher o perfil do OoT3D",
          "tools/oot3d/native_game_runtime/ctr_title_profile.h",
          r'if \(!titleId\.has_value\(\) \|\| IsOot3dTitleId\(\*titleId\)\)\s*\{\s+return Oot3dTitleProfile'),
    Guard("Zelda mudou: timeouts do kernel / serviços stub ligados por padrão no host",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_ctr_host.h",
          r'bool StubUnknownServices = false;.*?bool KernelWaitTimeouts = false;.*?bool MasterQuestUnlock = true;',
          "oot3d_native_a32_ctr_host_tests (TestOot3dProfileWaitsStayInfinite)"),
    Guard("desbloqueio do Master Quest mexia no system.dat de outro título",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_ctr_host_fs.cpp",
          r'const bool masterQuestUnlock = mConfig\.MasterQuestUnlock && !romfs &&'),
    Guard("boot de outro título parava em CreateMutex/CreateSemaphore/CreateTimer sem HLE",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_ctr_host.cpp",
          r'case 0x13:\s+case 0x15:\s+case 0x16:\s+case 0x1A:\s+case 0x1B:\s+case 0x1C:\s+case 0x1D:'
          r'\s+return HandleKernelSyncObjectSvc',
          "oot3d_native_a32_ctr_host_tests (TestGuestKernelTimers)"),
    Guard("timers do kernel não entravam no relógio do host (SetTimer nunca disparava)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_ctr_host.cpp",
          r'WakeSleepWaiters\(\);\s+// Ambos.*?FireKernelTimers\(\);\s+ExpireSynchronizationTimeouts\(\);'),
    # --- 05/11: fronteira de quadro genérica (commit de framebuffer no GSP) ---
    # O executável achava o fim do quadro só pelos PCs do OoT3D (GameState_Update
    # 0x00417014 + limitador 0x003FD27C/3A8/3B8). Outro título usa o provedor GSP;
    # o Zelda tem de continuar nos PCs, sem provedor no host (byte a byte igual).
    Guard("perfil OoT3D deixou de usar a fronteira de quadro pelos PCs do code.bin",
          "tools/oot3d/native_game_runtime/ctr_title_profile.h",
          r'CtrFrameBoundarySource FrameBoundary =\s+CtrFrameBoundarySource::Oot3dGuestPcs;'
          r'.*?GenericTitleProfile.*?profile\.FrameBoundary = CtrFrameBoundarySource::GspPresent;',
          "oot3d_native_a32_ctr_host_tests (TestFrameBoundaryProviderSelection)"),
    Guard("ganchos da fronteira do OoT3D mudaram (GameState_Update + limitador)",
          "tools/oot3d/native_game_runtime/ctr_frame_boundary.h",
          r'kOot3dFrameBoundaryGameStateUpdatePc = 0x00417014U;\s+'
          r'inline constexpr uint32_t kOot3dFrameBoundaryPacingBeginPc = 0x003FD27CU;\s+'
          r'inline constexpr uint32_t kOot3dFrameBoundaryPacingDecisionPc = 0x003FD3A8U;\s+'
          r'inline constexpr uint32_t kOot3dFrameBoundaryPacingCommitPc = 0x003FD3B8U;'
          r'.*?if \(pc != kOot3dFrameBoundaryGameStateUpdatePc\) \{\s+return false;',
          "oot3d_native_a32_ctr_host_tests (TestOot3dFrameBoundaryMatchesGsp)"),
    Guard("Zelda passou a entregar commits/VBlanks a um provedor de fronteira no host",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'if \(frameBoundaryProvider->Source\(\) ==\s+'
          r'Oot3dNativeGame::CtrFrameBoundarySource::GspPresent\) \{\s+'
          r'hostConfig\.FrameBoundary = frameBoundaryProvider\.get\(\);'),
    Guard("host passou a ter provedor de fronteira por padrão (Zelda mudaria)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_ctr_host.h",
          r'FrameBoundaryProvider\* FrameBoundary = nullptr;',
          "oot3d_native_a32_ctr_host_tests (TestHostReportsGspFrameBoundaries)"),
    Guard("os ganchos do limitador do OoT3D sumiram da janela (fronteira por PCs)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'const auto frameBoundaryHooks = frameBoundaryProvider->GuestHookPcs\(\);\s+'
          r'blockEntryHooks\.insert\(blockEntryHooks\.end\(\), frameBoundaryHooks\.begin\(\),'),
    Guard("ctr_title_boot_probe ligava núcleos manuais do OoT3D sem olhar o perfil",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_boot_probe.cpp",
          r'dispatch != nullptr && dispatch->ManualCompiledFunctions, true, 0U\);'),
    # --- 05/11: Super Mario 3D Land no executável com janela (Linux, Vulkan) ---
    # A janela ligava sempre os ganchos por PC do OoT3D (projeção, PlayState,
    # composição PICA, UI, TopScreen), typed gameplay, mass/true AOT e núcleos
    # manuais; o perfil genérico desliga tudo. O OoT3D tem de continuar igual.
    Guard("perfil OoT3D perdeu os ganchos da janela/TopScreen (Zelda mudaria)",
          "tools/oot3d/native_game_runtime/ctr_title_profile.h",
          r'bool Oot3dGuestHooks = true;.*?bool TopScreenUi = true;'
          r'.*?GenericTitleProfile.*?profile\.Oot3dGuestHooks = false;\s+profile\.TopScreenUi = false;',
          "oot3d_native_a32_ctr_host_tests (TestWindowProfileGatesOot3dHooks)"),
    Guard("janela de outro título manteve os ganchos por PC do OoT3D",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'genericTitle = !titleProfile\.Oot3dGuestHooks;.*?'
          r'if \(genericTitle\) \{\s+// Só os ganchos da fronteira de quadro.*?'
          r'blockEntryHooks\.assign\(frameBoundaryHooks\.begin\(\), frameBoundaryHooks\.end\(\)\);',
          "oot3d_native_a32_ctr_host_tests (TestWindowProfileGatesOot3dHooks)"),
    Guard("typed gameplay do OoT3D ligado na janela de outro título",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'enableCompiledFunctions && !launch\.DisableTypedGameplay && !genericTitle,'),
    Guard("Mario preso no boot da janela: lista de ganchos vazia avisava em todo bloco (1 VBlank em 60 s)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'genericTitleWithoutHooks = genericTitle && blockEntryHooks\.empty\(\).*?'
          r'nativeCandidateDispatch\.BlockEntry = nullptr;.*?'
          r'else if \(genericTitleWithoutHooks\) \{\s+process\.SetBlockEntryCallback\(nullptr, nullptr'),
    Guard("janela de outro título caía na detecção de idiomas do OoT3D na RomFS",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'titleProfile\.Kind == Oot3dNativeGame::CtrTitleKind::Oot3d\s+\? Oot3dNativeGame::DetectGameLanguages'),
    Guard("Mario parava em MissingBlock 0x003A0D38 (__aeabi_f2ulz do RVCT fora do whole-AOT)",
          "tools/oot3d/native_game_runtime/ctr_library_natives.h",
          r'kRvctAeabiF2ulzWords\{\s+0xE1B02BC0U,.*?if \(exponent > 0xBEU\) \{\s+return ~uint64_t\{0\};',
          "oot3d_native_a32_ctr_host_tests (TestRvctLibraryNatives)"),
    Guard("MK7 parava em MissingBlock 0x005E9F38 (__aeabi_f2lz do RVCT; -2^63 exato não satura)",
          "tools/oot3d/native_game_runtime/ctr_library_natives.h",
          r'\{CtrLibraryNative::AeabiF2lz, "__aeabi_f2lz", kRvctAeabiF2lzWords\}.*?'
          r'if \(negative \? bits > 0xDF000000U : exponent >= 0xBEU\)',
          "oot3d_native_a32_ctr_host_tests (TestRvctLibraryNatives)"),
    Guard("MK7 caía no err:f: fs:USER InitializeWithSdkVersion/SetPriority sem HLE",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_ctr_host_fs.cpp",
          r'if \(event\.Detail == kFsInitializeWithSdkVersionRequest\) \{.*?'
          r'if \(event\.Detail == kFsSetPriorityRequest &&',
          "oot3d_native_a32_ctr_fs_tests (TestFsSdkInitializeAndPriority)"),
    Guard("rotinas nativas da biblioteca do RVCT ligadas no OoT3D (só no título genérico)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'std::vector<Oot3dNativeGame::CtrLibraryNativeEntry> libraryNatives;\s+if \(genericTitle\) \{'),
    Guard("build Linux do executável com janela: Deko3dRendererSelected sem declaração fora do Switch",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'#endif\s+// Deko3dRendererSelected\(\).*?#include "fast/backends/gfx_deko3d\.h"'),
    Guard("build Linux do executável com janela: ~SwitchVsyncClock indefinido no link",
          "CMake/oot3d/game.cmake",
          r'if \(NOT CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch"\).*?oot3d_switch_vsync_clock\.cpp\)'),
    # --- 05/11: saves à prova de perda (docs/pesquisas/2026-11-05-saves-seguros.md) ---
    # weakly_canonical/canonical com raiz de dispositivo (sdmc:/) concatena o cwd
    # no devkitA64 e o DeleteFile respondia CommandNotAllowed no Switch.
    Guard("DeleteFile (Erase file) no Switch: contenção lexical, sem canonical/weakly_canonical com sdmc:/",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_ctr_host_fs.cpp",
          r'CtrArchivePathContained\(archive->Root, \*relative\)',
          "oot3d_native_a32_ctr_fs_tests (TestArchiveContainmentLexical)",
          forbid=r'std::filesystem::(weakly_canonical|canonical|absolute)\('),
    Guard("FormatSaveData apagava a pasta do save (remove_all); agora move para <save>-backups/format-*",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_ctr_host_fs.cpp",
          r'journal->MoveSaveAsideForFormat\(',
          "oot3d_native_a32_ctr_fs_tests (TestFormatNeverDeletesSave)",
          forbid=r'remove_all\(mConfig\.SaveDataDirectory'),
    # Commit por handle (05/11, rodada 2): a forma antiga journal->Commit(file.FilePath)
    # confirmava no Close/Flush de QUALQUER handle, até só de leitura (S3).
    Guard("escrita de save no lugar (queda no meio truncava o save): diário .triaevum-tmp/.triaevum-new, commit por handle",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_ctr_host_fs.cpp",
          r'journal->BeginWrite\(file\.FilePath, file\.SaveJournalWriter\).*?'
          r'journal->Flush\(file\.FilePath, file\.SaveJournalWriter\).*?'
          r'journal->Close\(file\.FilePath, file\.SaveJournalWriter\)',
          "oot3d_native_a32_ctr_fs_tests (TestSaveWriteIsAtomic, TestReadOnlyCloseDoesNotCommit)",
          forbid=r'journal->Commit\(file\.FilePath\)'),
    # A forma antiga (remove do destino + rename) deixava o save sumido se o rename
    # do novo falhasse depois do remove; agora o destino vira .triaevum-old e volta.
    Guard("troca do save no Switch (EEXIST): destino vira .triaevum-old e volta se o rename do novo falhar",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_ctr_save_safety.cpp",
          r'error == std::errc::file_exists.*?RenamePath\(destination, previous, asideError\).*?'
          r'RenamePath\(source, destination, error\);\s+if \(error\) \{.*?RenamePath\(previous, destination, backError\)',
          "oot3d_native_a32_ctr_fs_tests (TestHorizonRenameCommit, TestRenameFailureNeverLosesSave)",
          forbid=r'std::filesystem::remove\(destination, \w+\);\s+error\.clear\(\);\s+'
                 r'(std::filesystem::rename|RenamePath)\(source, destination'),
    Guard("Write/SetSize do save que falha (SD cheio) era confirmado no Close (S2)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_ctr_host_fs.cpp",
          r'if \(!success && journal != nullptr\) journal->MarkFailed\(file\.FilePath\);.*?'
          r'if \(!resized && journal != nullptr\) journal->MarkFailed\(file\.FilePath\);',
          "oot3d_native_a32_ctr_fs_tests (TestFailedWriteIsNotCommitted)"),
    Guard("svcCloseHandle do último handle escritor confirma a transação do save (S3)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_ctr_host.cpp",
          r'object->Type == "client_session:file:savedata".*?journal->Close\(object->FilePath, object->SaveJournalWriter\)',
          "oot3d_native_a32_ctr_fs_tests (TestReadOnlyCloseDoesNotCommit)"),
    Guard("prune dos backups apagava a geração recém-criada (ordem pela data do nome; relógio para trás) (S4)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_ctr_save_safety.cpp",
          r'AppendBackupOrder\(mBackupRoot, mGeneration\.filename\(\)\.string\(\)\);\s+'
          r'PruneCtrSaveBackups\(mBackupRoot, kCtrSaveBackupGenerations, mGeneration\);',
          "oot3d_native_a32_ctr_fs_tests (TestBackupPruneUsesSequence)"),
    Guard("backup truncado/estragado contava como cópia: cópia em .triaevum-tmp conferida (tamanho + checksum) (S1)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_ctr_save_safety.cpp",
          r'copy\.size\(\) == bytes\.size\(\) &&\s+Checksum\(copy\) == Checksum\(bytes\).*?'
          r'const auto partial = WithSuffix\(destination, kCtrSaveJournalTemporarySuffix\);',
          "oot3d_native_a32_ctr_fs_tests (TestBackupIsVerified)"),
    Guard("troca interrompida não era recuperada quando a execução começa por savestate (S8)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_ctr_host_fs.cpp",
          r'mSaveJournal = std::make_shared<CtrSaveJournal>\(mConfig\.SaveDataDirectory\);.*?mSaveJournal->RecoverOnce\(\);',
          "oot3d_native_a32_ctr_fs_tests (TestRecoveryBeforeFirstAccess)"),
    Guard("título genérico nunca usa a pasta de save do OoT3D (sdmc:/switch/oot3dre/savedata)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'ResolveCtrTitleSaveDataDirectory\(',
          "oot3d_native_a32_ctr_fs_tests (TestTitleSaveLayout)"),
    Guard("[compatibilidade] o save do OoT3D no Switch continua em sdmc:/switch/oot3dre/savedata",
          "tools/oot3d/native_game_runtime/oot3d_native_game.cpp",
          r'const_cast<char\*>\("--save-data"\),\s+const_cast<char\*>\("sdmc:/switch/oot3dre/savedata"\)'),
    Guard("captura .png gravava BMP (primeira imagem do Mario em PNG)",
          "tools/oot3d/native_demo_host/oot3d_demo_host_screenshot.cpp",
          r'const bool png = ScreenshotPathIsPng\(screenshotPath\);.*?WritePngFromRgb8Framebuffer',
          "oot3d_demo_host_png_tests"),
    # --- 05/11: SM3DL parado no logo amarelo (docs/pesquisas/2026-11-05-sm3dl-logo.md) ---
    Guard("SM3DL: SelfNCCH (arquivo 3, ícone do ExeFS) dava err:f e matava o processo",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_ctr_host_fs.cpp",
          r'archive->Id == kFsSelfNcchArchiveId.*?'
          r'validStaticBuffer && archiveId == kFsSelfNcchArchiveId',
          "oot3d_native_a32_ctr_host_tests (TestSelfNcchArchive)"),
    Guard("SM3DL: ncch_extract não extraía icon/banner/logo do ExeFS (SelfNCCH sem dados)",
          "tools/ctr/ncch_extract.py",
          r'EXEFS_SECTIONS = \(b"icon", b"banner", b"logo"\)',
          "python3 -I tools/ctr/test_ncch_extract.py"),
    Guard("SM3DL: janela/probe sem a pasta do ExeFS (SelfNCCH respondia ExeFSSectionNotFound)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'hostConfig\.ExeFsDirectory = manifest->ExeFsDirectory;'),
    Guard("SM3DL: cfg sem o bloco EULA 0x000D0000 = applet de EULA (0x406) esperado para sempre",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_ctr_host_system.cpp",
          r'case 0x000D0000U: value = \{0x7FU, 0x7FU, 0U, 0U\};.*?'
          r'else if \(auto fixed = AzaharDefaultConfigBlock\(blockId, size\)\)',
          "oot3d_native_a32_ctr_host_tests (TestCfgAzaharDefaultBlocks)"),
    Guard("SM3DL: CMDBUF_JUMP0/1 da PICA ignorado (sub-rotina sem IRQ, lista GX nunca concluía)",
          "tools/oot3d/native_pica_frontend/oot3d_native_pica_frontend.cpp",
          r'kPicaCommandBufferJump0 = 0x23CU;.*?pendingJumpChannel = id - kPicaCommandBufferJump0;.*?'
          r'commandListWords = jumpedListWords;',
          "oot3d_native_pica_frontend_tests"),
    Guard("SM3DL: seletor de Mii (applet 0x402) pré-carregado e GetAppletInfo esperado para sempre",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_ctr_host_applet.cpp",
          r'case kAptGetAppletInfoRequest: \{\s+if \(slot\.Registered && appletId == slot\.AppletId\).*?'
          r'SendAptParameter\(\{appletId, parent, kAptSignalWakeupByExit, nullptr, StandardMiiResult\(\)\}\);',
          "oot3d_native_a32_ctr_host_tests (TestAptLibraryAppletMiiSelector)"),
    Guard("SM3DL: APT:U sem o despacho do applet de biblioteca (Preload/GetAppletInfo viram stub)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_ctr_host_system.cpp",
          r'if \(auto applet = HandleAptAppletRequest\(state, memory, commandBuffer, event\)\) \{'),
    Guard("SM3DL: bloco de captura vazio do seletor de Mii (Map/Unmap no endereço 0) parava o processo",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_ctr_host.cpp",
          r'if \(appletBlock && memoryBlock->Object->FileSize == 0 &&.*?'
          r'case 0x20: \{.*?event\.Name \+= ":applet-capture-empty";',
          "oot3d_native_a32_ctr_host_tests (TestAptLibraryAppletMiiSelector)"),
    Guard("SM3DL: fs:USER OpenDirectory sem HLE (err:f no VBlank ~1080)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_ctr_host_fs.cpp",
          r'if \(event\.Detail == kFsOpenDirectoryRequest\) \{\s+return OpenFsDirectory\(',
          "oot3d_native_a32_ctr_host_tests (TestFsOpenDirectory)"),
    Guard("Listagem de diretório do save mostrava os arquivos do diário (.triaevum-*) ao jogo",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_ctr_host_directory.cpp",
          r'if \(journal != nullptr && IsSaveJournalName\(name\.string\(\)\)\) continue;.*?'
          r'journal->ActivePath\(it->path\(\)\)',
          "oot3d_native_a32_ctr_host_tests (TestFsOpenDirectory)"),
    Guard("SM3DL: depth do PICA sem stencil (D32) faz o quad de sombra pintar chão e céu de cinza",
          "runtime/three_ds_recomp/include/fast/renderer3ds/pica_vulkan_device_profile.h",
          r'for \(const auto format : \{VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT\}\)',
          "oot3d_native_pica_vulkan_bridge_tests (TestPicaDepthFormatKeepsStencil)"),
    Guard("SM3DL: janela sem leitor de command buffer (salto da PICA vira erro)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'picaFrontend\.SetCommandBufferReader\(\s+Oot3dNativeGame::MakeOot3dPicaCommandBufferReader\(picaMemoryView\)\);',
          "oot3d_native_pica_submission_tests"),
    Guard("despejo do estado do guest (TRIAEVUM_GUEST_STATE_DUMP) saiu do laço da janela",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'guestStateDumper != nullptr && guestStateDumper->Due\(vblankCount\)',
          "oot3d_native_a32_ctr_host_tests (TestGuestStateDump)"),
    Guard("build -O1 com GCC parava no -Werror do NRI (maybe-uninitialized em SharedVal.h)",
          "runtime/three_ds_recomp/CMakeLists.txt",
          r'if\(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND TARGET NRI_Shared\)\s+'
          r'(?:#[^\n]*\n\s*)*target_compile_options\(NRI_Shared PUBLIC -Wno-error=maybe-uninitialized\)',
          "build_title_boot_probe.sh com OPT=-O1"),
    Guard("SM3DL na apresentação paralela: pin resolvido nas versões já registradas deixava aposentar a "
          "versão capturada depois (producer version is not registered)",
          "runtime/three_ds_recomp/include/fast/renderer3ds/pica_texture_copy_pins.h",
          r'for \(const auto& reference : retained\) mCompletions\.insert\(reference\.CompletionId\);.*?'
          r'bool Retains\(uint64_t completionId\) const \{\s+return mCompletions\.contains\(completionId\);',
          "oot3d_native_pica_vulkan_bridge_tests (TestPicaTextureCopyPinCoversLaterVersions)"),
    Guard("--savestate-action FRAME:save:PATH ignorava o caminho (dois saves no mesmo segundo viram -2)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_savestate.cpp",
          r'const auto path = request->Path\.empty\(\)\s*\? savestateLibrary->NewStatePath\(request->WithSettings\)\s*: request->Path;',
          "investigação do savestate do SM3DL (save e load com caminho fixo)"),
    Guard("savestate regravava alvo de render abandonado por cima do heap (SM3DL W1-1, vtable de pixels)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_savestate.cpp",
          r'if \(!submissionQueue\.RenderTargetWrittenSinceDraw\(target\.ColorPhysicalAddress\)\) \{\s*'
          r'savestateStats\.PicaColorTargetBytesCaptured \+= color\.size\(\);\s*'
          r'runtime\.GuestMemoryOverlays\.push_back\(\{\*colorAddress, std::move\(color\)\}\);\s*\}.*?'
          r'!submissionQueue\.RenderTargetWrittenSinceDraw\(target\.DepthPhysicalAddress\)',
          "oot3d_native_pica_submission_tests (alvo escrito pela CPU depois do draw)"),
    Guard("alvo de render escrito pela CPU depois do draw tem de ser notado (carimbo por draw)",
          "tools/oot3d/native_game_runtime/oot3d_native_pica_submission.cpp",
          r'mRenderTargetStamps\[address\] = \{byteCount, mMemory\.RangeWriteGeneration\(address, byteCount\),.*?'
          r'return now\.has_value\(\) && now != stamp->second\.Generation;',
          "oot3d_native_pica_submission_tests (alvo escrito pela CPU depois do draw)"),
    Guard("tela de baixo de título genérico nunca aparecia na janela (SM3DL): PiP no canto, OoT3D fora",
          "tools/oot3d/native_game_runtime/ctr_title_profile.h",
          r'bool BottomScreenPictureInPicture = false;.*?GenericTitleProfile.*?'
          r'profile\.BottomScreenPictureInPicture = true;',
          "oot3d_native_a32_ctr_host_tests (TestWindowProfileGatesOot3dHooks)"),
    Guard("PiP da tela de baixo: o Vulkan desenha no canto (PlaceUiPictureInPicture) sobre o quadro",
          "runtime/three_ds_recomp/src/fast/backends/gfx_vulkan_pica_display.cpp",
          r'GfxNativePicaPresentationMode::AlphaOverlay \|\|\s*pictureInPicture\);.*?'
          r'const auto presentation = pictureInPicture\s*\? ::Oot3d::Renderer::PlaceUiPictureInPicture\(',
          "oot3d_native_pica_vulkan_bridge_tests (TestBottomScreenPictureInPictureLayout)"),
    Guard("PiP da tela de baixo: a janela apresenta o transfer de baixo depois do de cima",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'if \(titleProfile\.BottomScreenPictureInPicture &&.*?'
          r'LatestSnapshotCompletion\(.*?bottomSelection\.CompletionId = \*executed;.*?'
          r'PicaPresentationMode::\s*PictureInPicture\)',
          "oot3d_native_pica_vulkan_bridge_tests (modo PictureInPicture e LatestSnapshotCompletion)"),
    Guard("MemoryFill do PICA montava o padrão byte a byte com divisão: 7 ms por fill de 768 KB (SM3DL 3/quadro)",
          "tools/oot3d/native_pica_frontend/oot3d_native_pica_transfer.cpp",
          r'std::copy_n\(value\.begin\(\), first, bytes\.begin\(\)\);\s*'
          r'for \(size_t filled = first; filled < size;\)',
          "oot3d_native_pica_transfer_tests (fill 16/24/32 bits)"),
    Guard("LUT de iluminação: escrita de valor igual copiava 24 KB e refazia o hash (SM3DL: 78% das escritas)",
          "tools/oot3d/native_pica_frontend/oot3d_native_pica_frontend.cpp",
          r'table < Oot3dPicaLightingLutState::TableCount &&\s*'
          r'mLightingLuts->Entry\(table, index\) != writeValue\)',
          "oot3d_native_pica_frontend_tests (escrita repetida)"),
    Guard("clique no PiP da tela de baixo vira toque do 3DS (F7 esconde/mostra), só no título genérico",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'LUS_KB_F7\);.*?const bool lowerScreenPictureInPicture =\s*titleProfile\.BottomScreenPictureInPicture &&\s*'
          r'bottomScreenPictureInPictureVisible;.*?WidescreenMenuCornersActive\(\),\s*lowerScreenPictureInPicture\);',
          "oot3d_native_a32_input_tests (LowerScreenPictureInPicture)"),
    Guard("toque do PiP: o ponteiro é mapeado pelo retângulo do PiP, não pela tela inteira",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_input.cpp",
          r'NativeA32TouchPresentation::LowerScreenPictureInPicture\s*\? Oot3d::Renderer::MapUiPictureInPicturePoint\(',
          "oot3d_native_a32_input_tests (LowerScreenPictureInPicture)"),
    Guard("tela de baixo do SM3DL vinha de 80 linhas dentro do alvo de cima e ia pela CPU (azul): sub-retângulo só no genérico",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'submissionQueue\.SetSubRectDisplayTransfers\(titleProfile\.SubRectDisplayTransfers\);',
          "oot3d_native_pica_submission_tests (sub-retângulo) + oot3d_native_a32_ctr_host_tests (perfil)"),
    Guard("transfer de sub-retângulo: o Vulkan copia das linhas do alvo (origem no blit, NRI só da linha 0)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_vulkan_pica_display.cpp",
          r'PicaDisplayTransferSourceRowsFor\(.*?const bool nriDisplayCopy = sourceFirstRow == 0U &&.*?'
          r'blit\.srcOffsets\[0\] = \{\s*0, static_cast<int32_t>\(sourceFirstRow\), 0\};',
          "oot3d_native_pica_vulkan_bridge_tests (TestDisplayTransferSourceRows)"),
]

# Substituições nativas com VFP que custam mais que a tradução: precisam continuar
# desligadas até alguém medir de novo (FPSCR do guest, 2026-10-18).
DISABLED_OVERRIDES = {"0x003727F0", "0x003325F0", "0x0036E168", "0x00372070", "0x002CDE90"}


def check_guard(guard: Guard) -> str | None:
    path = ROOT / guard.path
    if not path.exists():
        return f"arquivo sumiu: {guard.path}"
    text = path.read_text(encoding="utf-8", errors="replace")
    if not re.search(guard.pattern, text, re.S):
        return f"correção não encontrada em {guard.path}"
    if guard.forbid and re.search(guard.forbid, text, re.S):
        return f"padrão proibido voltou em {guard.path}"
    return None


def check_overrides() -> str | None:
    path = ROOT / "tools/oot3d/native_a32_runtime/guest_overrides.json"
    data = json.loads(path.read_text(encoding="utf-8"))
    entries = data if isinstance(data, list) else next(
        value for value in data.values() if isinstance(value, list))
    disabled = {entry["entry"] for entry in entries if entry.get("disabled")}
    missing = sorted(DISABLED_OVERRIDES - disabled)
    if missing:
        return "substituições VFP religadas sem medição: " + ", ".join(missing)
    return None


def main() -> int:
    if "--list" in sys.argv:
        for guard in GUARDS:
            print(f"- {guard.bug}\n    {guard.path}" + (f"\n    teste: {guard.test}" if guard.test else ""))
        return 0
    failures = 0
    for guard in GUARDS:
        error = check_guard(guard)
        print(("FAIL " if error else "ok   ") + guard.bug + (f"\n     {error}" if error else ""))
        failures += error is not None
    error = check_overrides()
    print(("FAIL " if error else "ok   ") + "5 substituições VFP seguem desligadas"
          + (f"\n     {error}" if error else ""))
    failures += error is not None
    print(f"{len(GUARDS) + 1 - failures}/{len(GUARDS) + 1} guardas ok")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
