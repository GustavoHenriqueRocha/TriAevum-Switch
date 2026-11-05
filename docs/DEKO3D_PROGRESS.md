Português (original): [DEKO3D_PROGRESS.pt-BR.md](DEKO3D_PROGRESS.pt-BR.md)

# deko3d backend — progress and how to resume

Branch `deko3d` (worktree `~/projetos/TriAevum/.claude/worktrees/deko3d`, remote `gustavo`).
Do not merge into `topscreen-3.0` or send to the Switch before validating on the console.

## Goal

Replace the render thread's OpenGL/Mesa (nouveau) with **deko3d** (the Switch GPU's native
API), behind an option: `sdmc:/switch/oot3dre/config/renderer` with `deko3d` turns it on; a missing
file or `gl` keeps today's GL (the fallback always stays compiled and working).

Why (A/B tour26 × tour27, `config/early_frame_slot`): logic stutters dropped 201→115,
but render stutters rose 174→255. The bottleneck is the render thread (`pica_submit` 3–4 ms/frame
in Mesa). Target: no frame > 16.7 ms in normal play.

## Package status

| # | Package | Status |
|---|---|---|
| 1 | Map of the PICA→GL interface and design of the abstraction | **done** (this file) |
| 2 | deko3d init + swapchain + clear/presentation behind `config/renderer` | **code ready and compiled** (NRO `deko3d-p2`); the test on Eden is missing (the Ryzen went down during it) |
| 3 | Shaders: GLSL→DKSH (uam), disk cache, offline precompilation | **done and compiled** (NRO `deko3d-p3`): `deko3d.pack` package generated (272 from the GL cache + 4 uber bases, 0 failures), `ShaderStore`, program per draw (specialized > uber > skip), hybrid enabled |
| 4 | Vertex/uniform rings, textures (upload, formats), samplers | **part 1 in the code** (compiles; syntax checked with devkitA64): RGBA8/LUT textures/samplers/descriptors/eviction and per-draw streams; missing: persistent geometry, ETC1→ETC2, Shadow2D, and measuring on the console |
| 5 | Pipeline state, targets, display transfer, DRS, widescreen | **code ready** (syntax and host tests ok; build on the Ryzen: see below); DRS and post-processing come later |
| 6 | TopScreen UI (UiRenderBackend) and ImGui | **p6 in the code** (devkitA64 syntax and host tests ok): sampler contract, RGBA8 textures, 2 DKSH programs, ImGui (FPS counter) |
| 7 | Counters in perf.txt equivalent to GL's (A/B) | to do |
| 8 | Stability/memory | to do |

**Console test (10/24, real Switch, NRO p3 md5 `6e31a96d`, `deko3d.pack` in `shadercache/`,
`config/renderer`=deko3d):** black screen as expected; `logs/deko3d.txt`: `ready` (3 images of
3840 KiB, 2 frames in flight), `276 precompiled stages in the pack`, frames 1 and 60 presented
(acquire 48.8 ms summed over 60 frames, 0 overflows); no new error in the game log. Packages 2 and 3
validated on the console.

**NRO p5 (10/24):** `~/oot3d-work/oot3d_native_game.deko3d-p5.nro`, md5
`411e4d9f6018d8838e77a530ff637a4f` (147 093 055 bytes), compiled on the Ryzen from commit `66f9957e`
(11 steps, `-j 3`, `nice 10`, no shard). Still **not tested on the console**.

**How to test p5 on the Switch:** `config/renderer` = `deko3d` and `shadercache/deko3d.pack` (the p3 one,
`~/oot3d-work/deko3d-p3.pack`). What should appear: the game scene and the menus drawn by the PICA
(top screen; whatever the game puts on the bottom one too, if presented), without the TopScreen HUD/UI or the
FPS counter/ImGui (package 6), without projected shadows (Shadow2D) and without FXAA/SMAA/EASU. In
`logs/deko3d.txt`: `targets: ...` with the created targets, `pica: scanout shaders ready`, and every
1800 frames `pica (frame): draws issued=...` (high issued, `transfers`/`scanouts` > 0).
Quick diagnosis without a rebuild: inside-out geometry (walls vanish, the interior shows) →
`config/deko3d_cull_invert` with `1`; upside-down/mirrored image → note it (it's the scanout:
`tools/oot3d/deko3d_scanout/scanout_vsh.glsl`); black screen with `issued` > 0 and `scanouts` > 0 →
look at `deko3d debug:` and `queue in error state` in the log.

**p5 console test (10/24): black screen.** `deko3d.txt` only with `ready`, `276 stages`,
`programs: hybrid` and frames 1/60; `boot-status.txt` with `host_frames=1 guest_frames=1`. Analysis:
- `boot-status.txt` was only rewritten on frame 1 or when the geometry cache appears (persistent
  counters > 0) — deko3d doesn't use that cache, so the file **froze at 1/1** even with the
  game running. It doesn't prove the guest hung. Now also every 600 frames (guard).
