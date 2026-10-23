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
          r'OpenGlContextCurrent\(\)\) \{\s+ImGui_ImplOpenGL3_RenderDrawData\(data\);\s+\} else if \(mRendererRouting\.Deferred',
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
