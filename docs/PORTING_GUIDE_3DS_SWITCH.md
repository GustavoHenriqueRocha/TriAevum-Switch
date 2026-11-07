> Original Portuguese version: [PORTING_GUIDE_3DS_SWITCH.pt-BR.md](PORTING_GUIDE_3DS_SWITCH.pt-BR.md)

# 3DS → Switch porting guide (reusable recipe)

Recipe and checklist for porting the **next** 3DS game to Nintendo Switch homebrew,
reusing what was done in TriAevum (Zelda: Ocarina of Time 3D). OoT3D is a static
ARM11/A32 → C++ recompilation (12,439 generated functions in 256 shards, its own runtime,
no interpreter in the product), ported to the Switch with libnx, SDL2 and OpenGL/Mesa, compiled
with GCC 16 `-O3` and PGO.

The long-term goal is **Forge for Switch**: a generic ROM → `.nro` recompiler that combines
the function discovery of [fearkov/3dsrecomp](https://github.com/fearkov/3dsrecomp) with the
generator and Switch runtime of this repository. (The original project already uses the name "Forge" for the
module that compiles the title on Windows; here "Forge for Switch" is the equivalent plan for the
console.)

Sources for this guide: [SWITCH_PORT.md](../SWITCH_PORT.md) (main source),
[OOT3D_SWITCH_WHOLE_AOT_PORT.md](OOT3D_SWITCH_WHOLE_AOT_PORT.md),
[SWITCH_RESEARCH_2026-10-17.md](SWITCH_RESEARCH_2026-10-17.md),
[SWITCH_RESEARCH_XBOX360_2026-10-17.md](SWITCH_RESEARCH_XBOX360_2026-10-17.md),
[TRIAEVUM_AOT_LEVERS_RESULTS.md](TRIAEVUM_AOT_LEVERS_RESULTS.md),
[TRIAEVUM_NATIVE_CPU_COSTS.md](TRIAEVUM_NATIVE_CPU_COSTS.md), the code and the commit messages
of the `switch-port` branch (starting at `eba0b4b7`, 2026-10-16). Numbers without a source in the repository are
marked as **to be measured**; numbers from session notes (console tests that never became a
document) are marked as *(session notes)*.

> No ROM, `code.bin`, RomFS, generated C++, `.nro` or derivatives in Git or in a release. The `.nro`
> contains the translated game code: it is built on the machine of whoever has their own dump.

## Contents

0. [What is generic and what is OoT3D-specific](#0-what-is-generic-and-what-is-oot3d-specific)
1. [Pipeline](#1-pipeline)
2. [Native replacements (guest overrides)](#2-native-replacements-guest-overrides)
3. [Generator optimizations](#3-generator-optimizations)
4. [Switch runtime](#4-switch-runtime)
5. [Measurement and diagnostics](#5-measurement-and-diagnostics)
6. [Infrastructure](#6-infrastructure)
7. [Pitfalls and lessons](#7-pitfalls-and-lessons)
8. [Next-game checklist](#8-next-game-checklist)
9. [Known gaps in this guide](#9-known-gaps-in-this-guide)

---

## 0. What is generic and what is OoT3D-specific

| Piece | Generic (applies to any 3DS game) | OoT3D-specific |
|---|---|---|
| ROM extraction | `tools/triaevum_release/ctr_rom.py` (NCCH, ExeFS, RomFS, ExHeader) | canonical USA→EUR adapter (`oot3d-usa-input-canonical-eur-ef210566`) |
| Function discovery | A32 front end in `tools/oot3d/native_a32_runtime/upstream/` (CFG, blocks, dynamic exits) | the inventory `upstream/analysis/codebin_function_inventory.csv`, `runtime_discovered_entries.csv` and the selection `whole_aot_functions.json` were reviewed by hand for OoT3D |
| C++ generator | `generate_whole_aot_cpp.py` / `whole_aot_cpp.py` and all the options in section 3 | `HOST_WRITTEN_TEXT_RANGES` (0x003CC430, Obj_Mure) in literal folding; TopScreen hooks |
| CPU/memory runtime | `oot3d_native_a32_memory.*`, `oot3d_native_whole_aot_runtime.h`, VFP (`oot3d_native_a32_vfp_*`) — the name says "oot3d", the content doesn't | 3 audited host boundaries of the product |
| CTR services (fs, gsp, dsp, hid, ptm) | `oot3d_native_a32_ctr_*`, Azahar DSP HLE (`third_party/azahar_audio`) | Master Quest (`oot3d_native_master_quest_unlock.h`), clock read by `nn::fnd::DateTime::GetNow` |
| GPU (PICA → GL) | `runtime/three_ds_recomp/src/fast/backends/gfx_opengl_pica*` (state cache, residency, exact savings, profile) | menu widescreen (`gl_widescreen_menu_policy.h`), projection hook, TopScreen |
| Threads, audio, pacing, perf | `oot3d_switch_threads.*`, `oot3d_switch_thread_layout.h`, `oot3d_host_audio_stream.*`, `oot3d_native_interpolation_pacing.h`, `perf.txt` | — |
| Native replacements | the whole mechanism (prologue, decline, harness, counters) | the 23 functions in the list |
| Infra | build on the Ryzen/WSL, ccache, PGO, FTP/MTP deploy | path `sdmc:/switch/oot3dre/` (becomes `sdmc:/switch/<game>/`) |

Rule of thumb: **everything that is "how" is generic; everything that is "which addresses" belongs to the game.**

---

## 1. Pipeline

```
ROM (decrypted .3ds/.cci/.cia)
 └─ ctr_rom.py ──────────────► extract/ (exefs/code.bin, ncch0/exheader.bin, romfs)
     └─ (region adapter, if any)
 └─ generate_whole_aot.py ───► aot_program.json (functions, blocks, calls, dynamic exits) + inventory.csv
 └─ generate_whole_aot_cpp.py ► gen/cpp/oot3d_whole_aot_shard_000..255.cpp + whole_aot_cpp_manifest.json
     └─ apply_guest_overrides.py (or --guest-overrides) inserts the native prologues
 └─ CMake + devkitA64 (GCC 16, -O3, PGO) ► build-switch/oot3d_native_game.nro
 └─ stage-product.sh ────────► sd/switch/oot3dre/ (nro + game/ + resources.o2r + config/)
 └─ FTP (Sphaira, port 5000) or MTP ► SD card → open from the Homebrew Menu in title mode
```

### 1.1 Commands

Prerequisites: devkitPro (`switch-dev switch-sdl2 switch-mesa switch-glad switch-zlib`), `cmake`,
`ninja`, `ccache`, Python 3 with `capstone`. For the on-disk shader cache, the rebuilt Mesa
(section 4.9).

```sh
# 1. Extraction (and, for the OoT3D USA ROM, the adapter
#    input_adapters.adapt_extracted_inputs with the recipe oot3d-usa-input-canonical-eur-ef210566)
python tools/triaevum_release/ctr_rom.py ...        # produces exefs/code.bin, ncch0/exheader.bin, romfs
export OOT3D_OPERATIONAL_oot3dExtractRoot=/path/extract

# 2. Structural program and C++
cd tools/oot3d/native_a32_runtime
python generate_whole_aot.py --output $W/gen/aot_program.json --inventory-output $W/gen/inventory.csv
python generate_whole_aot_cpp.py --program $W/gen/aot_program.json --selection whole_aot_functions.json \
  --code $OOT3D_OPERATIONAL_oot3dExtractRoot/exefs/code.bin --output $W/gen/cpp \
  --shards 256 --shard-strategy affinity \
  --native-vfp-aarch64 --guest-overrides --giant-function-instructions 1000

# 3. Configure and compile the NRO
cmake -S . -B build-switch -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake \
  -DCMAKE_BUILD_TYPE=Release -DOOT3D_ENABLE_VULKAN_RENDERER=OFF \
  -DOOT3D_WHOLE_AOT_PRODUCT_MODE=ON -DOOT3D_REQUIRE_WHOLE_AOT=ON -DOOT3D_REBUILD_WHOLE_AOT=ON \
  -DOOT3D_REGENERATE_WHOLE_AOT=OFF -DOOT3D_WHOLE_AOT_GENERATED_DIR=$W/gen/cpp \
  -DOOT3D_WHOLE_AOT_PROGRAM=$W/gen/aot_program.json \
  -DOOT3D_WHOLE_AOT_CODE_BIN=... -DOOT3D_WHOLE_AOT_EXHEADER=... \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build build-switch --target oot3d_native_game_nro

# 4. SD folder
scripts/switch/stage-product.sh build-switch/oot3d_native_game.nro /path/extract /path/sd
```

Ready-made alternatives in `scripts/switch/`: `build-whole-aot.sh` (only the ARM64 `.a` archive of the
shards), `build-whole-aot-probe.sh` (minimal NRO that runs one block and writes a receipt — a good
**first gate** for a new game), `build-product.sh` (whole product), `run-product-eden.sh`
and `run-whole-aot-probe-eden.sh` (smoke test in the Eden emulator).

Times: full build ~1h30 on 4 cores (laptop) and ~30–60 min on the Ryzen 5 5600G with 12 jobs;
incremental with ccache ~1 min; PGO `use` build 18 min *(session notes)*.

### 1.2 What changes for a new game

- `ctr_rom.py` is generic. Region adapters exist only for OoT3D.
- **Function discovery is the big job.** OoT3D inherited a reviewed inventory
  (`upstream/analysis/codebin_function_inventory.csv`) and a profile-based selection
  (`whole_aot_functions.json`). For another game, the Forge idea is to generate that inventory with
  3dsrecomp's discovery (and use its `verify` as a test), then feed
  `whole_aot_program.py`. Entries found only at run time go to `runtime_discovered_entries.csv`.
- The `sdmc:/switch/oot3dre/...` paths are scattered across the runtime (configs, logs, PGO, shader
  cache). For another game, change the folder name (gap: it is not a single constant yet).
- Validate early with the probe (`build-whole-aot-probe.sh`): it runs the first block of the entrypoint and
  checks continuation by block limit.

---

## 2. Native replacements (guest overrides)

### 2.1 What they are

Game functions (or loops inside functions) rewritten by hand in native C++, with the **same observable
effect** as the translation: registers, lazy flags, VFP/FPSCR, memory, write generations,
block budget, hooks and tracing. It is a "partial decompilation" only where the profile says so.

- Implementation: `tools/oot3d/native_game_runtime/oot3d_native_guest_overrides.{h,cpp}`.
- List: `tools/oot3d/native_a32_runtime/guest_overrides.json` (format `oot3d_guest_overrides_v1`).
- Wiring into the generated C++: `apply_guest_overrides.py` (idempotent, `guest-override` markers,
  `--remove` undoes) or `generate_whole_aot_cpp.py --guest-overrides`. Only the shards of the listed
  functions change.
- On/off without recompiling: `config/no_guest_overrides` on the SD (desktop:
  `TRIAEVUM_GUEST_OVERRIDES=0`).

Two kinds:

1. **Whole function** (`{"entry": "0x..."}`): the prologue goes at the start of
   `Execute_<name>_<ENTRY>` and only acts when the function is called through its own entry.
2. **Block/loop** (`{"entry": block, "function": function, "resume": [PCs]}`): the prologue goes right
   after the `block_<BLOCO>:` label of the translated function; the native version runs the hot loop and returns
   `resumePc`, where the translation continues with `goto block_<PC>`, or `resumePc = 0` with an exit
   flow (budget limit etc.). Before the call the prologue does `commitState()` and afterwards
   `reloadState()`.

### 2.2 Picking candidates from the real console profile

1. Generate the name map and copy it to the SD:
   `python summarize_guest_profile.py --program aot_program.json --export-function-map guest_functions.txt`
   → `sdmc:/switch/<game>/config/guest_functions.txt`.
2. Create `config/guest_profile` (optional keys: `sample_every=4`, `dump_frames=900`,
   `top_functions`, `top_blocks`, `window=1`, `max_gap_us=200`). Play the heavy scene for a few minutes.
   Out comes `logs/guest_profile.txt`: exclusive time per block and per function (19.2 MHz counter),
   plus the `# native overrides` appendix with `native=N declined=M` per replacement.
3. Aggregate per function:
   `python summarize_guest_profile.py --program aot_program.json --switch-profile guest_profile.txt --top 40 --markdown`.
   The script joins static data (instructions, blocks, VFP, memory accesses, calls, leaf) and
   marks what already has a replacement.
4. **Subtract the profiler overhead.** Each block entry costs ~190 ns to the profiler itself
   (in the Kokiri forest `guest_profile.txt` short blocks show 184–216 ns per entry). This
   inflates functions made of many short blocks (the audio effect loops showed up at ~14%).
   `summarize_guest_profile.py` estimates the overhead (2nd percentile of ns/entry) and subtracts it; force it
   with `--overhead-ns`.
5. Prefer: leaves or near-leaves, copy/zeroing/search loops, VFP math without calls,
   functions with many entries and little work per entry. Confirm with a second source (the
   PGO count: `whole_aot_hot_from_gcda.py`). The 2026-10-17 batch was chosen this way (the six
   highest-count functions excluding audio in PGO collection 2).
6. Remove `config/guest_profile` from the SD when done: it makes the game slower.

> **Warning (read in the code, 2026-10-17):** in the measured frames the profiler installs the block-entry
> callback in "all blocks" mode (`oot3d_native_a32_window_frame.cpp`, empty PC
> list). With that, `Hooked()` returns true for any range and **every** replacement declines
> in those frames. Consequences: (a) with `sample_every=N`, up to ~1/N of the calls show up as
> declined because of the profiler itself — in Kokiri (N=4) most replacements were between
> 14% and 25% declined; (b) the measured time is always the translation's, never the native version's (good
> for picking candidates, useless for measuring their gain). Real decline is whatever goes well beyond that
> (see 2.6). The counters are only written with the profiler on (gap 9).

**Spike profile (`config/guest_spike_ms`, pgo5o).** tour19 showed that 79% of the 428 missed
refreshes had the game thread as the culprit (`stage=guest`), with 20–26 ms frames against an 8–11
average (Water Temple: 38 in one window). The per-function average of `guest_profile.txt` dilutes those
frames; the spike profile keeps only them. File present turns it on (default **off**: it uses the
"all blocks" mode, which makes the game slower); optional content `threshold [cutoff]` in ms (default
`14 50`). It turns the profiler on in every frame even without `config/guest_profile` (then without the
periodic report). How it works (`oot3d_guest_spike_profile.h`): each interval the profiler attributes to a
block also goes into a per-block `uint32` for the frame and into a list of touched blocks (allocated once;
nothing allocates per frame); at the end of the host frame, if guest time went over the limit, it aggregates the blocks
per function (map `guest_functions.txt`; without it, per block), keeps the 10 most expensive in a ring of 128 and
adds all of them into the summary (fixed hash); then it zeroes only the touched blocks. Every 150 frames (and at the end)
it writes `logs/guest_spikes.txt` (appends) and rewrites `logs/guest_spikes_summary.txt`. Frames
above the cutoff (loads) go to the log with `load=1` and stay out of the summary. The times include the
profiler overhead (~190 ns per block entry): subtract it using the spike's `entries=`. Format in
5.1. Test: `oot3d_guest_spike_profile_tests`.

### 2.3 Writing an EXACT replacement

Contract (header of `oot3d_native_guest_overrides.h`):

- **`false` = decline BEFORE any side effect.** The translation runs in full, untouched. Decline
  when: insufficient block budget (`Budget(context, n)`), some block hook covers the function's
  range (`Hooked(context, first, last)`), FPSCR outside the default mode (`VfpDefault`: LEN/STRIDE or
  traps), an access that would not take the memory fast path (`Readable`, `Writable`,
  `SinglePageReadable`), misaligned pointers, pointers onto the stack or onto the object itself, counts
  outside what is expected (e.g. channels > 4 in the audio effect).
- **`true` = left everything exactly as the translation would**: GPRs, CPSR (with the same liveness
  masks as the generated code), VFP, FPSCR, memory bytes, global and per-page write
  generation (each word written counts +1, the page is stamped with the generation of the last word),
  `BlocksRemaining/BlocksConsumed` (consume block by block; if the budget runs out midway, exit with
  `BlockLimit` exactly where the translation would exit) and the `Oot3dWholeAotFlow`.
- **Same order of memory accesses** and the same calls when the translation calls another function
  (head/tail of `Runtime_Memcpy` use the same calls as the translation).
- Guest arithmetic: integers in `uint32_t` (MUL/MLA wrap, arithmetic ASR, sign-symmetric
  rounding like the original); VFP through the same `Oot3dVfpNative*` operations the translation
  uses (do not use host `float` directly).
- Counters: `Decline(index)` increments `Declined`, `Accept(index)` increments `Calls`; the
  `gOverrides` table in the `.cpp` must have the entry and the name.

Step by step for a new one:

1. Read the translated function in `gen/cpp/oot3d_whole_aot_shard_NNN.cpp` (`Execute_*_<ENTRY>`) and the
   disassembly; note the PC range, how many blocks each path consumes and what is read/written.
2. Declare with `OOT3D_GUEST_OVERRIDE_DECLARE(<ENTRY>)` or
   `OOT3D_GUEST_BLOCK_OVERRIDE_DECLARE(<BLOCO>)` in the `.h`, implement in the `.cpp`, add to the table.
3. Add the item to `guest_overrides.json` (with `switch_share` from the profile).
4. Prove equivalence (2.4). Only then wire it into the generated C++ and recompile the affected shards.
5. Measure on the console with the profiler **off** and compare `perf.txt` (logic per step) with
   `config/no_guest_overrides`, same save, same scene, **same clock**.

### 2.4 Proving equivalence

- Harness: `tools/oot3d/native_a32_runtime/run_guest_override_equivalence.py` +
  `guest_override_equivalence.cpp`. The script extracts from the generated C++ (without prologues) each listed function
  and the closure of the ones it calls, renames them to `Translated_<ENTRY>` (block: the whole function and a
  copy with the prologue, `Patched_<BLOCO>`, both starting at the block) and compiles with the same
  floating-point flags as the build (`-fno-fast-math -ffp-contract=off`).
- Each case starts from identical copies (state, memory, budget, hooks, trace mode) and requires
  everything to be equal at the end: GPRs, CPSR, FPSCR, VFP, flow, budget, statistics, hook sequence,
  bytes, global and per-page generations, write fingerprint.
- Random cases + edges: pointers in data, crossing a page, misaligned, read-only,
  unmapped, onto the object, aliasing, object flush against the end of the page, overlap, stack,
  huge sizes, budget that runs out midway.

```sh
python run_guest_override_equivalence.py --generated $W/gen/cpp --code $W/code-canonico.bin \
  --nlohmann $W/build-switch/_deps/nlohmann_json-src/include --output /tmp/eq --trials 2000000
python run_guest_override_equivalence.py ... --only 0x004A0338            # just one
python run_guest_override_equivalence.py ... --extra-flags "-O3"          # like the build
python run_guest_override_equivalence.py ... --codegen-cxx aarch64-none-elf-g++   # counts ARM64 instructions
```

Volume used so far: 3M+1M cases per audio function and 3M on memcpy (ba8cce93); 300 thousand per
function (500 thousand on CirSquare) in the block batch (e26eb2d1); 1M per function at `-O3` after TBAA
(ad972f24). Zero differences in all of them.

ASan/UBSan and qemu-aarch64: the harness has no ready-made mode (gap 9: `--extra-flags` does not reach the
link). The repo's qemu pattern is `run_vfp_aarch64_tests_qemu.sh` (devkitA64 + shard flags +
`qemu_aarch64_test_runtime.cpp`, static `-nostartfiles` binary), also used by
`run_audio_dsp_mix_tests_qemu.sh` and `memory_lean_diff/run_store_sequence_diff.sh` (`TARGET=switch`).

### 2.5 The 23 current replacements (OoT3D)

`switch_share` = fraction of guest time in the console profile (PGO collection 1); `kokiri_share` = desktop
block profile in the Kokiri forest. They measure the **translated** cost; the real gain of each
one was not measured in isolation.

| Entry | Name | Kind | What it does | Profile weight |
|---|---|---|---|---|
| 0x004A0338 | AudioEffect_ProcessStereoReverb | function | stereo reverb, u32 integers, block-by-block budget | switch 5.35% |
| 0x004A022C | AudioEffect_ProcessFourChannelDelay | function | 4-channel delay | switch 2.02% |
| 0x00371738 | Runtime_Memcpy | function | 32-byte loop becomes a per-page-chunk copy (+1 generation per word) | switch 1.38% |
| 0x003727F0 | Math_SinF | function | VFP sine | kokiri 1.00% |
| 0x00307C94 | PicaCommandWriter_UploadVertexFloatUniforms | function | upload of float uniforms into the PICA packet | kokiri 1.03% |
| 0x003325F0 | Math3D_CylOutsideCylDist | function | cylinder–cylinder distance | kokiri 0.64% |
| 0x0036E168 | Math_SmoothStepToF | function | smooth approach | kokiri 0.61% |
| 0x002F9430 | PauseRenderBuffer_SetVec2Range | function | fills a vec2 range | kokiri 0.54% |
| 0x00372070 | oot3d_translate_mtx3x4_local | function | local translation of a 3×4 matrix | kokiri 0.44% |
| 0x003094CC | FUN_003094cc | function | (unnamed) | kokiri 0.41% |
| 0x002CDE90 | FUN_002cde90 | function | (unnamed) | kokiri 0.36% |
| 0x00466E68 | MeshCommandPacket_Submit:copy | block | copies the packet into the list | switch 3.02% |
| 0x003247B0 | BgCheck_LineTestImpl:clear_marks | block | zeroes the polygon map | switch 2.53% |
| 0x0047AF24 | FUN_0047af24 | function | search over 50 entries | switch 1.31% |
| 0x002FA674 | CalculateGyroscopeAxisStatus:mean | block | average over the ring | switch 1.04% |
| 0x00307B50 | PicaTev_ConvertSource | function | TEV source conversion | switch 0.84% |
| 0x00307DC8 | PicaCompare_ConvertFunction | function | comparison function conversion | switch 0.53% |
| 0x00308920 | NativeCurve_SampleFloat:search | block | keyframe search | switch 0.53% |
| 0x00308604 | CsabCurve_SampleRotation:search | block | keyframe search (rotation) | switch 0.34% |
| 0x002BBB14 | Math3D_CirSquareVsTriSquare | function | circle×triangle test, VFP only | switch 0.70% |
| 0x00303C78 | FUN_00303b24:copy | block | short 64-byte copy | switch 0.23% |
| 0x0046700C | MeshCommandPacket_PatchBoneUniforms:copy | block | short copy in a loop | switch 0.21% |
| 0x003563E0 | FUN_003562c0:walk | block | walk of the collision polygon list | switch 0.55% |

Documented gain: the first batch (audio + memcpy) was estimated at ~4% of logic (SWITCH_PORT.md).
The full batch (23) went to the console together with PGO 3, TBAA and fps60 ("final 3" build), with no isolated
A/B: **to be measured** with `config/no_guest_overrides`.

Windows reference (another machine, another compiler): lowering the same audio kernels via
`--memory-region` gave no gain (0.996, TRIAEVUM_AOT_LEVERS_RESULTS.md). On the Switch the hand-written
version showed up as a gain because GCC did not inline the helpers and the per-block overhead is higher —
measure before assuming it pays off on a third target.

### 2.6 Lesson: a high decline rate kills the gain

Counters from the Kokiri session (fps60 + overclock, `sample_every=4`, 2251 measured frames; file
`switch-logs/profile-kokiri/guest_profile.txt`, outside the repo):

| Replacement | Declined | Reading |
|---|---:|---|
| BgCheck_LineTestImpl:clear_marks | 99.4% | almost never runs native: wrong precondition for the common case |
| CalculateGyroscopeAxisStatus:mean | 96.4% | same |
| FUN_003562c0:walk | 76.0% | same |
| NativeCurve_SampleFloat:search | 65.5% | same |
| CsabCurve_SampleRotation:search | 59.1% | same |
| the other 18 | 14–25% | consistent with the profiler artifact (1 in 4 frames with all blocks on) |

A decline costs the precondition test **plus** the whole translation, and in block loops also the
`commitState()/reloadState()`. With 60–99% decline the replacement is worse than nothing. Rules:

- Before enabling, run on the console and look at `declined` against `native` (discounting the profiler's ~1/N).
- Decline only what the translation would really do differently; handle the common case in the native path
  (e.g. sweep the range across several fast pages instead of requiring a single one).
- Putting the decline **reason** in the report (a counter per reason) is the natural next step.

### 2.7 Pending updates

> Another agent (worktree `.claude/worktrees/nativas2`) is widening the preconditions of the
> high-decline replacements and adding new ones (e.g. MeshCommandPacket_Submit). Fill in
> here when it is integrated: which declines dropped, new items in table 2.5, equivalence run and gain
> measured.
>
> - [ ] declines after the fix (per replacement)
> - [ ] new replacements
> - [ ] A/B on the console with `config/no_guest_overrides`, same clock

---

## 3. Generator optimizations

Everything is in `tools/oot3d/native_a32_runtime/generate_whole_aot_cpp.py` (emission in `whole_aot_cpp.py`) and
in the header runtime (`tools/oot3d/native_game_runtime/oot3d_native_whole_aot_runtime.h`,
`oot3d_native_a32_memory.h`). Generic: applies to any ARM11 code.

| Technique | Where / option | Validation | Result |
|---|---|---|---|
| Inline fast path + cold slow path in helpers | `OOT3D_AOT_HOT/COLD` (GCC only); `ReadFast/WriteFast` → `*Slow` | slow bodies unchanged | to be measured (on GCC almost every helper became an out-of-line call) |
| LDM/STM by span | `_emit_block_transfer`; `ReadWordsFast/WriteWordsFast` | `test_whole_aot_transfer_lowering.py` (faults midway, partial write) | to be measured |
| Folded PC-relative literals | default; `--no-literal-folding` turns it off | folding/exclusion tests | to be measured |
| Native AArch64 VFP | `--native-vfp-aarch64` / `OOT3D_WHOLE_AOT_NATIVE_VFP_AARCH64` | 193.5 million checks in qemu, 16 modes | Switch: to be measured; x86 analogue on Windows: 0.924 of pre-NRI time |
| Aliasing via TBAA | default on GCC; `-DOOT3D_GUEST_ACCESS_MEMCPY` goes back to memcpy | `memory_lean_diff/run_store_sequence_diff.sh` | −4.0% instructions, −6.7% loads, −7.2% stores, −19.5% stack reloads |
| Giant functions | `--giant-function-instructions N` + `--hot-functions` | same external symbols, `.text` +0.02% | Fishing_UpdateFish compiles in 88 s (before 192 s) |
| Limited GCSE | `OOT3D_WHOLE_AOT_GCSE_MEMORY_KB=32768` | hot shards 091/243 byte-for-byte identical | shard 100: PRE 85 s → 1 s |
| Stable sharding | `--shard-strategy affinity-incremental` | same distribution on the 1st generation | changing one function: from 5–117 changed shards to 0–1 |
| Lean mode | `0126356c`, `fbb68c15`; `OOT3D_WHOLE_AOT_LEAN_STATS` | `69a49b4d` (memory differential against the base) | to be measured *(session notes: ~10% of instructions)* |
| Block budget | `WholeAotBlockBudget` 2048 on the Switch | frame ≤ 240 B, < 0.5 MiB of the 1 MiB stack | Linux: 64 → 128 gave +3.93%; 2048: to be measured |
| PGO (GCC) | `OOT3D_WHOLE_AOT_PROFILE_MODE=generate/use` | collection 3 with 0 coverage-mismatch | Switch: to be measured in isolation; Windows: 0.864 of pre-NRI time |
| VFP with exact zero in native | `--vfp-zero-aware` (with `--native-vfp-aarch64`) | 299 million checks in qemu, 16 modes; 9 mutations detected; differential over 314 real functions | see SWITCH_PORT.md, "Header round" |
| 16-byte flow | `OOT3D_WHOLE_AOT_FLOW_IN_REGISTERS` (default 1) | layout only; differentials with the new layout | stores −18% to −33%, stack stores −28% to −69% (static) |
| Lean stack in reads/LDM | `--lean-stack` | differential over 314 real functions (host and qemu) | see SWITCH_PORT.md |
| Registers in locals | `--local-state`; `--promote-registers` | `run_register_promotion_equivalence.py` | worse on the A57 (3.8); off |

### 3.1 Native VFP (`--native-vfp-aarch64`)

- Emits VADD/VSUB/VMUL/VNMUL/VDIV/VSQRT, VMLA/VMLS/VNMLA/VNMLS (as **separate** FMUL + FADD/FSUB),
  VCVT and VCMP/VCMPE as AArch64 instructions in `asm volatile`, with the guest FPSCR mode in the FPCR and
  the accumulated flags in the FPSR (Dynarmic's model). File: `oot3d_native_a32_vfp_aarch64.h`.
- `Oot3dGuestFpScope` at the entry of `ExecuteOot3dWholeAotFunction`: loads the guest FPCR; on exit
  (including unwind) merges the FPSR into fpscr and restores the host's. External calls, callbacks and
  slow memory paths (MMIO) run under the host FPCR.
- Where ARMv8 differs from the software model (sign of zero with FZ, NaN in VSUB with DN=0, UFC with
  FZ=0, denormal VCMP with FZ=1) it falls back to software; arithmetic with FZ=0 stays entirely in software. The software
  remains the oracle.
- Validate: `run_vfp_aarch64_tests_qemu.sh` (≈299M checks, value and flags, 16 RMode×FZ×DN modes; the
  known mutations must be detected).
- `--vfp-zero-aware`: instead of sending every zero/NaN result to software, a cold half
  (`ZeroAwareSlow<Op>`) keeps the native zero when it is exact in both models (finite, non-subnormal
  operands with exact cancellation, two zeros or a zero factor). Careful with FZ: a sum of
  normals that becomes a flushed subnormal is NOT exact cancellation. Mutations of the criterion:
  `run_vfp_zero_aware_mutations.sh`.

### 3.2 Aliasing (TBAA)

- Problem: `std::memcpy` on `uint8_t*` is an "any type" access; each guest write forced GCC to
  reload guest registers, flags, budget, page-table pointers and the generation
  counter, and each read forced it to store pending state.
- Solution (ad972f24): `ReadFast/WriteFast/ReadWordsFast/WriteWordsFast` access the bytes through the packed
  union `GuestCell` of 8/16/32/64-bit `enum class`. All sizes share the union's alias
  set (overlapping accesses of different sizes stay ordered); enum class does not
  alias everything, so the set is disjoint from host state. Slow paths, spans and regions keep
  using memcpy.
- **Contract**: in a function that inlines these paths, guest bytes only through these helpers, memcpy,
  `char` or `std::byte`. Breaking this is silent UB.
- Validate: differential of inlined sequences (100 thousand on the host and 100 thousand on devkitA64/qemu, zero
  divergences). The "one alias set per size" mutation must diverge (it diverges in 100%).
- Result (devkitA64 -O3, shards 000/050/091/100/180/243): instructions −4.0%, loads −6.7%, stores
  −7.2%, stack reloads −19.5%; fixture functions in qemu −17.6% instructions.

### 3.3 Giant functions and compile time

- Measured (GCC 16.1 -O3, Ryzen 5600G): header parsing 1.3–2.4 s per shard (2–6%); the rest is
  optimization, concentrated in the giant functions: `sched1` 50–65% and PRE/gcse-after-reload 15–45% in the
  slowest shards.
- `--giant-function-instructions N` marks functions with ≥ N guest instructions with
  `[[gnu::optimize("no-schedule-insns","no-gcse-after-reload")]]` on the signature line itself
  (line numbers don't change). `--hot-functions list.txt` excludes the hot ones.
- Hot list: `whole_aot_hot_from_gcda.py --gcda-dir <capture> --generated-dir <cpp> --output hot.txt`
  (matches each `.gcda` by `profile_id` = crc32 of the mangled name; matched 12439/12439, 3983 hot) or
  `whole_aot_hot_functions.py` from `guest_profile.txt`.
- The attribute changes neither `profile_id` nor CFG/line checksums: the `.gcda` files are valid with or without it. In the
  `generate` build it can mark all of them; in `use`, only the cold ones from the collection (with
  `-fprofile-partial-training` the non-executed ones stay optimized for speed).

### 3.4 Lean mode (write generations and block entry)

- Memory (0126356c): write tracing lives in the page table (with it on, the typed entries
  are null and the fast path tests nothing); 32-byte `FastPage`. Since 2026-11-05 a store
  marks a per-page card byte (stored before the group's entries) and a memory-wide written mark;
  `RangeWriteGeneration`/`WriteGeneration` derive generations when asked (SWITCH_PORT.md,
  "Recompiled-code overhead"). Before, a single counter (`mRangeGeneration`): `++` and a stamp per store.
- Block entry: `BlockEntryGate` (superset filter computed at initialization: one load and one
  bit test on the hot path; the slow one stays exact). `RefreshBlockEntryGate()` for whoever changes the
  hooks later.
- Derived `BlocksConsumed()` (fbb68c15): a single counter per block entry; generated code with the
  old layout stops compiling instead of silently going wrong.
- `OOT3D_WHOLE_AOT_LEAN_STATS` (default = PRODUCT_MODE) compiles out the per-call statistics.
- Validate: `69a49b4d` compares the new memory with the base revision's on random sequences
  (returns, faults, global/per-page generations, fingerprints, `CaptureState`).

### 3.5 PGO with GCC on the Switch

Flow:

1. Instrumented build: `-DOOT3D_WHOLE_AOT_PROFILE_MODE=generate` (writes to
   `OOT3D_WHOLE_AOT_GCC_PROFILE_RUNTIME_DIR`, default `sdmc:/switch/oot3dre/pgo`). Uses
   `-fprofile-generate=<dir> -fprofile-prefix-path=<build> -fprofile-update=single` only on the shards and links
   libgcov. Flat `.gcda` names, identical across builds
   (`CMakeFiles#oot3d_native_whole_aot.dir#...#oot3d_whole_aot_shard_NNN.gcda`).
2. Collection on the console: clear the `pgo/` folder on the SD, play ~15 min in the scenes that matter (with the
   same configs the user will use, e.g. fps60 and scale 2). The game creates the folder and writes the
   counters every ~3 min (`__gcov_dump` + `__gcov_reset`, `oot3d_whole_aot_gcov.h`) and on exit.
3. Download the `.gcda` files (FTP) into a collection folder.
4. Final build: `-DOOT3D_WHOLE_AOT_PROFILE_MODE=use -DOOT3D_WHOLE_AOT_PROFILE_FILE=<dir>`
   (`-fprofile-use -fprofile-partial-training -Wno-missing-profile -Wno-error=coverage-mismatch`).
   The generated C++ must be the same as in the collection and be in the same `OOT3D_WHOLE_AOT_GENERATED_DIR`.

Pitfalls solved:

- **libgcov and `tpidr_el0`** (2be7235d): libnx finds TLS via `__aarch64_read_tp` (`-mtp=soft`), but
  devkitA64's libgcc (indirect-call profiler, `__thread` data) reads `tpidr_el0` directly, which
  Horizon leaves at 0: the instrumented build crashed on the first indirect call. Fix in
  `oot3d_switch_threads.cpp`: mirror the TLS pointer into `tpidr_el0` in a priority-101
  constructor and in every new thread (`--wrap=threadCreate` and `--wrap=pthread_create`).
- **coverage-mismatch**: a function whose CFG changed since the collection (runtime header edited) just loses
  its profile, instead of breaking the build (7e820da8). Check in the log how many lost it; collection 3 gave 0
  *(session notes)*.
- The nfsmw-nx research suggests `-fno-profile-values` and `-fprofile-correction` on Horizon (not
  adopted here; evaluate).

**What invalidates the PGO** (requires a new collection, or accepting the loss of profile in the affected functions):
regenerating the C++ with another option that changes bodies (native VFP, literals, `--local-state`), changing the
shard strategy (functions move between files), reapplying/changing native replacements in the shards,
changing headers inlined into the shards (`oot3d_native_a32_memory.h`, `whole_aot_runtime.h`, TBAA),
changing the path of the generated directory or of the build. Does not invalidate: changes only in the host runtime
(GL, audio, threads), SD configs, the giant-function attribute.

When to redo it: after any item above that touches hot functions, or when the
`use` log shows many `coverage-mismatch`. Collections done: 1 (2026-10-16), 2 (lean), 3 (TBAA + 23
replacements + fps60).

### 3.6 ccache and stable sharding

- ccache with `base_dir=$HOME`, `hash_dir=false`, keeping **the same generated C++ directory** across
  builds (the path goes into the hash and into the `.gcda` names).
- `affinity` recomputes from scratch: one function more or less reshuffles ~30% of the shards.
  `affinity-incremental` keeps the shard of each existing function (previous manifest) and puts new ones
  next to their most-called neighbor (cap 1.25× the average). Only switch strategy before a PGO collection.
- When copying a build folder for a second build, **do not delete** `.ninja_log`/`.ninja_deps`
  (it turns into a full recompile). Estimate first with `ninja -n | wc -l`.

### 3.7 Floating-point exactness

- Shards always with `-fno-fast-math -ffp-contract=off` (9466d722): with fast-math, 1.97 million of
  86.4 million VFP checks diverged on the IXC flag.
- The shard archive uses `-g0` (the toolchain's Release adds `-g`; the generated code with debug info takes
  hundreds of MB).

### 3.8 Promoting registers to locals (`--local-state`)

> **Evaluated and turned off** (2026-10-17): `--promote-registers` (exact dirty set per block,
> differential `run_register_promotion_equivalence.py`) is worse on the A57: shard 243 +36% instructions,
> loads +77%, stores +89%. Integrated into the generator and header, off by default.
>
> Context already documented: on Windows, `--local-state` on all functions + PGO gained nothing
> (1.000, 318 MB module, TRIAEVUM_AOT_LEVERS_RESULTS.md). On the Switch, nfsmw-nx measured
> saving/restoring guest registers at ~14% of the main thread
> (SWITCH_RESEARCH_XBOX360_2026-10-17.md), and OoT3D's logic is the bottleneck in the Kokiri forest with
> fps60.

---

## 4. Switch runtime

### 4.1 Threads, priorities and cores on Horizon

Horizon rules (confirmed by nfsmw-nx):

- Only priority **0x3B** time-slices. At any other, a busy thread blocks those of
  equal or lower priority on the same core, even with other cores idle.
- A single-core mask never migrates. Homebrew only has cores 0–2 (3 belongs to the system).
- Without a wrapper, libnx puts every thread on core 0 at 0x3B: the render thread and compilation only ran
  when the game was waiting.

`fixed` layout (default, `oot3d_switch_thread_layout.h`, applied by `--wrap=pthread_create` in
`oot3d_switch_threads.cpp`, minimum stack 4 MiB):

| Role | Core | Priority |
|---|---|---|
| Game (guest CPU, DSP HLE, submission) | 0 alone | 0x2C |
| Render thread | 2 | 0x2D |
| PICA plan worker | 1 | 0x2E |
| Host and SDL audio (audren) | 1 | 0x2B |
| Services (spdlog, Mesa cache queue, ResourceManager, loaders) | 1\|2 | 0x3B |

- `config/thread_layout` = `fixed`, `float` (render/worker with mask 1|2) or `legacy`; option
  `pacer_spin_us=N`.
- devkitPro's SDL translates `SDL_THREAD_PRIORITY_TIME_CRITICAL` to 0x3B (below the game!):
  `__wrap_SDL_SetThreadPriority` makes it 0x2B.
- Services out of the way: every unclassified pthread was born at 0x2C and preempted the render thread;
  now 0x3B. spdlog drops the oldest line instead of blocking the game.
- No busy waiting: `svcSleepThread` wakes up on time (pacer without spin), the condvar is only signaled
  when someone is waiting, per-draw atomics avoided (A57 without LSE: nfsmw-nx measured 83 thousand atomics per
  frame = 1.1 ms).
- Result: the game alone on one core took SwitchWakerHD from 20 to 28 fps (external reference).
  The render thread took ~10 ms of GL off the game thread (3d342fcd).

### 4.2 pthread, TLS and exceptions

- devkitA64 newlib's `pthread_detach` returns `ENOSYS`: `std::thread::detach()` throws
  `std::system_error`. Don't use detach on the Switch.
- **Link with `-fno-fast-math`**: with `-ffast-math` at link time, `crtfastmath.o` comes after
  `crtend.o`, ld drops the `.eh_frame` terminator and **every** C++ exception aborts.
- TLS: mirror into `tpidr_el0` (3.5) if anything from libgcc is used.
- The Horizon FS does not rename over an existing file: saving config = remove and rename.

### 4.3 Audio: 48 kHz, resampler and its own thread

- devkitPro's SDL2 passes `spec.freq` straight to audren (it does not resample; the hint has no effect), uses
  only 2 wave buffers and waits in 5 ms frames for the newly queued one to start playing. With small
  buffers the voice starves (crackling) and the queue grows until it drops.
- Solution: output at 48000 Hz (`config/audio_sample_rate`) with our own polyphase sinc-Kaiser
  resampler (`oot3d_host_audio_resampler`, unit DC gain per phase, s16 saturation, fine rate
  adjustment ±0.5% to hold the queue); device buffer 1024 frames (`config/audio_device_frames`,
  256–4096).
- Host thread (`oot3d_host_audio_stream.*`, core 1, 0x2B, woken on each submit and every 4 ms):
  stretch by pitch periods only with a nearly empty queue or sustained slowness; concealment (repeats the
  last period, fade) when the game falls behind. The DSP HLE stays on the game thread: it is sequential with the
  guest and does not parallelize.
- Lesson from the stretch: the 1st version edited all the time (26 min: 153 s inserted, 346 s removed) because of a
  signed credit; fixed, the 30 min simulation at 100% edits 264 ms (0.015%).
- Cheaper DSP mixing, bit-for-bit identical (8ee3f632): host 0.0506 → 0.0288 ms per DSP frame
  (−43%); A57 estimate ~0.6 → ~0.33 ms per step. Proof: `oot3d_native_audio_dsp_mix_tests` +
  `run_audio_dsp_mix_tests_qemu.sh` (same digest on x86-64 and AArch64).
- Optional per-source interpolation (`config/audio_interpolation` = `cubic`/`sinc`); `original` is
  Azahar's code bit for bit. On the console: to be measured.
- Diagnostics: the `audio:` line of `perf.txt` (`device_hz` ~48000, `input_hz` ~32728 at 100%);
  `config/audio_dump` records 60 s into `logs/audio_dump.wav`.

### 4.4 Render thread and parallel presentation

- The GL context has to live on the render thread: the queue's `Run()` executes on the calling thread, so
  `MakeCurrent` goes through `Submit()+Drain()` (e46fe275). Without that, `glCreateShader` returned 0.
- Turn off for A/B: `config/serial_presentation`.
- PICA draw plans off the game thread (6471c483): the game thread puts in a placeholder and a
  worker builds the plan from the immutable snapshot of the submission (~2.5 ms per step left the game
  thread). `config/serial_pica` and `config/pica_plan_render_thread` for A/B.
- Shutdown (a3d7837f): `BeginShutdown`/`StopRendering()` first; no new command runs after
  the exit request (before, a queued frame landed in `glBufferData` and caused a Data Abort).
- nvc0 contexts in the same share group use the same push buffer **without a lock**: shader warm-up on
  another thread hangs; do it on the render thread.
- ImGui without a current GL context on the game thread rebuilt shaders, buffers and atlas every frame in Mesa's
  null dispatch (2c0e30e9).

### 4.5 Pacing, fps60 and interpolation

- The guest advances at 60 refreshes/s and the game takes one step every two. Presenting at 30 Hz with a limit
  of 1 refresh per presentation, the game runs at ½ speed (Eden hid this). Limit 2 (with the
  render thread, 4 gave the same speed with half the frames: ~11 vs ~21 fps).
- fps60: `config/fps60`; the pacing mode comes from the launch (`oot3d_native_launch_frame_rate.h`).
- Native replay skipped on GL (`config/native_replay=1` brings it back): the native namespace only serves for
  read-back/savestate, which GL does not use; drawing the scene twice cost half the GPU at 30 FPS.
- Pacing governor (`oot3d_native_interpolation_pacing.h`, `config/interp_pacing`): draws the
  in-between frame full, reduced (`config/interp_scale`) or drops it when the step+in-between pair
  does not fit in 33.3 ms. Starts full, only goes down with real measurement (3 decisions above 100%), goes up when it
  fits in 97%, probes every 60.
- GPU time without stall (`oot3d_native_gpu_frame_timer.h`): switch-mesa's GPU clock is not
  in ns (SwitchWakerHD measured 1.63 real ns per "ns"); calibrate against the CPU over ≥ 3 s before use.
- **GPU time without the window wait** (2026-10-22): the frame's `end − begin` included the gap
  in which the GPU sits idle while the render thread waits on `nwindowDequeueBuffer` (lesson 26),
  in the middle of the frame. In tour2 the governor saw 18.5 ms per step against 12.3 ms real (glperf) and
  dropped 74 of 75 in-betweens (`interp_dropped`): the "60 FPS" was 30 distinct images with
  repetition. Two timestamps around the acquire (`Fast::SetWindowAcquireHook`,
  `NativeGpuFrameBusyTicks`) remove the gap; `config/interp_count_acquire_wait` restores the old
  measure. Test: `oot3d_native_gpu_frame_timer_tests`. Along with it, a `glFlush` right before the acquire
  hands the already recorded scene to the GPU while the CPU waits (`config/gl_acquire_noflush` removes it).
- **Render thread wait out of the cost** (2026-10-23, tour14): the cost the governor learns
  per frame is `max(GPU, render thread)`, and the render thread time (`renderFrameCpuMs`, wall time of the
  frame's commands) included the blocked window acquire (glperf `clear`: 3.5 ms on average, 7–14 ms
  in the heavy windows). In windows with `interp_dropped` 60–74 the step cost 17.6 ms of "render
  thread" of which ~10 were waiting; the pair went over 33.3 and the in-between was dropped even with the GPU
  fitting. Chain: the EGL window has 3 buffers and the pacer uses the CPU clock, not vsync; with the
  display a hair slower than the pacer the queue fills up (1 on screen, 1 queued, the 3rd only comes back when the
  next one reaches the screen) and what sets the pace becomes `nwindowDequeueBuffer` in the middle of the frame
  (deferred to the scanout): the frame's slack shows up in `clear` with `pacing`≈0, or in `pacing`
  with `clear`≈0 — the sum is the same. Dropping the in-between does not reduce the wait (the cheap frame just
  waits longer), so the inflated cost sustained itself. Fix: the acquire hook
  (`Fast::SetWindowAcquireHook`) also measures the CPU wait with `armGetSystemTick`
  (`NativeRenderWaitClock`) and the governor learns `NativeRenderThreadBusyMs(wall, wait)`;
  `config/interp_cpu_count_wait` counts the wait again. Along with it, two holes in the 10-22 GPU
  measure: the "before the acquire" stamp was emitted **after** the `glFlush`, so it only reached the GPU with
  the "after" one (subtracted gap ≈ 0) — now it goes before the flush; and the end-of-frame stamp stayed
  unflushed until the swap, after the pacer wait (the idle GPU counted as work) — now a
  `glFlush` right after `FinishRenderFrameCost`, before the pacer, hands scanout+UI to the GPU while the
  CPU waits (`config/gl_pacing_noflush` removes it). Fields: `perf.txt` `rt_wait_ms_<type>` (part of
  `rt_ms_` blocked in the acquire), glperf `<type>_sec_acquire_ms` (window bind+clear), and
  `TRIAEVUM_INTERP_PACING ... cpu_count_wait= flush_before_pacing=` in the log. Model
  (`oot3d_native_cadence_governor_tests --report`, "tour14": swapchain of 3, display 0.2% slower,
  GPU 12.5–15.5 ms): counting the wait, 1281–1471 of 1793 in-betweens dropped (36–38 distinct
  images/s); only busy time, 0–10 (59.7–59.9/s); a 16.5 ms GPU keeps dropping (the pair does not fit).
  Tests: `TestRenderWaitIsNotCost` (cadence), `TestRenderThreadBusyWithoutTheWait` and
  `TestGovernorKeepsInBetweenWithoutTheRenderWait` (`oot3d_native_gpu_frame_timer_tests`).
- **tour15 result (pgo5j) and pair margin** (`config/interp_margin_ms`, default 5): without the
  wait in the cost, per window of 150 `interp` 53→65, `hold` 41→32, `interp_dropped` 23→11,
  distinct images ~51→~56/s, but average presented fps 59.34→58.52 (Haunted Wasteland 60→54.7,
  Hyrule Castle 59.4→55.5, Kakariko 59.1→55.8, DMC 60→56.5); `rt_wait_ms_step`=4.5. Per area, the loss
  only appears where the **measured** GPU pair is ≥ ~29 ms (29.2–32.8); up to ~27 it stayed the same or better
  (Castle Town Buildings 58.8→59.8). That is: what the timer measures as busy GPU is not all that
  a frame takes — the GPU does not stay fed end to end near 100%. The model
  (`oot3d_native_cadence_sim.h`, `Segment::GpuHiddenMs`) reproduces the console with 2.5–3 ms hidden
  per frame (14.95 ms measured → 55.8 FPS). Fix: the governor only keeps the in-between if the
  pair fits in 33.3 − margin. Per-area replay (areas with pair ≤ 33.3 − m use tour15, the others
  tour14): m=0 58.51 fps / 54.1 images/s; m=4 59.27/51.8; **m=5 59.32/51.6, 22 areas ≥ 59** (pgo5i:
  59.35/50.5/21); m=7 59.35/51.5. In the model (hidden 2.75) m ≥ 4 keeps ≥ 59.8 under all loads;
  m=4 gives more images (49.7 vs 46.7 for m=5), but the real replay calls for 5. Splitting the wait into
  "GPU still busy" × "display" (counting only the first, `SplitWaitByGpu` in the model: availability
  of the end-of-scene stamp when the acquire returns) was evaluated and **not implemented**: in the model
  it gives 59.84/49.1 without margin and gets worse with margin, below the margin alone. To go back exactly to
  pgo5i without a build: `config/interp_cpu_count_wait` (empty file) + `config/interp_margin_ms` with
  `0` (keys up to pgo5k; see the next item). `perf.txt` gained `decision_budget_ms`; the log, `margin_ms=`. Tests:
  `TestMarginDropsPairsNearTheBudget` (pacing), `TestMarginKeepsTheRefresh` (cadence).
- **Defaults back to pgo5i's (pgo5l, 10/23).** tour16 (pgo5k) went down with a Data Abort at ~4 min
  (corrupted heap, see 4.7). Without a better bench than pgo5i's, the default went back to its
  behavior, and the changes from 994ccd7d/fd358b46 were put behind keys (empty file
  enables): `config/interp_cpu_exclude_wait` (render thread cost without the acquire wait; the
  old `interp_cpu_count_wait` no longer exists), `config/interp_margin_ms` (default 0; use `5`
  together with the previous one, `kExcludedWaitMarginMs`), `config/gl_pacing_flush` (glFlush between the end of the
  frame and the pacer; replaces `gl_pacing_noflush`) and `config/gl_acquire_stamp_before_flush`
  (GPU "before the acquire" stamp before the glFlush). To reproduce pgo5k: the four keys +
  `interp_margin_ms` = `5`. `perf.txt`'s `rt_wait_ms_*` is still measured in both modes.
- **Two frames in flight** (`config/frames_in_flight`): with 1, the game's end of frame waits for the
  render thread to finish the previous frame, which waits for the display; a long step (16–18 ms) followed
  by a short in-between does not even out and the step misses the refresh. The model
  (`oot3d_native_cadence_governor_tests --report`, "slow areas" scenario) with a 21 ms step:
  55 → 59.7 images/s; 24 ms: 40.9 → 59.6. **On the console it hung** at the first warp (tour3b): game and
  render thread inside `malloc` with the heap corrupted — shared session state assumes the
  game is at most 1 frame ahead. The stream supports it (`SetMaxFramesInFlight`, tested), the session
  does not: the key is ignored until what the commands capture by reference is audited (run with TSan).
- **Spike-based measurement and pacer on the real vsync (pgo5n, 10/23).** Without VRR, what counts is each frame
  that misses the 16.7 ms refresh, and 150-frame averages hide the spikes. `config/frame_spikes`
  (or `config/gpu_timing`) turns on `logs/spikes.txt` (format in 5.1) and the `missed_*` summary in
  `perf.txt` (5.2). Per presented frame, only POD in an SPSC ring of 256 (`oot3d_frame_spike_log.h`,
  render thread, no allocation); the `perf.txt` window formats in batch and sends to the `AsyncLineWriter`.
  Refreshes per frame: with the vsync clock, a FIFO latch model (the buffer appears at the 1st vsync
  after the swap, and never before the vsync following the previous buffer's); without it, interval between
  swaps / 16.67 ms rounded. The sample is closed one frame later, when the game thread cost
  of the host frame that produced it arrives. Blame: `guest` (game busy > budget) → `render`
  (render thread busy, excluding waits) → `gpu` (latest GPU result, lags 1–3 frames) →
  `pacer` (nothing overran: pacer phase/drift, acquire, waking late). Budget = expected refreshes
  (1; 2 on a locked-30 step or a 30 target) × period.
  Vsync clock (`oot3d_vsync_clock.h`, `oot3d_switch_vsync_clock.cpp`): our own `viInitialize` +
  `viOpenDefaultDisplay` (the vi service is reference-counted; the display handle of the
  default window is static in libnx) + `viGetDisplayVsyncEvent`; a priority-0x20 thread
  waits on the event (`eventWait` 100 ms, `eventClear`) and stamps with `armGetSystemTick` (cntpct_el0;
  never cntvct_el0), converted to the `steady_clock` clock. Model: period by least
  squares of t × index (window of 1200 vsyncs; a missed vsync just skips an index), phase by the lower
  envelope (the wake-up is never before the vsync), restarts with residual > 1/4 of a period.
  `config/pacer_vsync` (default **off** in this 1st version, for A/B; optional content = offset in
  ms after vsync, 0–15, default 1): the pacer uses the measured period and snaps each deadline to the grid
  `last_vsync + k·period + offset` (`SnapDeadlineToVsync`); without a valid vsync, it goes back to the CPU
  clock. `frames_in_flight` does not change. Model (`oot3d_frame_spike_tests --report`, display 0.2%
  slower, swapchain 3, everything fitting): CPU pacer = 4 (1 in flight) to 35 (2 in flight) refreshes without
  a new image per minute and the render thread stuck in the acquire 29 s/min (full swapchain: the display,
  not the pacer, sets the pace); vsync pacer = 0 and 0, 3592 images in 60 s (one per refresh).
  Only the content mismatch remains (game at 60.00 vs display at 59.88: judder 0.74 ms RMS).
  **tour19 (pgo5n): the clock did not open** (`session_start vsync_clock=0 vsync_rc=0x1272`). 0x1272 =
  module 114 (vi), desc 9: undocumented on switchbrew and in the emulators (yuzu/citron only have
  1 OperationFailed, 5 PermissionDenied, 6 NotSupported, 7 NotFound). The suspicion is the second opening
  of the "Default" display in the same `IApplicationDisplayService` session (libnx's default window already
  opened it, and its handle, `g_viDisplay`, is static in libnx). Without the clock, `pacer_vsync` had no
  effect and `spikes.txt` counted refreshes using the 60.000 Hz clock. **pgo5o:** the log states the stage
  (`vsync_stage=1` viInitialize, `2` viOpenDefaultDisplay, `3` viGetDisplayVsyncEvent) and breaks down the
  rc (`vsync_rc_module`, `vsync_rc_desc`); without the event the **fallback source** kicks in
  (`vsync_source=acquire`): the return of a window acquire that blocked ≥ 1.5 ms
  (`AcquireVsyncFilter`). With the queue full the buffer only comes back on a vsync, so the return is vsync + a
  short latency and the lower envelope of the same model finds the phase; acquires that did not wait
  are left out. It becomes valid after ~30 acquires that blocked; until then (and if the queue never
  fills) the pacer follows the CPU clock. `config/pacer_vsync` remains **off** by default.
  Test: `TestAcquireFallbackFindsVsync` (latency 50–500 µs, 1 in every 1–3 vsyncs, 35% of acquires
  without waiting: period within < 50 ns, phase within < 150 µs).
- Result: fps60 with the "final 3" build = 48.6 fps average, 46% ≥ 55, speed 99.7%
  *(session notes)*. Heavy areas are limited by the logic on core 0.

- **Interpolation and the game's culling** (flicker when turning the camera, 10/24): the game culls geometry
  with the camera of **each step**, so the set of draws changes between two steps (screen edges,
  LOD). Generic rule for any port with in-between frames: (1) the frame at the instant of a
  step must be **exactly that step** (not the alpha-0 sample over the next step's list);
  (2) between two steps, an unpaired draw cannot be drawn with only one step's camera:
  measure the camera motion on the static paired draws (Δ = VM_ant × VM_atual⁻¹, by vote)
  and apply it to the unpaired ones — those only in the new step come out of the previous camera, those only in the previous one carry on
  and follow to the new one. A host test with a camera rotating over frustum-culled pieces shows the
  defect without a console (`oot3d_native_pica_visual_cull_tests`). At 30 FPS without interpolation the problem
  doesn't exist — if a visual defect disappears at 30, start with the interpolation.

### 4.6 Heap without fragmentation

- newlib's malloc has a global lock, shared with Mesa; `libdrm_nouveau` allocates each BO with
  `memalign`. Large blocks of varying sizes per frame fragment: `heap_arena_mb` went from 631 to
  1659 MB with ~700 MB in use (8.5 min, fps60).
- Rules: no large `new`/`free` per frame; pools that return clean memory
  (`Oot3dPicaPlanVector`), vectors that keep their capacity, recycled batches (the plan worker had
  152 KiB of heap per draw against 34 KiB for the serial one and degraded from 3.9 to 89 ms per frame in 3 min).
- Upload rings ≤ 2 MiB (up to 2 MiB nouveau suballocates from slabs; above that, each orphan is a `memalign`).
- `mallinfo()` walks the whole heap while holding the lock: only with `config/heap_stats` or
  `config/gpu_timing`, every `config/heap_check_windows` windows.
- Mandatory long-running test: `oot3d_native_pica_visual_frame_soak_tests` (12 MB and 4.8
  allocations ≥ 256 KiB per frame → 0.24 MB and 0), `oot3d_native_pica_deferred_plan_soak_tests`.

### 4.7 GL state cache and GPU memory

- `PicaGlStateCache` (`gfx_opengl_pica_state_cache.h`, no GL, with a test): only emits the call that changes
  the value; forgotten at every boundary with the legacy path. Synthetic 400-draw frame: 27.5 → 2.7
  state calls per draw. `config/gl_state_cache=0` for A/B.
- VAO: the PICA path must not leak the VAO into the legacy UI (the HUD inherited instance divisors,
  53d32a84); rebind on demand.
- Residency (`gfx_opengl_pica_residency.h`): textures by content with LRU and a budget
  (`config/gl_texture_budget_mb`, default 96), upload rings with unsynchronized map, out of
  memory is not fatal (skips the draw, counts `gl_oom`). Without this the console crashed after ~8 min with fps60.
- **tour16 (pgo5k): Data Abort in `st_FreeTextureImageBuffer` inside `glDeleteTextures` of
  `TrimPicaGlResidency`.** The level-0 `st_texture_image` of the evicted texture (idle for more than
  30 s, eviction by `MaxIdleFrames`; `gl_tex_mb` ≤ 58 of 96, `gl_oom=0`) had `pt` = 0x280c97def,
  an odd, misaligned pointer, and the refcount `ldaxr` faulted. Residency is the victim, not the cause:
  GL is safe by name (Mesa refcount, names never reused, `MaxKey + 1`), the
  Switch libdrm's `nouveau_bo_ref` waits for the BO fence (`nvFenceWait`) before `free`, and the
  GL backend neither pins nor keeps texture handles from the cache outside the draw. Mesa's struct was
  overwritten earlier by someone on the same newlib heap, and a single dump doesn't say who. Test:
  `PicaGlResidency.ReleasesEachNameOnceAndNeverAPinnedOne` (10 min session with warps, in-between
  pairs and failure trims mid-frame; each name released once and never in flight;
  ASan clean; catches `MinIdleFrames` 1 and the failure trim with 0).
- **ETC2 on the GM20B is not hardware** (`config/gl_etc2`, default off since pgo5l; `1` enables). Mesa 20.1's
  `nvc0_screen_is_format_supported` accepts ETC/ASTC only on class 0xA297 (GK20A), and the
  Switch is GM20B (checked in the binary: `mov w0, #0xa297` before the `return false`). The premise of
  a886ca69 ("the GM20B samples ETC2 in hardware") was false: every ETC2 upload went through the
  st/mesa fallback (`st_compressed_format_fallback`), with a `malloc` of the compressed copy per mip on the
  shared heap and an ETC2 → RGBA8 decode on the render thread's CPU, while the
  residency budget counted 4 bits per texel for a texture that takes 32 + 4. Turned off,
  ETC1 goes back to RGBA8 decoded ahead of time by the plan worker (the tour8 A/B saw no
  FPS difference).
- Loose uniforms make nvc0 copy the whole block through the FIFO (SwitchWakerHD: std140 UBO cut
  commands to 1/3); don't resend identical blocks.
- **Lean uniforms** (`config/gl_uniform_trim`, default on; `0` turns it off;
  `gfx_opengl_pica_uniform_trim.h`, test `gfx_opengl_pica_uniform_trim_tests`): when the GL
  program is created the final GLSL is scanned and the prefix `[0, N)` of each block it can read is kept
  (member → its end in the `pica_uniform_layout.h` layout; literal index `f[12]` → up to that register;
  non-literal index → the whole member; a0/aL relative addressing = `get_offset_register()`
  called → the whole `f[]`, only defined and never called → doesn't count; anything not understood →
  whole block). The draw copies only that prefix into the ring, but **reserves and binds the range of the whole
  block as before** (same `glBindBufferRange`; the rest of the range holds garbage nobody reads),
  so GL state doesn't change and the bytes read are identical. Dedup now compares only the
  prefix (a previous block that differs only in the unread tail is rebound). Console corpus (271
  programs from `shadercache`): fragment 6112 → **1184 bytes in 257** (without lighting: stops at
  `texture_lod_bias`) and 2096 in the 14 with lighting; **vertex stays at 1616 in all 271** (every program
  calls `get_offset_register`, relative indexing, and reads `f[95]`). `draw_uniform_sent_bytes_kb`
  now counts the copied bytes. Lesson: the declared UBO size is a contract of the layout, not of
  what the shader reads; measure on the real corpus before promising (the vertex block doesn't shrink).

### 4.8 GPU savings (exact)

  Console (tour13, 10/23, pgo5h, GPU 614, Link walking): uniforms 551→203 KB/frame (Water Temple), 777→318 (Market); `draw_submit_ms` 8.3→3.5 (with the end of the signature); average over the 29 areas 58.96→59.4, Water Temple 54.7→59.4, Kakariko 55.9→58.8. **Market stays at 58.6 with 2 in-betweens and 72 `hold` per window**: step 19.4 + in-between 17.5 ms > 33.3, so the governor drops the in-between (60 Hz of image with 30 distinct frames). The pair is ~4 ms short; target: `step_cpu_other` 5.9 ms (copying the 15.7 KB plans in `resolve_plans`) and `pica_top` 6.0.
  Lean plan (pgo5i, tour14): average `sec_resolve_plans` 1.32→1.08 ms, Market 58.6→59.8, average 59.40→59.34 (route noise: Water Temple 59.4→57.1 with min 50.9), 23 areas ≥59 (before 20). In every window the render thread spends ~3.5 ms in `clear` and ~5 ms in `pacing` **blocked**, and even so 41 of 95 frames per window are `hold`.
- `config/gl_lean` (default): skips repeated display transfer copies, clearing of the window the scanout
  covers, clearing of unused Shadow2D, identical uniforms; reuses the EASU when re-presenting.
- `config/gl_gpu_lean` (default, `gfx_opengl_pica_gpu_lean.h`): logic op COPY → off, identity
  blend → off, a draw with no effect is not emitted, a depth fill on D16/D24 also clears the
  stencil. Proof: fragment model + check on real GL (0 differing bytes). Gain: uncertain without
  measuring (estimate 0.2–0.6 ms on a full frame for the logic op).
- Mesa 20.1's nvc0: ZCULL never turns on (`ZCULL_REGION=0x3f`, `nvc0_validate_zcull` under `#if 0`).
  Per-pixel early-Z is automatic except with `discard`, `gl_FragDepth` or image writes.
- **Bottom screen in the TopScreen profile** (`config/skip_bottom_screen`, absent = draws as before):
  the bottom screen's 240×320 framebuffer never appears in this profile (HUD and menus come from the
  guest's UI primitives), but it cost ~2.7 ms of GPU and ~0.5 ms of render thread per step
  (glperf `pica_bottom`). The scheduler leaves out of presentation the draws, fills and transfers that only
  produce that screen (`oot3d_native_bottom_screen_skip.h`), all or nothing per sample: nothing is left out if
  another draw samples or writes its memory, or if a transfer copies it into something used. Use in a
  later sample is not seen: that is the key's risk. The top transfer (submitted separately) counts
  as a transfer of the sample, and a later transfer whose source was skipped is left out
  (`BottomSkipTracker`; before, GL threw "PICA display transfer source is missing" ~5 s after
  startup) and that target is drawn again. In tour4 almost nothing was skipped (`pica_bottom` 2.4 ms):
  the "all or nothing" rule declined the samples; the suspect is the single depth buffer of both screens
  (common on the 3DS), now accepted (only bottom-screen draws test that depth; the fill that also
  covers the top screen's use stays). `perf.txt` gained `bottom_skip: draws= missing_sources=` and
  the samples by reason (`skipped`, `no_bottom`, `unsafe`, `color_shared`, `depth_shared`,
  `texture_read`, `transfer_into`, `transfer_top`, `transfer_used`). Test:
  `oot3d_native_bottom_screen_skip_tests`.
- Resolution: `config/resolution_scale` 1/2/2.5/3; FSR 1 (`config/upscaler=fsr`), AA, anisotropy. At
  3× (720p) the GPU became the bottleneck: 23.4 vs 28.5 FPS at 2×. Default 2×.
- **Dynamic resolution (DRS) of the top-screen scene** (`config/dynamic_resolution`; **absent =
  off**, first version for A/B). Motivation (tour17, pgo5l, 3.0): Water Temple 55.1 FPS with a
  14.8 ms GPU step (`gpu_pica_top` 12.7): pair ~29.6 ms + ~2.5–3 ms the timer doesn't see doesn't fit in
  33.3, and the governor drops almost every in-between (10.5 `interp` vs 65 `hold` per window).
  File present turns it on; the optional text is the minimum scale (`2.25`, `2,5`; multiples of 0.25; empty =
  3 steps below the configured one, 3.0 → 2.25; never above the configured one). Technique:
  - **Sub-rectangle, no reallocation.** The top-screen target (native 480×400 key, without its own namespace
    scale) stays allocated at the configured scale (`AllocWidth/AllocHeight/AllocScale` in
    `PicaGlTarget`); at a smaller scale only `Width/Height/Scale/ScaleY` (the drawn extent) change, and
    viewport, scissor and the widescreen passes already derive from those (`PicaGlScale::DynamicSubRectangle`).
    It's viable because the GL backend never reads pixels back into guest memory: the target is only read
    by the display transfer and by Shadow2D.
  - **Switch only on the color clear** (up to pgo5n; since pgo5o also on the 1st draw, see tour19 below) (`RefreshPicaGlTargetScale` in `ApplyPicaFill` and
    `ClearPicaRenderTarget`): a frame never mixes two scales, and a frame that builds on the
    previous one's pixels (frozen background) stays at the scale they were drawn at. The frame requests the
    scale (`Fast::PicaGlDynamicScale::gState.Requested`, `gl_pica_dynamic_scale.h`) only when building a
    **step**; the following in-between clears the target with the same request, so it uses the step's scale.
  - **Display transfer**: the display image has the allocation's extent; the copy fills the lower-left
    corner (`ContentWidth/ContentHeight`) and replicates the last column and row in a band of
    2 texels (bilinear, EASU's 12 samples and RCAS reach up to 2 texels past the edge, where an
    older, larger copy may be). The presented shape uses `AllocScale` (the scale never changes the aspect). The
    identity of the lean copy (`DisplayCopyIdentity`) already includes the extent read, and EASU reuse
    does too.
  - **Upscale reads the sub-rectangle**: linear scanout with `pica_uv_scale`; EASU with the `FsrEasuCon` for
    viewport ≠ texture (6-argument `FsrEasuConstants`, as in ffx_fsr1.h); RCAS in
    resampling mode with `pica_rcas_uv_scale`. At full scale everything is identical to before (UV scale
    1, viewport = texture). With `config/upscaler=fsr` at 2.75 EASU upscales again (at 3.0 it is 1:1).
  - **Outside DRS**: SMAA/FXAA on (the AA images have the source's extent; `Supported=false`),
    configured scale 1, the in-between's reduced namespace (`config/interp_scale`, its own scale)
    and every target that produced or was sampled as Shadow2D (`DynamicExempt`: the consumer filters by
    the allocation's `textureSize()`). The game's HUD and menus are drawn by the guest **into the same target** as the
    scene, so they scale along (there's no way to separate them in the sub-rectangle); the host overlay (ImGui,
    FPS counter) is drawn on the window and doesn't change.
  - **Controller** (`oot3d_native_dynamic_resolution.h`, render thread): learns only the step's GPU time
    (the same stall-free `GL_TIMESTAMP` as the governor; not render thread time, because
    a smaller image doesn't save CPU); pair = 2 × step with interpolation. Goes down one step (0.25) when the
    pair average stays > 85% of the budget (28.3 of 33.3 ms) for 3 steps in a row; goes up when the
    **predicted** pair at the scale above (model `0,35 + 0,65·(s'/s)²`) stays ≤ 80% for 60 steps in a row
    (hysteresis: between 80% predicted and 85% measured nothing changes). After each change it discards 5
    results (the stamps arrive up to 8 frames late and are still from the old scale). A step up
    undone within 180 steps doubles that level's wait (up to 8×); a level that holds 180 steps
    goes back to 60. Water Temple in the model: 29.6 → 2.75 (~26.3) and stops there.
  - perf.txt (`interp:`): `drs` (on), `drs_scale` (current), `drs_min`, `drs_scale_avg` (average
    scale of the top frames presented in the window), `drs_pair_ms` (controller average),
    `drs_changes`/`drs_downs`/`drs_ups`/`drs_bounces` (in the window) and `drs_applied` (changes
    actually applied to a target on the clear; `drs_changes>0` with `drs_applied=0` = the target never
    gets a color clear and DRS doesn't act). Startup log `TRIAEVUM_DYNAMIC_RESOLUTION active= supported=
    max= min=`. Tests: `oot3d_native_dynamic_resolution_tests` (limits, hysteresis, settling,
    isolated spike, backoff, parser) and `PicaGlScale.DynamicSubRectangle` in `gfx_opengl_pica_scale_tests`.
    Measure on the console before thinking about turning it on by default (A/B: Water Temple and Desert Colossus, and that the
    areas ≥59 stay at 3.0 with `drs_changes=0`).
  - **tour19 (pgo5n, 3.0, DRS on): DRS did not act.** The controller went down (`drs_scale=2.25` in 76
    of 166 windows), but `drs_applied=0` and `drs_scale_avg=3` for the whole tour: the scene target never
    went through `RefreshPicaGlTargetScale` (the case foreseen above: it gets no color clear that
    covers it). And the criterion was blind: in the Water Temple `drs_pair_ms` ~26 < 28.3 while the governor
    was already dropping in-betweens (the hidden GPU of ~2.5–3 ms from lesson 37). **Fix (pgo5o):**
    - **Also apply at the frame's first draw.** The frame increments
      `gState.FrameSerial` together with `Requested` on each step; each eligible target keeps the
      `ScaleSerial` it has already seen. The first event of the new serial applies the scale: the color clear
      (as before) or, if it doesn't come, the first draw on the target (`ApplyPicaGlTargetScaleAtDraw`, after
      the pending fills and before viewport/scissor). At the draw, the switch does a **logical clear** of the
      color only (whatever the frame doesn't draw stays black, not with pixels from the other scale; depth has already
      been cleared by the frame's fill, with the PICA value, and is not touched). Once per serial and target:
      no frame mixes scales (pure `ScaleAtDraw`/`ScaleAtColourClear` in
      `gl_pica_dynamic_scale.h`, test `TestBackendAppliesAtFirstDraw`).
    - **Decide like the governor.** The pair becomes `2·GPU_step + HiddenGpuMs` (3 ms); a step in which the
      governor is dropping (in-between level below the preferred one, or the locked 30 of
      `fps60_mode`) counts as over budget if the GPU pair is ≥ 65% of it (21.7 ms) **and** the GPU
      is the larger side of the pair (GPU pair + hidden ≥ 2 × the average busy render thread without waiting;
      a CPU-caused drop doesn't improve with fewer pixels). Doesn't go up while there are drops. Calibration
      (`perf.txt` windows from tour17/tour19 at 3x, per GPU pair): drops 0–2% below 24 ms,
      3–9% in 24–28, 13–28% in 28–30, 33–80% above. Goes down with pair > 25.3 ms, up with predicted pair ≤
      23.7. Model: tour19's Water Temple (26) → 2.75 (~26.3 with the hidden part) and stops; areas at 23.5 stay
      at 3.0. tour18 (fixed 2x) shows the expected gain: Water Temple 55 → 59.9, hold 39 → 4.
    - `perf.txt` gained `drs_applied_draw` (of those applied, how many at the first draw), `drs_shed`
      (results counted as over budget because of governor drops) and `drs_exempt` (eligible targets that
      left via Shadow2D, cumulative). Expected in the next tour: `drs_applied` > 0 whenever
      `drs_changes` > 0; if `drs_exempt` > 0 and `drs_applied` = 0, the scene target became exempt.
- Draw merging (aurora/Dusklight technique): **measured and discarded, code removed**. The
  per-draw signature (`step_mergeable=` etc., `gfx_opengl_pica_merge_stats.h`) showed in tour11
  (10/23, 15 areas) 0.7 mergeable out of 74.7 draws per frame (~1%), longest run 1.8: almost every
  draw changes texture or uniform (per-object matrix), unlike Twilight Princess's GX. The
  signature itself (byte-by-byte FNV of uniforms and vertices) cost 3–4 ms per frame of render
  thread with `gpu_timing` (`draw_perf_sig_ms`, tour12), so it was deleted (the `perf_sig` phase, the
  `*_merge*` fields and the test were removed); earlier tours with `gpu_timing` come out pessimistic by that much.
  Draw breakdown in tour12 (Water Temple, 122 draws, 69 µs/draw with the signature): `glDraw*` 1.9 ms,
  vertices 1.1, uniforms 0.8 (**551 KB of uniforms per frame**, 181 uploads), state 0.5, texture
  upload 0.5, outside the submit 0.9; `sec_resolve_plans` 2.0 ms; texture hash at draw = 0.
- `sec_resolve_plans` (~2 ms on the render thread, Water Temple) **is not a plan built late**: in
  tour12, in a window of the area with `plan_late_draws=0`, `plan_late_ms=0` and `plan_lock_wait_render_ms=0`
  (the worker built everything in time) the section still gave 1.73 ms; in the other windows the real delay is
  0.08–0.19 ms per frame. The cost is **taking** the ready plan: `Oot3dPicaVulkanDrawPlan` is
  **15,712 bytes** (two `Oot3dPicaGeneratedFragmentShader` of 6,272 with the whole uniform
  state, the normal one and the uber one, plus 1,688 for the vertex one), and `plan = std::move(batch.Plans[i])`
  copies all of that per draw: ~1.9 MB per frame, cold (the worker batch and the frame vector add up to
  more than the 2 MB L2). Microbench on the host (x86, -O2): 0.43 ms per frame of 122 moves; the
  strict identity (`ComputeOot3dPicaVisualDrawIdentity`) is ~0. There is no simple fix without
  new shared state: the way is to shrink the plan (uniform state out of the
  shader struct, or the uber only when it exists) or have the frame keep the plan by pointer instead of
  moving it; both change the layout used by the scheduler and by interpolation.
- **Lean plan (10/23): 15,712 → 9,568 bytes (−39%), no key.** The plan's `UberFragmentShader`
  was a whole `Oot3dPicaGeneratedFragmentShader`, but nobody read anything from it besides `StateKey`,
  `SourceIdentity` and `Hooks` (the source comes from `SharedUberFragmentShaderSource`; the 6,112 bytes of
  `Uniforms` and the `Source` were always empty). It became `Oot3dPicaPlanUberFragmentShader` with only those
  three fields; the compiler proves nothing read the rest. It pays off on every plan copy/construction:
  `resolve_plans` (batch → frame), the `plan = {}` of each build in the worker, the placeholder on the
  game side and the frame split. Microbench on the host (122 cold moves): 0.385 → 0.231 ms per
  frame; on the console, estimated ~0.6–0.8 ms less in `sec_resolve_plans` (out of ~2) and less bandwidth
  in the worker. No key because it is layout, not behavior: there is nothing to turn off at
  run time. Options evaluated and discarded: **(a)** the frame keeping an index/const pointer into the worker's
  batch (the batch already lives as long as there is a placeholder, via an aliased `shared_ptr`) would require replacing
  the ~113 accesses to `frame.Draws[i]` (interpolation matching, strict identity, composition of the
  top screen that mutates the frame, savestate, reduced replay) with an accessor — a forgotten one reads the
  empty placeholder without crashing, just a wrong image; **(b)** a program cache by key already exists
  for the source (`Shared*Source`); what weighs is the per-draw uniforms (`FragmentShader.Uniforms`
  6,112, of which `ProcTexProgram` 3,616 and `FogLut` 1,024; vertex 1,604), which are the UBO's mirror
  (layout contract with the GLSL, see 4.7) and cannot leave the plan without touching the upload. Also
  discarded: the worker building directly into the frame's slot (shared mutable state) and swapping the
  batch's whole vector with the frame's (suppressed draws and split frames break the
  1:1 correspondence). Tests: `oot3d_native_pica_deferred_plan_tests` gained
  `static_assert(sizeof(Oot3dPicaVulkanDrawPlan) <= 10 KiB)`, the comparison of the uber program and of
  more fragment uniforms in `RequireSamePlan` (deferred-path plan == serial), and
  `TestResolvedFrameOutlivesItsBatch` (a resolved frame used as the in-between's previous frame
  stays identical to the serial one after the batch went back to the pool and was reused by the next
  frame; the `BasePlan` view points to it). Soaks and plan/interpolation/savestate tests
  pass; TSan clean on the deferred-plan tests and both soaks.

### 4.9 Mesa 20.1 vs Mesa 26

- Mesa 20.1 (devkitPro's switch-mesa): rebuilt with SwitchWaker/SwitchWakerHD's on-disk cache
  patches (+ the `thrd_success == 4` fix for newlib),
  `MESA_SHADER_CACHE_DIR=sdmc:/switch/oot3dre/mesacache` before SDL creates the context; already-seen shaders
  load in ~12 ms instead of 1–3 s.
- Mesa 26: converts GLSL to NIR in `glCompileShader` and keeps the NIR in the shader object. With the
  shaders attached, `glDeleteShader` only marks them: do **`glDetachShader` before deleting**
  (−2.0 MB per program on the host). The rest (~6–8 MB per program on the console vs ~1.5 MB on 20.1) is
  nvc0 keeping the whole NIR: only fixable with a Mesa patch.
- At 30 FPS with a warm cache the two tie *(session notes)*; with a cold cache 26 stutters more.
- **Ready-made cache in the package** (2026-10-22): the Mesa cache key is the Mesa build + the chip + the
  shader source, with nothing device-specific; the `.src` files in `shadercache/` (PICA→GLSL sources that the
  startup warm-up compiles) add up across SDs. How to generate: go through every area (warp via the
  lab channel, 5 s each, see 5.3) on the console or in Eden, gather the `.src` files (union) on the console's SD,
  open the game once (the warm-up compiles and writes `mesacache/`, including the folder
  `mesa_shader_cache_sf/<id>/NV12B/*.foz`) and copy `shadercache/` + `mesacache/` into the package.
  Check in `shadercache/status.txt`: `warmup_skipped=0` and a low `warmup_seconds` (269 shaders
  in 2.3 s). In the tour, areas like Water Temple went from 11.7 to 44 FPS with the warm cache alone.

### 4.10 Atmosphère crash reports

- Files in `sdmc:/atmosphere/crash_reports/`. The `.nro` is loaded by hbloader at a variable
  address: find the base by aligning the stack's return addresses with the ELF's `bl`/`blr` instructions
  (for the right base, `retorno − base − 4` lands on a `bl/blr` in almost every frame); then
  `aarch64-none-elf-addr2line -e oot3d_native_game.elf` with `address − base`.
- Keep the exact `.elf` of each `.nro` sent (`build-switch/oot3d_native_game.elf`).
- "perf.txt stopped and there is no crash report" is also a normal exit: ask before concluding it crashed.

---

### 4.11 deko3d: precompiled DKSH shaders (no compiling on the console)

*(2026-10-24, `deko3d` branch, see `docs/DEKO3D_PROGRESS.md`)* With deko3d there is no GLSL
compiler on the console: the shaders ship ready-made (DKSH, from devkitPro's `uam`) in a single package,
`shadercache/deko3d.pack`. The technique is reusable in any port with a GL backend:

- **Same source as GL.** The deko3d backend translates the canonical source with the same function as GL
  (`TranslatePicaShaderToOpenGl43`) and uses the GL 4.3 text as the key (FNV-1a 64 of the stage byte +
  text). That way the `.src` files (PGS1) that GL already writes to the SD become the whole package offline:
  `tools/oot3d/deko3d_shader_pack.py --src shadercache -o deko3d.pack` (272 stages in ~6 s with
  `-j 2` on the laptop; uam accepts Mesa's GL 4.3 unchanged: UBO `binding` = stage slot,
  `sampler` `binding` = texture slot).
- **Parametric (uber) programs in the package from the start:** `oot3d_deko3d_parametric_glsl_dump`
  writes the 4 bases (light × whole texture0) as `frag_<chave>.glsl`, and `--glsl <dir>` compiles them.
  The backend implements `PicaProgramPreparationBackend::UsesAlwaysAvailablePrograms() = true`, so
  every draw arrives with the specialized one **and** the parametric one: without the specialized DKSH, it draws with the
  uber; without both, it skips and counts. There is never compilation on the render thread.
- **What was missing records itself:** text without a DKSH goes to `logs/deko3d_shaders/` and enters the
  next package with `--glsl`. Each source identity is resolved only once (absence is also
  remembered), otherwise a draw without a DKSH would redo the ~31 KB translation every frame.
- **Loading:** the whole DKSH goes into code memory (`DkMemBlockFlags_Code`, alignment
  256) and `dkShaderMakerDefaults(&maker, bloco, offset)` without a separate control pointer.

### 4.12 deko3d: replacing GL with deko3d without changing the shaders (targets, state, scanout)

*(2026-10-24, package 5 of the `deko3d` branch)* Recipe for taking a GL backend with offscreen targets to
deko3d with the **same GLSL texts**:

- **Conventions:** device with `DkDeviceFlags_DepthMinusOneToOne | DkDeviceFlags_OriginUpperLeft`.
  Offscreen, memory stays the same as in GL (NDC y=−1 → row 0, `gl_FragCoord.y` = row
  index, texture t=0 = row 0), so GL viewport/scissor/blit apply 1:1 and the shaders don't change.
  Only drawing into the **window image** flips Y: in the scanout, `v = 0.5 − y/2` in the vertex shader.
- **Face winding:** with the origin at the top, the winding in the window flips (as in Vulkan): front
  CW for GL's "CCW". Leave a file key (`config/deko3d_cull_invert`) to undo it
  without a rebuild if the geometry shows up inside out.
- **Internal (scanout) shaders without a compiler on the console:** GLSL in `tools/.../*.glsl`, `uam` on the PC
  and a generated header with the DKSH in `uint8_t[]` (`tools/oot3d/deko3d_scanout/gen.py`); loaded
  with a fixed key into the same `ShaderStore`. In uam there are no loose uniforms: use a std140 block.
- **Barriers GL did on its own:** buffer→image copy (copy engine) before the draw that reads it:
  `dkCmdBufBarrier(Full, Image|Descriptors|Shader)` once (and 1×/frame, which also flushes the uniform
  cache over the reused stream memory); drawn target → 2D blit:
  `Barrier(Full, 0)` before and `Barrier(Full, Image)` after (the scanout samples the copy and the next
  frame writes the target again).
- **Images:** color target `RGBA8` with `UsageRender|Usage2DEngine` (it is a blit source), depth `Z24S8`
  with `UsageRender|HwCompression`; display image with `Usage2DEngine` (blit destination) and a
  descriptor slot of the global array; an image whose size changed goes onto a list and is only destroyed after
  `kFramesInFlight` frames.
- **Missing vertex attribute** (GL: a disabled array reads (0,0,0,1)): a 16-byte buffer in memory
  **with a CPU address** (`CpuUncached`) in a fixed slot, per instance. Bug seen here: the image
  block (`DkMemBlockFlags_Image` without `Cpu*`) has no CPU address; the constant was never written.
- **Logic operation:** the PICA order (Clear, And, AndReverse, Copy, Set, …) is not deko3d/GL's
  (… Copy=3, …, Set=15): map it case by case and test on the host; `logicOp = Copy` turns it off.
- **Widescreen/projection:** the game code decided the frustum widening by the API name
  (`"OpenGL"`); a new backend with GL's target has to go into the same rule, otherwise the scene is
  stretched.

### 4.13 deko3d: texture pack (Azahar format) and upscaling of menus/maps

*(2026-10-29, p7c of the `deko3d` branch)* Replacing textures with high-resolution files without touching the
romfs, reusing the `AzaharTexturePackRuntime` that already existed for Vulkan:

- **Name = hash of the native content:** `tex1_{L}x{A}_{HASH}_{fmt}_mip0.png`, with `HASH` = CityHash64
  (`Fast::Renderer::ContentHash64`, header-only) of the bytes of the **base level** in PICA format
  (bits per texel: RGBA8 32, RGB8 24, 5551/565/RGBA4/LA8 16, L8/A8/LA4 8, L4/A4/ETC1 4, ETC1A4 8).
  Host tool: `tools/oot3d/ptbr/texture_hash.cpp` (test vector: "hello" →
  `B48BE5A931380CE8`). If the texture in the romfs changed (translation), the hash changed: redo the pack.
- **Enabled by file:** `config/texture_pack` exists → `Configure` with `LoadCustomTextures`; the
  first line is the folder (default `sdmc:/switch/oot3dre/textures`). The scan is **recursive**:
  an optional subfolder (item icons) is turned on/off by moving the folder over FTP (`RNFR`/`RNTO`).
  L3+R3 → Video turns it on/off and sets the size cap (`config/texture_pack_scale`) at once
  (`LiveTexturePackSettings`, applied in `BeginFrame` by retiring entries of the earlier epoch).
- **Without stalling the frame:** on a cache miss, only color textures call `ResolveOrQueue`; if the PNG
  hasn't loaded yet, the native one is uploaded with `PackPending` and, on later hits, `PollOrQueue` swaps it
  when it's ready (the native one is then retired). Cap of **16 MiB of replacement per frame** (reset in
  `BeginFrame`). The replacement has its own key (fixed salt), is reduced to native x N on the pack worker
  (default 2) with its mip chain made there, and gets mip levels when the native texture has them (sampler
  LOD range moved down by the extra levels). After the upload the worker's decoded copy is released.
- **Pitfalls:** reference to the native `Texture`, not to the iterator (the replacement's `emplace` may
  rehash); inside `namespace Fast` the name `Oot3d::Renderer` resolves to `Fast::Oot3d` — use
  `::Oot3d::Renderer::`. The loader thread is `join()`ed, never `detach()`ed.
- **Generating the pack:** `texture_pack.py export` (menus 06/07, `menu/map/*`, area titles) →
  Real-ESRGAN `realesrgan-x4plus-anime` 4x (Vulkan, runs without an NVIDIA GPU) → `assemble` reduces to 2x
  with Lanczos and **restores the native alpha** (bicubic): the upscaler blurs transparent edges.
  OoT3D EU: 384 textures, ~15 MB.
- **HUD (UI renderer):** the native UI texture provider offers each HUD texture to
  `SetCustomTextureSource` (`oot3d_native_a32_window_setup.cpp`): `ResolveOrQueue` with
  `UserInterface = true` (3x cap, no mips, own cache) while the pack is on, else TopScreen's HD UI
  atlas (`config/topscreen_ui_atlas_hd.png`) for the exact `ui_atlas` bytes. UVs are normalized, so a
  replacement of any size draws with the same coordinates; the HD atlas stands in while a decode is
  pending (polled every 8 frames), a texture with only its native pixels is held (not drawn, polled
  every frame, at most `kUiHoldPolls`). HUD images decode on their own pack worker and are listed in
  `config/texture_pack_hud`, decoded ahead at the next start; from the title screen on the frame
  asks for the HUD's textures ahead (`HudTexturesAhead` → `N64UiFast3dRenderer::RequestAhead`, one
  per frame and per revision; the pack request is `Ahead`: the HUD worker hashes a copy of the
  encoded bytes and queues the image), so they are ready before the HUD's first frame. The pack
  generation is the provider's `Revision` (the ocarina text runtime, the renderer's provider, passes
  it through).
- **HD font** doesn't need the pack: `config/font_coverage.zip` is already backend-neutral; generate the
  coverage for the ROM's region (`message/eu/` on EU; the ready-made zip covered only `message/us`).

## 5. Measurement and diagnostics

### 5.1 Files on the SD (`sdmc:/switch/oot3dre/`)

| File | Content |
|---|---|
| `boot-status.txt` | initialization stage (`stage=runtime_running`), whole-AOT counts, readable error |
| `logs/perf.txt` | one `host_frames=...` line every 150 frames + `clocks:`, `audio:`, `interp:`, `sync:`, `threads:`, `glperf` blocks |
| `logs/oot3d_native_game.log`, `logs/glthread.txt` | general log and GL context switching |
| `logs/guest_profile.txt` | per-function profile (with `config/guest_profile`) |
| `logs/failed_pica_*.glsl` | PICA shader source that failed |
| `logs/spikes.txt` | with `config/frame_spikes` or `config/gpu_timing`: `session_start vsync_clock= vsync_source=event\|acquire\|none vsync_rc= vsync_rc_module= vsync_rc_desc= vsync_stage= pacer_vsync= offset_ms=` and one line per frame that missed the vsync (4.5): `spike t= host= guest= kind=step\|interp\|hold refreshes= expected= stage=guest\|render\|gpu\|pacer guest_ms= game_ms= render_ms= render_wait_ms= pacer_ms= swap_ms= gpu_ms= top=<glperf stage> top_ms= ctx=load,tex,shader,program,gl_oom,evict,drs tex_new= prog_new= shader_new=`. `game_ms` = game thread busy; `render_wait_ms` = acquire; `top` only with `gpu_timing` (`-` without); `ctx=load` = scene change in the last ~2 s; `shader_new` = Mesa's `glsl_compiles`+`nvc0_translates` |
| `logs/guest_spikes.txt` | with `config/guest_spike_ms` (2.2): `session_start threshold_ms= load_cutoff_ms= top= functions_in_map=` and, per game frame above the limit, `spike host_frame= guest_ms= entries= blocks= functions= load=0\|1 overflow=0\|1` followed by up to 10 lines `  fn=0x<entry> ms= pct= name=` (most expensive first; `pct` = % of the frame; `entries` = block entries, to subtract the overhead; `overflow=1` = more than 65536 blocks, the excess ones were left out of the top) |
| `logs/guest_spikes_summary.txt` | rewritten every 150 frames: header `# threshold_ms= load_cutoff_ms= frames= spikes= load_spikes=`, `# guest_ms_avg= guest_ms_avg_spike= ring_dropped= overflow_frames= summary_full=` and the 30 functions with the highest total time in non-loading spikes: `entry ms_total %spike_time frames ms/frame ms_max name` |
| `logs/hang.txt` | hang watchdog: stage and PC/LR/stack of each thread when the game or the render thread stall > 3 s; "HEAP CORRUPTED" from the heap scan |

### 5.2 Important `perf.txt` fields

- `glperf` CPU per PICA draw (only with `config/gpu_timing`; `step_`/`interp_`, ms per host frame): `draw_<phase>_ms` with phases `validate program target state uniforms vertices tex_hash tex_lookup tex_upload indices draw rest` (sum = `draw_submit_ms`, time inside `SubmitPicaDraw`; the `perf_sig` phase of the merge measurement was removed), `draw_outside_ms` (PICA stages minus submit = caller's work between draws), `draw_us` (submit per draw), per-frame counters `draw_draws`, `draw_tex_lookups`, `draw_tex_hashes`, `draw_tex_hash_bytes_kb`, `draw_tex_uploads`, `draw_tex_upload_bytes_kb`, `draw_uniform_sent[_bytes_kb]`, `draw_uniform_skipped[_bytes_kb]`, `draw_vertex_bytes_kb`, `draw_index_bytes_kb`; and `sec_<resolve_plans|pin_copy|scene_env|execute|visual_sample>_ms` (wall time of render thread sections, to break down `cpu_other`; `execute` includes the draws, `visual_sample` includes `pin_copy` and `execute`). Source: `gl_pica_draw_phases.h` (test `gl_pica_draw_phases_tests`).
- Main line (ms per host frame): `fps`, `guest` (game logic), `dsp_mix`, `audio_out`,
  `pica_submit`, `pica_plan`, `pica_backend`, `visual_present`, `visual_replay`, `present`,
  `pacing_wait`; memory `heap_arena_mb`, `heap_arena_peak_mb`, `proc_used_mb`/`proc_total_mb`
  (always stable in a long session); GL `gl_textures_live`, `gl_tex_mb`, `gl_programs`,
  `gl_buffers_live`, `gl_oom`, `gl_draws_skipped`.
- `clocks:` `cpu_mhz gpu_mhz mem_mhz` (real clock measured by sys-clk; `clk_src=sysclk_real`, or
  `sysclk` = target, `stock perf_cfg=0x…` = sys-clk absent, stock clocks of the HOS performance
  configuration, `none` = neither answered → MHz -1), `op_mode=handheld|docked`,
  `perf_mode=normal|boost`, `sysclk_profile`, `sysclk_enabled`, `sysclk_api`, `clk_ipc_us`. Read
  only: one `GetCurrentContext` call to `sys:clk` per window, session opened once; the app
  never opens clkrst/pcv nor changes clocks (SwitchWakerHD issue #33).
- Plans: `plan_worker_ms`, `plan_late_ms`, `plan_late_draws`, `plan_batches_live/queued/allocated`
  (small and constant), `plan_lock_wait_*`.
- Spikes (with `config/frame_spikes`/`gpu_timing`, per window): `missed_refresh` (missed refreshes
  = sum of refreshes − expected), `missed_frames`, `missed_by_stage_{guest,render,gpu,pacer}`
  (missed refreshes per blamed stage; they add up to `missed_refresh`), `hold_presents` (presentations without
  a new sample, `RenderFrameKind::Other`), `spikes_dropped` (ring full, cumulative), `vsync_clock`,
  `vsync_period_us` (measured display period), `vsync_resets`, `pacer_vsync`,
  `pacer_vsync_waits`, `pacer_vsync_snap_max_us` (largest pacer phase correction, cumulative).
- `sync:` `guest_wait_frames_ms` (game waiting for the render thread), `render_idle_ms`,
  `pacer_oversleep_max_ms` (> ~0.5 ms → try `pacer_spin_us=200`).
- `threads:` `layout=… <papel>_cpu=% @c0:c1:c2` (CPU per role via `svcGetInfo(ThreadTickCount)`):
  `game_cpu` 95–98% on core 0 = logic bottleneck.
- `audio:` `min_queued avg_queued device_hz input_hz trim_pct game_speed inserted_ms removed_ms
  concealed_ms`.
- `interp:` `level step_frames interp_frames interp_dropped reduced_refused gpu_ms_<type>
  rt_ms_<type> est_*`; dynamic resolution (4.8): `drs drs_scale drs_min drs_scale_avg drs_pair_ms
  drs_changes drs_downs drs_ups drs_bounces drs_applied drs_applied_draw drs_shed drs_exempt`
  (pgo5o; see 4.8).

### 5.3 Tools

- `python3 tools/oot3d/perf_report.py perf.txt [--session -1] [--skip 2] [--windows] [--top 8]`:
  per-session summary (a new session starts when `host_frames` decreases): fps, clocks, phases,
  memory and, with `glperf`, top GPU stages, render thread CPU and counters. If the clock changes in
  the middle of the session, it warns (window and clock before/after) and splits fps/`total_ms` by clock.
  In 60 FPS sessions it ends with the **60 FPS verdict**: GPU and *busy* render thread (StartFrame→swap
  minus idle and pacer) against 16.7 ms, the logic step pair (`game_pair_ms`) against 33.3 ms and the
  session's hitches (`hitch_*`); it marks what overruns. To get GPU and render thread, `config/gpu_timing`.
- Input bench (`config/bench_record` / `config/bench_replay`, `oot3d_input_bench.h`): records the
  controller per game frame starting from the boot warp and replays the same route on each build; in
  replay the game itself gets past the title screen. `~/oot3d-work/bench/run_bench.sh <nro> <label>` runs in
  Eden and averages `perf.txt`. It is for behavior regression, not for measuring speed
  (see lesson 22).
- Lab channel (`config/dev_lab`, `oot3d_dev_lab.h`): line-based JSON commands in
  `lab/commands.jsonl` (warp, wait, capture, status, graphics, peek...), responses in
  `lab/acks.jsonl`. On the console Sphaira's FTP **closes with the game open**: leave a script in
  `lab/script.jsonl`, which the game reads once at startup (renames it to `script.running`), starting
  with `{"op":"gameplay"}` (waits for the player to have control). Performance tour: warp to each
  "Better" area (0..48; 49 is the Debug room and freezes) + a 300-frame `wait`, then
  `tools/oot3d/lab_tour_report.py` matches the `perf.txt` windows with the acks by guest frame
  (sum of `guest_fps × 150/fps` per window) and sorts the areas by FPS. Run it **twice**: the
  first pass measures shader compilation, the second the real load.
  Walking tour: `{"op":"stick","y":1}` holds the circle pad (x/y in -1..1, ±154 in HID; y=1 walks
  forward) until another `stick`; `{"op":"stick","y":0}` releases. Put it after the warp and release before
  the next one (in the Market, y=-1: forward leaves the area).
  `"buttons":["R"]` in the same `stick` holds 3DS buttons (A B X Y L R ZL ZR) until the next `stick`;
  press = `stick` with the button, `wait` of ~6 frames, `stick` without it.
- `config/gpu_timing` (N, default 30): GPU per stage with non-blocking `GL_TIMESTAMP`, render
  thread CPU per stage, GL call counters (hooks on glad's pointers), split by frame
  type (`step`/`interp`/`hold`). `gpu_*` includes CPU wait when the render thread is the bottleneck.
- `config/gl_stats`: draws, state calls emitted/skipped, binds, `gl_us_per_draw`.
- **Guard pages for use-after-free** (`oot3d_switch_guard_alloc`): if the scan always flags
  the same chunk, the size range comes from its `size` (0x50 = requests from 57 to 72).
  `-Wl,--wrap=_malloc_r` (and `_memalign_r`, `_free_r`, `_realloc_r`, `_calloc_r`, `_malloc_usable_size_r`)
  catches the whole process in devkitA64's newlib — its `malloc()` sends n >= 32 to
  `_memalign_r(32, n)`, so wrapping only `_malloc_r` catches almost nothing. The block goes to
  `_memalign_r(0x1000, 2 pages)`; on free the object's page is closed
  (`svcSetMemoryPermission(Perm_None)`) and goes into quarantine; the second page is slack for the
  neighboring chunk's header. Caveats: **don't enable it at boot** (thousands of small objects × 8-12 KiB
  exhaust the heap: boot fails with "Not enough space") and put a cap on live blocks; the real
  `calloc`/`realloc` read the header of the chunk returned by malloc (divert their malloc);
  don't touch TLS before enabling (libnx calls malloc early); the exception handler
  (`__libnx_exception_handler` + `__nx_exception_stack`) cannot allocate. Idea: Chronicle PR #205.
- `config/heap_stats`, `config/heap_check_windows=1`: finds heap corruption ~5 s after it
  happens. Uses a bounded scan of newlib's bins (`oot3d_heap_bin_check.h`), not
  `mallinfo()`: the latter walks the lists without bound while holding the malloc lock and, with a corrupted
  list, freezes the whole process without a crash (that was the 2026-10-17 freeze).
- `python3 tools/oot3d/spike_report.py <dir> [oot3d_dev_warp_catalog.inc]`: spikes from
  `logs/spikes.txt` **from the last session only** (the file accumulates sessions: in tour20, 428 of the 591
  lines were from tour19), without `ctx=load`, by area (lab acks) and stage, isolated vs. in
  sequence and how much the game frame would have to shrink to fit.
- **Analysis of non-loading spikes (2026-10-24, tour20/pgo5o and tour22)**: tour20 = 164 spikes,
  121 in load, 43 outside (9 at boot; Kakariko 5, Desert Colossus 2, Deku Tree 2; ~22 in the heavy scene
  after the tour). tour22 = 227, 103 outside (Market 15, Fire Temple 7, Hyrule Castle 5). Almost all
  `stage=guest` with guest_ms ~20-21 ms (long logic step), and most **isolated** (a long step
  followed by a short one: the pair fits in 33.3 ms). Shrinking the game frame by 1.7 ms (sound + DSP
  on another core) would solve **3 of 95** guest spikes — **discarded**; 8 ms would solve ~45%.
  Hence `config/early_frame_slot` (below). Measurement artifact: with `config/gpu_timing` the
  heap scan (every 20 windows) costs 16-21 ms on the game thread and produces a spike at
  `host ≡ 150 (mod 3000)`; ignore those (or measure with `heap_check_windows` absent and without gpu_timing).
- `config/early_frame_slot` (opt-in; content `0` turns it off): the end-of-frame command returns the
  in-flight frame's slot when the pacer **starts** waiting (not after the swap), and the game
  thread uses as its refresh clock the pacer deadline of frame S-2 (not the time it woke up).
  It's still 1 frame in flight (`frames_in_flight=2` corrupted the heap in tour3b): the new overlap is
  only the start of the game's frame S with pacer+swap of frame S-2. Simulation (`oot3d_native_cadence_
  governor_tests --report`, tour20-type area): missed refreshes/min 73→32 (12 ms step),
  176→45 (14), 555→84 (16), 970→213 (18); without the deadline-based clock images/s **get worse**
  (alternating 0/2 refreshes). `perf.txt` `sync:` shows `early_slot`, `early_slot_releases`,
  `early_slot_clock_frames`.
- `config/debug_force_game_over` (example of a game-specific test trigger): zeroes health after
  N s to test a rare screen.
- Status Monitor (overlay) photos and album captures (`album_sd:` via FTP) for GPU %/cores.

### 5.4 Hangs without a crash report: watchdog

A freeze (frozen image, nothing in `crash_reports`) has no stack. Recipe used in the port:

1. **Cheap per-thread markers** (`oot3d_hang_watchdog.h`): one slot per role (game, render,
   plans, audio) assigned where the thread already gets its priority/core; the current stage is a relaxed
   store, progress a load + store by the owner (per frame / per command, never per draw).
2. **Lowest-priority watcher thread** (0x3B) waking every ~1 s; "hung" = game or render
   thread not idle with no progress for > 3 s with the app in focus (`appletGetFocusState`); ignores
   jumps of the clock itself (console sleep).
3. **Write without allocating**: the hang may be the malloc lock. Text in a static buffer with
   our own formatting (no `printf` of floats, which allocates in newlib's `dtoa`), file through libnx's FS
   API (`fsdevGetDeviceFileSystem("sdmc")`, `fsFsOpenFile` with `Append`, `fsFileWrite`
   with `FsWriteOption_Flush`). Once per hang; "recovered after N ms" if it comes back.
4. **PC/LR of the other threads**: `svcSetThreadActivity(h, Paused)` (the kernel waits for the thread to leave
   the core), `svcGetThreadContext3`, read the FP chain and scan the stack with
   `svcQueryMemory` before each read, `svcSetThreadActivity(h, Runnable)`. Only works with the
   thread paused and only if the NPDM allows it: test `envIsSyscallHinted(0x32)`/`(0x33)` first (a disallowed
   SVC kills the process). `svcGetDebugThreadContext` requires a debug session and doesn't work
   for the process itself. Addresses: offset relative to the start of the NRO's code segment
   (`svcQueryMemory` on a function address) → `addr2line`.
5. **Last service call** (literal name + start/end ticks) around each IPC that
   can block (`smGetService` of a nonexistent service waits for registration; a full port waits for a
   session).
6. **Read the last line of `perf.txt`**: an `ofstream` per window writes in chunks of `BUFSIZ`
   (1024 on devkitA64); a cut line without `\n` says the thread stopped inside that block,
   after the last chunk and before the next — the section can be located by the byte count.

### 5.5 A/B protocol

1. **Same clock.** Overclock (sys-clk: CPU 1785, memory 1600) and the default clock give different
   results: at the default clock OoT3D's logic reaches ~17 ms per step (game_cpu 95–98%, game
   between 71 and 100% speed); with overclock it holds a solid 30 FPS with game_cpu ~60%
   *(session notes)*. **Always compare at the same clock; `perf.txt` now records it**
   (`clocks:` block in each window, `perf_report.py` shows and splits by clock). Without sys-clk the MHz
   are -1: then ask for the clock before comparing.
2. Same scene and same save, 2–3 min per mode, discard the first windows (`--skip`).
3. Warm shader cache on both sides (the first pass compiles).
4. One variable at a time, via a file in `config/` (almost everything has an A/B key) — without recompiling.
5. For logic, look at `guest` and `game_cpu`; for GPU, `glperf`; for audio, `device_hz` and concealments.

---

## 6. Infrastructure

### 6.1 Build on the Ryzen (WSL2)

- Build machine: Ryzen 5 5600G, Windows 11, WSL2 Ubuntu (12 GB / 12 threads in `.wslconfig`),
  devkitPro in `/opt/devkitpro`, same `/home/<user>/...` paths as the laptop (so ccache and
  `.gcda` match). Access via ssh with a key (PowerShell shell). Address and user are kept out of the repo.
- Scripts (currently **outside the repo**, in `C:\Users\<user>\`): `ryzen-build.sh`/`ryzen-status.sh` (full
  build and status), `rt-build.sh` (recompiles only the runtime from `repo.tar.zst` and refuses if the
  shards changed; returns `rt.nro`), `lean-use.sh` (PGO `use` build), `chk.sh`. Run with
  `wsl -d Ubuntu -u root -- bash /mnt/c/Users/<user>/<script>`; avoid nested quotes through
  PowerShell.
- Copying sources: local `git ls-files | tar | zstd` → `scp` to Windows → extract in WSL.
- **WSL shuts down when the last `wsl.exe` session closes.** An ssh keepalive dies if the laptop
  suspends. For a long build without the laptop awake, create a scheduled task on Windows
  (`schtasks`) that runs `wsl -d Ubuntu -u root -- sleep N` (`Start-Process` via ssh doesn't survive the
  end of the session).
- The laptop (4 cores) is only for editing and light commands: check `sensors | grep Package` is below
  ~85 °C before anything heavy (it has reached 98 °C with agents compiling).

### 6.2 Deploy

- **Sphaira FTP**: `ftp://<IP do Switch>:5000/switch/oot3dre/`. The IP changes with the network (find it
  by scanning port 5000). `curl -T file.nro ftp://<ip>:5000/switch/oot3dre/` overwrites directly.
  **Check the size** afterwards (e.g. `curl -sI ftp://<ip>:5000/switch/oot3dre/oot3d_native_game.nro`
  and compare `Content-Length` with the local one, or `md5sum` of a downloaded copy).
- Downloading logs and `.gcda`: `curl ftp://<ip>:5000/switch/oot3dre/logs/perf.txt -o perf.txt`.
- **MTP** (Sphaira): `gio mount mtp://<dispositivo>/`, folder `microSD card/switch/oot3dre`. MTP doesn't
  overwrite: `gio remove` and then `gio copy`.
- Open from the Homebrew Menu **in title mode** (holding R on a game), not from the album (limited
  memory).
- Keep a copy of each NRO sent with name and md5 (`~/oot3d-work/oot3d_native_game.<version>.nro`) and the
  corresponding `.elf`.

### 6.3 Host tests

- Rule: **a change that runs all the time needs a long-running test before going to the console**
  (the plan worker leaked heap; the audio stretch oscillated; both only showed up after minutes of
  play).
- Policy without GL/without hardware in pure headers with tests: `gfx_opengl_pica_state_cache_tests`,
  `gfx_opengl_pica_residency_tests` (30 simulated min), `gfx_opengl_pica_gpu_lean_tests`,
  `gfx_opengl_pica_scale_tests`, `gl_widescreen_menu_policy_tests`, `oot3d_switch_thread_layout_tests`,
  `oot3d_native_interpolation_pacing_tests`, `triaevum_presentation_clock_tests`,
  `oot3d_native_gpu_frame_timer_tests`, `oot3d_native_bottom_screen_skip_tests`,
  `oot3d_native_cadence_governor_tests` (pipeline model; `--report`),
  `oot3d_native_dynamic_resolution_tests` (DRS controller and first-draw application),
  `oot3d_frame_spike_tests` (spikes, vsync clock and acquire fallback source),
  `oot3d_guest_spike_profile_tests` (guest spike profile).
- Concurrency with ASan/UBSan and TSan: `oot3d_native_render_command_stream_tests`,
  `oot3d_native_render_shutdown_tests`, `oot3d_native_reduced_replay_stress_tests`,
  `oot3d_native_pica_deferred_plan_tests`.
- Soak: `oot3d_native_pica_visual_frame_soak_tests`, `oot3d_native_pica_deferred_plan_soak_tests`.
- Bit-for-bit exactness: `oot3d_native_audio_dsp_mix_tests`, `oot3d_native_audio_interpolation_tests`,
  `oot3d_native_a32_vfp_aarch64_tests` (qemu), `memory_lean_diff`, `run_guest_override_equivalence.py`.
- The Linux GL backend is the same as the Switch's: a good proxy for visual correctness (on Linux, removing a
  global `glFinish` per completion went from 23.3 to 49.7 FPS), but it is **not** evidence of console
  performance. Eden is for boot smoke tests, not for measuring.

---

## 7. Pitfalls and lessons

1. The emulator (Eden) hides console bugs: half speed, GL context, `pthread_detach`.
2. `-ffast-math` at **link time** breaks all C++ exceptions (`.eh_frame` without a terminator).
3. `-ffast-math` in the **shards** breaks guest VFP (IXC flags).
4. `pthread_detach` → `ENOSYS` in devkitA64's newlib.
5. libgcov reads `tpidr_el0`, which Horizon leaves at 0: mirror libnx's TLS.
6. Equal priority doesn't time-slice on Horizon (only 0x3B): a service thread at the game's priority stalls the
   render thread; SDL's `TIME_CRITICAL` becomes 0x3B.
7. GL context only on the thread that made it current; nvc0 has no lock on the shared push buffer.
8. The Horizon FS doesn't rename over an existing file.
9. newlib heap: global lock and fragmentation; `mallinfo()` stalls everything (and with a corrupted heap
   it never finishes: a freeze without a crash); no large allocations per frame.
10. A texture cache without eviction = crash from GPU memory after minutes.
11. A native replacement with a high decline rate is worse than nothing; the guest profiler forces declines in the measured
    frames.
12. The per-block profile inflates short blocks (~190 ns per entry): subtract it.
13. Generated C++ or an inlined header changed → PGO loses functions; plan a new collection.
14. `affinity` reshuffles ~30% of the shards on every selection change: use `affinity-incremental`.
15. Don't delete `.ninja_log`/`.ninja_deps` when duplicating a build.
16. Compare performance only at the same clock, same scene, warm cache.
17. 720p (3×) went from a CPU bottleneck to a GPU bottleneck: default 2×.
18. switch-mesa's GPU clock is not in ns: calibrate.
19. WSL shuts down with the last session: `schtasks` for long builds.
20. Agent prompt: say where it may compile (Ryzen) and the laptop's temperature limit.
21. Froze without a crash report: check `logs/hang.txt` (watchdog) and where the last line of
    `perf.txt` was cut before assuming a deadlock.
22. Don't optimize guided by Eden (2026-10-22): it runs the Switch's ARM via JIT on x86, varies 5–10%
    between identical runs, the 1st run of each .nro is slower (cold JIT) and, with the window off-screen
    (hidden workspace in Hyprland), FPS drops to ~⅓. The same build did 3 FPS hidden and 10 visible;
    the console already did 30 FPS at 99.5%. Measure on the console, with `config/gpu_timing`.
23. The whole-AOT block quota on the Switch is 2048 (1 MiB main thread stack). Native replacements
    with a long loop (audio reverb/delay, ~2–3 thousand blocks) exit through the quota midway through the loop and the
    rest runs in the translation, because resumption enters through the translated function's `switch(entryPc)`, not through the
    replacement. Raising the quota to 16384 didn't change the logic in Eden (82 ms in both): it's not the brake.
24. The guest profiler (`config/guest_profile`) costs ~1.5 µs per block entry in Eden and doubles the game's
    time; the profile comes out flat (no function > ~10%): the cost is spread across the translation, and
    replacing function by function doesn't reach big gains.
25. The old console logs already say a lot: before asking for a new run, run `perf_report.py`
    on `~/oot3d-work/switch-logs/*`. In `iscale` (session 46, ~10/17, interpolated 60 FPS) the GPU used
    10.4 ms and the render thread 18.3 ms busy per interpolated frame: the limit was render CPU, with
    ~1 MB of texture upload per frame even in frames without a new scene.
26. An expensive `clear` stage on the render thread (4-5 ms) is waiting, not work: Mesa's EGL on the Switch
    (`egl_switch.c`) only calls `nwindowDequeueBuffer` (blocking) on the first clear/draw of
    framebuffer 0. Clearing the window at the start of the frame stalls the thread before the scene (which goes to
    its own FBOs). Leave the window clear pending until the first real use (scanout, UI, ImGui,
    copies, end of frame): `GfxRenderingAPIOGL::FlushDeferredWindowClear`, `config/gl_eager_clear`
    turns it off. With that wait subtracted, lesson 25's 18.3 "busy" ms become ~13.6 ms. Applies to
    any port with the same Mesa: measure the window clear before blaming the CPU.
27. Menu homebrew FTP (Sphaira) only exists with the menu open: to automate something during
    gameplay, the command has to be on the SD before startup (script read at boot) or come from a
    sysmodule. The Switch's Wi-Fi also sleeps with the screen off: FTP disappears until it wakes up.
28. Measure per area before optimizing (2026-10-22): in the warm-cache tour, 20 of 49 areas stayed
    locked at 60 and the worst ones (Water Temple 44, Market 46, Castle Town Buildings 51) have the guest
    above 12 ms per frame: it's game logic, not GPU. A single area (the Market) doesn't represent the
    game.
29. GPU time from frame start and end timestamps also measures the idle GPU (2026-10-22):
    if the render thread blocks in the middle of the frame (window acquire, fence, queue), the gap goes
    into the count. Compare the span with the sum of the glperf stages (18.5 vs 12.3 ms in tour2) and
    mark the blocking points with timestamps before and after. A governor fed with the inflated span
    dropped almost every in-between frame: the `fps` counter (presentations per second) stayed
    near 60, but `interp_dropped` and `hold_2` showed ~30 distinct images.
30. Skipping GPU work that "nobody uses" requires following the source to the end: the top transfer is
    submitted outside the sample's transfer list and a later frame's transfer can read the
    skipped target. And more slack in the queue between threads (2 frames in flight) isn't just a number: the heap
    got corrupted in 21 s because shared state assumed 1 frame (2026-10-22).
31. An "all or nothing" optimization needs a reason counter from the first version: the
    bottom-screen skip went to the console, ran without error and skipped almost nothing (tour4), and without the reason in
    `perf.txt` all that was left was guessing (depth shared between the screens was the suspect).
32. A skip can never empty the output: when saving the game, all the sample's draws were from the bottom
    screen and the composition was left with no draws (crash in PGO5). A sample with only bottom draws (menu, save,
    pause) is never skipped (`kept_empty`) and neither are the next 60 samples (`kept_menu`).
    Console (tour10, 10/23, pgo5b, CPU 1785/GPU 614/mem 1600): 29 slow areas without a crash, final save
    ok; average 59.59 and 27 areas ≥59 (PGO5 without skip, GPU 768: 59.56 and 25); 28,388 samples
    skipped, 120 `kept_menu`, 2 `kept_empty`, 0 `unsafe`.
33. **Clock on the Switch: `cntpct_el0` (or `armGetSystemTick()`), never `cntvct_el0`.** Horizon doesn't
    expose the virtual counter at EL0: `mrs cntvct_el0` gives a Data Abort on the first read (pgo5e
    died at startup with `config/gpu_timing`, PC exactly on the `mrs`). Guard in
    `tests/switch/test_bug_guards.py`; check in the ELF with `objdump -d | grep -c cntvct_el0` = 0.
34. **An expensive measurement leaves the hot path as soon as it answers the question.** The merge
    signature cost 3–4 ms per frame (more than all the uniforms) and stayed on in every tour with
    `gpu_timing`, inflating the conclusions; and before optimizing a measured section, check what it
    contains: `sec_resolve_plans` looked like "late plan" but was copying 15.7 KB structs
    (`plan_late_draws=0` and the section still at 1.7 ms).
35. **Before changing who holds a large struct, see which of its fields anyone reads.** The
    15.7 KB plan seemed to call for ownership by pointer (a refactor of ~113 accesses); 39% of it was an
    `Oot3dPicaGeneratedFragmentShader` used for only three fields. Changing the member's type to a struct
    with only what is read is exact (the compiler rejects any read of what was removed) and needs no
    key. And a high `step_cpu_clear` (8 ms on the step, up to 15 on `hold`, which barely draws) with a clear GPU
    time of ~0.003 ms isn't clear work: it's the render thread blocked on the first `glClear`
    after the swap (free swapchain image / previous frame's fence — vsync backpressure,
    which adds to `pacing`). Don't optimize it as a clear; check by adding `clear+pacing`.

36. **Cost for deciding is busy time; waiting never counts.** A wall time that includes a
    block (acquire, vsync, fence) grows precisely when there's slack, and a policy that cuts
    work based on that time locks itself in: cutting doesn't reduce the wait, so the inflated measure persists.
    Measure the block where it happens (a hook around the call) and subtract it — on both sides (CPU
    and GPU). And a GPU stamp only measures what has already been submitted: a `glQueryCounter` emitted after the
    last `glFlush` executes on the next flush, so "before" and "after" stamps around a wait, with no
    flush between them, give the same time. When instrumenting, check which flush each stamp goes out on.

37. **Removing an inflated measure can expose that the budget was never what it seemed.** After removing
    the wait from the cost, measured pairs of 29–30 ms "fit" in 33.3 and missed refreshes: a GPU near
    100% is not fed end to end, and the wait counted before worked, by accident, as a
    margin. Every measurement fix that changes a decision comes with the margin calibrated on the console (per
    area, against the best bench), not with the theoretical budget.
38. **A crash inside the driver doesn't incriminate the caller.** A Data Abort in Mesa's `free`, during the
    eviction of a texture, was a heap corrupted earlier by something else: start with the registers
    (a Mesa struct with an odd pointer = overwritten memory, not wrong logical state) and prove the
    caller's part with a host soak under ASan. And check in the binary what the driver really
    supports (`*_is_format_supported`): a format "accepted" by GL may be a software fallback
    that allocates and decodes on the CPU.

39. **Dynamic resolution: change what is drawn, not what is allocated, and only where the content starts over.**
    Recreating targets on every change invalidates caches that hold GL names (state, image unit) and costs
    allocation mid-game; a sub-rectangle of the maximum target switches for free. Before choosing, list
    who reads the target (here: only the display transfer and Shadow2D, because nothing goes back to guest memory) and
    handle each reader (content extent + replicated border for the filters; exempt what uses
    `textureSize()`). Apply the scale on the color clear, not on the request, so no frame mixes
    scales. And go up by the **prediction** at the scale above, not by the current slack: "below 85% now"
    at 2.75 can be 95% at 3.0, and the controller oscillates every window.

40. **Two "60 Hz" clocks are never the same clock.** The pacer counted 1/60 s of CPU time and the display
    runs ~0.2% slower: with a FIFO swapchain nothing "misses the vsync" at the swap (the queue absorbs it), but the
    swapchain fills up, the acquire starts setting the pace (render thread stuck half the time, latency
    +2 frames) and every time the phase wraps around a refresh goes out without a new image. Whoever presents should follow the
    clock of whoever displays (vsync event: measured phase and period), and the smoothness measure should
    count missed refreshes per frame (`spikes.txt`), not a window average.

41. **An effect that depends on a game event needs proof that the event happens.** DRS
    applied the scale only on the target's color clear; OoT3D never cleared the scene through that path and the
    controller spent a whole tour "changing" a scale that never took effect (`drs_changes` > 0,
    `drs_applied` = 0). Counting the application (not just the decision) showed it in the first measurement; the fix
    was a point that always occurs (the frame's first draw) with the same invariant (one change per
    frame). Every "applies at X" gets a counter for X.

42. **Two controllers over the same resource decide with the same measure.** The interpolation
    governor saw the hidden GPU (through the acquire wait) and dropped in-betweens; DRS saw only the
    timer and thought it fit. Whoever reacts later (DRS) must add what the other already knows or read
    its decision, otherwise one drops frames while the other says everything is fine.

43. **A system service that fails on a 2nd open: use what is already open or a derived
    source.** The vi vsync event failed (desc 9, undocumented) because the default window already
    had the display. Instead of depending on a static libnx handle, the return of a call that
    blocks on vsync (the full swapchain's acquire) gives the same phase with a short latency, and the
    lower-envelope model already tolerates that. Logging the stage and the decomposed rc avoids a lost tour
    next time.

**tour19 (10/23, pgo5n, 3.0, DRS and `gpu_timing` on):** 29 areas, average 59.47, 24 areas ≥59, Water
Temple 56.3 (min 48.6). DRS without effect (4.8), vsync clock not opening (4.5), 428 missed refreshes
with 79% blamed on the game thread (2.2, spike profile). Fixes in pgo5o.

**Best bench so far (10/23, tour17, pgo5l = 4d512833, pgo5i defaults + ETC2 off, GPU 614, Link walking):** 29 areas without a crash, average 59.44 (tour14/pgo5i: 59.34), 26 areas ≥59 (before 23), hold 41→39 per window. Worst area: Water Temple 55.1 (min 51.1).

**Best smoothness (10/23, tour20, pgo5o = 8f67f53a, config/dynamic_resolution, base 3.0, GPU 614, Link walking):** hold 39→4.6 per window (tour17 3x) — same as fixed 2x (4.4); interp 53→75; missed refreshes 5.7→3.7 per window (tour19); average 59.60 (3x 59.44, 2x 59.68), 26 areas ≥59; minimum FPS Water Temple 51.1→60.0, Desert Colossus 57.3→59.8, Market 59.9. Cost: the scale drops in almost every area (average 2.25–2.9; Market/Castle Town 2.25). Vsync: event fails with vi desc 9 at stage 2 (opening the display), `acquire` fallback source valid.

**pacer_vsync rejected (10/23, tour22 = tour20 + config/pacer_vsync, `acquire` source):** only the last run in perf.txt (the file accumulates every run; the first count mixed in the slow tour21 and gave 48 missed refreshes per window, wrong): missed refreshes 3.66→4.13 per window, interp 75→69, fps 58.84→58.69. Slightly worse; stays off.

**Always compare only the last run in perf.txt:** it is appended on every run; split at the windows where `host_frames` drops again.

**Guest spike profile (tour21, guest_spike_ms 14):** the per-function instrumentation took the guest to 39.5 ms/frame, so 97% of frames became "spikes" and the result is only a general profile (audio reverb+delay ~12%, Player 0x46C204 ~3%). To isolate spikes under instrumentation, the limit has to be relative to the average measured with the profiler itself on, not 14 ms. All guest threads run cooperatively on one core (NativeA32Process::Run): audio competes for the game's core.

---

## 8. Next-game checklist

Estimates based on what OoT3D took on the Switch (the whole Switch port took ~2 days of
intense work with several agents, starting from an already mature Windows recompilation). For a game
without a prior recompilation, steps 2–4 dominate and are **to be measured**.

| # | Step | Output / criterion | Estimated effort |
|---|---|---|---|
| 1 | Own dump, extract with `ctr_rom.py`, check ExHeader (entrypoint, `.text`) | `code.bin`, `exheader.bin`, RomFS | hours |
| 2 | Function discovery (3dsrecomp + A32 front end) → inventory | the game's `codebin_function_inventory.csv` | to be measured (days to weeks) |
| 3 | `generate_whole_aot.py` + initial selection (all functions) | `aot_program.json` without residual exits | to be measured |
| 4 | `generate_whole_aot_cpp.py` (without extra optimizations) and build of the ARM64 archive | `build-whole-aot.sh` passes | 1 day |
| 5 | Probe in Eden and on the console (`build-whole-aot-probe.sh`) | receipt `whole-aot-probe.txt` | hours |
| 6 | CTR services the game uses (fs, gsp, dsp, hid, ptm, shared page clock) | `boot-status.txt` reaches `runtime_running` | days (depends on the game) |
| 7 | Switch product: SDL2 + GL, `stage-product.sh`, game folder | image on screen on the console | 1–2 days |
| 8 | Console fixes from section 7 (already solved in the runtime: check they apply) | no crash, right speed | hours |
| 9 | Threads/priorities (`thread_layout`), render thread, 48 kHz audio | `perf.txt` with clean `threads:`, `audio:` | hours (reuse) |
| 10 | Generator options: native VFP, TBAA, lean, giants, `affinity-incremental`, ccache | full build + exactness tests | 1 day |
| 11 | PGO collection 1 on the console → `use` build | `.gcda` without coverage-mismatch | half a day (+ build) |
| 12 | Guest profile → candidates → exact replacements with harness | equivalence 1M+ cases, low decline | 1–3 days |
| 13 | GPU: state cache, residency, exact savings, scale/FSR | stable `glperf`, stable memory in a long session | reuse + hours |
| 14 | 30+ min soak on the console (heap, GPU, audio) | stable memory fields | hours |
| 15 | Final PGO collection with everything integrated → final build | final NRO + `.elf` kept | half a day (+ build) |
| 16 | Document in the guide what changed (generic vs. specific) | this file updated | hours |

---

## 9. Known gaps in this guide

- **Decline counters contaminated by the profiler**: they are only written in the appendix of
  `guest_profile.txt`, and the profiler forces declines in every measured frame. Still missing: writing
  `native/declined` to `perf.txt` without the profiler (and a counter per decline reason).
- The `DescribeOot3dGuestOverrides` message mentions `config/guest_overrides_off`, but the file that
  turns them off is `config/no_guest_overrides`.
- `run_guest_override_equivalence.py` doesn't pass `--extra-flags` to the link (ASan/UBSan don't link in) and has no
  qemu-aarch64 mode.
- The guest profiler's text range is hardcoded for OoT3D (`0x00100000`, size
  `0x003CA000` in `oot3d_native_a32_window_setup.cpp`); for another game, read it from the ExHeader.
- The `sdmc:/switch/oot3dre/` path is scattered across the runtime; a per-game constant is missing.
- Ryzen scripts (`ryzen-build.sh`, `rt-build.sh`, `lean-use.sh`) outside the repo.
- No script to find the NRO base in crash reports (`resolve_whole_aot_minidump.py` is for
  Windows).
- Isolated gains on the console (A/B at the same clock) still **to be measured** for: native VFP, lean mode,
  budget 2048, each PGO collection, the 23 replacements, `gl_gpu_lean`.
- `OOT3D_SWITCH_WHOLE_AOT_PORT.md` still describes the 128-block budget and presentation without
  interpolation; the current product uses 2048 and has `config/fps60`.
- Function discovery for a new game (integration with 3dsrecomp) hasn't been done yet: steps
  2–3 of the checklist are the biggest risk.