- p5 writes `resources: …` and `targets: resolution scale …` **inside `Init`, before the first
  frame** (without that there's no `frame 1 presented`). The log doesn't have those lines: that log is from a binary
  without package 4/5 (p3), or from an SD where the NRO wasn't replaced. I found no path in the code
  where p5 presents frames without writing them.
- **p5b:** label `backend p5b (built …)` on the log's 2nd line; first call of each kind that reaches
  the backend (`pica: first draw / first transfer / first presented transfer / first memory fill /
  first render-target clear / first completion queued / taken`); counters `pica calls`/`pica` already at
  frame 300 (and every 1800); frames 300 and 1800 also logged. Textures pre-decoded by the worker
  also in deko3d (before only in GL). NRO `~/oot3d-work/oot3d_native_game.deko3d-p5b.nro` (md5 `34933b33aece7763bdbbf7ad0eddf41a`, 147 093 055 bytes, Ryzen build -j 3).
- When testing p5b: if the 2nd line isn't `backend p5b`, the NRO on the SD isn't the new one. If it is and there's no
  `pica: first draw`, the PICA isn't reaching the backend (look in the game log for
  `native PICA … failed`).

**p5b on the console (10/25):** Data Abort crash on the 1st draw; symbolized with the p5b ELF:
`BindPicaTarget → dkCmdBufBindRenderTargets → ImageInfo::fromImageView`. Cause: `PicaTargets::Get` built
`ColorView/DepthView` on the local copy (stack) and copied it into the map → views pointing at garbage. p5 (User Break in
`Queue::checkError`) was the same bug. **p5c** (856abdd7): images and views built on the already stored target; new guard.
NRO `~/oot3d-work/oot3d_native_game.deko3d-p5c.nro` (md5 `4bed0f7bd238baa2299663c44d7ccae8`) + `.elf`, Ryzen build
-j 3 (16 steps, no shard). On the Switch as `oot3d_deko3d_p5c.nro` (menu "OoT3D [deko3d-p5c]", via
`tools/oot3d/rotular_nro.py`). Crash report: `atmosphere/crash_reports/*.log` + addr2line on the exact ELF.

**p5c on the console (10/25):** title screen with an image (116 thousand draws, no crash). Defects: stretched/striped geometry
(field edge, trees) and a dark/reddish "ZELDA" logo (to investigate). Entering the game closed it with
`integrated N64 UI presentation failed: renderer does not support the native UI sampler contract`.

### Package 6 (p6, 10/25)

- `gfx_deko3d_ui.cpp`: `SetNativeSamplerParameters` (the contract that closed the game), UI RGBA8 textures
  (`NewTexture/SelectTexture/UploadTexture/DeleteTexture`; own staging and memory retired after the frames in
  flight), `SetViewport/SetScissor/SetUseAlpha/SetOot3dNativeTransform` and `DrawTriangles` on the window with the 2 UI
  programs (solid = shade; textured = TEXEL0 × shade), classified by layout (8 or 10 floats per vertex).
- Shaders in `tools/oot3d/deko3d_ui/*.glsl` → `gfx_deko3d_ui_dksh.h` (`gen.py`, uam). Row-major matrix: `pos * M`.
- Pure conversions in `gfx_deko3d_ui_plan.h` (GL rect → origin at top, y negated on the window, ImGui → GL clip,
  samplers), tested in `TestUiPlan`.
- ImGui: `Fast3dGui::ImGuiRenderDrawData` sends a copy to `Deko3dRenderImGuiDrawData` on the render thread;
  only the font atlas (TexID 0) is sampled → the `config/fps_counter` shows up. Images from other ImGui textures
  are skipped and counted. Input: the controller (`FeedDeko3dGamepad`) and, since the touch work, the
  touchscreen as a touch-screen mouse (`FeedHostMenuTouch` in `oot3d_native_a32_window_input.cpp`).
- Log: `ui (...)` line with draws, textures and ImGui commands.

**p6 on the console (10/25):** got into gameplay, HUD and FPS counter show (56–60 fps), but the UI was upside
down and the scene rotated 180°; the scene "smeared" (trails: trees in slices, title logo over the game) and the sky
didn't appear. Cause: deko3d's clip has y up (GL) and OriginUpperLeft puts row 0 at the top → NDC −1 lands on the
**last** row, the opposite of the GL memory the whole backend assumes. **p6b:** viewport with negative height in
all draws (`MakeViewport`/`FlipViewportRows`, `config/deko3d_flip_y` "0" disables it). That also mirrors the
winding on the window: the sky (dome seen from inside) was discarded and, since OoT3D doesn't clear the top scene's color
(the sky covers everything), trails were left. If the sky is still missing: `config/deko3d_cull_invert` (A/B without a rebuild).

**p6b on the console (10/25): black screen.** deko3d 0.5.0 (`dkCmdBufSetViewports`) derives the hardware clip
rectangle from `ceil(y+h)-y`: with a negative height it becomes garbage and everything is clipped (log without errors, draws issued). In the
source: `viewportFlipY()` always inverts y in UpperLeft mode; master has `DkDeviceFlags_YAxisPointsDown`, which
0.5.0 doesn't. **p6c:** normal viewport + `DkSwizzle_NegativeY` swizzle (`ApplyDkViewport`), which negates y in clip space
before the viewport.

