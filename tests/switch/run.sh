#!/usr/bin/env bash
# Suíte de regressão do port Switch. Rodar antes de cada versão enviada ao console.
#
#   tests/switch/run.sh            guardas + testes de unidade no PC + DSP em AArch64 (qemu)
#   tests/switch/run.sh --quick    só as guardas estáticas (segundos)
#   tests/switch/run.sh --eden NRO também abre o .nro no Eden da Ryzen (teste de fumaça)
#
# Pré-requisitos (uma vez): build de host configurado em $HOST_BUILD (ver README.md desta pasta).
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
HOST_BUILD="${HOST_BUILD:-$HOME/oot3d-work/build-host-tests}"
HOST_DEPS="${HOST_DEPS:-$HOME/oot3d-work/host-deps}"
JOBS="${JOBS:-3}"

# Testes de unidade do runtime do Switch (alvos do CMake de host).
UNIT_TESTS=(
    oot3d_graphics_settings_schedule_tests
    oot3d_debug_menu_tests
    oot3d_fps_counter_tests
    oot3d_hang_watchdog_tests
    oot3d_hitch_recorder_tests
    oot3d_input_bench_tests
    oot3d_native_a32_ctr_fs_tests
    oot3d_native_a32_ctr_host_tests
    oot3d_native_audio_interpolation_tests
    oot3d_native_audio_storage_tests
    oot3d_native_audio_tests
    oot3d_native_cadence_governor_tests
    oot3d_native_interpolation_pacing_tests
    oot3d_native_offload_tests
    oot3d_native_pica_deferred_plan_tests
    oot3d_native_pica_visual_frame_equivalence_tests
    oot3d_native_render_command_stream_tests
    oot3d_native_render_shutdown_tests
    oot3d_pica_etc1_transcode_tests
    oot3d_pica_texture_predecode_tests
    oot3d_switch_thread_layout_tests
    oot3d_top_screen_mod_profile_tests
    oot3d_top_screen_pause_transaction_tests
    oot3d_top_screen_texture_overrides_tests
    triaevum_audio_service_tests
)

pass=0; fail=0; skip=0; failed=()
result() { # nome código
    case "$2" in
        0) pass=$((pass + 1)); echo "  ok    $1" ;;
        77) skip=$((skip + 1)); echo "  pulou $1" ;;
        *) fail=$((fail + 1)); failed+=("$1"); echo "  FALHA $1 (código $2)" ;;
    esac
}

echo "== guardas dos bugs corrigidos"
python3 "$here/test_bug_guards.py" > /tmp/switch-guards.$$ 2>&1; code=$?
grep -E '^FAIL|guardas ok' /tmp/switch-guards.$$ | sed 's/^/  /'; rm -f /tmp/switch-guards.$$
result test_bug_guards "$code"
[[ "${1:-}" == "--quick" ]] && { echo "passaram $pass, falharam $fail"; exit $((fail > 0)); }

echo "== testes de unidade (host: $HOST_BUILD)"
if [[ ! -f "$HOST_BUILD/build.ninja" ]]; then
    echo "  build de host não configurado; ver tests/switch/README.md"; result host_build 1
else
    nice -n 10 ninja -C "$HOST_BUILD" -j "$JOBS" "${UNIT_TESTS[@]}" > "$HOST_BUILD/switch-tests-build.log" 2>&1 \
        || echo "  aviso: algum alvo não compilou (ver $HOST_BUILD/switch-tests-build.log)"
    for test in "${UNIT_TESTS[@]}"; do
        binary="$(find "$HOST_BUILD" -type f -executable -name "$test" -print -quit)"
        [[ -z "$binary" ]] && { result "$test" 1; continue; }
        OOT3D_TEST_EXTRACT_ROOT="${OOT3D_TEST_EXTRACT_ROOT:-}" timeout 600 "$binary" \
            > "$HOST_BUILD/$test.log" 2>&1
        result "$test" $?
    done
fi

echo "== mixagem DSP com as flags do Switch (devkitA64 + qemu-aarch64)"
NLOHMANN_JSON_INCLUDE="${NLOHMANN_JSON_INCLUDE:-$HOST_DEPS/include}" \
    bash "$repo/tools/oot3d/native_game_runtime/run_audio_dsp_mix_tests_qemu.sh" \
    "${TMPDIR:-/tmp}/oot3d-audio-dsp-mix-aarch64" > "${TMPDIR:-/tmp}/oot3d-dsp-mix.log" 2>&1
result oot3d_native_audio_dsp_mix_aarch64 $?

if [[ "${1:-}" == "--eden" ]]; then
    echo "== teste de fumaça no Eden (Ryzen)"
    bash "$here/eden_smoke.sh" "${2:?caminho do .nro}"; result eden_smoke $?
fi

echo "passaram $pass, falharam $fail, pulados $skip"
((fail)) && printf '  falhou: %s\n' "${failed[@]}"
exit $((fail > 0))
