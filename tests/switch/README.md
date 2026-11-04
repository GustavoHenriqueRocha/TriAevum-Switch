> Portuguese original: [README.pt-BR.md](README.pt-BR.md)

# Switch port regression tests

Run before every version sent to the console. **Rule: every bug and every error
found gets a regression test here** (unit, guard or check on Eden),
so it doesn't happen again. The goal is that an already
fixed bug never comes back without someone noticing, and that improvements are measured.

| Layer | Command | Time | Catches |
|---|---|---|---|
| Guards | `tests/switch/run.sh --quick` | seconds | bug fix erased by merge/refactor |
| Unit (PC) | `tests/switch/run.sh` | ~5 min (with cache) | runtime logic: pacing, audio, threads, watchdog, FS, pause, FPS counter |
| DSP AArch64 | (included in `run.sh`) | ~1 min | audio mixing with the real Switch flags (devkitA64 + qemu) |
| Eden | `tests/switch/run.sh --eden build/oot3d_native_game.nro` | ~4 min | .nro that doesn't open, hangs or doesn't get past N frames |
| Console | `tests/switch/perf_gate.py logs/perf.txt --mode 30` | — | FPS drop, game speed, stutters |

## Guards (`test_bug_guards.py`)

One entry per bug seen on the console, pointing to the file with the fix and the
behavior test, when there is one. `--list` shows them all. **When fixing a new bug,
add its guard here.**

## Host build (once)

The host CMake needs headers that Arch doesn't have by default. Without sudo:

```sh
P=~/oot3d-work/host-deps   # Vulkan-Headers 1.4.357, nlohmann_json 3.12.0, tinyxml2 11.0.0
cmake -S . -B ~/oot3d-work/build-host-tests -G Ninja \
  -DCMAKE_PREFIX_PATH=$P -DVulkan_INCLUDE_DIR=$P/include \
  -DTHREE_DS_RECOMP_ENABLE_SSSR=OFF -DTHREE_DS_RECOMP_ENABLE_CACAO=OFF
```

`oot3d_native_audio_tests` skips (code 77) without `OOT3D_TEST_EXTRACT_ROOT` pointing
to an unpacked romfs. `oot3d_native_audio_dsp_mix_tests` on the host only matches
the stored digest in a Release build (`-O3 -ffast-math`); what counts is the AArch64 version.

## Performance (`perf_gate.py`)

Baselines in `perf_baseline.json` (final 4, CPU 1785 MHz, handheld):
30 FPS = 29.6 global fps / 98.8% speed; 60 FPS = 57.5 fps / 99.7%.
Only compares with the same CPU clock. A better version becomes the new baseline
with `--update`.

## Eden (`eden_smoke.sh`)

Uses Eden on the Ryzen (`D:\Emuladores\Eden-Windows-v0.2.1...`) in an interactive
scheduled task (`/IT`), because a process opened over ssh has no screen. The folder
`%APPDATA%\eden\sdmc\switch\oot3dre` must have `game/`, `config/` and
`resources.o2r`. The script closes any open Eden before running.

It also fails when: Eden's SD doesn't match the reference SD (corrupted
copy — not a build bug), `boot-status.txt` ends in `status=failed`
(shows the `detail=`) or the game changes files it should only read. These cases
came from the 2026-10-22 bug: upstream's log rotation on open (#72) uses
`rename`, which fails on Horizon, and the broken sink spoiled reading
`atlas_overrides.o3tu` ("payload hash mismatch").