**p6c on the console (10/25): correct image and the game runs** (orientation, sky and no trails). Crash when leaving
Link's house: "User Break" in `Device::Submit` → `dk::detail::RaiseError` (GPU queue in error from a command recorded earlier;
the release lib doesn't say which). **p6d:** links `libdeko3dd` (`TRIAEVUM_DEKO3D_DEBUG`, on for now; turn off
to measure performance): the debug callback writes to `logs/deko3d.txt` the cause (page fault + address, method
error, timeout) and the invalid calls.

**p6d on the console:** `PbdmaError` (info32 32, "GPU rejected command list") when leaving Link's house. **p6e:**
`config/deko3d_sync_check` (each operation is submitted and waited on; the log names the first one that brings the queue down) and the
`dkperf` line (GPU ms/frame via timestamps) in `perf.txt`, read by `tools/oot3d/compare_backends.py` (Mesa ×
deko3d). p6e: the queue goes down on a 6-vertex draw with prepare ok. **p6f** dumped the draw state: all valid
(float attributes, uniforms 1792/6144 B, common blend). **Cause:** the package has two lighting fragment shaders
with 105 GPRs and **4096 B of scratch per warp**, and the queue had deko3d's default (2048 B).
**p6g:** `perWarpScratchMemorySize = kPerWarpScratchBytes` (8 KiB) and `ShaderStore` refuses (with a log) a shader that
asks for more (`DkshPerWarpScratchBytes`; test `TestDkshScratch`, 2 guards).

**p6g didn't fix it** (deko3d's graphics bind doesn't even use `per_warp_scratch_sz`; the 8 KiB scratch stays, harmless).
p6h (also waiting inside the draw) didn't crash; p6i/p6j bisected via `config/deko3d_sync_check` and
`config/deko3d_barrier` (no build). **Cause (p6j + `l2`):** "GPU method error" in `VertexArrayLimit[14]` (method
0x7dc) with value `0x20060781`, which is an `IndependentBlend` header: the list was overwritten. deko3d 0.5.0's
`dkCmdBufBindVtxBuffers` does `reserve(5*n)` and writes 6 words per buffer, and a user `DkCmdBuf` has no
reserved words: our 16 buffers near the end of a 64 KiB block wrote up to 16
words at the start of the next block, which the following commands overwrote. **p6k:** `kCommandGuardBytes`
(4 KiB) free after each chunk of command memory handed out (`TestCommandGuard` + guard).

**Next step:** confirm on the console that p6k leaves Link's house; then package 6 (TopScreen UI + ImGui in deko3d), DRS,
Shadow2D (programs with an image in the package) and measure `pica_submit` against GL (package 7).

### Package 5 (what went in, 10/24, commit `66f9957e`)

- `gfx_deko3d_pica_pipeline.h` (pure, host): PICA enums → deko3d values (compare, stencil,
  **logic op** — the PICA order differs —, blend, primitive), cull plan, target key/extent
  (scale `config/resolution_scale(_docked)` like GL, widescreen widens only the top target
  480×400), viewport/scissor with the `PicaGlScale` edges, fill decoding, display
  transfer plan (same math and flip as GL, no DRS) and scanout position on the window.
- `gfx_deko3d_pica_targets.{h,cpp}`: target = one memblock with RGBA8 color (`UsageRender|Usage2DEngine`)
  + Z24S8 (`HwCompression`), cleared on creation (color 0, depth 1, stencil 0) like GL; RGBA8 display
  image with its own descriptor slot; an image replaced due to size is only destroyed 2 frames later.
- `gfx_deko3d_pica_draw.cpp`: `SubmitPicaDraw` (same rejections as GL; pending fills; Shadow2D
  producer skipped and counted; no-effect draw skipped; shaders, vertex UBO 0 / fragment 4, textures
  1..3 + LUT 13, vertices with the constant (0,0,0,1) in slot 15, blend/logic op/mask/depth/stencil/
  cull, no depth clamp, viewport/scissor and widescreen placement ported from GL without the corners),
  `SubmitPicaDisplayTransfer` (2D blit with barriers, repeated identical copy skipped like `gl_lean`),
  scanout (embedded DKSH, `tools/oot3d/deko3d_scanout/`; v inverted for top origin; window
  clear outside the viewport; AlphaOverlay with blend), `ClearPicaRenderTarget`, `SubmitPicaMemoryFill`,
  `ResetPicaState`. New log in `logs/deko3d.txt`: `targets: ...` (first 32), `pica (frame|shutdown):
  draws issued/skipped/shadow_producers/no_effect, transfers, scanouts, fills, clears, targets MiB`.
- **Package 4 bug found:** the constant attribute (0,0,0,1) was never allocated (`mConstantGpu`
  invalid) — every missing attribute would read address 0. Now in its own `CpuUncached` block (guard).
