#!/usr/bin/env bash
# Switch port regression suite. Run before every version sent to the console.
#
#   tests/switch/run.sh            guards + unit tests on PC + DSP on AArch64 (qemu)
#   tests/switch/run.sh --quick    only the static guards (seconds)
#   tests/switch/run.sh --eden NRO also opens the .nro in Eden on the Ryzen (smoke test)
#
# Prerequisites (once): host build configured in $HOST_BUILD (see this folder's README.md).
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
HOST_BUILD="${HOST_BUILD:-$HOME/oot3d-work/build-host-tests}"
HOST_DEPS="${HOST_DEPS:-$HOME/oot3d-work/host-deps}"
JOBS="${JOBS:-3}"

# Switch runtime unit tests (host CMake targets).
UNIT_TESTS=(
    oot3d_deko3d_backend_tests
    oot3d_graphics_settings_schedule_tests
    oot3d_debug_menu_tests
    oot3d_fps_counter_tests
    oot3d_frame_spike_tests
    oot3d_guest_spike_profile_tests
    oot3d_hang_watchdog_tests
    oot3d_hitch_recorder_tests
    oot3d_input_bench_tests
    oot3d_native_a32_ctr_fs_tests
    oot3d_native_a32_ctr_host_tests
    oot3d_native_audio_interpolation_tests
    oot3d_native_audio_storage_tests
    oot3d_native_audio_tests
    oot3d_native_cadence_governor_tests
    oot3d_native_dynamic_resolution_tests
    oot3d_native_interpolation_pacing_tests
    oot3d_native_offload_tests
    oot3d_native_pica_deferred_plan_tests
    oot3d_native_pica_visual_cull_tests
    oot3d_native_pica_visual_frame_equivalence_tests
    oot3d_native_render_command_stream_tests
    oot3d_native_render_shutdown_tests
    oot3d_pica_etc1_transcode_tests
    oot3d_pica_texture_predecode_tests
    oot3d_host_menu_touch_tests
    oot3d_rom_import_tests
    oot3d_switch_motion_tests
    oot3d_switch_thread_layout_tests
    oot3d_top_screen_hud_touch_tests
    oot3d_top_screen_mod_profile_tests
    oot3d_top_screen_pause_transaction_tests
    oot3d_top_screen_texture_overrides_tests
    triaevum_audio_service_tests
    three_ds_recomp_input_tests
)

pass=0; fail=0; skip=0; failed=()
result() { # name code
    case "$2" in
        0) pass=$((pass + 1)); echo "  ok    $1" ;;
        77) skip=$((skip + 1)); echo "  skip  $1" ;;
        *) fail=$((fail + 1)); failed+=("$1"); echo "  FAIL  $1 (code $2)" ;;
    esac
}

echo "== guards for fixed bugs"
python3 "$here/test_bug_guards.py" > /tmp/switch-guards.$$ 2>&1; code=$?
grep -E '^FAIL|guards ok' /tmp/switch-guards.$$ | sed 's/^/  /'; rm -f /tmp/switch-guards.$$
result test_bug_guards "$code"
python3 "$here/test_perf_tools.py" > /tmp/switch-perf-tools.$$ 2>&1; code=$?
grep -E '^FAIL' /tmp/switch-perf-tools.$$ | sed 's/^/  /'; rm -f /tmp/switch-perf-tools.$$
result test_perf_tools "$code"
(cd "$here/../.." && python3 -m unittest tools.triaevum_release.test_topscreen_30_pack) > /tmp/switch-topscreen.$$ 2>&1; code=$?
grep -E '^(FAIL|ERROR)' /tmp/switch-topscreen.$$ | sed 's/^/  /'; rm -f /tmp/switch-topscreen.$$
result test_topscreen_30_pack "$code"
[[ "${1:-}" == "--quick" ]] && { echo "passed $pass, failed $fail"; exit $((fail > 0)); }

echo "== unit tests (host: $HOST_BUILD)"
if [[ ! -f "$HOST_BUILD/build.ninja" ]]; then
    echo "  host build not configured; see tests/switch/README.md"; result host_build 1
else
    nice -n 10 ninja -C "$HOST_BUILD" -j "$JOBS" "${UNIT_TESTS[@]}" > "$HOST_BUILD/switch-tests-build.log" 2>&1 \
        || echo "  warning: some target didn't compile (see $HOST_BUILD/switch-tests-build.log)"
    for test in "${UNIT_TESTS[@]}"; do
        binary="$(find "$HOST_BUILD" -type f -executable -name "$test" -print -quit)"
        [[ -z "$binary" ]] && { result "$test" 1; continue; }
        OOT3D_TEST_EXTRACT_ROOT="${OOT3D_TEST_EXTRACT_ROOT:-}" timeout 600 "$binary" \
            > "$HOST_BUILD/$test.log" 2>&1
        result "$test" $?
    done
fi

echo "== DSP mixing with the Switch flags (devkitA64 + qemu-aarch64)"
NLOHMANN_JSON_INCLUDE="${NLOHMANN_JSON_INCLUDE:-$HOST_DEPS/include}" \
    bash "$repo/tools/oot3d/native_game_runtime/run_audio_dsp_mix_tests_qemu.sh" \
    "${TMPDIR:-/tmp}/oot3d-audio-dsp-mix-aarch64" > "${TMPDIR:-/tmp}/oot3d-dsp-mix.log" 2>&1
result oot3d_native_audio_dsp_mix_aarch64 $?

if [[ "${1:-}" == "--eden" ]]; then
    echo "== smoke test on Eden (Ryzen)"
    bash "$here/eden_smoke.sh" "${2:?path to the .nro}"; result eden_smoke $?
fi

echo "passed $pass, failed $fail, skipped $skip"
((fail)) && printf '  failed: %s\n' "${failed[@]}"
exit $((fail > 0))
