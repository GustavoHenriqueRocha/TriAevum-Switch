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
          r'__SWITCH__.*?errc::file_exists.*?filesystem::remove\(target',
          "config_atomic_save_tests"),
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