- `oot3d_native_a32_window_setup.cpp`: deko3d's widescreen projection follows the GL rule (before, it
  widened the frustum to 16:9 like Vulkan).
- Decisions/fallbacks: **ETC1 → decoded RGBA8** (direct ETC2 comes later; the draw doesn't
  need it); **Shadow2D**: producer skipped (no program in the package uses an image), consumer reads
  0xFFFFFFFF (no shadow); **DRS off**; **no FXAA/SMAA/EASU/RCAS**; **without the widescreen menu
  corners** and without the nearest sampling of the relocated menu; TopScreen UI/ImGui = package 6.
- Switches without a rebuild: `config/deko3d_cull_invert` (=1 inverts the face winding if geometry comes out
  inside-out), `config/gl_lean` (=0 recopies the display even when identical).
- Tests: `TestPipelineEnums`, `TestCullPlan`, `TestTargetsAndRects`, `TestFills`,
  `TestTransferAndScanout` + 6 guards (83/83 ok).

### Package 3 (what went in, 10/24)

- `gfx_deko3d_programs.h` (pure): `PickPicaProgram` (specialized > uber > skip), counters, and
  `PicaStageCache` (stage resolved once per canonical source identity; absence remembered).
- `GfxRenderingAPIDeko3d` implements `PicaProgramPreparationBackend` with
  `UsesAlwaysAvailablePrograms() = true` (the plan sends the parametric program on every draw);
  `PrepareNativePicaPrograms` only registers (nothing to compile). `SubmitPicaDraw` translates to GL 4.3
  (same function as GL) only the first time for each text and looks it up in the package; log
  `programs (frame|shutdown)` every 1800 frames.
- Package: `tools/oot3d/deko3d_shader_pack.py --src <shadercache> --glsl <bases> -o deko3d.pack`;
  uber bases with `oot3d_deko3d_parametric_glsl_dump <dir>` (host target; without CMake, compile with
  g++ against `liboot3d_native_pica_frontend.a` + `pica_gl_shader_dialect.cpp`). Generated on the notebook:
  `~/oot3d-work/deko3d-p3.pack` (md5 `66f481372245ab4a521694e2a7f500d6`, 1.4 MiB, 1 vertex + 275
  fragment). Base keys: `a64d0c333b2643ec` (no light), `891e9717e2716c91` (light),
  `e8e1dda8208db28e` (integer texture0), `5eff4c88329bdc4b` (light + integer texture0).

### Package 4, part 1 (what went in, 10/24)

- `gfx_deko3d_pica_plan.h` (pure, host): wrap/filter/LOD like GL (`ToPicaGlWrap`,
  `PicaGlMinFilter`, BASE_LEVEL = start of the view, MAX_LEVEL limited to the chain), texture key
  equal to GL's (sampler state is part of it), RGBA8 mip plan, descriptor slots, vertex
  plan (all bindings in one frame block, aligned to 16; missing attribute = constant
  buffer (0,0,0,1) in slot 15) and UBO size (multiple of 256, up to 64 KiB).
- `gfx_deko3d_pica_resources.{h,cpp}`: descriptor arrays (4096 images, 512 samplers; image 0
  = white 1×1, 1 = R32UI 0xFFFFFFFF), texture = its own `DkMemBlock` + RGBA8 `DkImage` with mips
  (pre-decoded by the plan worker when available, otherwise `DecodePicaTextureMips`) uploaded
  via stream + `dkCmdBufCopyBufferToImage`; light LUT R32UI 256×24; eviction with the GL policy
  (`gfx_opengl_pica_residency.h`, budget `config/gl_texture_budget_mb`, 96 MiB) right after
  waiting on the slot's fence; per-draw vertices/indices/uniforms in stream memory (a uniform equal to the
  previous one in the same frame is reused). Shadow2D → fallback integer texture (counted).
- Package 4 pending items: persistent geometry (`PicaGeometryRegistry`), direct ETC1→ETC2
  (`config/etc2`), anisotropy, image sub-allocator (today one memblock per texture: measure),
  stream that grows and never shrinks (measure on the console).

### Ryzen (10/24)

- Cleanup done: the `EdenDeko3d` task no longer existed; on Eden's SD there was no `config\renderer`
  and the `.nro` was the original (same md5 as `eden_deko3d_orig.nro`, which was deleted).
- Existing build validated: NRO from `out/` with the p2 md5 (`c8d4032d…`), the day's 30 objects all
  valid ELF, `.ninja_log/.ninja_deps` intact.
- **Bug found:** the Ryzen's `/root/agentes/deko3d/build.sh` still had `ninja -j12` and
  `nice -n 5` and no refusal with Eden open (the repo copy already had `-j4`/`nice 10`).
  Replaced by the repo copy (`tools/oot3d/deko3d_ryzen_build.sh`, md5 `636b00a1…`). Check the
  md5 of both before each build.
- p3 build (17 steps, runtime only, no shard) ok: NRO md5 `6e31a96d79e229181cf7f4fd43c3edc4`.
- **The p3 NRO isn't on the notebook yet:** it stayed in `/root/agentes/deko3d/out/oot3d_native_game.nro`
  and in `C:\Users\flavin\deko3d-p3.nro` (md5 `6e31a96d…`, 147 015 231 bytes). The partial copy is
  in `~/oot3d-work/oot3d_native_game.deko3d-p3.nro.parcial` (don't use). When the Ryzen comes back:
  `scp -O ryzen:deko3d-p3.nro ~/oot3d-work/oot3d_native_game.deko3d-p3.nro` and check the md5. p3
  was compiled at commit `3b12e0e1` (package 3, without package 4 part 1).
- The Ryzen stopped responding (~13:51) in the middle of the NRO `scp` and didn't come back within 30 min (Windows OpenSSH: default `scp` gives
  "Connection closed"; `scp -O` worked until it went down). No build or Eden running at that time.
- **10/24 (p5):** `build.sh` now with `ninja -j 3` and refuses if a `ninja` is already running (md5
  `ac83157e…`, same as the repo copy). Source synced by a tar of the files changed since `3b12e0e1`
  (includes package 4 part 1, which had never gone over) with `--touch`; short scripts via `scp -O` +
  `wsl -d Ubuntu -u root -- bash /mnt/c/Users/flavin/x.sh` (`$A` variables get lost in `ssh '…'`).
- Rules: never build and Eden together; `-j 3` with `nice 10`; `.wslconfig` with `memory=6GB`; nothing
  > 5 GB; Eden only for short tests and never by an agent without being asked.

## Map of what GL does today (package 1)

Who calls the backend (`GfxRenderingAPI`, created in `Fast3dWindow::InitWindowManager`, case
`FAST3D_SDL_OPENGL`):

- **Render thread** (`NativeRenderCommandStream`): `beginRendererFrame` (frame.cpp) does
  `UpdateFramebufferParameters(0,…)`, `StartFrame`, `HasActiveFrame`, `StartDrawToFramebuffer(0)`,
  `SetClearColor`, `ClearFramebuffer`, `SetViewport/SetScissor`; then the PICA plan calls
  `PublishPicaCompositionSequence`, `SubmitPicaDraw`, `SubmitPicaMemoryFill`,
  `ClearPicaRenderTarget`, `SubmitPicaDisplayTransfer` (with `Present` it draws the scanout on the
  window), `QueuePicaCompletion/TakePicaCompletions`; the TopScreen UI (`N64UiFast3dRenderer` →
  `Fast3dUiRenderBackend`) uses `NewTexture/SelectTexture/UploadTexture/SetSamplerParameters/
  DeleteTexture`, `LookupShader/CreateAndLoadNewShader/LoadShader` (only 2 programs: solid and
  textured), `SetOot3dNativeTransform`, `DrawTriangles`; ImGui (`Fast3dGui`) draws
  last; `window.EndFrame()` → `Interpreter::EndFrame` → `rapi->EndFrame`,
  `wapi->SwapBuffersBegin` (SDL_GL_SwapWindow), `rapi->FinishRender`.
- **GL outside the backend:** `gfx_sdl2.cpp` (context/EGL/swap), `Fast3dGui.cpp` (ImGui OpenGL3),
  `oot3d_native_a32_window_setup.cpp` (context moved to the render thread, glthread.txt),
  `oot3d_native_gpu_frame_timer.h` (glQueryCounter, only with `config/gpu_timing`),
  `oot3d_native_a32_window_frame.cpp` (glFlush only with `config/gl_pacing_flush`),
  `FlushDeferredWindowClear` (acquire hook → fallback vsync clock / GPU timer).

GL PICA backend (`gfx_opengl_pica.cpp`, 4.7 thousand lines) — what needs an equivalent:

| GL | What it is | deko3d |
|---|---|---|
| `PicaGlTarget` (RGBA8 color + DS24S8 + Shadow2D R32UI image) per (namespace, color addr., depth addr., size, formats) | PICA targets at internal scale (`resolution_scale`), widescreen widens the top target's Y axis | color + depth `DkImage` in an image heap; Shadow2D deferred (none of the cache's 271 programs uses an image) |
| `PicaGlDisplay` + `glBlitFramebuffer` | display transfer (copy with 2:1 filter) | `dkCmdBufBlitImage` (2D engine, linear/nearest filter) |
| scanout + post (FXAA/SMAA/EASU/RCAS) | draws the display image on the window (portrait→landscape) | own scanout shader (post-processing comes later) |
| programs (`GetPicaGlProgram`) | canonical GLSL 4.5 source → `TranslatePicaShaderToOpenGl43` → Mesa | same translation → DKSH (uam); cache by hash of each stage's source |
| rings `UniformRing/VertexRing/IndexRing` + `PicaGeometryRegistry` (persistent geometry) | per-draw upload | per-frame ring in CPU-visible memory + persistent buffers in a heap |
| textures (`GetPicaGlTexture`, pre-decoded by the plan worker, keyed by content hash) + light LUT (R32UI 256×24) | | RGBA8 `DkImage` (upload via staging + `dkCmdBufCopyBufferToImage`), descriptors in a pool |
| state (blend, logic op, depth/stencil, cull, viewport/scissor, color mask) with cache | | `DkColorState/DkBlendState/DkDepthStencilState/DkRasterizerState/DkColorWriteState`, dynamic viewports/scissors |
| DRS (`gl_pica_dynamic_scale.h`): sub-rectangle of the top target | | same logic (arithmetic only; reuse the `PicaGlScale` helpers) |
| widescreen (`gl_widescreen_*_policy.h`) | per-draw placement | same policy (files already GL-free) |
| fills (`SubmitPicaMemoryFill`), target clear | | `dkCmdBufClearColor/ClearDepthStencil` with a whole-target scissor |
| fps60 interpolation (`interp_exact_step`, `interp_carry_unmatched`) | happens **before** the backend (visual plan) | nothing changes: the backend only receives draws |

Programs seen in the package cache (`pacote-mercado/.../shadercache/*.src`, 271 files):
**1 vertex shader** (31 KB, the game's PICA vertex program) and **271 specialized fragment shaders**
(~5 KB). Bindings: vertex UBO `binding=0`, fragment UBO `binding=4`,
`sampler2D` 1..3, `usampler2D pica_lighting_lut` 13. They use `gl_FragCoord`, `textureSize`, 46 write
`gl_FragDepth`, 41 have `discard`. None uses an image (Shadow2D).

## Decisions

- **Runtime selection, same NRO.** `WindowBackend` stays `FAST3D_SDL_OPENGL`
  (nothing changes in the rest of Ship); in `Fast3dWindow::InitWindowManager`, on the Switch with
  `config/renderer` = `deko3d`, it creates `GfxRenderingAPIDeko3d`. Without the file: today's GL.
- **No SDL window in deko3d mode.** The Switch SDL always creates an EGL surface on
  `nwindowGetDefault()` (`SWITCH_CreateWindow`), which conflicts with the deko3d swapchain.
  In deko3d mode `GfxWindowBackendSDL2` initializes SDL video (events, touch, applet)
  but creates no window or context; window size = 1280×720 (handheld) or 1920×1080
  (docked at launch), as today.
- **Coordinate conventions:** deko3d device with `DepthMinusOneToOne | OriginUpperLeft`. For
  off-screen targets this gives the same memory as GL (row 0 = y 0, NDC −1 → row 0), so
  GL's viewport/scissor/blit math holds 1:1 and the shaders are **the same GL 4.3 texts**
  that Mesa compiles (same `z_gl = 2z−w`). Only drawing to the swapchain image (row 0 = top
  of the screen) inverts Y, in a single place (scanout/UI/ImGui).
- **Shaders without stutters:** the backend implements `PicaProgramPreparationBackend` with
  `UsesAlwaysAvailablePrograms() = true`: every draw also carries the parametric program (4 bases).
  It draws with the specialized one if its DKSH is in the cache; otherwise with the parametric one, and records the
  missing source on the SD to compile later (offline with devkitPro's `uam` on the Ryzen; on the
  console with uam as a library in a worker, package 3b). Never compiles on the render thread.
- **Missing vertex attributes** read (0,0,0,1) as in GL: per-instance constant buffer.
- **Shadow2D (R32UI image)**: out of the first version; a shadow-producer draw is skipped and
  counted (`deko_skipped_shadow`).
- **No ImGui in the first version** (FPS counter, L3+R3 menu off in deko3d mode).

## Build on the Ryzen (how to resume)

- Own directory: `/root/agentes/deko3d` in WSL (`ssh ryzen`, `wsl -d Ubuntu -u root`).
- To avoid recompiling the 256 shards: a copy of the notebook's `build-switch` (blink state,
  `62a6ac47`) in `/root/agentes/deko3d/build`, source in `/root/agentes/deko3d/src`, and the build runs
  in a mount namespace (`unshare -m`) with `src` bound to `/home/gustavo/projetos/TriAevum`
  and `build` to `/home/gustavo/oot3d-work/build-switch` (same paths ⇒ same command
  lines ⇒ only the runtime recompiles). The source files keep the main checkout's mtime
  (only the changed ones become new).
- `gen/cpp-lean` also comes from the notebook (`/root/agentes/deko3d/gen`, bound to
  `/home/gustavo/oot3d-work/gen/cpp-lean`): the Ryzen's differs and made the shards recompile.
  `ninja -n` doesn't work for counting steps (the glob check always "reruns CMake"); the
  script watches the log and kills ninja if a shard starts. Target: `oot3d_native_game_nro`.
- Copy of the script in the repo: `tools/oot3d/deko3d_ryzen_build.sh`. Syncing the source: tar of the
  files that differ from `topscreen-3.0`, extracted with `--touch`.
- Before sending to the Ryzen, check the syntax on the notebook (light, one file at a time): take the
  `gfx_opengl.cpp` line from `ninja -t commands` in `~/oot3d-work/build-switch`, replace the repo path
  with the worktree's, `-O3`→`-O0`, drop `-w` and the `-MD … -o …`, and run with `-fsyntax-only` on the
  deko3d file (the `$(…)` needs `bash -c` because of the `-D` quotes).
- Copying the NRO: `wsl … cp out/oot3d_native_game.nro /mnt/c/Users/flavin/x.nro` and `scp -O ryzen:x.nro`
  (default `scp` against Windows OpenSSH closes the connection).
- The script `/root/agentes/deko3d/build.sh` writes `/root/agentes/deko3d/status.txt` and the NRO to
  `/root/agentes/deko3d/out/`; aborts if a shard starts recompiling.

## Package 2 (what went in)

- `gfx_deko3d.h` (pure selection `Deko3dRendererConfigSelects` + `Deko3dRendererSelected`),
  `gfx_deko3d_device.{h,cpp}` (device `DepthMinusOneToOne|OriginUpperLeft`, queue with zcull
  off, swapchain of 3 RGBA8 images 1280×720/1920×1080, 2 slots per frame with fence,
  command memory in 1 MiB slices + extra blocks, stream memory per slot,
  acquire deferred to the window's first use with the same hook as GL, log in
  `logs/deko3d.txt`), `gfx_deko3d.cpp`/`gfx_deko3d_api.h` (GfxRenderingAPI: window clear
  deferred, present in `FinishRender` after SDL's pacing; PICA/UI accepted and counted, nothing
  drawn yet; completions passed through as in GL).
- Hooks: `Fast3dWindow` (creates deko3d if selected; otherwise GL), `gfx_sdl2` (no SDL window),
  `Fast3dGui` (no ImGui backends), setup (doesn't move the GL context), frame (no `glFlush`),
  `gl_frame_profiler` (off). `libdeko3d` linked in the Switch CMake.
- Tests: `oot3d_deko3d_backend_tests` (selection, key and index of the shader package),
  new guards in `test_bug_guards.py`, `tests/switch/eden_deko3d.sh` (smoke test with
  `config/renderer` only during the test; screenshot).

## Credits and licenses

Design reference: the deko3d renderer of Aurora from "Alek's Ultimate NX Edition" (MIT) and
SwitchWaker (centollOS, MIT), whose platform layer comes from SwitchWakerHD (MPL-2.0). No
code has been copied so far; if any is, it goes in with the original header and license.
deko3d and uam: devkitPro (fincs), zlib.

### p6m (10/25): real GPU measurement and dynamic resolution

- p6l's `gpu_ms` (14.5) counted the `dkQueueAcquireImage` wait (the GPU idle waiting for the screen to release the
  image); the GL timer already excluded that wait, which is why Mesa gave 8.5. Now 4 stamps per frame: `busy_ms`
  (scene + final), `scene_ms`, `acquire_wait_ms`, `tail_ms`, `busy_max_ms` on the `dkperf` line; the compare uses `busy_ms`.
- The intermediate-frame governor and DRS had no GPU time in deko3d (`NativeGpuFrameTimer` is GL):
  `Deko3dTagFrame`/`Deko3dTakeGpuFrame` deliver each frame's busy time with its type (step/interp) — it also
  fills glperf's `*_gpu_ms` in deko3d.
- DRS (`config/dynamic_resolution`) in deko3d: the top-screen target draws into the sub-rectangle of the same image
  (`PicaTarget::Alloc` × `Extent`), switches only on the frame's 1st draw with a logical clear (like GL since tour19), the
  display copy keeps the allocation and replicates 2 border texels; the scanout reads only the content.

## Mesa × deko3d p6l comparison (tour27, 2026-10-25)

Same route (132 steps, 44 areas), clocks 1800/600/1600 MHz, DRS off. Mesa = the topscreen-3.0 NRO.

| metric | Mesa | deko3d | Δ |
|---|---|---|---|
| average fps | 58.89 | 59.23 | +0.6% |
| frame ms avg / max | 17.00 / 19.33 | 16.89 / 19.14 | −0.6% |
| render thread CPU | 34.3% | 20.5% | −40% |
| game thread CPU | 59.7% | 57.4% | −4% |
| present ms | 6.47 | 6.91 | +7% |
| GPU ms (different measurement) | 8.5 (glperf) | 14.5 (whole-frame timestamps) | — |

Per area: deko3d has min FPS ≥ 59.9 in 12 areas (Mesa: 3). Worst area: Mesa Water Temple 52.9; deko3d Fire Temple 52.2.
GPU ms isn't comparable: glperf sums passes, dkperf measures from the top to the end of the frame (includes waiting).

### Tour27 with p6m (10/25, DRS on)

132/132 steps, no GPU error. Truly busy GPU (`busy_ms`): **8.73 ms/frame** (Mesa 8.52; p6l showed
14.5 with the wait). The acquire wait was 5–6 ms. fps 59.20, render −40% CPU vs Mesa (same as p6l). DRS
applied (average scale 3 in 113 of 171 windows, went down to 2.25). **Caution:** 79 of 171 windows with the GPU at 900 MHz
(p6l and Mesa ran everything at 600): partial comparison. Worst area: Shadow Temple (min 47.6, max 20.99 ms).

## p6o–p6q: present wait (MQ tour, 10/25)

| build | missed/window | 1 refresh | late steps | spikes (pacer) |
|---|---|---|---|---|
| p6o | 2.34 | 98.4% | 461 | 455 (296) |
| p6p `deko3d_present_thread` | 2.19 | 98.5% | 278 | 288 (137) |
| p6q + `deko3d_swapchain_images` = 4 | **1.32** | **99.1%** | **165** | **183 (53)** |

- p6o: on pacer spikes, `dkQueuePresentImage` (nwindowQueueBuffer) held 15–21 ms.
- p6p: queue-buffer on its own thread; the wait moved to the next acquire (~26 ms).
- p6q: 4 images in the window take the screen queue off the limit; best result so far.
- Shader package v4 (305): MQ and adult without uber. Adult Water Temple still 8 missed/window (not shaders).

## p6v (10/26): spikes outside loading in the p6q tour (56)

Classification of `spikes.txt` (last session, without `ctx=load`): 29 with `render_wait_ms` 20-38 (the
game waiting on render), 12 with `dk_finish_ms` 14-18 exactly on frames 300, 3600, 5400…
(multiples of 1800), 10 from game logic, 4 from the GPU.

- **The 12 every 1800 frames were ours:** `Deko3d::Log` opened/wrote/closed `logs/deko3d.txt`
  on the render thread, and every 1800 frames the frame line and the PICA counters are emitted. Now
  `Log` only puts the line in a queue and a service thread writes it; `Device::Destroy` calls `FlushLog`.
- **The 29 render_wait ones:** the window acquire calls `WaitPresent`, which waits for the present thread to
  finish the previous frame's `nwindowQueueBuffer`; in p6u's `dkperf` almost every window has 1-8
  `queue_buffer` of 23-27 ms (and some of 36-44). Hypothesis: that block passes to render and
  from it to the game. `spikes.txt` gained `dk_wait_present_ms`, `dk_acquire_ms` and `dk_qb_ms` (at the end
  of the line) to confirm in the next tour.

### p6v tour result (10/26)

- The `dk_finish` spikes every 1800 frames are gone: the session's largest `dk_finish` was 0.26 ms. The asynchronous log solved it.
- Outside load: 27 spikes, versus 56 in p6q (guest 16, pacer 10, render 1, gpu 0).
- On render_wait spikes, `render_wait_ms` ≈ `dk_wait_present_ms` almost always, and `dk_qb_ms` stays at 24–26 ms (sometimes 35–60 ms right after load frames of 100–190 ms).
  That is: render waits on the present thread, which is stuck ~1.5 vblank inside `nwindowQueueBuffer`.
  `dkQueueAcquireImage` only takes long (18–41 ms) when there was no WaitPresent.
- Next step: render must not wait on the previous queueBuffer. Options: move the acquire to the present thread, leaving the next image ready, or drop the present when the thread is busy.

### p6w: present mailbox (`config/deko3d_present_mailbox`)

libnx lends only one window image at a time (`cur_slot`), so the next one can't be taken ahead of time.
In p6w render draws into 3 images of its own. A separate deko3d queue, on the present thread, dequeues, copies the newest frame and queues. A frame not copied in time is dropped.

| tour20 (Water/MQ) | windows | misses | misses/window | windows without misses | spikes outside load |
|---|---|---|---|---|---|
| tour20 (pgo5o, Mesa) | 166 | 151 | 0.91 | 66% | 43 |
| p6q | 168 | 177 | 1.05 | 64% | 56 |
| p6v | 175 | 257 | 1.47 | 73% | 27 |
| **p6w** | 198 | **105** | **0.53** | **78%** | **18** |

- Render's acquire fell to 0.004 ms in the worst case (in p6v it reached 61 ms) and the GPU stayed the same (13.9 ms).
- 266 frames dropped. 150 of them in a single window: the game thread was stopped 4.4 s on the IPC `sys:clk GetCurrentContext` (hang.txt, uptime 312 s), and the screen stopped with it.
- p6x: the sys-clk read moved to its own thread; the perf line uses the previous result (`clk_busy`).
- p6x: crash on the first perf. `std::thread::detach()` throws std::system_error ("Function not implemented"), because there's no pthread_detach in devkitA64's newlib. p6y: the thread is kept in an object that is never destroyed.
- p6y (clean tour20, no hang): 125 misses (0.63/window), 74% of windows without misses, 19 spikes outside load, 126 frames dropped by the mailbox (up to 7 per window; no long stall) and `clk_busy` never 1.
  Drops are also a visual imperfection (a skipped frame), and 42 of the 49 p6w windows that had drops also had misses: the drop is usually the tail of the same stutter.
