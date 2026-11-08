> Português (Brasil): [SWITCH_PORT.pt-BR.md](SWITCH_PORT.pt-BR.md)

# Ocarina of Time 3D on Nintendo Switch — port notes

This repository is a **fork** of [TriAevum / Echoes of Time](https://github.com/coccofresco/TriAevum),
by **coccofresco**: the native AOT recompilation of *The Legend of Zelda: Ocarina of Time 3D*. All of
the recompilation, runtime, renderer and tooling work belongs to the original project and to the projects
it credits (see [README.md](README.md) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).
Here are the changes made so the **Switch homebrew** (`.nro`) runs and is playable on a
real console, on the `switch-port` branch.

> **Reusable guide:** the recipe for porting other 3DS games with the same techniques (pipeline,
> native replacements, generator optimizations, runtime, measurement and checklist) is in
> [docs/PORTING_GUIDE_3DS_SWITCH.md](docs/PORTING_GUIDE_3DS_SWITCH.md).

> **No ROM, game code, C++ generated from the game, or asset is distributed here.**
> You need your own decrypted dump of the cartridge. License: GPL-3.0, like the original.

## Status (2026-10-16)

- Runs on the console (Erista, Atmosphère, launched from Sphaira in title mode), tested for hours.
- Correct game speed; 30 FPS in light scenes; heavy scenes still CPU-bound
  (the translated logic costs ~25–30 ms per step at 1785 MHz). CPU work in progress.
- Recommended in sys-clk: CPU 1785 MHz and memory 1600 MHz.

## What was fixed / added

Bugs that kept the Switch from running (they only showed up on the console; Eden hid several):

| Problem | Cause | Fix |
|---|---|---|
| Build without Vulkan broken | DLSS/NGX and iMMERSE/MXAO modules included `vulkan.h` | excluded without `THREE_DS_RECOMP_ENABLE_VULKAN` |
| Crash on startup | `-ffast-math` at link time puts `crtfastmath.o` after `crtend.o` and ld drops the `.eh_frame` terminator; every C++ exception aborted | link with `-fno-fast-math` |
| Crash on startup | devkitA64 newlib's `pthread_detach` returns `ENOSYS` | no `detach()` on Switch |
| Black screen / shader failing | parallel presentation ran GL on a thread without a context | GL context moved to the render thread (`Submit`+`Drain`) |
| Game in slow motion (½ speed) | 60 refreshes/s expected, but 1 per presentation at 30 Hz | up to 4 refreshes per presentation |
| Settings not saving | Horizon FS does not rename over an existing file | remove then rename |
| Crash with warm-up on a thread | nvc0 contexts share a push buffer without a lock | warm-up on the render thread |
| Game closed when erasing a file on the file select screen | the host's DeleteFile made the save root `sdmc:/switch/oot3dre/savedata` canonical; std::filesystem treats devoptab paths as relative and put the cwd in front, so every delete failed and the game called its fatal-error handler | delete `save root / path`, as OpenFile and CreateFile do (`oot3d_native_a32_ctr_fs_tests`) |

Performance and usability:

- **On-disk shader cache**: devkitPro Mesa rebuilt with the cache patches from centollOS's Wind Waker
  ports (`MESA_SHADER_CACHE_DIR=sdmc:/switch/oot3dre/mesacache`); already-seen shaders
  load in ~12 ms instead of 1–3 s. See `docs/OOT3D_SWITCH_WHOLE_AOT_PORT.md`.
- **Threads**: game alone on core 0; render thread, plan worker and audio with their own role;
  other threads as service (0x3B, cores 1|2), 4 MiB stack (`oot3d_switch_threads.cpp`,
  `--wrap=pthread_create`; policy in `oot3d_switch_thread_layout.h`, A/B in `config/thread_layout`).
- **Render thread** (optional): file `config/parallel_presentation` on the SD.
- **Internal resolution** (optional): file `config/resolution_scale` on the SD with `1`, `2` or `3`
  (absent = 2 on Switch, 1 on PC). PICA targets, depth/stencil, shadows and display
  transfer images become N× larger; viewport/scissor/blits scaled; GPU-only cost. 3 = 1200×720
  (1:1 fit in handheld). On desktop: `TRIAEVUM_GL_RESOLUTION_SCALE=2`.
- **GL post-processing** (optional, all off without the files; read once, GPU only), in
  `config/` on the SD (desktop: variable `TRIAEVUM_GL_<NAME>`):
  `antialiasing` = `fxaa` or `smaa` (same shaders/preset as the PC NRI path);
  `upscaler` = `fsr` (AMD FSR 1: EASU upscales the image N× to the window, RCAS sharpens);
  `sharpen` = `0.0`–`1.0` (linear RCAS strength; with `fsr` the default is `0.2`, `0` disables RCAS);
  `anisotropy` = `2`/`4`/`8`/`16` (only PICA textures with linear filter and mipmaps).
  Order: render N× → AA → EASU → RCAS → window. Top screen only (opaque presentation).
  OpenGL renderer only: deko3d (`config/renderer`) has no EASU/RCAS step, so L3+R3 > Video >
  "Upscaling" is shown disabled there with the note "no effect with the deko3d renderer"
  (`VideoSettings::UpscalerApplies`, `oot3d_video_settings_tests`).
- **16:9 widescreen** (on by default on the console): `config/widescreen` = `0` disables (back to 5:3
  with bars), `1` enables. Only the top-screen target is widened (400→427 logical; 853 px at scale 2,
  1280 at scale 3) and the horizontal FOV grows by the same ratio through the projection hook the PC
  already uses; HUD, menus and text stay in a centered 5:3 frame, as in the PC Vulkan path. Desktop GL:
  `TRIAEVUM_GL_WIDESCREEN=1`.
  - **Menus in widescreen** (`config/widescreen_menus`, on by default; `0` restores the previous
    behavior; desktop `TRIAEVUM_GL_WIDESCREEN_MENUS`): previously only the `Ui` domain (HUD, text, pause)
    went into the 5:3 frame; title screen, file select, name entry and the normal/Master
    Quest choice (`Unknown`/`Scene` domain) were stretched ~6.7%. Now they also go into the frame, via
    PICA state: a viewport that does not cover the presented width (the bottom menu that TopScreen
    relocates, Y 40..360) and an orthographic projection in c0-c3 with square pixels whose geometry
    does not cover the viewport (panels, text, logo). Full-screen fades/washes, frame-copy redraws and
    the atmosphere layer stay stretched with the scene. Perspective 3D models placed in the frame
    whose frustum the hook widened (measured anisotropy = 1/factor) get their viewport widened by the
    same factor around the center, clipped to the frame (so they are not squeezed). Policy and tests:
    `gl_widescreen_menu_policy.h`, `tests/gl_widescreen_menu_policy_tests.cpp`.
- **Touch screen**: read from HID (libnx `hidGetTouchScreenStates`), since SDL reports no fingers
  without a window (deko3d). Touch works where the mouse works on PC (title/file select screen and
  TopScreen's Items/Map/Equipment pause pages). A tap on those pages lands on the element under the
  finger: TopScreen draws the 320-wide lower-screen pages one pixel per pixel on the 400-wide top
  screen at X + 40 (the widened Items grid at X + 63, its sixth column at X + 80), and the touch
  inverts that (`oot3d_top_screen_page_layout.h`, which the page target plans and the widened Items
  draw also read; test `oot3d_native_a32_input_tests`). Until 0.1.0 the touch scaled 400 to 320
  instead, so on the left of the Items page a tap selected the item to the right (first column:
  native X 76 instead of 32). During gameplay a tap on a TopScreen HUD control
  presses the control it pictures while the finger is down: X, Y, Item I (labelled R), Item II
  (labelled L), B, and the D-pad direction of the action icons around the cross (View/Navi,
  Ocarina, boots...).
  Each finger hit-tests where it lands (several fingers at once). The first finger that lands
  outside every control aims by dragging (bow, slingshot, hookshot, first-person view), added to
  the right stick and the gyro like PC mouse aim: `aim.touch_enabled` (default on) and
  `aim.touch_degrees_per_pixel` (default 0.14: half the screen, 640 px, turns about 90°) in
  `oot3d_controls.json`, also in L3+R3 → Controls → Aiming; `aim.invert_x`/`invert_y` apply. It
  feeds the game's gyro, which the game reads only while aiming, so a drag in third-person play
  does nothing. Other fingers outside a control are ignored. Short taps are held until the game
  reads them. While the L3+R3 menu (or another ImGui window) is open, the first finger that lands
  is an ImGui touch-screen mouse: tap buttons, checkboxes, tabs and combo items, drag sliders and
  scrollbars, drag empty space to scroll; the controller keeps navigating. A tap (a finger that
  lifts within 16 px at 720p) always clicks where it landed, however short: the pressed pointer
  holds still until the finger leaves that slop, so a finger rolling as it lifts can't carry the
  release off a small button. A finger landing within 24 px of a window's title-bar X (17 px, under
  2 mm on a Lite) presses the X, which closes the menu as L3+R3 does. That finger never reaches
  the game, and fingers that landed before the menu opened stay out of it. Hit-test, fit and
  ownership: `oot3d_top_screen_hud_touch.h`, `oot3d_top_screen_hud_touch_tests.cpp`; the menu
  gesture: `oot3d_host_menu_touch.h`, `oot3d_host_menu_touch_tests.cpp`.
- **AOT generator**: hot helpers with an inline fast path and cold slow path; per-block LDM/STM;
  PC-relative literals folded; shards without `-ffast-math` (VFP exactness).
- **Block budget** 2048 (was 128) on Switch.
- **On-console profile**: `logs/perf.txt` every 150 frames (ms per phase + Mesa counters).
- **Gyro aiming**: the console's own six-axis sensor feeds the controller motion that devkitPro's
  SDL does not report (`oot3d_switch_motion.cpp`): player 1's Pro Controller, else the right Joy-Con
  of a dual pair (left if only it is connected), else Handheld (the Switch Lite). A single sideways
  Joy-Con is not supported. With `aim.source` `automatic` (the default) tilting the console aims
  in first person; `aim.invert_x`/`invert_y` and the gyro sensitivity apply as on PC, and
  "Calibrate controller motion" (L3+R3 menu, Controls) removes drift. The sensor is stopped out of
  focus and restarted on return (HOME, sleep) or when its data stalls.
  Stick, touch and gyro add up in the same aim; turning with the stick or a touch drag never
  changes how the gyro maps afterwards (it used to invert pitch after ~180° of stick/touch turn).
- **TopScreen 3.x**: 2.1.1 was removed from GameBanana; the importer now accepts the 3.0, 3.2.1 and
  3.2.2 packages, and the Switch build downloads 3.2.2 and builds the pack itself (below).
- **USA ROM** accepted in the build (the project's own canonical USA→EUR adapter).
- **Input paths** configurable via `OOT3D_OPERATIONAL_<variable>` (previously hardcoded to the author's Windows).
- Original icon for the `.nro` (`res/switch/icon.jpg`).

### Game data from the player's own dump (first launch)

No PC tools are needed for `game/`: copy a **decrypted** cartridge dump (`.3ds` or `.cci`) to
`sdmc:/switch/oot3dre/`, or `code.bin` (decompressed), `exheader.bin` and `romfs.bin` (IVFC, as
`ctr_rom.py` extracts them) to `sdmc:/switch/oot3dre/import/`, and start the game. Before anything
reads `game/`, `main()` calls `RunSwitchRomImport()` (`oot3d_switch_rom_import.cpp`, libnx console
screen), which:

- identifies the revision by the hashes of the decompressed `.code`, the ExHeader and the RomFS,
  exactly as TriAevum's recipes do. Supported: USA Rev 1 (En,Fr,Es), converted to the EUR layout
  with the project's adapter, and EUR (original and Rev 1, plus the project baseline dump). Anything
  else, encrypted dumps and `.cia` files are rejected with the list of supported revisions;
- writes `game/code.bin`, `exheader.bin`, `romfs.bin` and `oot3d_native_process_manifest.json`
  through `*.partial` files, checks the source and output hashes while streaming the RomFS (4 MB
  chunks, never the whole image in memory), re-reads the written files and checks them again, and
  only then replaces the previous files (the old manifest goes first, so an interrupted commit
  imports again on the next boot). `game/rom_import.json` records the source (name, size, time)
  so later boots skip the import; a different or re-copied dump imports again;
- asks afterwards whether to keep the dump (A, the default) or delete it (Y). Logs go to
  `logs/rom_import.txt`.

The core (`oot3d_rom_import.cpp`, standard C++ only) is a port of `ctr_rom.py`,
`input_copy_adapter.py`, `oot3d_region_assets.py` (`oot3d_usa_resources_to_eur_v2`) and
`build_process_manifest.py`. The USA code recipe is compiled in as offsets only
(`oot3d_rom_import_usa_copies.inc`, regenerated by `generate_rom_import_copies.py` from
`input_coverage/recipes/adapters/oot3d-usa-ef210566-copies.json`). Why the USA RomFS differs from
the canonical one: the output is the Level-3 view without the 4 KB IVFC superblock, `message/us` and
`misc/us` become `eu`, `us.qm` becomes `eu.qm`, the hint movies `hint182/186/188` become
`hint183/185/187`, the hash chains are rebuilt and the QM language slots 2/4/6 point at the USA
English/French/Spanish payloads; no payload changes. On the PC, `oot3d_rom_import_tests --import
<dump> <dir>` produced files byte-identical to the PC pipeline's for the USA Rev 1 cartridge in
about 9 s; on the console expect roughly 30-60 s, bound by the SD card.

### HUD textures from the TopScreen mod (first launch)

The HUD and menu art comes from "OoT3D: Single Screen Experience" by M-1
([GameBanana 695893](https://gamebanana.com/mods/695893), CC BY-NC-ND 4.0): it may be downloaded
and installed but not redistributed, so releases never contain it or anything built from it. The
console downloads it and builds `config/atlas_overrides.o3tu` itself, as upstream's Forge does on
the PC (`docs/TRIAEVUM_TOPSCREEN_INSTALLATION.md`). Right after the game data import, before the
launch arguments look for the pack, `main()` calls `RunSwitchTopScreenImport()`
(`oot3d_switch_topscreen_import.cpp`; the libnx console screen, shared with the import in
`oot3d_switch_console_screen.h`, puts stdout and stderr back on exit).

- When the pack is missing, or exists without `config/topscreen_ui_atlas_hd.png` (an install
  from before the HD UI atlas), `game/romfs.bin` exists and `config/topscreen_offer` is not
  `never`, the screen names the mod, its author, page and license and offers **A** download (56 MB) and
  start the game, **B** not now (asked again at the next start), **X** never ask again
  (`config/topscreen_offer` = `never`). For an existing install the text says the download adds the
  mod's HD HUD art; everything is rebuilt from the archive and the old files stay until the new ones
  are written. L3+R3 > Updates > HUD textures > "Offer the download at the
  next start" writes `ask` for players who skipped; it takes effect at the next start.
- A downloads `https://gamebanana.com/dl/1879915` (version 3.2.2; GameBanana redirects to its CDN)
  on a worker thread with the updater's HTTPS code (`oot3d_switch_network.{h,cpp}`: sockets and
  libcurl only while it runs, HTTPS only, certificates checked by the system), to
  `singlescreen_322.zip.partial`, with a progress bar, B to cancel and auto-sleep off. The body may
  not exceed 56,493,832 bytes and is hashed while it streams; size and SHA-256
  (`d07f5ae7…fda31aa0`) must match. The archive is then read for three entries only, the pack,
  the font coverage and the HD UI atlas are built from `game/romfs.bin`, and the download is deleted.
- An official archive the player copied to `sdmc:/switch/oot3dre/` (`singlescreen_322.zip`, 3.2.1's
  `singlescreen_321.zip` or 3.0's `singlescreen_30.zip`; the newest when there are several) is
  verified and used without network or question, and left there.
  With a pack already installed it is only used to add a missing HD UI atlas, and only a release
  that carries one (3.2.x; 3.0's is not pinned, so a 3.0 install is offered the 3.2.2 download).
- Written to `config/`, each through `<name>.partial`, read back and compared, then moved over
  the old file (removed first: Horizon cannot rename over a file): `font_coverage.zip`,
  `topscreen_import.json` (mod, author, page, license, archive/pack/font SHA-256 and the HD UI
  atlas's source and output SHA-256 and scale), `topscreen_ui_atlas_hd.png` and
  `atlas_overrides.o3tu` last, since it is what the game looks for. Log:
  `logs/topscreen_import.txt`.
- Failures leave nothing installed and the game starts as before, without the pack: no network,
  an HTTP error ("the server answered HTTP 404"), a stalled transfer (30 s below 1 KiB/s), too
  little free space, or a different file from GameBanana. If the mod is updated and the link
  serves a new archive, it is cut off at the expected length or fails the SHA-256 check ("GameBanana
  sent a different file than the 3.2.2 release this game knows"); supporting it needs a new
  release with the new pin and, if the layout changed, builder changes. The failure screen offers
  A try again, B start without the textures, X never ask again, and explains how to copy the
  archive from a PC.
- HD font: the runtime loads `font_coverage.zip` from beside the pack
  (`oot3d_native_a32_window_setup.cpp`) and uses it from 480 output pixels high, so the Switch
  (720p) uses it: the mod's 64x64 Latin `ltn16.qbf` covers the native 16x16 glyphs, paired by
  character. Its `sys8.qbf` uses an encoding the pairing does not support and is not imported.
- HD UI atlas: the archive's `SingleScreen_3.2.x/<region>/Emulator/load/textures/<title>/UI/
  tex1_512x256_1FAF9B5B2ADF6038_13_mip0.png` (Azahar's name for `custom_ui01`, 4096x2048,
  7,007,958 bytes, SHA-256 `082582da…d4fde71e`, byte-identical in the EUR, USA and JPN folders) is
  checked, decoded (8-bit RGBA PNG, own zlib decoder), reduced to 3x the native 512x256 atlas
  (1536x768: the HUD is drawn at 3x on 720p) with `azahar_texture_scale`'s area-weighted,
  alpha-aware filter and written as `config/topscreen_ui_atlas_hd.png` (about 1.3 MB). Peak memory
  of the step about 45 MB (the 32 MB decode), 1 s on the PC. The Switch launch passes
  `--topscreen-ui-atlas-hd` when the file exists beside the pack; the window setup decodes it
  (`stbi_load`) and requires a whole multiple of the pack's `oot3d/topscreen/3.0/ui_atlas`, else it
  logs `topscreen: HD UI atlas unused: ...` and keeps the native atlas. The HUD's UI texture
  provider replaces the exact native atlas bytes with it (`ChooseUiCustomTexture`).
- What draws from it: the HUD now draws its 3.x art from `custom_ui01` itself, as upstream's
  `oot3d_top_screen_mod_profile_hud.cpp` does: the four button circles and the A circle, the D-pad
  cross, the ZR/ZL and X/Y/B labels, the View eye/Navi/camera prompt, the L/R tab hints and their
  backings, the Ocarina arrows, the Items "SELECT ?" hint and the D-pad cycle marks. Before, they came
  from the painted `menu_top_parts00`/`menu_cursor00` replacements and the composed 2.1.1
  `menu_atlas`, all native size; those textures stay in the pack (the native touch copies, hearts,
  magic meter and pause pages still use `menu_top_parts00`'s native art). Without the pack's
  `ui_atlas` these elements are left out.
- Core (`oot3d_topscreen_import.{h,cpp}`, standard C++ and zlib, host-buildable): a port of the 3.x
  path of `build_topscreen_texture_override_pack.py` (ZIP reader, Level-3/IVFC RomFS reader,
  ETC1A4/RGBA4/RGBA8 codecs, hash-matched `menu_top_parts00`/`menu_cursor00` replacements, O3TU
  v2) and of `prepare_qbf_font_pack.py`. `oot3d_topscreen_import_tests --build <archive>
  <romfs.bin> <pack> <fonts>` matches both Python tools byte for byte on the real archive: canonical
  EUR RomFS, pack `6e79f87d…22ea7f` (2,949,460 bytes, also p7b's 3.0-built file) and fonts
  `f625baf7…7ed5289`; raw USA Rev 1 RomFS, the same pack and fonts `5be4041b…6d00267`. On the PC
  the build takes 0.03 s after a 0.3 s archive hash. With a sixth argument it also writes the HD UI
  atlas: 1536x768, pixels SHA-256 `f8be9f0e…f807998be`; its coverage (alpha >= 128, reduced to 1x)
  differs from the native `custom_ui01` on 16% of texels (50% upside down: the redraw has sharper
  edges and lighter shadows).
- Without the pack (B, X, or a failed download) the game runs with the game's own HUD art: the
  bridge leaves out the elements drawn from `custom_ui01` (`SetTopScreenUiAtlasAvailable`), and any
  other request for a pack-only profile texture (`oot3d/topscreen/2.1.1/*`, `oot3d/topscreen/3.0/*`)
  resolves to one transparent texel in `Oot3dNativeA32UiTextureProvider::Resolve`
  (`profile_asset_absent`). Before 0.1.1-test.2 the Items hint and the D-pad cycle badges asked
  for `menu_atlas`, the lookup failed with "OoT3D UI semantic is not a native localized texture",
  the UI frame threw and the game closed (logs7, 05/11).
- A fatal error (any exception that reaches `main`) shows the system's error dialog with the
  message and the log's path, then closes the application through AM (`__nx_applet_exit_mode = 1`,
  as Exit game does). Returning to the homebrew loader made it abort (`hbl + 0x3810`, 2001-0106).

### TopScreen 3.2.2 and the mod's HD HUD images (07/11)

- **Download.** GameBanana deleted 3.2.1 (file 1875895: `dl/1875895` now redirects to the mod page)
  when 3.2.2 replaced it, so the console download failed. The console now downloads 3.2.2
  (`https://gamebanana.com/dl/1879915`, `singlescreen_322.zip`, 56,493,832 bytes, SHA-256
  `d07f5ae7a77b83951836d80a000a9625a5e2da52ae0fa262d9b44409fda31aa0`); 3.2.1 and 3.0 zips copied to
  `switch/oot3dre/` still build (the newest is used). The 7,007,958 bytes in the 3.2.1 entry were
  never a download: they are the HD UI atlas PNG's size. Against the archived 3.2 (`dl/1873594`), 3.2.2
  only changes the six `code.ips`; `custom_ui01.ctxb`, `ltn16.qbf` and the PNGs are byte-identical.
  From the canonical EUR and raw USA Rev 1 RomFS, 3.2.2 builds pack `6e79f87d…22ea7f` (as 3.0/3.2.1)
  and HD atlas pixels `f8be9f0e…`, C++ and Python alike.
- **The mod's other HD images.** 3.2 and 3.2.2 ship three Azahar PNGs per region, all byte-identical
  across EUR/USA/JPN: `tex1_512x256_1FAF9B5B2ADF6038_13_mip0.png` (custom_ui01, already imported),
  `tex1_512x256_BB00B25B152582B6_4_mip0.png` (8,970,114 bytes, `1237d061…`) and
  `tex1_128x128_8064FDFB5065B51D_4_mip0.png` (478,849 bytes, `baf550ec…`). The names are Azahar's
  legacy hash (detiled, rows flipped) of the game's own English `menu_top_parts00.ctxb` (hearts, magic
  meter, B label and icon, touch buttons, pause pages) and of `menu_cursor00.ctxb`; TopScreen 3.x leaves
  both CTXBs unchanged, so on Azahar they replace them. The import now also extracts these two, reduces
  them to 3x (1536x768, 384x384; coverage against the RomFS textures 4.3% and 7.8% off, about 47-50%
  upside down, so not flipped) and writes `config/topscreen_hd/<same name>`; the Switch launch passes
  `--topscreen-hd-textures` beside the pack, and the HUD draws an image when Azahar's legacy hash of
  the request matches its name and the player's texture pack has no image of it (the pack wins; while
  it decodes, the image stands in). 3.2.1's images are not pinned (its archive cannot be checked).
  "HD art installed" is now the atlas plus these images (`HdArtInstalled`): older installs are offered
  the download again.
- **Identity of overridden textures.** This port's O3TU rewrites `menu_top_parts00`/`menu_cursor00`
  (the 2.1.1 sprites in their 2.1.1 rectangles), and the HUD looked texture-pack images up by the
  rewritten bytes (English `menu_top_parts00`: legacy hash `6C570A11…` instead of the game's
  `BB00B25B…`), so neither HenrikoMagnifico's nor the mod's image ever applied: hearts (lavender empty
  hearts), the B label and the cursor kept the game's 1x art. The UI texture provider now names the
  request after the game's own bytes (decoding them too for formats 5 and up), while the override's
  pixels stay the stand-in.
- What the screenshots on the mod page show: the Kokiri Forest one (blue-grey empty hearts, white
  D-pad outline) is the HD art (custom_ui01 HD atlas and the `BB00B25B` image); the Epona one (lavender
  hearts, dark D-pad outline with tick marks) is the 1x art (the native `menu_top_parts00` and the 1x
  `custom_ui01` rect 3 (385,108,29,29)). The D-pad's centre is transparent in both; its colour is the
  scene. The archive has no other art sets: only `<region>/<Emulator|3DS Hardware>`, no optional
  folders; "Project Restoration HUD style" is a layout option of the mod's code, drawn from the same
  atlas (the port offered it until hud18, see "HUD: Restoration removed, R/L labels, no press
  motion"). Item icons and rupee digits (`num_all00`, legacy hash `FABD759BB95C1C6A`) have no image in the
  mod; a texture pack that has them (HenrikoMagnifico's does) applies as before.

### HUD: Restoration removed, R/L labels, no press motion (hud18, test.10 report)

- **Restoration HUD removed.** Its layout was wrong (report). L3+R3 > HUD has no layout choice; the
  `TopScreenHudLayout` enum, `TopScreenUiConfig::HudLayout`, the ZR+X/ZR+Y chords
  (`TopScreenExtendedInputFrame::RestorationLayout`, its X/Y fields, the X/Y slot suppression), the
  Item I/II placements and counters under ZR, the I/II labels, the right-lane A button and the
  untappable Item I/II lanes are gone, and so is `config/topscreen_ui.restoration.example.json`. A
  saved `"hud_layout"` (normal or restoration) is still read and ignored, so an old file loads and
  draws the Normal HUD; the config no longer writes the key, nor does the summary JSON.
- **Labels R/L.** The Item I/II lanes said ZR/ZL, but on the Switch the items are on the physical R/L
  (ZL/ZR do the game's Z-target/shield; `ConnectedPhysicalDeviceManager.cpp`). Neither
  `custom_ui01` (HD atlas; yellow labels A, ZR, I, II, ZL, X, Y, ZL, B, START, SELECT) nor the game's
  own `menu_top_parts00` has an R or L label; the atlas's white R and L are text-font letters of
  another style. So the lanes draw the second letter of the existing labels: the R of ZR (rect 15,
  from x +7.625, 7.75 wide: the R and its outline start at HD column 61 of 128, the Z ends at 60)
  and the L of ZL (rect 14, from x +8, 6.375 wide), 16 high, at the label's 1:1 atlas scale, each
  centred where its two-letter label was (x 372 and 347.7; y 31 and 55). Same art, colour and
  outline as X/Y/B (`BuildTopScreenTouchLabelsGeometry`). Controls are not swapped.
- **Press motion.** Report: R moved the ZR circle and the Y circle, L only ZL, X/Y/B/A never. The
  2.1.1 logic lowered a lane by 2 px while its PauseTouchButton field (+0x4C X, +0x50 Y, +0x44
  Item I held, +0x48 Item II held) was set or ZR/ZL were held, and the item icons read those four
  offsets by item region (0 Item I, 1 X, 2 Y, 3 Item II) instead of by lane, so lane 2 (Item I,
  ZR) moved region 2, the Y item; ZL's lane 3 matched region 3. X/Y never moved because nothing sets
  +0x4C/+0x50 from a button. No HUD element moves on a press now: `ReadTopScreenTouchDynamicState`
  reads only the fade, and the circles, labels, icons and counters take no offsets. Not a
  Restoration leak: it was in Normal.
- **Tests:** `oot3d_top_screen_mod_profile_tests` (saved `hud_layout` loads, R/L label rects and
  centres, ZR/ZL use Item I/II alone, icons and counters stay put with the X press field set),
  `oot3d_top_screen_hud_touch_tests` (Item I/II lanes take taps, every icon in one circle, labels
  over icons), `oot3d_top_screen_item_dispatch_tests` (ZR leaves the X/Y slots native). Guards
  `hud18: …`.

### Fixes from upstream after beta.1c (07/11)

Triage of the 125 commits that upstream `coccofresco/TriAevum` `main` (`8f69a26f`, 22/10, GPL-3.0-or-later)
has after this fork's merge base with it (`23eac6b`, beta.1c r12). Ported commits are cherry-picked with
`-x` (the source sha is in each message).

**Ported**

| Upstream | Here | What |
|---|---|---|
| `657e3d0f` (#104) | `bcf1f8ad` | The process faulted shortly before the end credits: the game invalidates DSP-shared buffers with dsp::DSP command 0x14 (header `0x00140082`), which the host left unserved (`TRIAEVUM_HOST_REQUEST_UNSERVED … detail=0x140082`). FlushDataCache (0x13) and InvalidateDataCache (0x14) are now validated and acknowledged like their gsp::Gpu counterparts (guest memory is coherent on the host). Test: `oot3d_native_a32_ctr_host_tests`. |
| `034405bd` | `faf59da1` | Leaving a hint-stone vision closed the game: when the running guest thread called ExitThread and no other thread was ready, `NativeA32Process::Run` reported Terminated although the others only waited, and the window loop (`oot3d_native_a32_window.cpp`, `oot3d_native_a32_window_frame.cpp`) ended. Run reports Waiting until every thread has exited (`ResolveStoppedThreadRunKind`). Tests: `oot3d_native_a32_process_tests` (`TestStoppedThreadRunKind`, `TestThreadExitWhileOthersWait`). |
| `e6f2ff67` | `153207ec` | Diagnostics only: "renderer reached guest state while the guest was running" names the access (memory read/write, raw copy, PPF interrupt, deferred writes). |

Both fixes have guards in `tests/switch/test_bug_guards.py`.

**Already in this branch**

- `a4582abb` (one log per launch, null grass ActiveRects) as `05e09969`; `cf84b46b` (#80, SELECT opens
  Save/Quit) as `06885227`.
- `5704e15a` (#89, dungeon map floors take the D-pad and stick): fixed here on 04/11 (`311d8fd7`) the other
  way round: `PauseDungeonMap_HandleCursorInput` (0x00442F38) is not intercepted at all, so the native floor
  and item navigation always runs (guards forbid the hook). Upstream re-intercepts it only to keep B with the
  mod; nothing observed here needs that.
- `68d527c6` (#73, texture packs replace the TopScreen HUD textures): this branch's own `7f943738` (and the
  07/11 HUD identity work above) lets packs reach the HUD.

**Not ported**

- SingleScreen 3.0/3.1 source port and its follow-ups (`5acf8138`, `14edc1eb`, `1c4e9f06`, `ecda6605`,
  `898dfd11`, `379276ac`, `b0ca70f9`, `be6bf208`, `f181ec69`, `fc5ddfa6`, `5da8aa7c`, `d8ffc8f1`, `6f478bcc`,
  `ac63eb21`, `94b293ea`, `54fe8edf`, `10366406`, `46ce35d4`), Roll Dash (`4b88e124`, `a3eeb1e2`), the Navi
  hat redirect (`5b34bd61`, `3c2ed0f3`), the vision lists on the upper screen (`0046239c`) and Ocarina
  Reframed (`f7b5637f`, `96fb3438`, `98a1f30a`, `669e7f98`): large features on code this branch does not
  have (its TopScreen is the 2.1.1 port with 3.x art). The fixes in the list only repair that code. A
  port of the 3.x profile would be its own project.
- Grass default rules 26 and 27 (`186b1a00`): the Switch grass reads the same rules
  (`triaevum_product_graphics.inc`), but this adds grass on two more surfaces, so it costs GPU time on the
  console. Worth a measured round of its own.
- Fixed x2/x3 interpolation samples (0.5, 1) instead of (0, 0.5) (`830902e7`): this branch's fps60 path
  relies on alpha 0 being the exact previous game frame (`config/interp_exact_step`,
  `config/interp_carry_unmatched`), so the change needs its own design and test.
- Input-latency work for the PC's SDL/swapchain presentation (`9299fc31`, `2af66a68`), the PC window
  (`c9b6e000`), the clang 22 developer build (`d8df14f8`), diagnostics for PC investigations (`4a8709dd`),
  the file-select environment identity (`9be3444d`, `7d19ce99`), the in-game settings menu and photo mode
  (`91a06ca5`), the Link remodel overlay (`9c90abb9`).
- PC-only renderer and tooling: Vulkan/NRI and Vulkan PICA (`b830df28`, `cf2eaca3`, `7943a0a0`,
  `b43f76a2`, `e91ee5e4`, `80d9a50b`, `45d542e2`, `1c07ff0c`, `7541876c`), shadows (`3d5e2ae1`), the
  Enhanced shader corpus (`4d7febdf`, `cb8c5665`, `c958bb01`, `dace75f1`, `3d786374`, `0952b590`,
  `c2ffa2d4`, `a0b3a838`, `e2f2ffa3`, `e71efe89`, `4f7250d0`, `fbcd5c6a`, `8247e9b2`, `58815b7f`,
  `50a399d2`, `2e161cf4`), the GPU grass (`d277df25`, `cc4ebfe7`, `72514cea`, `da1177dc`, `eb5857b8`,
  `589de532`, `74ef7bdc`, `8e84f519`, `5a4bdd4d`, `fd3a3398`, `c34dd7b0`, `14ed0534`, `8d988c02`,
  `eb4a70c1`, `0bdc0f9d`, `3948b846`, `940bc7eb`, `93c2e14d`, `33c9e31d`, `c5707462`, `92412701`,
  `680d148c`, `4963cc1e`, `2ebf9eec`, `28b21c0b`; the Switch grass is its own deko3d path), reflections and
  MXAO (`b20ff67e`, `ef7dbb28`).
- Windows, Forge, release, AppImage, docs and licence files (`eb1aef24`, `0947435e`, `f048fc2a`,
  `f637990e`, `5701149a`, `8ab04c75`, `d67c7e7b`, `d78398ed`, `da5bec5d`, `7655c7b4`, `399ef12e`,
  `32855408`, `a4b881da`, `1ec489b8`, `c291e707`, `17218b0c`, `0d1c5637`, `a56f9649`, `f3524220`,
  `13a95dcc`, `16befec8`, `e53ac808`, `a741eda6`, `6fed40ae`, `48590372`, `8f69a26f`) and the merge
  `b9f248bb`.

## How to build (Linux)

1. devkitPro (`switch-dev switch-sdl2 switch-mesa switch-glad switch-zlib`), `cmake`, Python 3 with `capstone`.
2. Extract the ROM with `tools/triaevum_release/ctr_rom.py` and, if it is USA, apply the adapter
   (`input_adapters.adapt_extracted_inputs` with the recipe `oot3d-usa-input-canonical-eur-ef210566`).
3. Generate the program and the C++:
   ```sh
   export OOT3D_OPERATIONAL_oot3dExtractRoot=/path/extract   # exefs/code.bin, ncch0/exheader.bin
   cd tools/oot3d/native_a32_runtime
   python generate_whole_aot.py --output $W/gen/aot_program.json --inventory-output $W/gen/inventory.csv
   python generate_whole_aot_cpp.py --program $W/gen/aot_program.json --selection whole_aot_functions.json \
     --code $OOT3D_OPERATIONAL_oot3dExtractRoot/exefs/code.bin --output $W/gen/cpp --shards 256 --shard-strategy affinity
   ```
4. Configure and build (~1h30 on 4 cores):
   ```sh
   cmake -S . -B build-switch -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake \
     -DCMAKE_BUILD_TYPE=Release -DOOT3D_ENABLE_VULKAN_RENDERER=OFF \
     -DOOT3D_WHOLE_AOT_PRODUCT_MODE=ON -DOOT3D_REQUIRE_WHOLE_AOT=ON -DOOT3D_REBUILD_WHOLE_AOT=ON \
     -DOOT3D_REGENERATE_WHOLE_AOT=OFF -DOOT3D_WHOLE_AOT_GENERATED_DIR=$W/gen/cpp \
     -DOOT3D_WHOLE_AOT_PROGRAM=$W/gen/aot_program.json \
     -DOOT3D_WHOLE_AOT_CODE_BIN=... -DOOT3D_WHOLE_AOT_EXHEADER=...
   cmake --build build-switch --target oot3d_native_game_nro
   ```
   (For Mesa with the cache, see the corresponding section in `docs/OOT3D_SWITCH_WHOLE_AOT_PORT.md`.)
5. Stage the SD with `scripts/switch/stage-product.sh` and copy `switch/oot3dre/` to the card.
   Launch from the Homebrew Menu **in title mode** (holding R on a game), not from the album.

### PGO: collecting a profile on the console

GCC profile-guided optimization of the 256 whole-AOT shards (`OOT3D_WHOLE_AOT_PROFILE_MODE`,
`CMake/oot3d/native_runtime.cmake`), following nfsmw-nx and sa2-nx (sa2-nx measured ~15% from one
session). The workshop wrapper is `~/workshop-OOT/scripts/build-fork.sh` (env `PGO`, `PGO_PROFILE`,
`BUILD_DIR`); with plain CMake, add the `-D` options it passes (below).

**How the counts are kept.** The instrumented shards are compiled with
`-fprofile-generate=sdmc:/switch/oot3dre/pgo -fprofile-prefix-path=<build dir> -fprofile-update=single
-fno-profile-values -fprofile-info-section=gcov_info`. libgcov writes nothing by itself:
`oot3d_whole_aot_pgo.cpp` streams the whole session (the counts only grow) with
`__gcov_info_to_gcda` into `sdmc:/switch/oot3dre/pgo/oot3d_pgo_<N>.stream`, a new `<N>` per session,
every 3 minutes, when the game loses focus (HOME, sleep) and at exit. The writer is a service
thread (priority 0x3B, cores 1-2), never the game thread. Each dump goes to `.stream.tmp` and
replaces the stream only when complete, so closing the game mid-dump keeps the previous one. Every
dump appends `oot3d_pgo_<N>.stream <why> bytes= ms=` to `pgo/dumps.txt`, and the game log says where
the counts go (`pgo: instrumented build, <n> objects streamed to …`, one object per shard). `-fno-profile-values` keeps
libgcov's value profilers out (shared lists that need atomics; the indirect-call one read
`tpidr_el0`).

**Indirect call census (since 11/08).** The instrumented build also counts every whole-AOT
indirect call (`BLX reg`, computed `ADD pc` calls) per return address and target for the whole
session (`oot3d_native_indirect_profile.h`, installed by `Oot3dWholeAotPgoStart` before guest code
runs; lock-free, both guest cores record). Each dump writes it beside the stream as
`sdmc:/switch/oot3dre/pgo/oot3d_pgo_<N>.indirect.txt` (same session number, same `.tmp` → remove →
rename steps; `dumps.txt` lines gain `indirect_bytes=`): a `# oot3d indirect census v1` header with
`calls= pairs= dropped= threads=`, then one `return target count` row per pair (hex, hex, decimal),
most calls first. `return` is the guest LR at the call (call pc + 4 for `BLX`, the resume pc for
computed calls). It costs ~1.5 MiB plus 512 KiB per recording thread. The census is data for a
later round of guarded direct calls (N2); this round promotes nothing, so the generate and use
builds compile the same dispatcher.

**Size and cost (measured on shard 064, devkitA64 GCC 16.1).** 35 functions, 342 KB of code
without instrumentation: 570 KB instrumented (x1.67) and 90.8 KB of counters (0.27 byte per byte of
code). Over the 133.8 MB `.text`: about 36 MB of counters at most (`.bss`, RAM) and a `.text` of about
220 MB (the linker adds branch veneers past 128 MB; the 2 MB-page copy then takes ~230 MB of the
~3 GB heap). A stream holds every counter of the functions that ran and 8 bytes for each untouched
counter array, so one session file is at most ~36 MB (one file per session, rewritten in place);
at 15-25 MB/s that is 1.5-2.5 s of SD writing per dump on the service thread [estimate: check
`ms=` in `dumps.txt`]. The game thread does not wait for it; other SD reads in that window can.
The instrumented game runs slower (every branch counts): play it for coverage, not for frame rate.

**Steps for the integrator.**
1. Instrumented build, in its own build directory (the product's `build-switch-deko3d` keeps its
   objects):
   ```sh
   PGO=generate scripts/build-fork.sh        # ~/workshop-OOT; BUILD_DIR defaults to build-switch-pgo-gen
   ```
   It compiles the generated C++ from `aot-cache/fork-gen-<tree>-pgo/cpp` (a copy of the normal
   generation; the `.gcda` files are named after that path, so the use build must compile the same
   path) with `-DOOT3D_WHOLE_AOT_PROFILE_MODE=generate`; no `CMAKE_ARGS` needed. All 256 shards
   compile instrumented (more memory per shard: keep `AOT_JOBS` at 4-6). Install
   `build-switch-pgo-gen/oot3d_native_game.nro` in place of the normal NRO (same
   `switch/oot3dre/`, so the game data and settings are used); keep the release NRO to put back.
   A round with extra generator flags (11/08: `--sparse-block-entries --fold-vfp-literals`, see
   "Generator round of 11/08") passes them to both generations through the wrapper's
   `AOT_GEN_FLAGS`; the generate and the use build must get the same value (the flags are part of
   the cache key, so the use build would otherwise look for another `-pgo` path).
2. Play 20-30 minutes, in one or several sessions (each makes its own stream; they add up), with the
   default settings (no `config/fps60`): title and file select, Kokiri Forest, Hyrule Field by day and
   night, Market/Kakariko, a dungeon with fights (the Forest Temple Wolfos, the slowest spot of the
   logs), one boss, the pause menu (items, map, quest), an ocarina song, swimming and a cutscene. End
   with Exit game (L3+R3) for the final dump; after a HOME press wait ~5 s before closing (the focus
   loss dump; `dumps.txt` shows it). Profile only the build that will be optimized: a regenerated
   shard or an edited runtime header loses the profile of the functions it changes.
3. Bring the counts back: copy `sdmc:/switch/oot3dre/pgo/` (FTP or USB) to
   `~/workshop-OOT/pgo/streams/`, then merge them into `.gcda` files with devkitA64's gcov-tool:
   ```sh
   docker run --rm --name pgo-merge -v $HOME:$HOME -w $HOME/triaevum-switch/tools/oot3d/native_a32_runtime \
     retuned-switch-prep:local python3.12 whole_aot_pgo_merge.py \
     --streams ~/workshop-OOT/pgo/streams --output ~/workshop-OOT/pgo/profile
   ```
   (`gcov-tool merge-stream` per session, then each file's summary gets runs = sessions and
   sum_max = the largest count: the stream API writes zeros, which would make every block hot.) It
   refuses an `--output` that already has `.gcda` files (merging adds); lone `.stream.tmp` files are
   skipped. Then empty `pgo/` on the SD card. Keep `~/workshop-OOT/pgo/profile` with the release.
   The `.indirect.txt` files in the same folder (ignored by the merge) are added up with
   ```sh
   python3 tools/oot3d/native_a32_runtime/whole_aot_indirect_census.py ~/workshop-OOT/pgo/streams \
     --program ~/workshop-OOT/aot-cache/fork-gen-<key>/aot_program.json \
     --output ~/workshop-OOT/pgo/indirect_census.json
   ```
   which prints the calls, the share of calls from single-target sites, the share the 1/2/3
   hottest targets of each site cover and the hottest sites (N2 candidates), and writes every site
   with its targets as JSON.
4. Optimized build:
   ```sh
   PGO=use PGO_PROFILE=~/workshop-OOT/pgo/profile scripts/build-fork.sh   # BUILD_DIR defaults to build-switch-deko3d
   ```
   It lists the functions the collection ran (`whole_aot_hot_from_gcda.py`), regenerates the `-pgo`
   C++ in place with `--hot-functions` (the giant functions that ran keep sched1/gcse-after-reload;
   the attribute changes no profile checksum) and configures `-DOOT3D_WHOLE_AOT_PROFILE_MODE=use
   -DOOT3D_WHOLE_AOT_PROFILE_FILE=<profile> -DOOT3D_WHOLE_AOT_HOT_FUNCTIONS=<list>`: the shards get
   `-fprofile-use -fprofile-partial-training -fprofile-correction -fno-profile-values`
   (`-fprofile-correction`: counters updated without atomics by several threads, and read while
   running, are not flow-consistent) and, since test.8, `--param=max-inline-insns-auto=64
   --param=inline-unit-growth=12`: recompiled code marks no function inline, so GCC's default limit
   of 30 decided every call; sa2-nx measured 41 % fewer executed calls at +2 % size with these
   values. Every Switch build also compiles the shards with `-fomit-frame-pointer` (devkitA64's GCC
   sets up x29 in every non-leaf function by default). Neither changes a profile checksum: inlining
   and the frame layout come after the profile is read. With plain CMake and an empty `OOT3D_WHOLE_AOT_HOT_FUNCTIONS`,
   the configure derives the list itself (`<build>/whole_aot_hot_from_gcda.txt`) and warns when
   pre-generated C++ was not generated with it. All 256 shards rebuild (new generated path). A plain
   `scripts/build-fork.sh` afterwards goes back to `PROFILE_MODE=none` and the normal C++.
5. Measure: the same spots and save as a 0.1.1 session (Forest Temple Wolfos, Hyrule Field);
   compare `guest` and `game_pair_ms` per `perf.txt` window.

## On-console diagnostics

- `sdmc:/switch/oot3dre/boot-status.txt` — startup stage and readable error.
- `logs/perf.txt` — FPS and ms per phase, the area of each window and, with `config/actor_profile`,
  the cost of the actors (format below, "Area ids and actor costs"); `logs/hitches.txt` — one line
  per late presentation, with its area; `logs/oot3d_native_game.log` (with a guest fault report,
  below, "Guest fault report"); `logs/glthread.txt`.
- Crashes: `atmosphere/crash_reports/` (the `.nro` is loaded by hbloader; the base can be
  found by aligning the return addresses with `bl/blr` instructions in the ELF).
- Hangs (frozen image, no crash report): `logs/hang.txt`, from the hang watchdog
  (below). Disable with `config/no_hang_watchdog`.

### Freeze at the Normal/Master Quest choice and hang watchdog (2026-10-17)
Symptom (integ5, `fps60` legacy, `parallel_presentation`, `config/heap_check_windows=1`): ~5 s
after opening, the image froze on the animating cards; no crash report and no error in the log.
`switch-logs/crash1/perf.txt`, session 68: windows 150 and 300 and nothing more.

**Where the game thread stopped (probable, from file evidence).** The last line (window
300) ends mid-way, at `... present_4p=0 hold_`, without `\n`. The line is built by a
`std::ofstream` opened and closed every window; the libstdc++ buffer on devkitA64 is `BUFSIZ` =
1024 bytes and, when a string does not fit, `xsputn` writes buffer + string at once: the file
only receives the line in ~1 KiB chunks and, if the window finishes, the destructor closes and writes the rest.
The 2060 bytes written are two such chunks; the third would come out around byte ~3083, which in a
complete line falls inside `mesa:`. So the game thread entered the window-300 block and never
reached `close`: it stopped between `hold_` and the end of `mesa:`. In that stretch there is no wait on the render
thread, the pacer or the stream (`GetWaitStats` and the `interpolationPerf` mutex come before
`hold_`); sys-clk had already answered (`clocks: ... clk_ipc_us=16` is in the line); the backend and Mesa
statistics (`mesa_switch_get_stats`, `cache_status`) only read atomics/a static vector.
What remains: the **`mallinfo()`** from `config/heap_check_windows=1` (every window) and the write of the
chunk to the SD. newlib's `mallinfo()` (`__malloc_update_mallinfo`, checked in the disassembly of
`libc.a`) holds the global malloc lock and walks each bin via `bk` until it returns to the header, with no
limit or check: with a corrupted free list (a double free leaves `p->bk == p`; a
use-after-free writes into `fd/bk`) the loop never ends, the lock stays held and any thread that
allocates stops — the whole process freezes without a crash. Heap corruption had already been found by
`mallinfo` (crash at the 21st window, `interp_scale=1.5`, interpolation section), and the config was
enabled precisely to hunt the next one. Probable cause: **heap corruption (origin still
unknown) + `mallinfo` in an infinite loop holding the malloc lock**; not proven (the thread's PC is
missing), and the SD write is not ruled out. The watchdog below answers this next time.
Three other sessions in the same file also end mid-line (3150, 13650, 34950;
34950 also at `hold_`): they may be HOME exits during the block, nothing can be concluded from them.

**Hypotheses revisited and ruled out for this case:**
- *Lost wakeup in `NativeRenderCommandStream`* (signal only when someone is waiting, 9b45d8e6): all
  predicate state and the waiter counters (`mWorkerWaiting`, `mProgressWaiters`) change
  under `mMutex`; the waiter increments the counter under the lock before `wait`, which releases the lock
  atomically; the signaler tests the counter under the same lock after changing the state. Either the
  signaler sees the counter > 0 and signals, or it changed the state first and the predicate (tested under the
  lock before sleeping) already sees it. `BeginShutdown` changes `mShutdown` under the lock and always signals both
  condvars. There is no lost signal. Stress proof:
  `oot3d_native_render_command_stream_stress_tests` (1 and 2 frames in flight, mixed Run/Drain,
  empty/yield/sleep commands, concurrent reader with `try_lock` like the watchdog, stop requested from
  inside a command; a sentinel aborts if nothing advances for 20 s): clean under TSan (100 thousand and 300
  thousand commands per configuration) and, without TSan, 4 million commands (2 M with 1 and 2 M with 2 frames
  in flight: 952 thousand submitter waits, 436 thousand worker waits, 2.19 M skipped signals) plus 1050
  stops from inside a command, with no hang at all. TSan found only a new race in the reader itself (`StartWorker`
  wrote `mStop` outside the lock): fixed.
- *PICA plan resolves with the build mutex every up to 32 draws*: a single build mutex,
  never held together with the queue's; the batch pool (`BatchPool::Mutex`) is only taken alone
  or inside the build one (never the reverse): acyclic lock graph. The atomic stop
  (`mStopRequested`) is only a shortcut; the worker's wait uses `mStop` under the queue mutex.
- *sys-clk*: the window-300 call answered (16 us). The session is opened once, at window 1.
- *Pacer/cadence and chunked menus/16:9 fade*: they run on the render thread or outside the
  `perf.txt` block; they do not explain the game thread stopped inside it.

**Fix: bounded walk instead of `mallinfo()`** (`oot3d_heap_bin_check.h`). Same sum
(free = bins + top; `heap_used_mb` = arena − free, i.e. `uordblks`), with the malloc lock, but
each link is checked (pointer inside `[fake_heap_start, fake_heap_end)` and aligned, plausible
size, `p->bk->fd == p` and `p->fd->bk == p`), cycle detected (Brent) and a step cap. A
corrupted list becomes `heap_corrupt=<reason> bin=… chunk=… size=… fd=… bk=…` in `perf.txt` and a
"HEAP CORRUPTED" report in `hang.txt` with the state of all threads; the walk then stops
for this session. New field `heap_free_chunks`. Tests: `oot3d_hang_watchdog_tests` (synthetic heap
in the newlib layout: sum equal to `mallinfo`'s, double free, self-loop, cycle, pointers outside the
heap/misaligned, absurd sizes and 3000 heaps with random links — always terminates and never
reads outside the heap, ASan/UBSan). The corruption itself remains: the report gives the chunk and the threads.

**The corruption was a one-off event (fixed on 10/26).** The report (`bin=10 chunk=0x27fe173a0
size=0x51 fd=0x2805553e0 bk=0xed53910000001a`: free chunk of 0x50, requests of 57 to 72, `fd` intact and
only `bk` changed, looks like a write after free) appears in the folders of tours 16 to 27, but the
Switch `perf.txt` accumulates sessions: it is the same line (10887, session 96 = tour16, pgo5o/Mesa)
copied along. In the next 17 sessions with the walk enabled and in the 4 with the guard (p6r-p6u) it did not
come back. Before concluding that something "repeats", check which session of the accumulated file the line
is in.

**Guard pages (`config/guard_alloc`, `oot3d_switch_guard_alloc.h/.cpp`).** The link wraps
newlib's `_malloc_r`, `_memalign_r`, `_free_r`, `_realloc_r`, `_calloc_r` and `_malloc_usable_size_r` (each one
in its own object, so it catches malloc/new/delete for the whole process). devkitA64's `malloc()` sends
every request of 32 bytes or more to `_memalign_r(32, n)`: without wrapping that one, nothing would get a guard;
that is why the object stays 32-aligned. With the file on the SD and from the start of the game session
(`GuardAlloc::Arm()` alongside `HangWatch::Start`), requests in the range get 2 pages: header and
object at the end of page 0 and page 1 as slack (the header of newlib's next chunk sits 16
bytes before its aligned pointer and cannot land on a closed page). On free, page 0 loses
access and the block sits in a FIFO quarantine before returning to malloc. An access to a freed block
becomes an exception immediately. File: `min max quarantine live`, empty = `57 72 4096 32768` (quarantine of
32 MiB; above 32768 live blocks, 256 MiB, requests go to the normal malloc).

*p6r (10/26) did not open*: with the guard from the constructor, every small boot object cost 12 KiB
and boot ran out of memory (`boot-status.txt`: `status=failed detail=Not enough space`; Atmosphère crash
report = User Break in hbl). Fix: enabled only in the session, 2 pages instead of 3 (no
guard page for overflow: the corruption is use-after-free) and the live cap.

*p6s, 2nd run (10/26)*: with the quarantine at 32768 the game crashed on the first frame of the session inside
`SWITCHAUDIO_PlayDevice` (SDL audio thread), and the handler did not even write `hang.txt`: each
closed page splits a kernel memory block and tens of thousands exhaust the process. The 1st
run (quarantine 4096) completed the whole tour: 1.1 million guarded blocks, no fault, but the
game frees ~2900 blocks of 57-72 bytes per second, so 4096 only watches ~1.4 s.

**Canary mode (default since p6t).** The file gained a 5th field `canary|page`; empty =
`57 72 131072 32768 canary`. In canary mode the block is the object with a 256-byte header in front
(ordinary allocation, no kernel call). On free the object becomes 0xFD and sits in a quarantine of
131072 blocks (~45 MiB, ~45 s); when it leaves, the bytes are checked and, if any changed,
`hang.txt` gets "USE AFTER FREE (canary)" with serial, size, how long it lived, how long ago it was
freed, the changed ranges with the bytes, and the allocation and free stacks. A double free becomes
"DOUBLE FREE (canary)" (the second free returns nothing). It does not give the PC of the write, but it gives the
object's owner; then page mode can target only it. Page mode stays capped at 4096 in quarantine.
`perf.txt`: `guard_canary=1 guard_canary_hits guard_double_frees`.

*p6t (10/26) froze the whole console*: the canary filled the object with 0xFD on free. The
`hang.txt` showed "EXCEPTION" in `__wrap__free_r` (game thread, `vector<Oot3dPicaDrawSubmission>`
reallocating): free read `ptr - 256` to find the header and the address was not mapped (`far`
= ptr − 0x100, `esr=0x92000007`). Fixes in p6u: a mark at `ptr - 8` (in an ordinary block that is
newlib's chunk size, always readable) before reading the rest of the header, and free keeps a
**copy** of the object (384-byte header, range up to 128) instead of filling with 0xFD: reads
after free keep seeing the old data, as without the guard, and only writes are flagged
(report with the from/to bytes).

*p6u (10/26)*: whole tour (44 warps, 32400 frames) with the canary: 1.55 million blocks of 57-72
bytes, each checked ~45 s after free, no write after free, no double free and
244 heap walks without corruption. `config/guard_alloc` was removed from the SD; the exception handler stays
(always on) and so does the walk (`config/heap_check_windows`), to catch the next occurrence.

The libnx exception handler (always on, writes and returns, so the Atmosphère crash report
still comes out) writes "EXCEPTION" to `hang.txt`: `far`, PC/LR/chain of the faulting thread, `USE
AFTER FREE` with serial, size, offset in the object, how long ago it was freed and the allocation
and free stacks (`addr2line` on the offsets), plus the other threads. `perf.txt` gains `guard_alloc=1
guard_made guard_live guard_quarantine guard_fallbacks guard_skipped`. Cost: 8 KiB per block in the
range and a kernel call on free; diagnostics only. Test: `oot3d_switch_guard_alloc_tests`.
Guard-page idea: Chronicle PR #205.

**Hang watchdog** (`oot3d_hang_watchdog.h/.cpp`, on by default on Switch,
`config/no_hang_watchdog` disables):
- Cheap markers: each thread with a role (game, render, plans, host audio, SDL audio)
  gets a slot in `PlaceSwitchThread`. Stage = one relaxed store (`frame_begin`, `guest_refresh`,
  `present_visual`, `ui_submit`, `present_submit`, `frame_end`, `perf_window`, `perf_clocks`,
  `perf_heap_walk`, `perf_file_close`, `wait_frames_in_flight`, `wait_drain`, `sync_command`,
  `render_idle`, `render_command`, `render_pacer`, `render_swap`, `render_resolve_plans`);
  progress = load + store by the owner (one per host frame on the game, one per command on the render thread).
  Nothing per draw; the stream waits only mark when they actually block.
- A service thread (0x3B, cores 1|2) wakes every ~1 s. If the game or the render thread
  (not idle) goes > 3 s without progress with the app in focus, it writes **once**
  `logs/hang.txt` (append, `FsWriteOption_Flush`), via the libnx FS API directly — no `fopen`,
  no float `printf`, no allocation at all: the hang may be the malloc lock itself.
  When everything moves again, it writes `--- recovered after N ms` and re-arms (at most 8
  reports per run). Out of focus (HOME) and jumps of > 2.5 s in the watchdog itself (console
  sleep) reset the count.
- Contents: stage, progress (= game `host_frames` / render-thread commands), stall time
  and, for each registered thread (including service ones: Mesa, spdlog…), **PC, LR, SP, FP, x0–x2,
  x19, the frame-pointer chain (16) and a stack scan** for code addresses. The
  thread is paused for an instant with `svcSetThreadActivity` (the pause waits for it to leave the core),
  read with `svcGetThreadContext3` (only works with the thread paused; the same way a
  debugger does) and resumed; only if the NPDM allows it (`envIsSyscallHinted(0x32/0x33)`), otherwise the
  report says so and keeps to the markers. Addresses inside the NRO come out with the offset
  `(+0x…)` for `aarch64-none-elf-addr2line -e oot3d_native_game.elf`. Also: the last service
  call (`sm:AtmosphereHasService(sys:clk)`, `sm:GetService(sys:clk)`, `sys:clk GetApiVersion`,
  `sys:clk GetCurrentContext`; `IN_PROGRESS` if it did not return), the stream state read with
  `try_lock` (`queued`, `frames_queued`, `busy`, `worker_waiting`, `progress_waiters`,
  `submitted/executed`, waits and `signals_skipped`; `mutex=BUSY` if stuck) and the pacer.
- `perf.txt` (`threads:`) gained `hang_wd=1 hang_reports=N`.
- How to read: game in `perf_heap_walk` with PC in malloc/`__libc_lock_acquire_recursive` = malloc
  lock stuck; look in the other threads for whoever is inside `malloc/free`. Game in
  `wait_frames_in_flight` + render in `render_swap`/`render_pacer` = render thread stuck in
  swap/pacer. `last_service_call=… IN_PROGRESS` = IPC with no response.

### Update (2026-10-16, night)
- Real on-console profile (`config/guest_profile` → `logs/guest_profile.txt`): the most expensive
  functions were audio effects (reverb/echo) and `Runtime_Memcpy`; rewritten in native C as exact
  replacements (millions of cases, zero differences) — estimated gain ~4% of logic.
- Native ARM64 VFP enabled (`--native-vfp-aarch64`), validated with 193 million checks under qemu.
- Text literals rewritten by TopScreen excluded from constant folding.
- Builds done on a Ryzen 5 5600G machine (WSL2) with ccache: full ~30–60 min, incremental ~1 min.
- Switch controls: buttons by label, as on the 3DS (A/B/X/Y in the right places; devkitPro's SDL2
  mapped by position, Xbox style). ZL/ZR do the game's L/R (aim/shield) and physical L/R do
  ZL/ZR. Mapping in `ConnectedPhysicalDeviceManager.cpp`.
- Switch defaults (no files on SD): render thread on, 2× resolution (3× = 720p made the GPU the bottleneck: 23.4 vs 28.5 FPS measured), FSR 1 and 4× anisotropic.
  Disable with `config/serial_presentation`, `config/resolution_scale`=1, `config/upscaler`=linear, `config/anisotropy`=1.
- HUD fixed: the PICA VAO leaked into the host-drawn UI (hearts, rupees, icons), which
  inherited instance divisors and sampled a single texel. Audio: larger audio queue ceiling,
  high-quality resampling and render thread below audio priority (residual crackle under study).

### Native replacements and the guest profile (2026-10-17)
- The profile footer's "refusals" were an artifact: in measured frames the profile installs the
  callback on all blocks and `Hooked()` refused every replacement (block ones at loop top, on every
  iteration). The measured time of those functions was the translated code's. Now the profile
  registers itself (`Oot3dGuestOverrideProfileHook`), replacements only refuse if the profile would
  forward a PC from the range, and native time is attributed to the entry block.
- Reading the profile: ~180 ns per block entry is the profile's own; a block's real cost is
  `ns/entry - 180`. Short, heavily executed blocks (audio loops, mutex) cost almost nothing.
- Largest real cost in Kokiri Forest: `Mtx3x4_Multiply` (0x36C174, ~440 calls/frame, manual
  external call). New exact kernel (`oot3d_native_mtx3x4_kernel.h`, native VFP with the guest's
  FPCR): ~4.7x fewer instructions; the old double path got IXC/UFC and the sign of flushed zeros
  wrong (~10% of test matrices diverged from software).
- New prologues use `"inline": true` in `guest_overrides.json`: they don't shift shard lines, and
  PGO for the shard's other functions stays valid (only the function with the prologue loses its profile).
- Systemic bottleneck of generated VFP: `Oot3dVfpNative*` falls back to software on every zero
  result, including exact zeros (x*0, a-a), common in rotation/scale matrices. Solved in the
  header round (below, `--vfp-zero-aware`).

### Header and generator round (2026-10-17)
Changes that recompile all 256 shards and require a new PGO collection, bundled into a single round.
Each has a switch for A/B; without the generator flags the generated C++ is byte-for-byte the previous one.

**Final C++ generation command** (from `tools/oot3d/native_a32_runtime`; `--guest-overrides`
already applies the replacements, so `apply_guest_overrides.py` afterwards is idempotent and changes
nothing):
```sh
python generate_whole_aot_cpp.py --program ~/oot3d-work/gen/aot_program.json \
  --selection whole_aot_functions.json --code ~/oot3d-work/extract/exefs/code.bin \
  --output ~/oot3d-work/gen/cpp-lean --shards 256 --shard-strategy affinity \
  --native-vfp-aarch64 --vfp-zero-aware --lean-stack --guest-overrides \
  --giant-function-instructions 1000
python apply_guest_overrides.py --generated ~/oot3d-work/gen/cpp-lean
```
(Without `--vfp-zero-aware --lean-stack` the output is exactly the current `cpp-lean`; checked file by file.)

1. **VFP with exact zero in native** (`--vfp-zero-aware`, requires `--native-vfp-aarch64`). All
   arithmetic operations (VADD/VSUB/VMUL/VNMUL/VDIV/VSQRT/VMLA/VMLS/VNMLA/VNMLS) use
   `Oot3dVfpZeroAware*` (`oot3d_native_a32_vfp_aarch64.h`). Hot path identical to
   `Oot3dVfpNative*` (one instruction and one test); a zero/NaN result calls the cold half
   `ZeroAwareSlow<Op>`, which keeps the native zero when it is exact in both models (finite,
   non-subnormal operands; exact cancellation `x == -y`, two zeros, or a zero factor; in VMLA and
   family the native product must also match the software one) and otherwise runs software as before.
   FZ case: a sum/product of normals giving a flushed subnormal is NOT exact (ARMv8 keeps the
   sign, software gives +0, and there is UFC/IXC): goes to software. The Mtx3x4 kernel uses the same
   predicates. Proof: `run_vfp_aarch64_tests_qemu.sh` (Switch devkitA64 -O3 under qemu, 16 modes
   RMode×FZ×DN, value and flags) with 299 million checks: 3.6 million exact zeros stayed native
   and 1.5 million inexact zeros were recomputed, zero divergences; Mtx3x4 (230
   million operations) and vfp_fast pass. `run_vfp_zero_aware_mutations.sh`: 9 mutations of the
   criterion (accept subnormal in sum or product, accept flushed product, accept any
   zero in each operation) are all detected.
2. **Stack traffic.**
   - 16-byte `Oot3dWholeAotFlow` (`OOT3D_WHOLE_AOT_FLOW_IN_REGISTERS`, default 1; 0 restores the
     12-byte layout, always program-wide). With 12 bytes (one byte, padding and two
     words) GCC built the flow in a stack temporary on every return and every call
     (store, reload, store again before the common epilogue), including the hot path of every
     guest call. With a fourth `Reserved` field (always 0) it goes straight into x0/x1.
   - `--lean-stack`: the destination of a guest read is no longer initialized to 0 (`ReadFast`
     always writes when it returns true; with false the value is dead) and LDM with a single-page
     path doesn't pass the address of `spanWords` to the word-by-word path (which reads into its own
     temporaries): the array stays in registers and the words go from load straight into state. The
     `str wzr, [sp]` before each load and the LDM words' round trip through the stack vanish from the
     hot path; the cold path (page-crossing/MMIO) gets slightly bigger.
   - Spills around cold calls: they stay only on cold paths (GCC already splits live
     ranges); the ones left on the hot path are the `frame`/`context`/`state` pointers around
     each guest call, which cost the same in callee-saved. Not touched.
3. **Generation counter `mRangeGeneration`: evaluated, no change** (replaced on 2026-11-05 by
   store cards, "Recompiled-code overhead" below). The fast write does load,
   add, store of the counter and store of the page stamp. Generations are observable (exclusive
   monitor LDREX/STREX via `WriteGeneration()`, GPU cache page stamps,
   `CaptureState`), so they can't be eliminated. Reordering (read before the guest store, write the
   counter last) didn't change any instruction in the measured shards: after each write the
   fast and slow paths (`WriteFastSlow`, which changes the counter) merge, and at the merge GCC
   reloads. Batching would require a per-function local counter, written before every slow
   path, call, hook and exit (the same problem as register promotion); not done.
4. **Register promotion** (`--promote-registers`, evaluation commit integrated; off,
   it only adds `Oot3dAotRunBlockEntryHook` at the end of the header): future rounds don't need a
   full build just for it.

Static measurement (devkitA64 GCC 16.1 with the shards' real flags, no PGO; instructions / loads /
stores / stack loads / stack stores; "all" = new header + `--vfp-zero-aware
--lean-stack`):

| shard | before | Flow 16 B | all |
|---|---|---|---|
| 000 | 157339 / 30343 / 27669 / 4505 / 13287 | 157204 / 29464 / 18464 / 3622 / 4080 | 160288 / 28969 / 17239 / 2950 / 2277 |
| 091 | 84854 / 16718 / 14211 / 1864 / 6479 | 87987 / 16569 / 9986 / 1722 / 2258 | 87840 / 16690 / 9334 / 1826 / 1586 |
| 150 | 116001 / 28857 / 22997 / 10189 / 14011 | 116861 / 28229 / 18800 / 9527 / 9809 | 123776 / 30679 / 19965 / 11559 / 10481 |
| 243 | 114658 / 28978 / 20779 / 10090 / 11973 | 115653 / 27833 / 18886 / 8957 / 10102 | 124378 / 30696 / 19982 / 11171 / 10490 |

The static count mixes hot and cold paths: both generator flags grow cold code
(call to `ZeroAwareSlow`; one `ReadFast` per word in word-by-word LDM) and shrink the
hot code. The dynamic count under qemu is in the next table.

Dynamic count (qemu-aarch64 with one instruction per TB, executed PCs × objdump; differential
harness with the same code on both sides, 32 cases × 40 attempts, same inputs; translated
code only, in millions; instructions / loads / stores / stack loads / stack stores):

| variant | instr | ld | st | stack ld | stack st |
|---|---|---|---|---|---|
| before | 1459.85 | 362.05 | 197.71 | 32.13 | 40.53 |
| Flow 16 B | 1459.81 | 362.00 | 197.70 | 32.08 | 40.52 |
| `--lean-stack` | 1451.42 (−0.58%) | 362.05 | 189.28 (−4.3%) | 32.13 | 32.10 (−20.8%) |
| all | 1451.38 (−0.58%) | 362.00 | 189.27 (−4.3%) | 32.08 | 32.09 (−20.8%) |

The harness functions (replacements and their closure: reverb, memcpy, curves, matrices) are
loops with few calls: the Flow gain barely shows here; it is at call and return sites
(static above), proportional to the game's number of guest calls. Inputs are
random and barely exercise VFP with FZ=1 (54 thousand software VFP instructions in 1.46
billion), so the `--vfp-zero-aware` effect only shows in the game; the Mtx3x4 kernel, which uses the
same criterion, measured ~4.7x fewer instructions with exact zeros native.

Round proofs (scripts in the repository; run on the Ryzen and the notebook):
- `run_register_promotion_equivalence.py --baseline <current cpp-lean> --promoted <generated with
  --vfp-zero-aware --lean-stack>`: the same 314 real functions (replacements, functions with
  block replacement and the call closure) from both generators in two namespaces, equal state,
  memory, generations, budget, hooks and tracing: host 32 cases × 100 thousand, qemu-aarch64
  (Switch -O3, native VFP) 32 cases × 20 thousand, zero failures.
- `run_guest_override_equivalence.py --qemu-aarch64` on the new output: 20 of 21 cases OK. The failure,
  `Math_SmoothStepToF` (0x36E168, attempt 140844: FPSCR with extra OFC in the native replacement,
  FZ=0/RN), **already existed**: it repeats identically with the current `cpp-lean`. It belongs to the
  replacement, not this round (the translated side uses software in this mode); to be fixed in the replacement itself.
  **Fixed (2026-10-18):** the replacement used `Oot3dVfpAdd/Subtract/Multiply`
  (`oot3d_native_a32_vfp_fast.h`), whose fast path does host float math. On console and
  under qemu they run inside `Oot3dGuestFpScope` (guest FPCR, FPSR folded into the guest FPSCR on
  exit), so flags from auxiliary computations leak to the guest: in the TwoSum of `s0 - s4`
  (0x36E1C0) with `s4 = FLT_MAX` and `s0 = 0x7EC70697`, `sum - a` overflows (OFC, and IOC from inf - inf)
  although the subtraction itself doesn't overflow. Now the replacement uses the same `Oot3dVfpNative*` as the
  generated code (guest instruction with FZ=1, full software in other cases; the first
  comparison, before the last refusal, stays in software). The differential test gained edges for
  this function (near FLT_MAX, inf, NaN, subnormals, all RMode×FZ×DN) and 16 fixed regression
  cases (attempt 140844 in the 16 modes; the old version fails on the first). Proof:
  host 20 million and qemu-aarch64 4 million attempts, zero failures.
  **The others (2026-10-18):** 0x00372070, 0x002CDE90, 0x003325F0 and 0x003727F0 also used the
  fast path inside the guest scope. Two real leaks, found by the new edges under qemu:
  - `oot3d_translate_mtx3x4_local` (attempt 9019, FZ=0/RN): `m[7] += 2^-24 * -0x0C7FFFFF`. The
    exact product lies just below 2^-126 and rounds to -2^-126; the fast path's double -> float
    conversion detects tininess before rounding (ARMv8) and raises UFC, which
    software doesn't raise;
  - `Math3D_CylOutsideCylDist` (attempt 7766, FZ=1): the TwoSum of `dx = x0 - x1` with `x1` near
    FLT_MAX overflows (extra OFC/IOC).
  `FUN_002cde90` (just `s0 * 0.1f`) and `Math_SinF` (inputs bounded by |x| < 12867) didn't leak in
  4 million attempts with edges, but had the same mechanism.
  Fix: all replacements do VFP arithmetic through the `Vfp` namespace of
  `oot3d_native_guest_overrides.cpp`. On AArch64 it calls the generated C++'s `Oot3dVfpZeroAware*`/`Native*`
  (`--native-vfp-aarch64 --vfp-zero-aware`): one instruction with FZ=1, full software in
  other cases. On other hosts (no guest FP scope) it uses the host float fast path.
  The differential test gained `EdgeFloat`/`NearMinNormalFactor`/`EdgeFpscr` (near ±FLT_MAX,
  high exponents, inf, quiet and signaling NaN, subnormals, minimum normals, products just
  below 2^-126, the 16 RMode×FZ×DN modes with accumulated flags) in the VFP functions (the 5 above,
  `FUN_003094cc`, `Math3D_CirSquareVsTriSquare`, the two curves and `Mtx3x3_Multiply`). It also gained
  fixed cases: 0x00372070 with both cases in the 16 modes (32) and 0x003325F0 with 16. The old
  versions fail on the first; without the fixed cases, random edges find both failures.
  Proof (Ryzen): qemu-aarch64 with 4 million attempts per VFP function (10 functions; two seeds
  on the 5 main ones) and 200 thousand on all; host with 20 million per VFP function and 1 million on all.
  Zero failures. New measurement knobs in the test: `OOT3D_QEMU_FPSCR`, `OOT3D_QEMU_CLEAN` and
  `OOT3D_NO_REGRESSION`.
  Mtx3x4 kernel (`Mtx3x4MultiplyFast`): the `HostFloat` path (FZ=0) runs in an external call,
  under `Oot3dGuestFpHostCallScope`, with host FPCR and FPSR discarded on return, so it doesn't leak. The
  native path saves and restores FPSR. `Mtx3x3_Multiply` already used `Oot3dVfpZeroAware*`.
  **Cost and decision:** instructions executed under qemu (`-one-insn-per-tb`), game FPSCR
  (0x03000000), no hooks or short budget, 2000 attempts, per call and with refusals
  included:

  | function | translation | new | old |
  |---|---|---|---|
  | 0x00372070 mtx3x4_local | 1410 | 1606 | 2470 |
  | 0x002CDE90 | 569 | 664 | 684 |
  | 0x003325F0 CylOutsideCylDist | 917 | 1521 | 1794 |
  | 0x003727F0 Math_SinF | 37197 | 37270 | 37649 |
  | 0x0036E168 SmoothStepToF | 807 | 1032 | 981 |

  The new version is cheaper than the old in 4 of 5, but none beats the current translation (which already
  has `--vfp-zero-aware --lean-stack`). So the 5 were marked `"disabled"` in
  `guest_overrides.json`: they are not enabled in the generated C++ (`load_manifest` skips them) and remain in the
  differential test (`include_disabled=True`). This takes effect at the next C++ generation. Instruction
  count is not time on the A57; to re-enable any, measure on console.
- `test_whole_aot_cpp.py` (44 tests, including those for the new flags: optional, dropped from the manifest
  when off, identical C++ without them).

### Generator round of 11/08: N4, VFP literals, indirect census
Changes for the next PGO collection (they change every shard, so the old profile no longer applies).
Each is a generator flag; without them the shards are byte for byte the previous ones (the registry
gains `Oot3dWholeAotHookGates()`, which says "every block").

**Generation command** (from `tools/oot3d/native_a32_runtime`; the wrapper passes the two new
flags through `AOT_GEN_FLAGS`):
```sh
python3.12 generate_whole_aot_cpp.py --program <aot_program.json> --selection whole_aot_functions.json \
  --code <code.bin> --output <cpp> --shards 256 --shard-strategy affinity \
  --native-vfp-aarch64 --vfp-zero-aware --lean-stack --guest-overrides \
  --giant-function-instructions 1000 --sparse-block-entries --fold-vfp-literals
python3.12 apply_guest_overrides.py --generated <cpp>
```

1. **N4: budget and hook gate only where needed (`--sparse-block-entries`, `--hook-gates`).**
   Every block used to run `Oot3dAotEnterBlock` (budget decrement and hook gate test). Now
   (credit: ReXGlue/XenonRecomp-style loop barriers only at loop headers; sa2-nx's pruning):
   - the **budget** stays at each function entry and at every target of a DFS back edge of the
     function's `goto` graph (every directed cycle contains a back edge, so every loop, irreducible
     ones too, passes a decrement; the generator re-proves it with a topological sort of the rest);
     a block without its own decrement is charged by its dispatch (`switch (entryPc)`: resumes,
     indirect calls into a block, the registry's re-dispatch). Every nested call enters a callee at
     a charged entry, so the native call depth is still bounded by the budget (the 2048 of the
     Switch keeps its stack bound), and a run executes at most budget × largest function blocks;
   - the **hook gate** stays only at the PCs of `whole_aot_hook_gates.json` (330 PCs), the union
     of every runtime hook list, written by the host tool `oot3d_block_entry_hook_census` from
     `Oot3dNativeBlockEntryHookPcs()` (`oot3d_native_block_entry_hooks.{h,cpp}`, the only place
     that builds the list; `tests/switch/run.sh` checks the file is current). A gated block charges
     too, so a run's first block is always charged and "first block" stays `BlocksConsumed() == 1`;
   - every block stays a dispatch entry (resumable anywhere; the product audit is unchanged);
   - **refusal**: under sparse code, setup throws (fatal screen, boot status) when a hook PC has
     no gate or a mode wants every block (`--trace-a32-blocks`, `--profile-a32-blocks`), the
     dispatch throws for any later ungated list, `config/guest_profile` and `config/guest_spike_ms`
     are refused with a `TRIAEVUM_GUEST_PROFILE refused:` line (the game continues), and the
     title plugin refuses sparse code (its ABI has no gate list).
   - **What changes for the guest:** nothing in state; `BlockLimit` exits (the cooperative thread
     switches and the core-0 kernel-lock retakes) come only at charged blocks. Measured on the
     last PGO session's real block counts (16 hottest shards, half of guest time, 1.30 G block
     entries, `.gcda` paired with rebuilt notes): 73.7 % of block entries now run no entry code,
     25.8 % only the decrement, 0.5 % the gate; 3.8 block entries per budget unit. With the
     unchanged Switch budget of 2048 a slice runs ~3.8× as many blocks (about 0.7 → 2.5 ms of guest
     code), so `BlockLimit` exits per step drop ~3.8× and a thread readied by core 1 or by host
     events may wait up to ~2.5 ms more at worst (SVCs still switch as before). World stops wait
     correspondingly longer for core 1 to leave guest code.
   - Static (devkitA64 16.1, Switch shard flags, no PGO; shards 077/091/180/217, the hottest):
     instructions 454,854 → 389,835 (−14.3 %), loads 118,552 → 101,351 (−14.5 %), stores
     69,102 → 53,778 (−22.2 %), `.text` 1.82 → 1.56 MB; all 256 shards: 169,729 entry sites →
     314 gated + 20,862 budget-only + 10,236 dispatch charges, C++ 349 → 325 MB.
   - A pre-existing gap found on the way: 29 hook PCs start no block in the whole-AOT code, so their
     hooks never run in either lowering (`hook_gates_without_block` in the manifest): 26 typed-gameplay
     exits (developer builds) and 3 player-temporal PCs (0x0023C8A4, 0x0023C8F0, 0x0023CAB0).
2. **Loads without the page-flag check (`--fold-vfp-literals`).** `VLDR Sd/Dd, [pc, #imm]` reads
   the read-only text segment like `LDR [pc]`, which was already folded: it becomes a constant
   (same `LiteralImage`, same host-written exclusions; text is mapped read-only for the whole
   process, so the read always returns the `code.bin` bytes and can never fault; only the
   diagnostic read-watch counters stop seeing it, as for `LDR`). 30,246 sites (27,755 addresses,
   11 % of the `ReadFast` sites); 0.073 per block entry in the real profile. Every other load keeps
   its check: a page without `kGuestPageLoadFast` means unmapped (a guest fault), window off,
   write tracing, a watch or a deferred GPU readback, and none of those is static. The next
   provable subset, one check for consecutive loads from the same unmodified base on one page,
   would remove 0.22 checks per block entry (static: 13,473 of ~270k sites); not done this round
   (~0.03 ms per step [estimate]). Stores are unchanged.
3. **Indirect call census** for a later N2 round: see "PGO: collecting a profile on the console".
4. **Fix:** `apply_guest_overrides.py` found no definition for overrides inside a giant function
   marked `GIANT_COLD_ATTRIBUTE`, which a generation without a hot list (every `PGO=generate`
   build) produces: the mirror natives of 0x0046C204 made that generation fail.

Tests: `test_whole_aot_cpp.py` (N4 shape, irreducible loop, refused combinations, VFP literals,
byte-identical when off), `test_apply_guest_overrides.py`, `oot3d_native_block_entry_hooks_tests`
(the 64 option combinations against the previous inline list), the census `--check`,
`oot3d_native_perf_diagnostics_tests` (concurrent recording, also under TSan),
`test_whole_aot_indirect_census.py`, and the A/B of `run_register_promotion_equivalence.py
--n4` (`OOT3D_N4_EQUIVALENCE`: base vs sparse on 259 real functions (the 34 override cases, the
top 60 of the guest profile and the 175 other functions owning a real gate), 2,648 functions per
side with the call closure and the registry owner of every dispatch entry; each side runs to
completion through every `BlockLimit` resume, half the trials with 1-48-block slices; hooks at
the 330 real gates plus test hooks, incl. hooks that change state and skip-first; half the inputs
shaped (registers and stack in a fake heap with plausible words, plausible `.data`/`.bss`);
guest state, memory, write generations, write trace and the hook sequence must be equal, the
budget counters are free). Host g++ -O2, 20k trials per case for N4 and for N4 + VFP literals:
259/259 each, 5.15 M trials compared, 523.5 M hooks, zero differences (30.5 % of shaped trials
return, 25.4 % unshaped; `0046C204`, `003F96BC`, `003FC2F8`, `00300328`, `0041E968` were
only covered up to their early exits on the host). qemu-aarch64 (devkitA64 -O3), 16 hot cases ×
2k: 16/16, zero differences. Budget used, N4 / base: 0.92 overall, 0.17-0.75 in the branchy hot
functions, ~1.0 where every block is a loop header.

### Audio off the game thread (2026-10-17)
- The DSP HLE (mixing) stays on the game thread: each audio frame reads the DSP shared regions,
  writes status/aux/final_samples to guest memory and signals the guest, which runs right
  after and writes the next frame's configuration (including the reverb/echo aux buffers). It is a
  strictly sequential chain with the guest; moving it to another thread would parallelize nothing.
  Only the 2 × 64 KiB copies zeroed every frame were eliminated (persistent buffers).
- The host side (stretch, 32728→48000 resampling, SDL queue) runs on its own thread
  (core 1/2, priority 0x2B), woken on every game submission and every 4 ms.
- In normal play the stream edits nothing: only fine rate adjustment (±0.5%, inaudible) holds the queue
  at target (100 ms). Whole pitch periods are inserted only with the queue nearly empty (below
  ~37 ms) or during a measured sustained slowdown (game below 93% speed: tempo follows
  speed, up to 1.5×, without changing pitch); removed only near SDL's discard ceiling (2.6× target).
  If the game lags and the queue reaches the critical level, the last period is repeated (40 ms full, then
  80 ms fade) and the return is continuous, without the re-prime pause. One period (~12 ms) is always kept
  pending for this.
- 1st version (6c0012f) sounded grainy: the stretch credit was signed and an insertion overshooting
  the request turned into a removal right after (and vice versa), editing all the time (26 min on console:
  153 s inserted and 346 s removed). The high queue (min_queued=10396, target 4800) indicates the
  device drained below 48 kHz: SDL's audren backend has only 2 wave buffers and waits, in
  5 ms frames, for the just-queued buffer to start playing; with 512 frames (10.7 ms) only
  ~1 ms was left to queue the next one and the voice ran dry between buffers (grain). Simulation with a 5% slower
  drain reproduces the console numbers. Device buffer now 1024 frames
  (`config/audio_device_frames`, 256–4096; `512` restores the previous).
- `logs/perf.txt`, `audio:` line (per 150-frame window): `min_queued avg_queued device_hz`
  (real device drain; should be ~48000) `input_hz` (game delivery; ~32728 at 100%)
  `trim_pct game_speed`, and accumulated `inserted_ms removed_ms concealed_ms conceal_events starved
  pump_ms_*` (in ms of DSP audio).
- A/B: `config/audio_thread` = `0` (host mixing goes back to the game thread, stretch stays) and
  `config/audio_stretch` = `0`; both `0` = identical old path. Desktop: `TRIAEVUM_AUDIO_THREAD=1`,
  `TRIAEVUM_AUDIO_STRETCH=1`.

### Per-source interpolation in the DSP (2026-10-17)
- Where: `third_party/azahar_audio/audio_core/interpolate.cpp`, called by
  `hle/source.cpp` (`Source::GenerateFrame`), which resamples each voice (game rate) to 32728 Hz.
  The original follows Citra/Azahar: None = hold, Linear and "Polyphase" = linear.
- `config/audio_interpolation` (desktop: `TRIAEVUM_AUDIO_INTERPOLATION`): absent/`original` =
  old code, bit for bit; `cubic` = 4-point Catmull-Rom (no delay); `sinc` = polyphase sinc-Kaiser
  (β 8) Q15, 256 phases with linear blending between phases, 16 taps up to rate 1 and widened
  up to 32 taps with cutoff at 1/rate for sped-up voices (anti-aliasing). Fixed delay of 14
  voice input samples (0.43 ms at 32728 Hz). None stays hold in both modes.
- The stepping (positions, consumed samples, xn1/xn2, fposition, status, loops) is the same code as the
  original; only the sample value changes. The stream is continuous across chained buffers and loops
  (`State::history`, 32 samples, serialized into the savestate only when non-zero).
- Tests: `oot3d_native_audio_interpolation_tests` (`--bench` measures 24 voices). SNR sinc 79–94 dB
  vs 13–45 dB for linear; aliasing −81/−84 dB vs −4/0 dB. Estimated cost on Switch
  (llvm-mca A57 of the NEON loop): ~+0.3 ms per step with 24 voices always active, half sped up.

### Cheaper DSP mixing, bit-for-bit identical (2026-10-17)
- Mixing (`dsp_mix` in `perf.txt`) stays on the game thread; it got cheaper without changing any
  byte of what the guest sees or of the PCM (interpolation mode `original`). Where the time went (an
  OoT3D-like scene: 18 voices, sequenced music in ADPCM loops, PCM16 stream, effects, reverb on aux 0):
  ~1/3 ADPCM decoding (the whole buffer on every loop iteration, with allocation + zeroing), ~1/4
  linear interpolation, ~1/4 `MixInto` (3 mixes per voice, even with zero gain), the rest copies of the
  shared regions (4 × 32 KiB per frame), mixers and output.
- What changed:
  1. `DecodedSampleQueue` reuses storage (no `new`/zeroing/`free` per buffer) and
     remembers which ADPCM decode it holds: a loop that comes back with the same address, size,
     coefficients, initial predictor state and identical encoded bytes (`memcmp`) reuses the
     samples and final state, which are exactly what decoding would give. Host only, not in
     the savestate; encoded buffers > 64 KiB are not remembered. (Replaced on 05/11 by the
     decodes shared between sources, below.)
  2. ADPCM: 14-sample frame without the clamp on the feedback path (multiply-add + shift, ~5
     cycles/sample on A57 vs ~9); if any value leaves s16 the frame is redone by the original
     code. One 32-bit write per sample (both channels).
  3. None/Linear interpolation (`original`): the number of outputs whose index reads straight from the buffer is
     computed beforehand; the loop doesn't test a virtual index per output. On AArch64 Linear uses NEON on
     both channels (saturating subtraction = the clamp, exact 64-bit product, shift 24): ~6.6 vs
     ~14.6 cycles per output (llvm-mca A57). Absurd steps (rate > 65536) use the original loop.
  4. Mix with all 4 gains zero is skipped in `MixInto` and in the mixers (adding zero changes nothing).
  5. Output copied via `memcpy` (final_samples and host PCM); only the ungenerated tail of the
     source's frame is zeroed.
- Measured on host (i3-8130U, -O3 -ffast-math): 0.0506 → 0.0288 ms per DSP frame (−43%).
  Estimated on A57 at 1785 MHz (llvm-mca per loop × scene counts): ~0.6 → ~0.33 ms per step.
  The 4 × 32 KiB copies per frame remain (`Read`/`Write` need the same ranges for
  write-generation accounting) and become ~1/4 of the cost. (Removed on 05/11, below.)
- Proof: `oot3d_native_audio_dsp_mix_tests` runs 4 random titles × 3000 frames through the
  whole `ProcessFrame` (24 sources, all formats, loops, resets, filters, extreme gains and
  denormals, aux with guest effect, output formats, sample data rewritten during the loop,
  absurd rates, savestate every ~150 frames with msgpack in the digest) and compares the digest of both
  regions after each frame + PCM + savestates against the old code's: identical on x86-64 and on
  AArch64 with Switch flags under qemu (`run_audio_dsp_mix_tests_qemu.sh`, devkitA64 16.1).
  `oot3d_native_audio_interpolation_tests` (original digest) and the storage tests pass.
  Note: the `DspStatus` padding is copied from an uninitialized variable since the
  original (stack garbage); the test ignores those 28 bytes and the code was not changed there.

### DSP mixing in battles: in place, shared decodes, two outputs per step (05/11)
- Seen on the console (logs8, Forest Temple, two Wolfos): `dsp_mix` 0.45 → 1.3–1.6 ms per host
  frame. Part of that is the window measure: at ~33 fps a host frame holds ~6 DSP frames instead of
  ~3.4, so per DSP frame it went ~0.15 → ~0.25 ms, i.e. ~1.0 → ~1.7 ms per 30 Hz step.
- Where the AArch64 instructions went (qemu, Switch flags, `oot3d_native_audio_dsp_mix_tests`
  scenes, per DSP frame): linear interpolation 42–48%, the shared-region copies 15–31% (memmove,
  and on the console those 128 KiB also cost memory bandwidth), `MixInto` 17–19%, ADPCM decoding
  0–15% (sound effects and notes decoded again on every trigger).
- What changed (the guest sees the same bytes; same PCM):
  1. The shared regions are processed in the guest's bytes: `NativeDspMemoryAccess` is now
     `BeginUpdate`/`EndUpdate` (`NativeA32Memory::BeginHostUpdate`/`EndHostUpdate`), with the
     accounting of `ReadBytes` of both regions and `WriteBytes` of each one back, in the same order
     (deferred device writes resolved, watch counters, write generations, write observers).
     `EndHostUpdate` must keep `WriteBytes`' bookkeeping exactly (guard in `test_bug_guards.py`).
  2. `AdpcmDecodeCache` (`audio_core/adpcm_decode_cache.h`, one per `NativeA32DspHle`): a decode
     is shared by every source that dequeues the same address, length, coefficients and starting
     predictor state while the encoded bytes are identical (compared in full: the guest may
     rewrite them). The queue holds a reference, no copy. Least recently used decodes go beyond
     2 M samples (8 MiB); buffers over 128 KiB encoded are decoded but not kept. Host only, never
     in the savestate. It replaces the per-queue memo of 10/17.
  3. Linear (AArch64): two outputs per iteration (both x0/x1 pairs gathered with TRN, one
     fraction register per output): ~4.6 vs ~6.6 cycles per output (llvm-mca A57).
  - Tried and dropped: mixing the three intermediate mixes in one pass (llvm-mca: 6.1 vs 5.0
    cycles per sample with one mix, the common case).
- Measured, per DSP frame (base → now):
  - AArch64 instructions (qemu, 30 frames after 150): quiet (10 voices) 70.8k → 41.7k (−41%),
    music (18 voices) 116.3k → 81.6k (−30%), battle (24 voices, a note or effect every 3
    frames from 12 samples) 147.9k → 89.9k (−39%).
  - Host x86-64 (Ryzen 5 5600G, -O3 -ffast-math, no NEON paths): quiet 0.0245 → 0.0188 ms,
    music 0.040 → 0.031 ms, battle 0.0527 → 0.0417 ms.
  - Console expectation: ~−30–40% of `dsp_mix`, about −0.5 ms per 30 Hz step in the logs8
    battle (1.3–1.6 → ~0.9–1.1 ms per host frame at the same fps); the shared decodes count
    for as much as titles retrigger the same samples, the copies on every frame.
- Proof: the `run_audio_dsp_mix_tests_qemu.sh` digests (AArch64, Switch flags) are unchanged;
  the test now also checks the region access order. `oot3d_native_audio_storage_tests`
  compares an in-place update with read + write back (bytes, generations, watch counters,
  observers, deferred writes) and covers shared queues.

### Save states on deko3d (05/11)
- Seen on the console: every save state ended in "Save state failed: native PICA color capture
  failed: rendering backend does not capture native PICA color targets". A save writes the game's
  render targets into the saved copy of guest memory (`SaveState`, graphics-free states) and asks
  the backend for them with `CapturePicaColorTargets`; only Vulkan implemented it, deko3d kept the
  interface default, which refuses. The load side (`RestorePicaColorTargets`) had the same default.
- Now deko3d implements both with Vulkan's contract (`gfx_vulkan_pica_state.cpp`):
  - Capture (`gfx_deko3d_pica_draw.cpp`): at the savestate boundary (render thread drained), the
    open frame's commands are submitted and waited for, as is the present thread
    (`Device::WaitPresentIdle`). Every target, in `FindColor`'s key order, one at a time: the copy
    engine reads its drawn extent (colour RGBA8, depth/stencil Z24S8; the images are block-linear,
    the depth one compressed) into a CPU-visible staging block, which is copied once into cached
    memory and freed. The snapshot is at the guest framebuffer extent
    (`kPicaGuestExtentRenderScalePermille`): colour area-averaged (`ResamplePicaRgba8`), depth and
    stencil nearest (`ResamplePicaNearest`), namespace, addresses, formats as the target key.
    W-buffering and resolved scene effects stay false: deko3d has neither (Vulkan sets
    W-buffering again at the target's next draw).
  - Restore: requires the reset renderer (no target), validates like Vulkan (scales, extents,
    sizes, depth in [0, 1], unique identities) and creates each target at this renderer's scale
    (the top target widened by widescreen), with the colour bilinear and depth/stencil nearest
    from the snapshot. A legacy snapshot (saved at a renderer's scale) is resampled from its own
    extent; its guide images are not read (deko3d has none). The copy engine writes the images,
    then Zcull is invalidated (the creation clear told it depth 1 everywhere). A failure removes
    every target restored so far.
  - The Z24S8 word layout the copy engine gives is probed once per run (first save or load): a
    256×256 compressed target cleared to depth 0.25 / stencil 0x5A is read back, then a pattern is
    written and read back. `logs/deko3d.txt` says which layout came out (`savestate: Z24S8 words
    read as ...`); an unrecognised word or a write that does not round-trip fails the save with
    the words in the message, instead of storing swapped depth and stencil.
  - The pure parts (word layout, snapshot fields, resampling, validation, restore pixels) are in
    `gfx_deko3d_pica_snapshot.h`, tested by `oot3d_deko3d_backend_tests` through SaveState's
    encode into guest memory and LoadState's decode.
- Memory: only the guest-extent snapshots accumulate (1.7 MiB for a 480×400 target); one
  target's staging and full-size copy (a few MiB at 2–3× scale) live at a time and are freed
  before the next. The frame path is unchanged: nothing runs without a save or a load request.
- The native namespace replay stays skipped on deko3d (`PicaNativeNamespaceObservable` false):
  a state keeps what that namespace holds; the presented scene (visual namespace) is never in a
  state, on any backend, and the first frame after a load draws it again.
- Using it: L3+R3 → Game → Save states. "Save state" / "Save state + settings" save, "Load last
  state" / "Load last state + settings" load (the folder is at the top of the tab; states listed
  below can be picked). The result shows as a notice.


### PICA planning off the game thread (2026-10-17)
- The plan for each draw (shaders, uniforms, bindings, texture hashes; `pica_plan`, ~2.5 ms per
  step) was built on the game thread while draining the queue. Now the game thread puts only a
  *placeholder* into the accumulator (id, decoded state, copy producers) and the plan is built from
  the same immutable snapshot of the submission (vertex/index/texture bytes already copied at
  submission time) by a plan worker (priority 0x2E, cores 1/2) or, at the latest, by the render
  command that executes the frame / by the dependency flush (`Run`, guest stopped). Fills and display
  transfers are still captured immediately; execution order does not change.
- A/B on SD: `config/serial_pica` reverts to the old path; `config/pica_plan_render_thread` builds the
  plans on the render thread without the worker. Only applies with the render thread enabled (no
  `config/serial_presentation`). Semantic trace and extended diagnostics use the serial path.
- `perf.txt` gained `pica_plan_deferred`, `plan_worker`, `plan_worker_ms`, `plan_late_ms`
  (built by the consumer because the worker had not got there yet), `plan_late_draws` and
  `plan_serial_ms` (ms per host frame).
- Desktop: `TRIAEVUM_DEFERRED_PICA_PLAN=1` (worker) or `=render`. Test: `oot3d_native_pica_deferred_plan_tests`.
- Fix (the first version degraded with play time on the console: worker 3.9 → 89 ms per frame
  in 3 min, 30 → 9 FPS): after building each plan the worker did `submission = {}`, and a default
  PICA packet allocates and zeroes four 16 KiB shader areas (64 KiB per draw); in addition each
  pass allocated submission and plan vectors of several MB. That was ~118 KiB of extra heap per
  draw (152 KiB vs 34 KiB on the serial path), in large blocks freed by other threads:
  memory bandwidth and newlib heap fragmentation, which weighed on all threads. Now the
  batches are recycled (vectors keep their capacity, the drained vector comes back by swap) and only
  the submission's resources are freed: heap traffic equals the serial path. `perf.txt` shows
  `plan_batches_live/queued/allocated` (should stay small and constant). Long-running test:
  `oot3d_native_pica_deferred_plan_soak_tests` (2400 frames × 160 draws, in all three modes).
- Pinned cores (`PlaceSwitchThread`, `oot3d_switch_threads.h`; only applies if the process mask
  allows): render thread on core 2; PICA plan worker and audio thread on core 1. Previously the
  creation order put the render thread and the worker on the same core, with the other nearly idle.

### Thread scheduling (2026-10-17)
Horizon rules (confirmed by StevensND/nfsmw-nx): only priority 0x3B time-slices; at
any other, a busy thread blocks those of equal/lower priority on the same core, even with
other cores idle; a single-core mask never migrates.

| Thread | Before (core, prio) | Now `fixed` (default) | `float` | Wait |
|---|---|---|---|---|
| Game (main: guest CPU, DSP HLE, submission) | 0, 0x2C | 0, 0x2C | 0, 0x2C | condvar (frames in flight, Drain) |
| Render thread (`NativeRenderCommandStream`) | 2, 0x2D | 2, 0x2D | 1\|2 (ideal 2), 0x2D | condvar; pacer: sleep + spin 500 us → sleep only |
| PICA plan worker | 1, 0x2E | 1, 0x2E | 1\|2 (ideal 1), 0x2E | condvar; build mutex |
| Host audio (`HostAudioStream`) | 1, 0x2B | 1, 0x2B | 1, 0x2B | condvar with 4 ms timeout |
| SDL audio (audren, TIME_CRITICAL) | creation core (1 or 2), 0x2B | 1, 0x2B | 1, 0x2B | audren (blocking) |
| spdlog (async writer) | 1 or 2, **0x2C** | 1\|2, 0x3B; full queue drops the oldest | same | condvar |
| Mesa: disk shader cache queue (util_queue) | 1 or 2, **0x2C** | 1\|2, 0x3B | same | condvar |
| ResourceManager (BS::thread_pool), room shadows, texture pack, inner sun prebuild | 1 or 2, **0x2C** | 1\|2, 0x3B | same | condvar |
| SDL joystick/events | — (no thread on Switch: polling on the game thread) | — | — | — |

Without glthread, Mesa compiles shaders on the render thread (no compile thread of its own).
Previously every service pthread was born at the game's priority (0x2C) and preempted the render thread
(0x2D) and the worker (0x2E) whenever it had work (log writing, Mesa cache compaction,
loads).

- **`config/thread_layout`** (first word; absent = `fixed`): `fixed`, `float` (render thread
  and worker with mask 1|2 — nfsmw-nx saw the worst case get worse with the render thread pinned) or
  `legacy` (exact previous behavior, including the 500 us spin). Option `pacer_spin_us=N`
  (0–4000) after the mode, e.g. `float pacer_spin_us=200`.
- **Busy waiting**: the pacer no longer spins before the deadline (`svcSleepThread` wakes on time); the
  `NativeRenderCommandStream` only signals the condvar when someone is waiting (each signal is a
  kernel call); the plan worker checks the stop request via an atomic, with no lock per draw.
- **Per-draw atomics** (A57 without LSE): placeholder counter per batch (summed in `Publish`),
  worker counters per batch; resolves on the render thread take the build mutex once per
  run of up to 32 draws from the same batch (the run ends right after a draw built
  there, so the worker does not wait).
- **`perf.txt`**: `plan_lock_wait_render_ms/_worker_ms` (build mutex contention),
  `plan_resolve_locks`, `plan_worker_idle_ms`; `sync: guest_wait_frames_ms` (game waiting on the render
  thread), `guest_wait_drain_ms`, `render_idle_ms` (render thread waiting on the game),
  `signals_skipped`, `pacer_spin_ms`, `pacer_oversleep_ms/_max_ms`; `threads: layout=… <role>_cpu=%
  @c0:c1:c2` (CPU of each role via `svcGetInfo(ThreadTickCount)` and where the render thread/worker
  were seen running).
- **How to measure (A/B)**: same scene and same save, 2–3 min per mode, writing `config/thread_layout`
  with `fixed`, `float` and `legacy`; compare `fps`, worst frame, `guest_wait_frames_ms` (drops = render
  thread less behind), `pacer_oversleep_max_ms` (if above ~0.5 ms, try `pacer_spin_us=200`)
  and, in `float`, how often `render@…` appears on core 1.

### Guest threads: time per thread and the requested core (06/11)
All guest threads run cooperatively on the game thread (`NativeA32Process::Run`, core 0), unless
`config/second_core` moves the processor-1 threads to their own host thread (next section). To
see whether work (the game's sound engine, for instance) has its own guest thread and could move
off the game thread, two things are always on:
- **Game log**, one line per guest `svcCreateThread`, at warning level (the release logger drops
  info): `guest_thread: id=<n> entry=0x… prio=0x… processor=<r4, signed> stack_top=0x…`.
  `processor` is what the game asked the 3DS kernel for: -2 = the process default (the core the
  main thread uses), -1 = any core, 0/1 = that core. A thread that asked for core 1 ran beside the
  main thread on the 3DS.
- **`perf.txt`**, its own line per window after the main one:
  `guest_threads: host_frames=<n> run_ms=<x> switches=<y> <id>:<entry>:<prio>:<proc>=<ms> …`.
  Times are host ms per host frame (same unit as `guest=`), only threads that ran in the window;
  id 0 is the main thread (`proc=-2`), `?` = a thread a save-state restore brought in, `other=` =
  ids ≥ 31. `run_ms` = all time inside `Run()` and equals the sum of the threads; `switches` =
  guest thread switches per host frame.
- **What a thread's time includes**: its recompiled code, the host services its SVCs run inline
  (IPC, GSP/DSP commands, waits that return at once) and the scheduler. Work outside `Run()` (DSP
  mixing `dsp_mix`, PICA submission/plan, presentation) is not in any thread.
- **Cost**: one system-counter read (`cntpct_el0`) at `Run()` entry, one per thread switch and one
  at exit, into a fixed array (no allocation); the line is built once per window. It replaced
  `thr_<entry>_us/_n` (`config/gpu_timing`/`config/thread_timing`), which read `steady_clock` twice
  per dispatch and twice per SVC. Test: `oot3d_native_a32_process_tests` (two threads switching:
  totals equal the time inside `Run()`; the line and log formats).

### Second guest core: `config/second_core` (06/11, off by default)
The game creates 7 threads; thread 6 (prio 0x18, the sound engine: reverb 0x4A0338, delay
0x4A022C) asks for **processor 1**, the 3DS's second core, and took 3.4–4.5 ms per 60 Hz host
frame (7–9 ms per 30 Hz step, ~25% of guest time) cooperatively on core 0. With the file
`config/second_core` (desktop: `TRIAEVUM_SECOND_CORE=1`) every thread that asks for processor 1
runs on its own host thread, pinned to Switch core 1, concurrently with the cooperative
scheduler. Absent = the previous single-thread code paths exactly (nothing below takes a lock).
- **Kernel lock** (`oot3d_native_a32_second_core.h`): one recursive lock for the guest kernel and
  the host emulation — SVCs and scheduling of both cores, thread records, waits, handles, the host
  services, the DSP HLE, savestates and every out-of-line `NativeA32Memory` path (slow paths, byte
  and pointer APIs, page table, deferred writes, observers). The game thread holds it for its
  host work and releases it around guest code (`Run`'s dispatch), while blocked on the render
  stream, and around the frame loop's host work that touches no kernel, thread or host-service
  state (`HostWaitRelease`, frame-loop level only): the host frame start after the menus'
  `StartDraw` (overlay, input polling, savestate requests, settings schedule, renderer frame
  begin) and the UI submission plus presentation hand-off; their guest memory reads take the
  lock themselves. The PICA submission is not released: its draw loop signals GSP interrupts
  through the host services between draws, and `PresentVisualFrame` reads the GSP framebuffer
  state. The second core (`NativeA32Process::RunSecondCore`) takes it for its SVCs, scheduling
  and DSP frames and sleeps on its condition variable while it has nothing to do;
  `ResumeThread` (signals, arbitration, timers, DSP/GSP interrupts from either side) wakes it.
- **Guest memory** is shared; plain loads and stores stay plain (inline header paths unchanged).
- **Exclusive monitor** (`memory.cpp`): with the switch on, LDREX marks the core's 8-byte granule
  and returns the value as the token; STREX succeeds only while the mark survives (another core's
  exclusive store clears it) and a host compare-and-swap still finds the value (dynarmic's
  ExclusiveMonitor). Pages without a fast entry take the same steps through the checked paths.
  A context switch on a core clears its reservation. `ArbitrateAddress` reads (and for
  DecrementAndWaitIfLessThan decrements) the word with an exclusive pair. `WriteGeneration` and
  `RangeWriteGeneration` take store cards with an atomic exchange.
- **Barriers**: the 3DS's ARMv6K code uses the CP15 DMB/DSB, which the generator lowers to a
  seq_cst fence; ARMv7 `dmb/dsb/isb` encodings are no-ops (the game cannot contain them). Every
  STREX/LDREX/SWP is a seq_cst host atomic and every SVC crosses the kernel mutex
  (acquire/release), so the orderings the game builds on (barrier, exclusive pair, kernel object)
  hold on the host.
- **World stop**: page-table rebuilds (`MapRegion`, TLS page of a 9th thread, assignments) and
  savestates wait until the other core is at a dispatch boundary (it then holds no page pointer)
  and outside an SVC; one core owns a stop at a time.
- **DSP** (since 06/11 b; logs13sc: core 0 waited 6-7 ms per frame in the Forest Temple's rooms
  for the sound thread to park before each frame, and the time-stretch fill and conceals went
  up): with the host audio stream (the console's default) the **second core delivers the DSP
  frames itself**, as its idle work (`NativeA32Process::SetSecondCoreIdleWork`,
  `DeliverDspAudioFrameOnSecondCore`): one frame each time it has nothing else ready — the
  sound thread back in its wait, no thread above the main thread's priority ready or running on
  core 0, a frame pending — so the in-place HLE mix never meets the sound thread and each
  frame's interrupt is consumed before the next (a frame waits, never drops). It mixes, signals
  the DSP interrupt, updates the DSP statistics and submits to the host audio stream with the
  kernel lock held throughout with `config/kernel_retake` `0`; by default the mix and the submit
  run with it released (section "Kernel-lock retake: the DSP mix without the lock" below); a
  mix error becomes the game thread's next `Run()` fault. The game thread only adds
  the refresh's frames and notifies (no pump, no flush, no wait: `dsp_wait_ms` 0); a core-0
  thread above the main thread's priority that waits or exits notifies too. The frames go out as
  soon as they are pending, where the switch-off path delivers them at the same point (or, with
  `config/audio_deferred`, after the presentation). Without the host audio stream (SDL player
  and its resampler on the game thread) the previous pump stays: frames delivered between the
  game thread's `Run()` calls once the second core is idle, the rest at the deferred points
  after a bounded (50 ms) wait (`dsp_late` counts the timeouts).
- **Per thread**: each host thread builds its own `Oot3dWholeAotContext`; the compiled-function
  and whole-AOT stats are `thread_local` (the game thread's are reported). The second core runs
  with no block-entry hook, product mod or native-function callback: the projection policy, the
  guest profiler, TopScreen routing and `--profile-a32-runtime` stay on core 0 (the guest profile
  no longer includes thread 6). Faults on the second core are returned by the next `Run()`.
- **Placement** (`oot3d_switch_thread_layout.h`, role `guest_core1`): core 1 at **0x2D**, below
  host and SDL audio (0x2B), above the PICA plan worker (0x2E: it was preempting the sound thread,
  whose wall time on core 1 was 8.0-8.4 ms per frame for ~4.2 ms of CPU time, while the plan
  worker idles 26-31 ms per frame), at render's priority (on core 2), above the time-sliced
  service threads (0x3B). Game 0/0x2C, render 2/0x2D, plan 1/0x2E, audio 1/0x2B unchanged.
- **Savestates** made with the switch on carry `processor_id` per thread (others unchanged); a
  thread saved inside an SVC restarts it.
- **perf.txt**: `guest_threads:` charges thread 6's host-thread time (guest code + its SVCs) to
  its id and adds `core1_ms=` (all second-core time) and `core1_lock_ms=` (its waits for the
  kernel lock); `run_ms` stays the game thread's time inside `Run()`. Own line
  `second_core: dsp_pumped= dsp_flushed= dsp_late= dsp_wait_ms= dsp_core1= dsp_core1_ms=
  core1_lock_wait_ms= core1_preempted_ms= core0_saved_ms=` per host frame: `dsp_core1` the
  frames the second core delivered and their ms; `core1_lock_wait_ms` every wait of its host
  thread for the kernel lock (after dispatches, in SVCs and memory paths); `core1_preempted_ms`
  its busy wall time minus those waits minus its thread CPU time (`svcGetInfo`
  `InfoType_ThreadTickCount`, sampled each time it goes to sleep; `CLOCK_THREAD_CPUTIME_ID` on the
  host; `na` where unknown or in the first window): time other threads of core 1 took from it;
  `core0_saved_ms` its CPU time minus `dsp_wait_ms`, the work taken off core 0.
- **Not made safe** (inline header paths, no shard rebuild): a page-pointer flip on core 0
  (`ObserveWrites`, `DeferWrites`) can meet a second-core store through the old pointer; store
  marks from both cores share the `mWritten` byte (plain stores, cache-line traffic); the guest
  override call counters are not atomic (diagnostics only). (`config/indirect_profile` records
  safely from both cores since the generator round of 11/08.)
- **Tests**: `oot3d_native_a32_second_core_tests` (monitor rules and two host threads, guest
  LDREX/STREX counter, event ping-pong, arbitration, sleep timeout, fault, savestate, scheduler
  exclusion, switch absent; DSP frames on the second core: never with the sound thread running,
  each consumed (none merged), the game thread running guest code meanwhile; cadence: with a slow
  main thread and host work in each refresh, what each frame waits for besides the sound thread's
  own work (delay after its refresh minus that work) stays within 5 ms of the switch-off
  schedule's (measured 0.2-0.7 ms; a frame held behind the main thread or the host work would
  wait 30 ms or more); switch absent the idle work never runs; an idle-work error reaches `Run()`;
  the `second_core:` line); `--stress` for the long runs. TSan: configure a host tree with
  `-DCMAKE_CXX_FLAGS='-fsanitize=thread -O1' -DCMAKE_C_FLAGS=… -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread`
  and run the target with `--stress` under `setarch "$(uname -m)" -R` (TSan rejects the
  high-entropy ASLR mappings; in a container that needs `--security-opt seccomp=unconfined`).

### Guest kernel: time per thread split, quiet wakes, cheaper SVCs (`config/kernel_wake`, `config/kernel_svc`, 11/07)
The test.10 guest profile (`logs18b`) charged 11–27 µs to each SVC stub (ArbitrateAddress
`0x0030E280` 19.5 µs, WaitSynchronizationN 27 µs, SleepThread `0x004221C8` 11.7 µs, LightEvent's
inline ArbitrateAddress `0x0030B384` 11.5 µs), several times what the host services do, and with
the second core on `core1_lock_ms` was ~5 ms per host frame.
- **Cause** (second core on): every guest exit of the game thread (`LeaveGuest`: each SVC, block
  limit and fallback) and every `ResumeThread` notified the kernel condition, on which the idle
  second core sleeps most of the frame. It woke, queued for the kernel lock the game thread holds
  through the SVC, got it when the game thread released it to re-enter guest code (Horizon
  mutexes hand ownership to the waiter), so the game thread's next exit waited for core 1 to be
  scheduled, re-test its predicate and sleep again. Host test (`TestQuietWakes`, Debug, 4 runs):
  218–348 such wakes per 2000 game-thread SVCs and 9.5–18.4 µs per SVC round trip off, 0 wakes
  and 3.9–4.9 µs on.
- **`config/kernel_wake`** (absent = on, `0` = the previous path; desktop
  `TRIAEVUM_KERNEL_WAKE=1` or `TRIAEVUM_KERNEL_FAST=1`; only matters with `config/second_core`):
  a core's guest exit and the end of a second-core SVC notify only while a `WorldStop` waits
  (`gStopWaiters`; only its predicates read `gInGuest`/`gInKernel`), and the game thread readying
  one of its own threads does not notify (no waiter's predicate becomes true by it: the second
  core's idle work only waits longer). Everything else still notifies: a second-core thread
  readied, DSP frames queued, a core-0 thread above the main thread parking, stop requests,
  faults. Notifications only make a host thread re-test its predicate, so the guest sees the same
  scheduling and results. `perf.txt` `second_core:` gains `core1_wakes=` and
  `core1_empty_wakes=` (wakes of its idle wait per host frame, and those that found nothing).
- **`config/kernel_svc`** (absent = on): ArbitrateAddress and SleepThread no longer build their
  SVC event's name per call (4–6 allocations): the event keeps its numbers and the history writes
  the same text when read (`NativeA32CtrSvcEventName`, also in the unserved-request fault);
  `LookupHandle` goes through an 8-entry cache valid only while the handle table is unchanged
  (every create/close moves its version on), so it returns the scan's first match. Always:
  WaitSynchronizationN builds its name in place and keeps its handles on the stack.
- **`perf.txt` `guest_kernel:`** (always, after `guest_threads:`), per host frame:
  `svc_calls= svc_ms= lock_ms=` (the game thread's SVCs and kernel-lock retakes), then for each
  thread that ran `<id>:guest=,svc=,lock=,sched=,calls=` (ms in its guest code, in its SVCs' host
  services, retaking the kernel lock after guest code, the scheduler's rest; on the second core
  `svc` is the whole exit and `lock` lies outside its `guest_threads` time), then
  `top=<imm>:<calls>x<us per call>us,…` (the game thread's six costliest SVCs). Cost: two or
  three `cntpct_el0` reads per dispatch and two per SVC.
- **Guest thread 4 stays on the game thread.** It is most likely nngx's GSP interrupt receiver
  (`ReceiverThreadFunc` 0x00401C00: its stack top 0x005B6108 is the thread setup's
  `object + 0x1078` for the nngx object next to 0x005B50B0; it waits on the interrupt event,
  clears it, drains the queue with LDREX/STREX and runs the registered callbacks under a
  recursive lock, then signals waiters with ArbitrateAddress). Moving it to core 1 was not done:
  - it asked for processor -2, the process's own core: on the 3DS it never runs beside the main
    thread, it preempts it. Run concurrently, every piece of game state its callbacks share with
    the main thread could interleave in ways the game never met; the result would depend on host
    timing and could not be shown identical (thread 6 asked for processor 1 and was written for
    that);
  - the second core runs without block-entry hooks, product mod and native-function callbacks
    (projection policy, TopScreen routing, PICA composition hooks, guest profile); renderer code
    reached through its callbacks would skip them;
  - its work is SVC-dense (wait, clear, signal per interrupt, GSP requests from callbacks) and on
    the main thread's critical path (the main thread waits for its signals), while core 0 holds
    the kernel lock for all its host work: core 1 already waits ~5 ms per frame for it.
  The `guest_kernel:` line shows its guest/SVC split on the console.
- **Tests**: `oot3d_native_a32_second_core_tests` runs every two-core test with
  `kernel_wake` off and on (same guest results) plus `TestQuietWakes` (2000 game-thread SVCs: 0
  idle wakes of the second core with it on); `oot3d_native_a32_ctr_host_tests` runs every test
  with `kernel_svc` off and on, and `TestFastSvcEquivalence` drives the same random sequence
  (ArbitrateAddress of every type and invalid ones, sleeps, WaitSynchronizationN, events, handles
  closed and created between lookups) through both and compares results, registers, guest state,
  kernel state and the SVC history after every call; `oot3d_native_a32_process_tests` checks the
  split and the `guest_kernel:` line.

### Guest profile written off the game thread (`config/guest_profile`, 11/07)
`logs18b`: every 900 host frames the guest profile's report (walk of ~1 M code slots, sort,
formatting, SD write) ran on the game thread right after the `FrameEnd` mark: 0.41–0.47 s
frames, and one of 3.36 s that tripped the hang watchdog (`hang.txt`, `progress=6301` = the
7th dump). Now:
- At a dump the game thread swaps its counters with the writer's zeroed set (two vector swaps),
  copies the frame-level totals and formats the override counters' appendix, then hands over.
  The **writer thread** (a pthread, so a service thread at 0x3B on cores 1–2) folds the counters
  into its totals (`window=0`; with `window=1` it reports them alone), zeroes them for the next
  swap, sorts and writes the same report as before.
- While the writer is still on the previous report (a slow SD write) the dump waits for a later
  frame instead of blocking (`try_lock`); the session's end writes the last counters.
- The function map is read once at `Start` (boot), shared read-only by the writer and the spike
  profile. Memory: one more counter set (12 MB) while the profile is on.
- Test: `oot3d_native_guest_profile_tests` (cumulative and window reports, the appendix, the last
  report at the end; a report stuck on a FIFO nobody reads: the next dump returns at once and
  goes out later).

### fastmem window, phase 1: `config/fastmem_window` (11/06; on by default since phase 2)
Phase 1 of fastmem (one load or store into a flat window instead of the page-table walk; ~11
AArch64 instructions and three chained loads per guest load today). Phase 1 only lays the window
down, **with no shard-visible change** (`oot3d_native_a32_memory.h`, the whole-AOT runtime header,
generated headers, `upstream/` and the generator untouched: the 256 shards and the PGO profile
stay valid) and **no speed gain expected**: the inline paths still walk the page table.
- **Window** (`oot3d_switch_guest_window.{h,cpp}`): `virtmemFindAslr` + `virtmemAddReservation` of
  4 GiB + 2 MiB, base rounded up to 2 MiB. Every region of the process's image (text, read-only
  data, data+bss, heap and linear commits, primary stack, GSP/HID shared memory, VRAM, DSP RAM,
  config memory, shared page, TLS pages) lives at `base + guest address`, backed by 2 MiB slabs
  mapped on demand and never unmapped: `memalign(2 MiB)` heap lent with
  `svcMapProcessCodeMemory` straight to the window address, then `svcSetProcessMemoryPermission`
  RW; if the kernel refuses, the shadow route of libnx's `JitType_CodeMemory`
  (`svcMapProcessCodeMemory` to a code-region shadow, `svcMapProcessMemory` of it RW at the
  window address). The first slab's route sticks (`route=` in the log line). Lent heap cannot go
  back to hbloader, so the first slab sets `__nx_applet_exit_mode = 1` (as the code pages do).
- **`Region::Bytes` stays a `std::vector`**: `GuestWindow::Reserve` arms one placement (thread,
  size, address) and calls `reserve(size)`; the newlib wrappers in `oot3d_switch_guard_alloc.cpp`
  (`_malloc_r`/`_memalign_r`) hand the armed window address to exactly that allocation, and
  `_free_r`/`_realloc_r`/`_malloc_usable_size_r` recognise window buffers (the slab stays, the
  placement ends). A placement never overlaps a live one: an image holding window buffers
  (moved out of the owner) makes the owner's new region an ordinary heap buffer (`conflicts=`).
- **Owner**: the newest `NativeA32Process` claims its image; a bare `NativeA32Memory` (tests)
  claims the window with its first region while no buffer lives there. Copies of the owner
  (savestate snapshots, `CreatePrimaryThread`'s staged image) are ordinary heap images.
  **Re-homing**: the owner's copy assignment copies straight into the window, its move
  assignment (from `CreatePrimaryThread`, `RestoreState`, the process savestate restore) copies
  every buffer that arrived from elsewhere to `base + address` and rebuilds the page table. All
  of it and every new slab happen under the second core's world stop (`MapRegion`, the
  assignments), so two host threads stay safe.
- **Flat tables** (phase 1 only; phase 2 replaced them with per-image tables in
  `NativeA32Memory`, next section): page flags derived from the typed page entries at every
  rebuild and pointer flip, and cards set wherever `memory.cpp` set a group card, under the
  invariant "flat dirty implies group dirty". The file's text `verify` re-derives the flag table
  after every change and at each savestate capture (`verify checks= mismatches=` in the log line).
- **Switches**: `config/fastmem_window` (desktop/host: `TRIAEVUM_FASTMEM_WINDOW=1|verify`, honoured
  only where the allocator hook is linked). In phase 1 the file turned it on; since phase 2 the
  window is on by default on Switch and the file's text `off` (or `0`) turns it off. Off = no
  reservation, no hook fires, every region allocated exactly as before. No process handle
  (emulators), no 4 GiB of address space, or any refused slab mapping: off (or no new placement
  from then on), and the reason is logged.
- **Log line** (game log, warning level, once the image is mounted):
  `fastmem_window: on base=0x… regions=N mapped_kb=… slabs=… region_kb=… route=direct|shadow
  access=window` or `fastmem_window: off (reason) access=slow` (`access=` since phase 2: whether
  the game's image takes the inline window paths).
- **Memory**: the regions move into the slabs (no second copy); the overhead is the 2 MiB rounding
  of each slab group (~8–12 MiB with the game's layout: code/data 3 slabs, heap and linear commits,
  1 slab for a 16 KiB stack, the shared memory, VRAM, the DSP/config/TLS slab) plus the two 1 MiB
  tables; the 4 GiB reservation is address space only. Transient copies (savestate, primary
  thread staging) are as before.
- **Tests**: `oot3d_switch_guest_window_tests` (host backend: `mmap` reservation, `MAP_FIXED` slabs,
  operator new/delete hook in `oot3d_switch_guest_window_host_alloc.cpp`): the game's layout at
  `base + address` (15 slabs), re-homing after copy, move and `RestoreState`, conflicts with a
  moved-out image, a refused slab (today's allocation, reason in the line), the flat tables
  through observe/defer/watch/trace/cards with 0 mismatches, the process (staging, a ninth
  thread's TLS page, savestate restore), two host threads counting with LDREX/STREX while the
  game thread maps new slabs and restores a savestate after every run (no update lost),
  switch absent; `--stress` for the long two-core run. The memory, process, CTR host, second-core,
  savestate, FS and PICA submission tests link the host hook and pass with
  `TRIAEVUM_FASTMEM_WINDOW` unset, `1` and `verify` (each prints `fastmem_window at exit:
  placed=…`); the second-core and window tests also under TSan `--stress` with the window on.
- **Console proof**: `fastmem_window: on …` in the log; a save state and its load (no fault, same
  game); `config/second_core` on and off; `verify` once (mismatches=0).

### fastmem window, phase 2: the inline paths access the window (11/06, ABI 5)
Phase 2 changes the shard-visible memory header (`oot3d_native_a32_memory.h`,
`WriteObservationAbiVersion` 5): **every one of the 256 shards and every runtime object that
includes the header must be rebuilt** (no regeneration). **Without the window every typed guest
access takes the slow path**, so on Switch the window is now on by default: no file, or any text
but `off`/`0`, turns it on; `verify` adds the flag-table checks.
- **Inline load/store** (`ReadFast`/`WriteFast`/`ReadWordsFast`/`WriteWordsFast`): one flag byte
  per 4 KiB guest page in a flat table of the image (`GuestPageFlag`, own alias set like
  `GuestCard`; `kGuestPageLoadFast`, `kGuestPageStoreFast`), the page-crossing test of 2/4/8-byte
  accesses, then the access itself at `mWindowBase + address`. No page-table walk. A flag allows an
  access only when the page entry's own pointer is the page's window address
  (`DerivePageFlags`), so every page the page table traps (unmapped, read-only for stores,
  observed, deferred, watched, traced) traps inline too and reaches the unchanged slow body
  (`ReadFastSlow`/`WriteFastSlow`: page table, then the checked path). An image that is not in
  the window (file says `off`, emulator, refused mapping, savestate snapshots and other copies)
  points at `sNoFastPages`, a shared table of zeros: everything traps.
- **AArch64 per access** (shard 064, devkitA64 -O3, Cortex-A57 tuning), word load:
  phase 1 `and, cmp, b.hi, mov #80, lsr #20, add, ldr group, cbz, ubfx, add, ldr entry, cbz,
  ldr value` (13, three chained loads); phase 2 `and, cmp, b.hi, ldr flags, lsr #12, ldrb flag,
  tbz #0, ldr base, ldr value` (9; the value hangs on two loads, the flag on a predicted branch).
  Word store: phase 1 the same walk to `Write` + `str` + `strb` card + `strb` written mark (15);
  phase 2 `and, cmp, b.hi, ldr flags, lsr, ldrb flag, tbz #1, ldp base+cards, str value, ldrb
  card, cmp #3, b.ne` (12; the card store is out of line). Byte accesses drop the crossing test.
- **Cards** (`GuestCardState`, two bits): one flat byte per page and image (1 MiB, allocated
  with the first region). Bit 0 is taken by `RangeWriteGeneration` (the page's certificate), bit 1
  by `WriteGeneration` (the architectural generation), each with an atomic and-fetch, so a racing
  store's mark survives. A store makes the card `Dirty` only when it is not already: after the
  first store of a page the card line is only read (shared by the two cores, no bouncing).
  `mWritten`, the memory-wide written byte every store wrote, is gone; `WriteGeneration()` scans
  the cards of the mapped pages instead (8 bytes at a time; savestates and copies only).
- **Exclusives**: LDREX/STREX/SWP always use the global exclusive monitor
  (`oot3d_native_a32_second_core.h`), with the second core on or off: the token is the loaded
  value, STREX a compare-and-swap on the guest word. A guest context switch drops the reservation
  (`ClearExclusive`), every savestate restore resets the monitor. The difference from the
  single-core path of phase 1: a plain store between LDREX and STREX that leaves the word's value
  unchanged no longer fails the STREX (as with the second core since it exists); a write anywhere
  else no longer fails it either.
- **Window off = slow path.** With the file saying `off`, on PC builds (the allocator hook is
  linked only into the host tests) and on emulators, every typed access calls `ReadFastSlow`/
  `WriteFastSlow` (a call, the page-table walk, then the access). See the measurements below.
- **Measurements.** Shard 064 (same flags): 82,526 → 83,175 AArch64 instructions (+0.8%, cold
  code: 1,327 → 1,500 slow-path calls and the out-of-line card stores of its 618 store sites; the
  hot path of each access is shorter); `ldr*` 16,389 → 18,797 (flag byte, base and card loads),
  `str*` 9,434 → 9,159 (no written mark), `ldp` 332 → 803 (base and cards in one pair). Host
  micro-benchmark of the real `NativeA32Memory` (x86, -O3, median of 3; phase 1 / phase 2 window
  on / phase 2 window off):
  64 word stores 117.7 / 61.3 / 259.7 ns, 64 independent word loads 84.4 / 36.3 / 328.2 ns, 64
  dependent loads on new pages 378 / 152 / 561 ns, `VPUSH {d8-d15}` word by word 39.1 / 28.2 /
  69.6 ns, as one span 8.1 / 6.0 / 80.2 ns, `VPOP` word by word 33.7 / 24.5 / 90.0 ns, as one
  span 7.1 / 6.2 / 87.9 ns. With the window on, guest loads and stores cost about half of
  phase 1; off, two to twelve times phase 1 (spans never take the inline path off).
  **Expected on the console** (an estimate, not measured): the page-walk sequences are ~19% of
  shard 064's instructions; if they are 15–20% of guest time in heavy rooms, halving them saves
  ~7–10%, about 2–3 ms of the 25–30 ms of guest logic per 30 Hz step. With the window off the
  same arithmetic costs +20–50% of guest time, hence on by default.
- **Log line**: `fastmem_window: … access=window` (the game's image takes the inline window
  paths) or `access=slow`.
- **Exactness**: `memory_lean_diff/run_store_sequence_diff.sh` (the generated-code store
  sequences against phase 1 e4280d76, whose exclusives run with its second core active; host and
  `TARGET=switch` under qemu-aarch64 with `qemu_mman.cpp`, `WINDOW=1` and `0`),
  `run_memory_lean_diff.sh` (2,000,000 random operations, window on and off) and
  `run_guest_override_equivalence.py` (the translated side in the window); the memory, process,
  CTR host, second-core, savestate, FS, PICA submission and window tests with
  `TRIAEVUM_FASTMEM_WINDOW` unset, `1` and `verify`; the second-core and window tests under TSan
  `--stress`, window off and on.
- **Rebuild**: the 256 `oot3d_whole_aot_shard_*.o` and the 61 other objects that include the
  header (among them `oot3d_whole_aot_generated.o`, the guest overrides, the process and memory,
  the CTR host services, savestate, the window/setup objects, PICA submission and transfer, the
  typed bridges and the top-screen UI), plus `oot3d_switch_guest_window.o` (its source changed);
  a PGO build needs a new collection (the inline paths of every function changed).
- **Console proof**: rooms 0, 2 and 8 of a heavy scene with the file present, `access=window` in
  the log; `config/second_core` on and off; a save state and its load; `guest_ms` in `perf.txt`
  against the same rooms without the file (`access=slow`) and against phase 1.

### Real fps60 and GPU savings in the GL backend (2026-10-17)
- **fps60 did not present at 60 Hz**: the pacer and sample cadence come from the `FrameRate` mode of the
  graphics settings (default `Original30`, chosen in the PC F1 menu); `--presentation-rate 60`
  only entered the contract and never reached the pacer. With `config/fps60` the console kept presenting
  30 frames/s with two 60 Hz refreshes each and never drew the interpolated sample. Now, on
  Switch, `ResolveLaunchFrameRateMode` (`oot3d_native_launch_frame_rate.h`) converts the launch
  timing+rate into the pacing mode (`native30_interpolated`+60 → `Interpolated2x`; +30 →
  `Original30`, same as the previous default). Since 06/11 the 30 FPS launch is also
  `native30_interpolated` (see "FPS: L3+R3 > Video, applied at once"). Test:
  `oot3d_native_frame_rate_tests.cpp` (`triaevum_presentation_clock_tests`).
  Caution: the interpolated sample redraws the whole frame (twice the GPU); at 720p it does not fit.
- **`config/gl_lean`** (on by default; `0` disables for A/B), exact GL backend savings:
  1. the display transfer copy that presentation resends (the sample already executed the same copy)
     is skipped when the source target has not changed (per-target version); at 720p it avoids reading+writing
     ~3.7 MB per presented frame;
  2. clearing the window before scanout is skipped when the scanout covers the whole window
     (16:9 widescreen);
  3. the Shadow2D image (R32UI, target-sized) is only cleared when a shadow producer/consumer
     uses it — previously each color fill cleared ~3.7 MB (top) + ~2.8 MB (bottom screen) per
     frame at 720p even without shadows;
  4. uniform blocks equal to the previous one are not resent (nvc0 pushes the constant
     buffer upload through the FIFO, word by word) and the unit 5 image is only rebound when it changes.
- **Bounded GPU memory** (crash after ~8 min with fps60: nouveau failed to map a
  buffer and Mesa copied a draw's uniforms to NULL inside `glBufferData`):
  1. the PICA texture cache (by content) never evicted — each animated texture created a
     new GL texture every frame. Now anything unused for 1800 frames is evicted and, above the
     budget (`config/gl_texture_budget_mb`, default 96, 16–2048), the least recently used
     (never one used by the current/previous frame). Lighting LUTs follow the same idleness
     rule;
  2. per-draw uniforms, vertices and indices go to 8/4/1 MB rings (unsynchronized map,
     ranges that never overlap within a generation), orphaning only when the ring wraps — previously
     it was one `glBufferData` (reallocation) per draw;
  3. allocation/mapping failure (`GL_OUT_OF_MEMORY`, null map) is no longer fatal: it evicts all
     idle textures and skips the draw (counted in `gl_oom`/`gl_draws_skipped` in perf.txt;
     stderr with `TRIAEVUM_GL_OUT_OF_MEMORY` does not reach the console log);
  4. `logs/perf.txt` shows `gl_textures_live`, `gl_tex_mb`, `gl_tex_created/evicted`,
     `gl_luts_live`, `gl_programs`, `gl_targets`, `gl_buffers_live`, `gl_buffer_mb`,
     `gl_ring_orphans`, `gl_oom`, `gl_draws_skipped`, `heap_used_mb` and `proc_used_mb`/`proc_total_mb`:
     all should stay stable over a long session. Test:
     `runtime/three_ds_recomp/tests/gfx_opengl_pica_residency_tests.cpp` (30 simulated min).
- **Fractional scale**: `config/resolution_scale` accepts `2.5` (or `2,5`; multiples of 0.25). The
  targets become 1000×600 (1067×600 with widescreen) and FSR EASU+RCAS upscales to 1280×720: ~69% of the
  pixels of 3×. Viewport/scissor/copy edges = round(native edge × scale), so neighboring
  rectangles still fit together; with an integer scale the math is the same as before. Tests:
  `runtime/three_ds_recomp/tests/gfx_opengl_pica_scale_tests.cpp`.
- At 3× EASU was already skipped (1:1 output); only RCAS runs (`config/sharpen` `0` swaps it for a simple
  copy). `config/anisotropy` `2` reduces texture cost on oblique surfaces.

### GL state cache in the PICA draw path (2026-10-17)
- Each draw resent all fixed state (~27 calls: framebuffer + draw buffer, VAO, program,
  4 texture units with `glActiveTexture`, color mask, blend/logic op, depth, stencil, cull,
  viewport, scissor) and then rebound the legacy path's VAO. Mesa filters almost all
  repeats, but each one still pays dispatch + context + enum validation on the render thread; and the
  VAO back-and-forth was not a repeat: it made the state tracker rebuild the vertex arrays on every
  draw (twice on the interpolated frame).
- `PicaGlStateCache` (`gfx_opengl_pica_state_cache.h`, no GL, tested in
  `gfx_opengl_pica_state_cache_tests`) keeps the last value of each state and only emits the call that
  changes something. The legacy VAO is now rebound on demand (`RestoreLegacyVertexArray`, in the
  legacy functions that use vertex arrays). Draw order, values and draw calls do not change.
- The cache only holds between consecutive PICA draws: it is forgotten at the start of each execution list
  (`PublishPicaCompositionSequence`), on any PICA entry that is not a draw (transfer, fill,
  clear, presentation/post), on any legacy path call (`MarkPicaGlStateForeign`) and
  when the legacy caches show it has drawn. Within the draw, creating program/target/texture,
  fills and Shadow2D clears forget only the state they touch.
- Side effect fixed: the new lighting LUT was created (bound) on unit 3 after
  binding the textures, replacing the slot 2 texture with it for that draw; now it is fetched beforehand.
- A/B: `config/gl_state_cache` with `0` reverts to the old behavior (all calls + rebinding the
  legacy VAO). `config/gl_stats` (file exists, even empty) enables in `perf.txt`, per host
  frame: `gl_draws`, `gl_presents`, `gl_state_calls` (emitted), `gl_state_skipped`,
  `gl_state_calls_per_draw`, `gl_program_binds`, `gl_texture_binds`, `gl_vao_binds`, `gl_fb_binds`,
  `gl_ubo_uploads`, `gl_draw_ms` (render thread time inside `SubmitPicaDraw`) and
  `gl_us_per_draw`. The counters run in both modes, so the same build measures before and after.
- Synthetic 400-draw frame (test): 27.5 → 2.7 state calls per draw.

### Per-stage GPU/CPU profile: `config/gpu_timing` (2026-10-17)
- File `sdmc:/switch/oot3dre/config/gpu_timing` (optional content: N, sample the GPU 1 frame
  every N; default 30). Desktop: `TRIAEVUM_GL_GPU_TIMING=30`. Without the file nothing is installed: each
  marker point costs one atomic load and a branch.
- Each 150-frame window of `perf.txt` gains a `glperf host_frames=... ` line with averages per
  host frame type: `step` (runs a new game frame), `interp` (only the repeated /
  interpolated fps60 sample) and `hold` (re-presentation). Keys `<type>_gpu_<stage>` (ms per sampled
  frame), `<type>_gpu_ms` (frame start → after swap), `<type>_cpu_<stage>` (render
  thread ms per frame) and `<type>_n_<counter>` (per frame; `*_kb` in KiB).
- Stages: `begin`, `clear` (window, PICA targets, fills), `pica_top`/`pica_bottom`/`pica_off` (PICA draws
  of the displayed sample per target: top screen, bottom screen, others such as shadows/RTT), `pica_canon`
  (canonical, non-displayed execution of a new frame), `transfer` (display transfers), `post_aa`,
  `post_easu`, `scanout` (last pass to the window: RCAS, direct EASU/FXAA or copy), `ui` (HUD/menus
  TopScreen), `present` (EndFrame + swap); CPU only: `other`, `pacing`, `idle`.
- GPU: `glQueryCounter(GL_TIMESTAMP)` at each stage change in the sampled frame (pool of 4×128
  queries); read 2+ frames later with `GL_QUERY_RESULT_AVAILABLE`, never blocks (no result,
  the sample is discarded: `gpu_dropped`). Difference between timestamps = GPU execution **plus**
  waiting for CPU commands within the frame: if the render thread is the bottleneck, `gpu_*` includes
  idle time.
- Counters (hooks on the glad pointers, installed only with the file): draws, clears,
  `fbo_binds/fbo_changes`, `tex_binds`, `prog_binds/prog_changes`, VAO/UBO binds, loose uniforms,
  bytes sent (`buf_upload_kb`, `map_kb`, `tex_upload_kb`), `buf_reallocs` (glBufferData),
  blits, `barriers` (glMemoryBarrier), `syncs`, `finish`, `flush`, `get_error`, `gets`
  (glGet*/glIsEnabled/glCheckFramebufferStatus), `readbacks`, `gen_delete`, `enables`.
- Cost when enabled: one indirect call + increment per counted GL call, one clock read per
  stage change and ~20–60 timestamps in the sampled frame.
- Memory: `mallinfo()` (locks newlib's malloc, shared with Mesa, while it walks the
  free lists) now only every 20 windows (`heap_used_mb` appears only in those); `heap_arena_mb` and
  `heap_arena_peak_mb` come from the totals malloc itself maintains (no lock) in every window.
- Summary: `python3 tools/oot3d/perf_report.py perf.txt [--session -1] [--skip 2] [--windows]` —
  fps per session, clocks, phases, memory and, with `glperf`, top GPU stages, render thread CPU and
  counters per frame type.

### Clocks in each `perf.txt` window (2026-10-17)
- Each `host_frames=` line gains `clocks: cpu_mhz= gpu_mhz= mem_mhz= clk_src= op_mode=
  perf_mode= sysclk_profile= sysclk_enabled= sysclk_api= clk_ipc_us=`
  (`oot3d_switch_clocks.cpp`). Always compare measurements at the same clock; `perf.txt` now
  records which clock each window was measured at.
- MHz source: sys-clk public IPC (`sys:clk`, command `GetCurrentContext`, the same one the
  overlay uses), `realFreqs` (measured) when API ≥ 2, otherwise the target (`clk_src=sysclk`). The session is
  opened on the first window and kept; beforehand, Atmosphère's sm is queried
  (`AtmosphereHasService`, 65100) because `smGetService` on an unregistered service blocks waiting.
  Without sys-clk: the stock clocks of the performance configuration HOS applied
  (`appletGetCurrentPerformanceConfiguration`, AM, 7.0.0+), mapped by the switchbrew table
  (PTM services, PerformanceConfiguration) in `oot3d_switch_clocks.h`: `clk_src=stock
  perf_cfg=0x00020003` (an ID missing from the table keeps MHz -1); `clk_src=none` only if that also
  fails. sys-clk is retried once a minute. Cost: the reads run on the clock reader thread (one per
  perf window, plus one per half second while the overlay is on); the game thread only copies the
  last result.
- Read-only and no clkrst/pcv in the app (SwitchWakerHD issue #33: clkrst in the app hung the console
  on wake from sleep); clocking remains sys-clk's job.
- `op_mode` (handheld/docked) and `perf_mode` come from `appletGetOperationMode`/`appletGetPerformanceMode`
  (values cached by libnx, no IPC) and work even without sys-clk.
- `perf_report.py` shows each session's clock; if it changes midway, it warns (window, before → after) and
  splits fps/`total_ms` by clock; `--windows` lists `cpu/gpu/mem mode` per window.

### Area ids and actor costs in `perf.txt` and `hitches.txt` (2026-11-05)
- **Area, always on.** Once per host frame the game thread reads the current game state
  (`0x005043E0`); when it is the PlayState (u8 `+0x100` = 3) it reads the scene id (s16 `+0x104`)
  and the current room (s8 `+0x4C30`) (`oot3d_native_area.h`). Cost: four guest reads per frame.
  - Each `hitches.txt` line ends with the area of its frame:
    `frame=8123 t=271.204 dt_ms=58.4 late_ms=41.7 cause=logic guest_ms=29.8 game_ms=31.0 tex_new=0
    prog_new=0 scene=0x51:spot00 room=0`.
  - Each `host_frames=` line gains, after `total_ms`, the window's area and its scene changes:
    `scene=0x51:spot00 room=0 scene_changes=0`. The area is the scene with the most frames in the
    window (frames outside gameplay count as `scene=-`) and that scene's most frequent room;
    `scene_changes` counts switches between two gameplay scenes, including one at the window's
    start.
  - Format: the scene id in hex, then `:` and the scene's file stem when the title's scene table
    names it (`kNativeSceneStems`, the table of `tools/triaevum_release/scene_lights.py`):
    `0x00:ydan` Deku Tree, `0x0D:ganontika` Ganon's Castle, `0x20:market_day` Market, `0x2A:kakariko`
    Kakariko Village, `0x51:spot00` Hyrule Field, `0x55:spot04` Kokiri Forest, `0x05:mizusin` Water
    Temple. `room=-` without a room, `scene=- room=-` outside gameplay (title, file select).
- **Actor costs, `config/actor_profile` (file exists, content ignored; off by default).** Times
  game-logic calls with the host counter (`cntpct_el0`, 19.2 MHz; `oot3d_native_actor_profile.h`)
  at block-entry hooks of the policy (`oot3d_native_a32_projection_policy.cpp`), added to the hook
  list only when the file exists:
  - Actor_UpdateAll `0x00461344` until its return `0x002E4058` in Gameplay_Update; each actor's
    update from block `0x00461784` (r4 = actor; the block calls `Actor+0x13C`) to `0x004617B4`;
    CollisionCheck_AT `0x00460548` → `0x002E401C` and CollisionCheck_OC `0x0045DC34` → `0x002E4028`;
    Actor_DrawAll `0x00461904` → `0x002E2CE0`; each actor's draw at Actor_Draw `0x002D5F68` (r1 =
    actor) → `0x00461BB8` (or `0x00461D2C`, lens pass). Every PC is an emitted whole-AOT block label;
    an entry whose return address is another caller is not timed.
  - Each window gains a line after the `host_frames=` line:
    `actors: scene=0x51:spot00 room=0 steps=75 draws=75 n=41.0 update_ms=6.12 actor_update_ms=5.30
    collide_ms=2.05 at_ms=0.61 oc_ms=1.44 draw_ms=3.80 actor_draw_ms=3.02 hooks=172 overhead_ms=0.015
    dropped=0 by_cat=enemy:3.10,player:2.40,prop:0.90 top=Player/player:2.40x1,En_Skb/enemy:1.20x4,...`
    (example values).
  - Fields: `steps` Actor_UpdateAll calls (one per game step), `draws` Actor_DrawAll calls, `n` actor
    updates per step. `update_ms`, `actor_update_ms` (the sum of the actors' own updates),
    `collide_ms` (`at_ms` + `oc_ms`) are ms per step; `draw_ms` and `actor_draw_ms` ms per draw pass.
    `by_cat` sums update + draw per actor category (`switch bg player explosive npc enemy prop
    itemaction misc boss door chest`), `top` the 8 actor ids with the most time:
    `name/category:ms x instances per step`. Names come from the decomp's actor ids
    (`oot3d_native_actor_names.h`); unnamed ids print as `0x01D7`, ids past the table as `other`.
  - Cost: `hooks` per step (~4 per actor) and `overhead_ms`, the profile's own time per step. Each
    hook also costs the guest register flush/reload and the policy call, which `overhead_ms` does
    not include; measure the whole cost by comparing `guest` with and without the file at the same
    spot. `dropped` counts intervals that could not be paired or took over 250 ms.
- **Other fixes.** `guest_in_pica` is now ms per host frame of the window like the other phases
  (it was the running total in seconds since the start). `threads:` CPU shares are measured from each
  thread's registration, so the first window of a session no longer shows `na`.

### Guest fault report in the game log (2026-11-05)
- **Why.** A player's game closed in the final battle with only `native A32 dispatch exited
  unexpectedly: kind=4 pc=0x004C0A8C detail=268439552`: a memory fault (kind 4) of a store to
  `0x10001000` in `TextTokenUtf8_ParseNext` (`0x004C08C0`). That store is the loop that copies a
  text code's argument bytes, one word each, into the 0x28-byte token on the stack; the count comes
  from the game's table of codes `0x00-0x2F` (`0x004D6708`, at most 5). Reaching `0x10001000` takes
  thousands of words, so the text had `7f` followed by a byte past the table (from `0x30` the table
  is other data, e.g. counts of `0x006F0072`): the loop overwrote the rest of the stack, the
  shared-memory page the game maps at `0x10000000` and faulted on the unmapped page after it. Which
  text, which thread and which caller the log could not say.
- **What the log now has.** On any guest fault (memory fault, unsupported exit, unserved host
  request), before the usual one-line error, `NativeA32Process::Fault` writes a report
  (`oot3d_native_guest_fault_report.h`, host side only, nothing the whole-AOT shards compile):
  the thread id and entry, exit kind/pc/detail and what the fault address is mapped as;
  r0-r15, cpsr, fpscr, TLS; which stack sp is in; a backtrace (pc, lr and the return addresses
  found on the stack, i.e. words in code right after a `bl`/`blx`); the area as numbers
  (`scene=0x.. room=..`, never a name); the other threads' pc/lr/sp; 64 bytes (hex and ASCII)
  around the fault address and around every register that points into mapped data, each with its
  mapping (`primary_stack rw [0FFFC000-10000000)`, `heap`, `shared memory`, ...); 128 bytes from
  sp; and the last three perf windows (`fps`, `guest_ms`, `present_ms`, `total_ms`,
  `gpu_busy_ms`, `gpu_fence_ms`). For this crash the registers hold the text pointers (r1 the
  code byte, r2 the read position, r8 the token start in `TextCommandStream_ExecuteUtf8`), the
  stack dump holds that function's frame (text start and length at sp+0x2C/+0x30) and the
  backtrace names its caller. Cost: none until a fault; the perf trail is one copy per perf window.
- **PT-BR patch tools.** `tools/oot3d/ptbr/msgs.py` guessed 6 argument bytes for `7f0e` (sound) and
  none for `7f0d`; the game reads 5 for both. A PT-BR sound code then took the `7f` of the next
  English code as its sixth byte, and the game read the following letter as a code past its table:
  the crash above, for any message where that happens. The table is now the game's, `7f06` takes its
  argument from the 3DS message like the other codes the N64 text lacks, and `assemble.py` refuses
  to write a message the game cannot parse (`grammar_error`; every message of the original eu.qm
  passes). Whether the player used a PT-BR romfs is unknown; the report above shows the text bytes.
- Tests: `oot3d_native_guest_fault_report_tests`, `oot3d_native_a32_process_tests`
  (TestFallbackBoundary), `tools/oot3d/ptbr/test_ptbr.py`; guards in `tests/switch/test_bug_guards.py`.

### Game code on 2 MB pages (`config/no_code_hugepages`, 2026-11-05)
- **What.** Adapted from sa2-nx (`sdk/src/ui/switch_code_blocks.cpp`, from kameo-nx and the Torchlight
  port; BSD-3-Clause as part of sa2-nx's SDK, notice in `LICENSES/sa2-nx-SDK-BSD-3-Clause.txt`):
  `oot3d_switch_code_pages.cpp`. hbloader maps the NRO at a random
  address whose virtual and physical addresses do not line up modulo 2 MB, so the kernel maps the
  133.8 MB `.text` with 4 KB pages and the game thread misses the instruction TLB constantly
  (Torchlight measured +115 ns per instruction-fetch TLB miss on 4 KB pages, +7 ns on 2 MB blocks;
  data reads +81 / +5 ns; combat 2-4% faster). The first constructor copies text and read-only data
  to heap that lines up modulo 2 MB and swaps the mapping at the same virtual address
  (`svcUnmapProcessCodeMemory` / `svcMapProcessCodeMemory` / `svcSetProcessMemoryPermission`) from a
  code page of its own, so no pointer changes.
- **Order.** The swap is constructor priority 101; the TLS mirror (`oot3d_switch_threads.cpp`) and
  guard_alloc's early start (`oot3d_switch_guard_alloc.cpp`) moved from 101 to 102. Only libstdc++'s
  iostream set-up (90) runs earlier (checked in the linked `.init_array`); it starts no thread. The
  PGO-instrumented build registers its counters in a section, not with libgcov constructors. Text and read-only data are swapped in two
  calls: a refused data swap leaves the text on blocks.
- **Exit.** After the text moved, hbloader's own unmap would fail if the NRO returned to it (hbl
  error 24). A successful swap therefore sets `__nx_applet_exit_mode = 1` for the whole process: every
  return from `main` (including declining the game data import or the HUD offer) and every `exit()`
  closes the application through AM, as Exit game and the fatal error dialog already did. Back to the
  HOME menu, not to hbmenu.
- **Cost.** The loader's copy cannot be given back (start of the heap, `.data` after it): about
  138 MB more memory (the heap peaked at 892 MB of ~3 GB in the 0.1.1 logs). It is skipped when less
  than 1.5 GB would stay free (an applet). The copy takes tens of ms at start-up.
- **Kill switch.** `sdmc:/switch/oot3dre/config/no_code_hugepages` (exists = off). Read directly in
  the constructor: libnx's `__appInit` mounts sdmc (`fsdevMountSdmc`) before `__libc_init_array` runs
  the constructors.
- **Log.** One line in `logs/oot3d_native_game.log` (warning level: the release logger drops info):
  `code_pages: remapped text_kb=… rodata_kb=… blocks_2mb=65+1 copy_ms=… swap_ms=… code=0x… copy=0x…`,
  or `code_pages: off (config/no_code_hugepages)`, or `code_pages: left on 4 KB pages: <reason>
  (<value>)`. Under an emulator (no hbloader, no process handle) the code stays put and the swap is
  tested on two pages of its own: `code_pages: no loader (emulator) … self-test passed`. Each
  `perf.txt` window has `code_2mb=1|0` in its `threads:` part.
- **Measure.** Same save, same spot (e.g. Forest Temple, the two Wolfos), one session with and one
  without `config/no_code_hugepages`; compare `guest` and `game_pair_ms` of the windows with
  `code_2mb=1` against `code_2mb=0`. Expected 2-4% of guest time or more (the text is 3.4x sa2's).
- Tests: `oot3d_switch_code_pages_tests` (alignment of the copy, block count, heap check, log line);
  guards in `test_bug_guards.py` (constructor order, exit mode, stub page, log line).

### Recompiled-code overhead (2026-11-05)
- **Flat dispatch table.** Every whole-AOT indirect call (`BLX reg`, computed `ADD pc` calls; 2,099
  sites) and every dispatcher re-entry looked its target up with `FindDispatchEntry`: a 4 KiB page
  bucket, then `std::lower_bound` over the 162,757 entries (about 168 per page, so about 8
  unpredictable compare-and-branch steps). The registry (`_render_registry_source`,
  `whole_aot_cpp.py`) now has one `uint32_t` slot per guest code word from the first entry to the
  last (`0x00100000..0x004C97AC`: 992,748 slots, 3.79 MiB of `.bss`, filled once at static
  initialization, no relocations): the slot holds one plus the entry's index, zero when there is
  none. The lookup is a subtract, a rotate (a pc that is not word aligned rotates its low bits
  above the bound), one compare and two loads, with the same entries and the same external-call
  fallback. Host check over the real entries: identical results for every byte address from
  64 KiB below to 64 KiB above the range; 66.7 → 3.5 ns per lookup over 64 hot targets and
  110 → 5.8 ns over random entries (x86 host). On AArch64 the lookup is inlined:
  `ExecuteOot3dWholeAotIndirect` reaches its tail call in 27 instructions with no stack frame,
  where it used to call the search (19 instructions, then 10 per bisection step, about 7 steps for
  the ~168 entries of a page: about 120 instructions per call with the wrapper) with spills around
  the call. Changes only `oot3d_whole_aot_generated.cpp`.
- **Indirect call profile, `config/indirect_profile` (file exists, content ignored; off by
  default).** While it is on, the dispatcher counts calls per site and target
  (`oot3d_native_indirect_profile.h`); off, it costs one load and one predicted branch per indirect
  call. The counts cover the whole session (lock-free: keys claimed by CAS, counts in per-thread
  single-writer tables, so both guest cores may record); each `perf.txt` window gains a line
  `indirect: calls=N pairs=K dropped=D top=002E4058>00461344:3000,...`, the difference from the
  previous window: the calls in the window, the distinct pairs, the calls whose pair found no slot
  (65,536), and the 16 hottest `return>target:count`.
  `return` is the guest LR at the call: the call pc + 4 for `BLX`, the resume pc for computed calls.
  It is meant to pick the targets worth a guarded direct call. A `PGO=generate` build always
  records and writes the session census beside its PGO stream (see "PGO: collecting a profile on
  the console").
- **VFP multiple transfers: one page walk.** `VLDM`/`VSTM`/`VPUSH`/`VPOP` of two or more words
  (8,062 sites in the 256 shards: 5,060 loads, 3,002 stores) walked the two-level page table once
  per word. They now take `ReadWordsFast`/`WriteWordsFast` first, like `LDM`/`STM`: one page check
  and one copy when the whole range lies on one untraced fast page (the same bytes, generation
  steps and page stamp as the word-by-word path); otherwise the previous word-by-word lowering
  runs unchanged (fault order, partial stores, lane updates). Host micro-benchmark of the real
  `NativeA32Memory`, `VPUSH {d8-d15}`: 47.7 → 8.5 ns, `VPOP {d8-d15}`: 35.8 → 7.5 ns (x86 host).
  Shard 064: 85,045 → 84,548 AArch64 instructions. Changes the generated shards.
- **Store cards instead of the global store counter.** Every guest store did load, add and store of
  `mRangeGeneration`, stored it into the page's stamp and tested it for wrap-around; consecutive
  stores were chained through that counter in memory. Now each 1 MiB page group starts with one
  card byte per page (`FastPageGroup`, `oot3d_native_a32_memory.h`) and a store does two byte stores
  of a constant: its page's card (`strb w1, [group, page]`) and the memory-wide written mark. No
  load, no branch. `GuestCard` is a one-member union of an `enum class`, so GCC gives it its own
  alias set: guest registers held in host registers are not reloaded after a card store (checked
  in the AArch64 output).
  - `RangeWriteGeneration` turns the set cards of the range into one fresh certificate (larger
    than every earlier one) when asked; a page nobody wrote keeps its certificate. Every slow or
    host write path (`WriteSized`, `WriteBytes`, `WriteHostBytes`, `Fill`, `GetWritePointer`,
    `DeferWrites`) marks the cards of every page it covers (`NoteWrite`), so a store crossing a
    page boundary marks both. Copies, moves, restores and page-table rebuilds restamp every page.
  - `WriteGeneration()` steps once when it sees the written mark: two observations differ exactly
    when something was written in between, which is all the LDREX/STREX token compares, so the
    exclusive monitor behaves exactly as before (a write anywhere, by any thread or service,
    between LDREX and STREX loses the reservation; a thread switch without writes does not). The
    value counts observations with writes instead of writes; savestates still round-trip it.
  - Guest overrides and `NativeA32UnobservedMemory` call `NoteWrite` for what they wrote;
    generated code no longer commits a region store count. `WriteObservationAbiVersion` is 4.
  - Per store on AArch64: 5 bookkeeping instructions (`ldr`/`add`/`str` of the counter, `str` of
    the stamp, `cbz`) become 2 `strb` (plus a `mov #1` that GCC shares). Shard 064:
    84,548 → 82,526 instructions. Host micro-benchmark (x86): 64 stores 191 → 162 ns,
    `VPUSH {d8-d15}` word by word 47 → 42 ns, loads unchanged. Changes the memory header: every
    shard, the guest overrides and the runtime must be rebuilt (no regeneration needed for the
    product shards).

### Performance overlay: L3+R3 > Video (`config/fps_counter`)
- "Performance overlay" checkbox in the Video tab; applies in the same frame and creates/removes
  `config/fps_counter` (present = on, the same file as the old FPS counter). Two lines in the
  top-left corner, shadowed yellow ImGui text at 1/32 of the window height:
  `59.9 FPS  max 18.2 ms  speed 100%` and `CPU 1785  GPU 768  MEM 1600  sys-clk` (MHz; `stock`
  without sys-clk, `--` when unknown).
- FPS = new images per 0.5 s window: a game step's frame or an fps60 in-between frame, each counted once
  however many refreshes it stays (a dropped in-between, which repeats the step image, is not one); `max`
  = longest time one image stayed on screen in the window (a held frame at 60 Hz reads 33.3 ms; an image
  still up at the window's end counts its time so far); `speed` = guest refreshes run / refreshes owed
  (run plus dropped by the presentation scheduler), `--` when none was owed (loading screens). Text formatted only
  when a window closes (`oot3d_fps_counter.h`, tests in `oot3d_fps_counter_tests.cpp`).
- Needs ImGui on the window: with deko3d and `config/serial_presentation` nothing ImGui draws shows.

#### FPS = new images, not refreshes (07/11, `fpstruth`)
- Before, FPS counted presented refreshes, so a held image counted twice: logs16b session 2, Sacred Forest
  Meadow (`spot05`, host frames 18150-25050) read 60 with `level=dropped step_frames=75 interp_frames=1
  interp_dropped=74` per 150 refreshes (about 30 new images/s); log16 Kokiri Forest read a median 54.4
  against about 37.
- Now the render thread counts Step/Full/Reduced frames (`newImagesPresented`, in
  `FinishRenderFrameCost`); the overlay takes the count since the previous host frame
  (`PerfOverlay::ObserveFrame(now, newImages)`). 30 FPS setting reads 30, 60 with in-betweens 60, 60 with
  in-betweens dropped ~30.
- `perf.txt` interp section: new key `new_fps` = (step + full + reduced frames) / window seconds; `fps`
  still counts refreshes, so old and new logs compare.
- Tests: `FpsCounterWindows` (30 setting, full 60, hitch, frozen picture) and `DroppedInBetweens` (the
  spot05 pattern reads 30.0 on the overlay, 30.4 over the 150-refresh window).
- Console check: overlay on, 60 FPS, Sacred Forest Meadow: reads about 30-35 FPS, `max 33.3 ms`
  (read 60.0 before); `perf.txt` there has `new_fps` ≈ 30 next to `fps=60`.

### GPU clock: L3+R3 > Video (`config/gpu_performance`, 07/11, off by default)
- "GPU clock" combo in the Video tab: **Standard (307 MHz)** (default, file absent) or **High (460.8 MHz,
  more battery)** (file present). Applied at once and at start (the Video panel is created at window setup
  and applies the file then); `oot3d_switch_gpu_performance.h`, implementation in `oot3d_switch_clocks.cpp`.
- How: the application's own apm session (`apmInitialize`, kept open for the run) and
  `apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, 0x92220008)`, the official handheld
  configuration CPU 1020 / GPU 460.8 / memory 1331.2 MHz (switchbrew PTM services). Only the handheld
  (Normal) configuration is touched: docked already runs the GPU at 768 MHz. Standard puts back what
  `apmGetPerformanceConfiguration(Normal)` reported before the first change (0x00020003 if that read
  nothing or High). Standard with no prior High opens no apm session: the default build behaves as before.
  No clkrst/pcv (the wake-from-sleep hang stays avoided); a running sys-clk overrides whatever HOS applies.
  libnx in `retuned-switch-prep:local` has `apmSetPerformanceConfiguration`/`apmGetPerformanceConfiguration`
  and the weak `__nx_applet_PerformanceConfiguration` (not used: it only applies at start and only for
  applications).
- Log: one game-log line per change, `TRIAEVUM_GPU_PERF high=1 available=1 rc=0x0 failed=- requested=0x92220008
  standard=0x00020003 effective=0x92220008 gpu_mhz=461 mem_mhz=1331 op_mode=handheld` (`effective` is
  `appletGetCurrentPerformanceConfiguration` right after). If apm refuses (`failed=apmInitialize` or
  `failed=apmSetPerformanceConfiguration` with its result code), the menu shows "Unavailable: ... refused
  (0x...)" and the clock is unchanged.
- Effect: up to −33% on GPU-clock-bound work [estimate]: Forest Temple room 8 step frames 14.9 ms →
  about 10-11.5 ms [estimate]. No CPU change; battery and heat as Nintendo's own titles that use it.
- Tests: `oot3d_fps_counter_tests` (`GpuPerformance`: the configurations' MHz, the configuration to
  restore, the file round trip, no apm off the Switch) and three guards.
- Console check (1 minute, without sys-clk): overlay on, choose High: the overlay's `GPU` reads 461 (was
  307); `perf.txt` windows show `gpu_mhz=461 perf_cfg=0x92220008`; the game log has the `TRIAEVUM_GPU_PERF`
  line. Back to Standard: `GPU 307`. Restart with High saved: the line appears at start.

### FPS: L3+R3 > Video, applied at once (`config/fps60`, 06/11)
- **Was a bug, not a limit.** "30 FPS" and "60 FPS (interpolated)" both run the game at 30 steps a
  second (60 guest refreshes, the A32 update rate 2, the same gameplay clock); only the presentation
  differs (30 Hz `Original30`, or 60 Hz `Interpolated2x` with an interpolated picture between steps).
  `Enhanced60` (60 Hz simulation) is a different mode, never chosen by the Switch launch. The pacing
  mode was fixed at launch (`launchFrameRateMode`) and the 30 FPS launch used the
  `native30_no_interpolation` contract, which forbids interpolation, so the menu could only write
  `config/fps60`. The PC's F1 menu already changed the same mode while the game runs.
- **Now:** the Switch launches `native30_interpolated` at both rates (30 → `Original30`, nothing
  interpolated). The Video tab still creates/removes `config/fps60` and publishes the choice
  (`LiveFrameRate`, `oot3d_native_launch_frame_rate.h`); `RunFrame` takes it at the next frame boundary,
  before the pacing policy is resolved, and the change goes through the path the F1 menu uses:
  - pacer (render thread): new target rate, deadline restarted (the switch frame is shown at once,
    then one new period apart; nothing owed from the old rate). With `config/pacer_vsync` (the release
    zip ships it, so it is on) the deadlines sit on the display's vsync grid and a presentation now
    lasts as many display periods as the target asks (`VsyncPeriodsPerPresentation`: 2 at 30 FPS, 1
    at 60, read at every wait). It was always one period: a 30 FPS target on the anchored pacer
    presented every vsync. Test: `oot3d_frame_spike_tests` (`TestAnchoredPacerHonoursTargetRate`:
    2 periods at 30, 1 after the switch to 60, 2 again after switching back);
  - composition and visual sample cadence; turning interpolation on or off clears the interpolation
    history (previous/latest visual frame, prepared transition, continuity tracker), as a start does;
  - `EnsureInterpolationPacing` sees the new rate and calls `ConfigureRatePacing`
    (`oot3d_native_rate_pacing.h`): in-between governor (pair budget 33.3 ms at 60, 66.7 at 30; learned
    step cost and ratios forgotten), dynamic resolution (same pair budget; a switch to 60 begins at
    the scale 60 last ran at in the session, or the floor, a switch to 30 at the top: see "Dynamic
    resolution on deko3d" below; render-busy average reset), cadence governor (`config/fps60_mode`
    at 60, legacy at 30; lock, miss window, game pairs and the last present forgotten). Only the
    cumulative perf counters carry on;
  - grass (deko3d, `config/grass`): the other rate's profile (`Deko3dSetGrassFrameRate`: distances and
    visible cap, through the same code as the start). The instance stream is sized for the larger
    cap (22000, 2.75 MiB in 4 slots, was 1 MiB at 60).
- **Unchanged by the switch:** the guest refresh clock (`NativePresentationScheduler`, 60 refreshes a
  second at both rates, at most two per host frame: no catch-up burst), audio (DSP per guest refresh;
  the stretch follows the queue, not the presentation), the A32 frame pacing writes, the save-state
  timing field (`native30_interpolated`; a state loads at either FPS, only a 30/60 Hz simulation
  mismatch is refused, as before).
- **Logs:** a `TRIAEVUM_FRAME_RATE rate_hz= pair_budget_ms= decision_budget_ms= fps60_mode=
  interpolation=` line at each switch; perf.txt windows show the new `pair_budget_ms`/
  `decision_budget_ms` and, in `cadence:`, `fps60_mode` (the mode in effect: legacy at 30) and the new
  `rate_hz`; hitches and spikes already use the live rate. `boot-status.txt` follows the current
  `presentation_rate_hz`/`visual_interpolation`. A perf window that contains the switch mixes both rates.
- Tests: `oot3d_native_cadence_governor_tests` (`TestLiveRateSwitchMatchesFreshStart`: a session that
  ran heavy at one rate, switched, decides exactly as a fresh start at the other, given the same
  start scale, for light and heavy loads, both ways; `TestLiveRateSwitchKeepsSixtyScale`;
  `TestLiveRateSwitchFirstPresents`), `triaevum_presentation_clock_tests` (the
  choice, the simulation contract at both rates, no catch-up refreshes, pacer restart),
  `oot3d_grass_switch_tests` (`FrameRateSwitchesLiveAsAStartWould`) and the `FPS in L3+R3 > Video`
  guards.
- To check on the console: switch back and forth in Kokiri Forest and in the first room of the Forest
  Temple: the picture changes rate within a frame, no speed-up or stutter of the game or the music;
  perf.txt windows after the switch show `decision_budget_ms` 33.3 (60) / 66.7 (30) and `rate_hz` 60/30.

### HD texture packs: L3+R3 > Video (`config/texture_pack`, `config/texture_pack_scale`)
- **HUD:** while HD textures are on, the HUD's textures (drawn by the UI renderer, not PICA) resolve
  the pack too, with the same hash, as `AzaharTextureRequest::UserInterface` requests: reduced to 3x
  native (`kUiTextureMaxScale`) whatever "HD texture size" says, no mip chain, cached apart from
  world textures. The pack wins over TopScreen's HD UI atlas; while the pack is indexing or decoding
  the HD atlas stands in, and a texture with only its native art is not drawn yet (held, see "HUD
  art without the old art first" below). Turning HD textures off changes the pack
  generation, which the UI provider reports as its `Revision`, so the UI renderer resolves its
  textures again on the next frame and the HUD replacements go at once. The HD UI atlas is not a
  pack: it is used whenever the file exists. The UI path's memory is its own: a resident texture per
  UI texture key (`gfx_deko3d_ui.cpp`, any size; a superseded texture is retired after the frames in
  flight); the HD atlas costs 4.5 MiB of GPU memory and 4.5 MiB of RAM (the decoded copy the
  provider hands out again after a revision change), plus a transient 4.5 MiB staging block and
  4.5 MiB copy at each upload.
- **Which pack:** Azahar/Citra packs load as they are. For HenrikoMagnifico's *Ocarina of Time 3D 4K*
  pack use the **1080p** download (`OOT3D 4K 5.0a (1080p).zip`, 1.8 GB; its images are 4x the native
  textures); the game reduces it to 2x. The 4K download (8x, 3.7 GB) works too but takes twice the SD
  space and four times the decode work for the same result.
- **Install:** in the zip, copy the *contents* of `user/load/textures/0004000000033500/` (the
  `CHARACTERS`, `CREATURES`, `EFFECTS`, `ENVIRONMENT`, `LINK`, `MAPS` and `UI` folders and
  `pack.json`) to `switch/oot3dre/textures/` on the SD card, so that `pack.json` ends up at
  `switch/oot3dre/textures/pack.json`. Then L3+R3 → Video → "HD textures". The region folder name
  does not matter: the game reads `switch/oot3dre/textures/` whatever the dump, and the pack's
  texture names are hashes of the texture data, which is the same in a USA Rev 1 dump and in the
  converted game files (checked: the 5,039 distinct textures of the RomFS's models and menus hash the
  same; 1,943 of the pack's 2,161 images match one of them, the rest are textures this check does
  not reach or that only exist with the pack's own mods). `user/load/mods/` (code patches and RomFS
  files of other mods) and the ReShade files are not used.
- **Smaller copy** (`tools/switch/shrink_texture_pack.py`, Python 3 + Pillow, on the PC): reads the
  zip or an extracted folder, keeps only `load/textures/<title id>/`, and writes each texture at
  most `--world` (2), `--models` (as `--world`; `CHARACTERS`, `CREATURES`, `LINK`) or `--hud` (3,
  the `UI` and `MAPS` folders) times its native size, the caps
  the console applies anyway, so the SD card holds less and the Switch decodes smaller PNGs.
  Area-averaged (`BOX`) with premultiplied alpha; textures already within their size are copied
  byte for byte; `mipN` files, other-language `UI/<folder>`s (`--language`, default English, whose
  art is in `UI` itself) and non-texture files are left out, and a kept language folder's texture
  replaces the same name elsewhere. `--parts hud,models,world` selects folders (models =
  `CHARACTERS`, `CREATURES`, `LINK`). Measured on chubbycubby's Vanilla UI/HUD 3.2 (8x HUD art,
  64 HUD textures, 398.8 MiB): 97.9 MiB at 4x, 58.7 MiB at 3x, 27.9 MiB at 2x, about 5 s on 16
  cores. On Henriko's 1080p pack 5.0a (2,161 PNGs, extracted) the default run writes 2,009 files,
  352 MiB, in 11 s: HUD and maps 231.8 to 43.7 MiB, models 82.3 to 21.2 MiB (38.3 MiB with
  `--models 3`, which needs "HD texture size" 3x on the console), world 1,250 to 287
  MiB, 153 other-language textures left out. That "1080p" pack is not uniformly 4x: its HUD images
  run 4x to 16x and some world images 8x to 32x the native size.
  `tools/switch/test_shrink_texture_pack.py` covers sizes, alpha edges, parts and languages.
- **`pack.json`** is read from the folder's root: `use_new_hash` (Henriko's pack: `false`, Azahar's
  legacy hash of the decoded texture), `flip_png_files` and the `textures` name map are honoured.
  `skip_mipmap` is not needed: mipmaps are always made from the image (what Azahar does with
  `skip_mipmap: true`, the pack's setting); `mipN` files are not read. The scan is recursive and
  only `.png` is decoded.
- **Settings:** `config/texture_pack` present = HD textures on (first line, optional: another
  folder). `config/texture_pack_scale`: the size cap as a multiple of the native texture, `0` = no
  cap, `1`–`8`; absent = 2. The Video tab writes both ("HD textures", "HD texture size": 1x, 2x
  default, 3x, 4x, No limit) and says when `switch/oot3dre/textures/` (or the configured folder)
  does not exist. Turning HD textures off deletes `config/texture_pack`; a custom folder line comes
  back when they are turned on again in the same session, but not after a restart while off.
- **Applies at once:** the tab publishes the values (`LiveTexturePackSettings`), and the deko3d
  renderer applies them at the start of its next frame (`PicaResources::ApplyTexturePackSettings`):
  every cached texture becomes an earlier "epoch". Off: draws use native textures from that frame
  on (made from the draw's own bytes where missing), pack images leave GPU memory once no frame in
  flight can read them (two frames, the eviction rule), and the pack worker drops its decoded
  images and queued decodes. On, or another size: each native texture is retired and made again on
  its next draw, so it resolves through the pack; the pack is scanned again and images decode
  lazily, as at start. Only the deko3d renderer loads packs.
- **Size cap (pack worker):** after the PNG decode, an image wider or taller than native x N is
  reduced to at most native x N on each axis (`azahar_texture_scale.cpp`): every output texel is
  the area-weighted average of the source texels it covers (any ratio, e.g. 3 → 2, not only
  halving), colour weighted by alpha so transparent texels do not leave dark fringes on cut-out art,
  plain colour average where nothing is opaque. Values are averaged as stored, like the GPU's own
  filtering of these UNORM textures. Never enlarged; cap 0 keeps the file's size.
- **Mipmaps (pack worker):** the full chain is made from the reduced image with the same filter.
  The renderer uploads levels only for textures the game samples with mipmaps: the native chain
  plus the levels the larger image adds (one at 2x), with the native sampler's LOD range moved down
  by those levels. Textures without native mipmaps stay single-level. Cost: +33% memory and upload
  for mipmapped textures; in exchange distant surfaces read small levels instead of the full 2x
  image, which saves texture bandwidth on a GPU that is already at its limit and stops shimmering.
- **Memory:** pack images count against the texture budget (`config/gl_texture_budget_mb`, default
  96 MiB) like native ones, and a native texture is retired as soon as its pack image is uploaded.
  A 2x image is 16 bytes per native texel (4x a native texture, +33% with mipmaps); the 1080p pack
  uncapped would be 64. From the RomFS (models of each scene, all rooms together, every texture the
  pack replaces): Jabu-Jabu's Belly 35.6 MiB at 2x (142 MiB at 4x), Deku Tree 25.2 (101), Forest
  Temple 21.9 (88), Kokiri Forest 15.5 (62), Hyrule Field 11.5 (46), Link 2.2 (8.9); with mipmaps at
  most a third more. A scene at 2x plus actors, effects and the HUD fits in 96 MiB, so the default
  stays. With "No limit" or 4x a dungeon no longer fits: textures not drawn for two frames are
  evicted and decoded from the SD again when seen (pop-in); raise `gl_texture_budget_mb` (e.g. 256)
  for those sizes. Decoded images are kept in RAM only until the renderer uploads them
  (`AzaharTexturePackRuntime::ReleaseDecoded`; an evicted image is decoded again,
  `PollOrQueue`); before, every image decoded in a session stayed in RAM (the whole matched pack is
  806 MiB at 2x, 3.2 GiB at 4x, for a ~3.1 GiB heap).
- **Expected cost:** more GPU memory traffic per texture (2x = 4x the texels of a native texture),
  the pack worker's PNG decodes (on its own thread; a 512x512 image of the 1080p pack is decoded and
  reduced to 256x256), and uploads capped at 16 MiB per frame (a texture shows native for a few
  frames before its pack image). `logs/deko3d.txt` reports `pack images=` (uploaded; `(again N)`:
  uploaded again after an eviction), `uploaded=` and `live=` MiB in each `resources (frame)` line,
  and `evicted=N (budget M)` the textures the budget took. Not yet measured on the console.
- Logic: `azahar_texture_scale.{h,cpp}`, `azahar_texture_pack_settings.h`; host tests
  `oot3d_texture_pack_scale_tests` (sizes, ratios, alpha, cap 0, mip chain, settings files, the
  on/off cache rule) and the runtime's `CapsMipmapsAndReleasesReplacementsOnTheWorker`.

### HUD art without the old art first (07/11)
- **Symptom (test.7):** with the HD pack and the TopScreen HUD, "it always shows the old one for a
  second then pops in the new". Video `2026110706055900` (start, Temple of Time): at 13.30 s the
  X/Y/ZR/B item icons and their counters are the game's own art, at 13.85 s the pack's; hearts,
  rings and labels (HD UI atlas, no decode) do not change. About 0.5 s, at every start.
- **Cause:** the TopScreen HUD is drawn by the UI renderer (`oot3d_n64_ui_renderer.cpp`), and its
  pack images (`AzaharTextureRequest::UserInterface`) were decoded on first sight by the single pack
  worker, in one queue with the world images. The HUD first appears as a scene fades in, when that
  scene's world PNGs are queued (logs15p: 143 pack images in the first minute; the 1080p pack's UI
  images are 4x-16x, a 512x256 atlas is a 6.7 MB PNG). Until its turn the native pixels stood in,
  polled every 8 frames. Not eviction: HUD images have their own cache, untouched by the texture
  budget and `ReleaseDecoded` (the 73 budget evictions and `again 20` of logs15p are world
  textures), and the UI renderer keeps a resident texture per key; once decoded, a HUD image does
  not fall back in the session (a guest address change of the same image finds it cached by hash).
  A HUD image whose content was never seen this session (an item equipped for the first time)
  decoded late too.
- **Fix, three parts:**
  - HUD images have their own worker thread (`UiWorkerMain`, own queue `mUiLoadJobs`): a HUD
    texture waits only for other HUD images, never for a world PNG.
  - `config/texture_pack_hud` (`azahar_hud_image_list.h`): each HUD image decoded on first sight
    adds a line `<hash> <native width> <native height>` (256 lines at most); when the pack's index
    is ready (title screen), the listed images the pack has are queued on the HUD worker in that
    order, so from the second start the HUD's first draw has its pack art. Lines of a pack no
    longer installed are dropped. HD textures off: nothing read, the file is kept.
  - A HUD texture seen while its pack image decodes, with only its native pixels to show
    (`UiTexturePixels::native_stand_in`; not with the HD UI atlas as stand-in, which is new art), is
    not drawn: its primitives are skipped, it is polled at every frame and drawn as soon as the
    image is ready, for `kUiHoldPolls` (30) frames at most, then the native pixels as before. A
    texture already resident for the key (HD textures turned on again) keeps drawing meanwhile.
- **Start-up cost:** no game-thread work; the HUD worker decodes during the title screen. Measured
  on the PC (WSL host, -O2) for 9 UI images of one size, Henriko 5.0a 1080p as installed / shrunk
  by `shrink_texture_pack.py --hud 3`: 512x256 atlases 3.26 s / 0.32 s, 256x256 1.90 s / 0.14 s,
  128x128 (8) 0.37 s / 0.03 s, 256x32 0.13 s / 0.015 s; RAM 4.5 MiB per 512x256 at the 3x cap (as
  much as decoding it later). The console's A57 is several times slower [not measured]; the
  `hud pack` line of `logs/deko3d.txt` gives the real figures (fields below, "HUD art from the
  first frame, first start included").
- **Memory:** unchanged in total (HUD images were already kept for the session); they are decoded
  earlier.
- Tests: `oot3d_azahar_texture_pack_tests` (`HudImagesDoNotWaitBehindWorldImages`,
  `HudImagesSeenOnceAreDecodedAtTheNextStart`, in the runtime's foundation tests; checked with a
  standalone build since host GCC 12 cannot link that target), `oot3d_n64_ui_renderer_tests`
  (`TestHeldFirstDraws`), `oot3d_native_ui_texture_provider_tests`, `oot3d_texture_pack_scale_tests`
  (list format, damaged lines, limit, pruning); three guards.

### HUD art from the first frame, first start included (07/11, test.8)
- **Symptom (test.8):** "the HUD textures still make a quick noticeable update". `log16/deko3d.txt`,
  the first start with test.8: `hud pack (frame): listed=2 preloaded=0/0 in 0 ms (0 KiB); first
  sight=2 (wait max 263 ms)`, later `listed=3 … first sight=3`. The list was empty (first start), so
  nothing was decoded ahead: the two HUD images were first asked for when the HUD appeared, held
  (not drawn) up to 263 ms, about 16 frames at 60 FPS: the elements appeared late.
- **Cause:** a HUD image was only asked for at its first draw (`N64UiFast3dRenderer::PrepareTextures`)
  or, from the second start, from `config/texture_pack_hud`. The HUD's textures are fixed, though:
  the TopScreen HUD draws pause_top_page (hearts, rupees, button art), item_icons (every item icon
  is a region of this one 512x512 atlas, `AppendTopScreenNativeItemIconCopies`) and number_glyphs,
  plus custom_ui01, item_page (pause edge) and ocarina_page (ocarina lane); the game loads them with
  the pause UI (`PauseUi_LoadSharedTextureAssets`, from `PauseUi_Init`; the pause UI's Omote/Ura
  selector already runs on the title screen). Also, the UI renderer's provider is the ocarina text
  runtime, which did not pass the pack's generation through (`Revision()` stayed 0), so the
  renderer never re-resolved its textures when the index became ready or HD textures changed.
- **Fix:**
  - Every frame until done, `Oot3dNativeUiLifecycleBridge::HudTexturesAhead` lists those textures
    the game has loaded, in the order the player first sees them, and
    `N64UiFast3dRenderer::RequestAhead` asks the provider for one of them per frame
    (`UiTextureProvider::RequestAhead`), once per UI texture revision: a revision is the pack's
    generation, which changes when its index is ready, so a request made while the pack was still
    indexing is made again. A texture the HUD already drew at this revision is not asked for.
  - The native provider identifies the texture as for a draw (guest CTXB, after the TopScreen
    override) but does not decode it: the pack request is marked `Ahead`
    (`AzaharTextureRequest::Ahead`), copies the encoded base level and returns. The HUD worker
    hashes it (decoding it first for a legacy-hash pack such as Henriko's, formats 5 and up) and
    queues its image, counted as decoded ahead, and listed in `config/texture_pack_hud` as before.
  - `TopScreenOcarinaTextRuntime` passes `Revision()` and `RequestAhead` through to the native
    provider (also makes turning HD textures on or off refresh the HUD at once, as documented).
  - An item equipped for the first time is another region of the item_icons atlas: no new texture.
- **Which images (USA ROM, Henriko 5.0a 1080p):** all six are in the pack (hashed from the RomFS's
  `menu/01_US_ENGLISH/*.ctxb` with the runtime's own code): menu_top_parts00 `BB00B25B152582B6`,
  icon_item_menu00 `CF461E58E637A97A`, num_all00 `FABD759BB95C1C6A`, menu_item_parts00
  `18DDBB03B81C1D60`, menu_okarina_parts00 `439913BD09FA2671` (custom_ui01 is TopScreen's: not in
  that pack, the HD UI atlas stands in).
- **Start-up cost:** game thread: one texture per frame, a guest read of its encoded bytes (at most
  256 KiB), the TopScreen override check and a copy; no decode [sub-millisecond: INFERENCE, not
  measured on the console]. HUD worker (PC, WSL host, -O2), shrunk pack (`--hud 3`) / as installed:
  pause_top_page 61 / 516 ms, item_icons 102 / 880 ms, number_glyphs 5 / 64 ms, item_page 23 / 786
  ms, ocarina_page 50 / 449 ms, i.e. 0.24 s / 2.7 s, plus 2-10 ms each to identify (ETC1A4
  decode and hash of item_icons: 10 ms). The console is several times slower [not measured]; the
  title screen lasted ~90 s in log16. RAM at the 3x cap: 4.5 + 9 + 1.1 MiB for the HUD's own
  three, 2.25 + 4.5 MiB for item_page and ocarina_page (21.4 MiB): the same images the session
  decoded anyway when first shown (the HUD's image cache is kept for the session), now from the
  title screen on; the last two are held earlier than before (until the first pause / ocarina).
- **Log:** `hud pack (…): listed=N ahead=D/Q in T ms (K KiB); first sight=S (wait max W ms)`:
  `ahead=` images queued (Q) and decoded (D) ahead of the HUD's first frame, from the list and
  the requests above; `T` the time from the index to the last of them; `first sight=` HUD images
  the HUD asked for before they were ready (held), with the longest wait. Expected on a fresh start:
  `first sight=0`.
- Tests: `oot3d_azahar_texture_pack_tests` (`HudImagesAskedAheadAreReadyAtFirstSight`: empty list,
  ready on the HUD's first request, listed; a first sight during the ahead decode counts as late;
  `HudImagesAskedAheadMatchALegacyHashPack`: ETC1A4 hashed on the worker matches the HUD's own
  request; standalone build as above), `oot3d_n64_ui_renderer_tests` (`TestRequestAhead`: one per
  call, again at a new revision, the HUD's first frame not held and the pack's size, a first equip
  resolves nothing, and the same HUD without ahead requests held), `oot3d_native_ui_texture_provider_tests`
  (ahead requests not decoded, overridden bytes), `oot3d_native_ui_lifecycle_bridge_tests` (the
  list, its order and completeness), `oot3d_top_screen_ocarina_text_runtime_tests` (pass-through);
  three guards.

### Pause pages without the old art first (`config/texture_pack_menu`, 07/11, test.10)
- **Symptom (test.9):** "the menu textures still swap". `logs17/deko3d.txt`: `hud pack (frame):
  listed=5 ahead=5/5 in 1289 ms (21888 KiB); first sight=0 (wait max 0 ms)` all session: the HUD (UI
  renderer) was right. Video `2026110713005300-…`: every pause page opens with the game's tab bar
  ("GEAR MAP ITEMS", capitals) and changes to the pack's ("Gear Map Items") in view: Items frames
  711-719 (0.3 s), Map 752-759, Gear 780-784 (frames of the 29.76 fps capture; the item icons and page
  art were already the pack's).
- **Cause:** the pause pages are not the UI renderer's: TopScreen moves the game's lower screen onto
  the top one and deko3d draws it from the PICA stream (`Ui` composition domain). Its textures took
  the world path: decoded on first sight on the world worker (the native texture drawn meanwhile),
  released from RAM after the upload (`ReleaseDecoded`), keyed by guest address. The tab bar is
  `menu_item_parts00` (Henriko 5.0a `18DDBB03B81C1D60`): the very image the HUD decodes ahead at the
  title screen (item_page), but into the HUD's own cache, which the PICA path never asked. Each page
  draws it at another address, a new texture, and its image had been released: decoded again, the old
  art shown meanwhile. `again 75` re-uploads in the session.
- **Fix:** a colour texture without mip levels of a `Ui` draw (`PicaTextureIsMenuArt`,
  `gfx_deko3d_pica_plan.h`) asks for its pack image as a HUD image (`AzaharTextureRequest::UserInterface`
  + `Menu`): the HUD cache, so the images the HUD asked for ahead (`menu_item_parts00`, the item icons
  `icon_item_menu00`, the digits `num_all00`, `menu_okarina_parts00`) are ready at a page's first draw
  from the first start on; kept for the session (not released after the upload: another address finds
  it at once); decoded on the HUD worker, never behind world PNGs; listed in `config/texture_pack_hud`,
  so the other page textures (map, gear and option parts, item names) are decoded at the title screen
  of the next start. A texture asked for at its draw (HUD or menu) now goes to the front of the HUD
  worker's queue, also when it was queued ahead further back (a long list does not delay it). 3x cap and
  no mip chain, as HUD images; mipmapped textures (3D models in menus) and the world keep the world path.
- **A/B:** `config/texture_pack_menu` = `0` (read at start) gives the world path back.
- **Cost:** RAM for the menu images seen, kept for the session and decoded at each start once listed
  (list limit 256): 256x32 item names 0.3 MiB each, a 256x256 page 2.25 MiB, a 512x256 one 4.5 MiB at
  the 3x cap (Henriko 5.0a: all of `UI` and `MAPS` at 3x would be 216 MiB; a playthrough sees a part of
  it). Start-up: the HUD worker decodes them during the title screen (logs17: the HUD's 5 in 1.3 s).
- **Log:** `menu pack (…): textures ready at first draw=R, native first=N; decoded on sight=S (wait max W
  ms)`: `N` is the swaps in view (expected: 0 for the tab bar from the first start, 0 for every page from
  the second); `hud pack` `listed=` counts the menu lines too, `first sight=` stays the HUD's.
  `resources: texture pack on (…), menu textures as HUD images on` at start.
- Tests: `oot3d_azahar_texture_pack_tests` (`MenuTexturesShareTheHudImagesAndStayDecoded`,
  `TexturesOnScreenGoBeforeHudImagesAskedAhead`; standalone build as above), `oot3d_deko3d_backend_tests`
  (`TestMenuArt`). `deko3d.pack` unchanged (no shader change).

### File select, world textures and grass without pop-in (07/11, after test.10)
- **Reports (test.10):** the file-select texture still pops from plain to HD on later runs; right after
  loading into an area textures pop in; in Hyrule Field the grass pops in together with the textures.
- **File select** (videos `2026110714541100` frames 312-318, `2026110715172100` frames 258-262: the file
  cards are the game's cream art, then the pack's stone): the relocated bottom screen (cards, header,
  background) comes in the title scene's atmosphere layer, Scene domain (see "File select in
  widescreen"), so `PicaTextureIsMenuArt` never saw a menu draw: world path, decoded on sight behind the
  title scene's images, released after the upload, every start. No `menu pack` line before the first
  pause in logs18a/b/c; the first `resources (frame)` window (title and file select) has `pack images=36
  (at frame top 36)`, all native first. **Fix:** a piece of the relocated bottom screen
  (`GlRelocatedMenuDraw`: partial viewport, square-pixel orthographic, textured; the predicate the GL
  nearest sampling already used) is a menu draw too (`RelocatedMenuDraw`, `gfx_deko3d_pica_resources.cpp`):
  its textures resolve as HUD images (HUD worker, kept for the session, listed in `config/texture_pack_hud`,
  decoded at the title screen of the next start). First start: decoded on sight at the front of the HUD
  queue. Name entry and the version choice benefit too. `config/texture_pack_menu` = `0` turns it off with
  the pause pages. RAM: the file-select images stay for the session (3x cap, a few MiB).
- **World textures** (video `2026110714581200` frames 734-742: the market's walls and Link go from the
  game's art to the pack's during the fade-in; `2026110714541100` frames 438-442 the Temple of Time
  pedestal): world images were decoded only when a draw first asked, on one worker, behind the scene
  before's queued decodes (logs18c: 313 of 342 pack images at frame top). **Fix**, three parts:
  - *Scene lists* (`config/texture_pack_scenes`, `azahar_scene_image_list.h`): each world image a scene's
    draws ask for (or draw from the GPU, `NoteSceneImage`) is listed for that scene, `<scene> <hash> <w> <h>`
    (512 per scene, 16384 in all; lines of a pack no longer installed dropped at start). The game thread
    hands the scene over at each change through the render stream (`NoteScene`, in frame order); the
    listed images then decode on the world workers, now two (`kWorldDecodeWorkers`), so the first frames
    find them. Images a draw asks for go before those decoded ahead. The scene before's queued decodes are
    dropped at the change (a draw that still needs one asks again). `config/texture_pack_preload` = `0`:
    no lists, no preload.
  - *Hold* (`config/texture_pack_hold`, ms per scene, default 1000, `0`/`off` = never, at most 2500): in a
    scene's first 60 frames the renderer waits at frame start for the pack images of the world textures
    drawn native the frame before (`WaitForImages`, which moves them to the front), within the budget;
    the game follows once the frame queue is full, so the fade-in starts that much later. Covers first
    visits (no list yet).
  - *RAM:* images decoded ahead stay until their first draw uploads them (then released as every world
    image), those never drawn go at the next scene change, and no more are decoded while the unused ones
    hold 128 MiB (`kScenePreloadBudgetBytes`; a dungeon's whole list at 3x with mips is ~65 MiB).
    Nothing else stays resident; the list file is a few hundred KiB at most.
- **Grass:** it does not wait for HD images: `pica_grass_source.cpp` decodes the ground's native base level
  itself and `PicaGrassPlacements::Update` gates on that native observation. The lag is the placement
  build: one builder worker, built again at every visit (logs18c frame 7199: `queued 4`, `no result 60`
  entering Hyrule Field; other scenes 3-15), on the same service cores as the PNG decodes. **Fix:** the
  PC's placement disk cache on the Switch (`config/grass_placement_cache`, default on;
  `shadercache/grass_placements_v1`, at most 256 MiB / 96 files, least recently used out; one write per
  new ground, 2 s after its build, on the builder worker): a ground of an earlier visit or session loads
  instead of building. The key holds the geometry identity (guest addresses), so a scene loaded at other
  addresses builds again `[INFERENCE]`. The key also held the exact transform the first camera rounds (Hyrule
  Field was built again in 2 of 5 test.12 sessions): see "Grass at a scene's start".
- **Log** (`deko3d.txt`): `resources: ... scene lists on (config/texture_pack_preload), hold N ms per scene`;
  at each scene change and with the frame statistics `world pack (scene S left|now, F frames): first draws
  ready=R native=N; swapped at start=A later=L (pop-ins); hold W ms over H frames (T with images left);
  preload listed= queued= decoded= in ms (KiB) used= on GPU= unused= over budget=; on sight=, listed now=,
  dropped=`. `later` is the pop-ins left in view; `native` at a second visit should be near 0. Grass:
  `grass: placement cache on|off`, and the first `grass:` line ends `built B in X ms (longest M ms this
  session), loaded L in Y ms, written W`.
- Tests: `oot3d_azahar_texture_pack_tests` (`SceneImagesSeenOnceAreDecodedWhenTheSceneStarts`,
  `SceneChangesDropTheBacklogAndTheHoldWaitsForImagesOnScreen`; standalone build), `oot3d_texture_pack_scale_tests`
  (list format, damaged lines, limits, settings), `gl_widescreen_menu_policy_tests`
  (`FileSelectPiecesAreRelocatedMenuDrawsInEitherDomain`), `oot3d_grass_async_placement_builder_tests`
  (load/build split, limits), `oot3d_grass_switch_tests` (`Oot3dGrassPlacementCache`). No shader change:
  `deko3d.pack` unchanged.

### HUD against the mod's screenshots (07/11, test.9 report)
- **Report:** two images (`2026110713043700-…` and `2026110713045100-…`), "the normal looks better but
  they still don't look quite like the images from the mod website" (GameBanana 695893, Kokiri Forest
  `ss05`, Epona `ss06`). `…043700` is the Normal layout (B and A in a row, X/Y/ZR/ZL cluster, bomb on ZR),
  `…045100` Restoration (X/Y/A/B diamond, no ZR/ZL lanes). `logs17/topscreen_import.txt`: 3.2.2
  downloaded (status 200, SHA-256 `d07f5ae7…`), HD UI atlas and both HD images installed.
- **What differs, and why:**
  - *Scale:* the mod's screenshots are at HUD scale ~0.70 (1.85 px per HUD unit on a 640-pixel-high
    image: X-to-ZL 91 px for 48 units, heart pitch 21 px); the Switch default is 0.90
    (`config/topscreen_ui.switch.json`, the mod's own default), 2.71 px per unit on 720p (Y-to-ZR
    130 px). Setting: L3+R3 > HUD > HUD scale (0.70 or 0.75 for the screenshots' look).
  - *Default now 0.75 (hudscale75):* `config/topscreen_ui.switch.json` (staged as
    `config/topscreen_ui.json`) and the Switch code default when no config exists
    (`kTopScreenHudScaleDefault`, `#ifdef __SWITCH__`; PC stays 0.80) are 0.75. The menu has no
    reset-to-default. The release zip ships the author's reference `config/topscreen_ui.json`
    (`release-fork.sh` copies `$reference/config/*`), which must say 0.75 too. An existing install
    keeps its saved scale (0.90 or whatever it chose): the in-app updater never writes `config/`
    (`oot3d_update_release.cpp` protected roots). Only extracting the release zip by hand over the
    install replaces `config/topscreen_ui.json` (and every other HUD setting in it); the menu changes
    the scale at any time. Guards: `hudscale75` in `tests/switch/test_bug_guards.py`.
  - *Labels under icons:* the X/Y/B/ZR/ZL labels and the item icons were both layer 13 and the icons
    came after, so a larger icon covered its label (the bomb over the R of ZR, the bow over Y); on the
    mod's screenshots the labels are on top. Now `kTopScreenButtonLabelLayer` (14) puts them over the
    icons (`oot3d_top_screen_hud_touch_tests` `TestLabelsOverItemIcons`).
  - *Item icons and counts:* the player's texture pack (Henriko's `icon_item_menu00`, `num_all00`); the
    mod has no images for them and its screenshots show the game's own. Larger, rounder art.
  - *Hearts, magic meter, B and View/Back:* the pack's `menu_top_parts00` (`BB00B25B…`) wins over the
    mod's image of the same name; both are the same art (the mod's is Henriko's with the View/Back
    captions kept), so hearts look the same either way.
  - *Circles, D-pad cross, labels:* the mod's HD UI atlas, as on `ss05` (translucent disc, 20% black:
    hard to see on a dark scene). D-pad left/right items: the Switch D-pad maps boots (adult) and
    boomerang/slingshot (child), shown only when owned; `ss05` shows sticks and nuts.
  - *Perf overlay:* the FPS lines are drawn over the hearts in both images.
  - Upscaling and OC do not touch the HUD (the UI renderer draws at the output resolution).

### Toon and per-pixel lighting (deko3d): L3+R3 > Video > Lighting (`config/lighting`, off by default)
- **Switch:** `sdmc:/switch/oot3dre/config/lighting`, first word `toon` or `perpixel`; absent, empty or
  anything else = off (native PICA lighting, as before). **The model lives in the graphics settings**
  (one source of truth, the one Lighting > Scene lights reads): per-pixel *is* the Modern lighting model
  (`Effects.SurfaceLighting` PerPixel, validated to PerPixelPbr), toon is `Effects.Toon` PicaMaterial,
  off is Native (`Deko3d::WithLightingMode` / `LightingModeOf`). At start deko3d reports the
  `NativeSurfaceLighting` capability (available when `deko3d.pack` loaded) and applies the file to the
  settings; a mode moves an Authentic preset to Custom (the Switch starts from the Custom preset anyway:
  no settings file, no F2 override). The Video tab's "Lighting" combo (Native, Toon bands, Per-pixel (Modern model)) applies the model and writes the file; the renderer reads the model and the
  scene environment from `SnapshotForRendering()` each frame. Off: the variant code is not reached
  (`ResolvePicaProgram` checks the mode once per draw), the native shader keys and the pack's existing
  entries are unchanged, and nothing new runs on the game thread.
- **Toon:** upstream's Toon "PICA material" banding (`pica_toon_shader.cpp`, default style: 4 bands,
  shadow tint, rim) as a text mutation of the draw's GLSL 4.3 fragment text: the toon declarations
  (constants baked in, no extra uniform block) before `main()` and one banding line before the depth.
  Only opaque scene draws that test and write depth and write RGB (`ClassifyPicaToonDraw`); the vertex
  stage stays native. Meant to fit at 60 fps at stock clocks.
- **Per-pixel:** a trimmed copy of upstream's per-pixel surface lighting: the draw's native lights,
  ambient, the sun (scene and environment directionals), at most 4 scene point lights (Navi, fairies,
  flames; the first 4 the selector keeps) and the baked-surface branch (outdoor vertex colour
  redistributed by the light, a horizontal face keeps its native colour), plus upstream's rim light and
  the environment's fog contribution. No placed lights, water optics, GI or shadows. Admission is
  upstream's (`surfaceCandidate`: scene domain, world layer, perspective, relightable appearance, a typed
  surface vertex program). The vertex variant adds upstream's surface normal and tint outputs
  (`BuildPicaVertexSurfaceShader`, locations 9 and 10); the fragment variant reads upstream's 2992-byte
  `PicaSurfaceLighting` block, member for member, bound at fragment slot **5** instead of upstream's 16
  (deko3d has 0-15; slot 4 is the PICA fragment uniforms). The block is built per draw on the render
  thread (`BuildPicaSurfaceLightingUniforms`, `TrimSurfaceLighting`, then `AssignPicaSurfaceRim` on the
  draws `PicaDrawReceivesRimLight` admits) and streamed like the PICA uniforms (an equal block is bound
  again). The release profiles (`runtime/triaevum_resources/environments/`) ship inside `resources.o2r`
  (`environments/`, `scripts/switch/stage-product.sh`) and are read from it only while the Modern model
  is on under deko3d (`SceneEnvironmentRuntime::SetDefaultsReader`). Nothing ties it to `config/fps60`:
  it runs at 30 or 60 fps. **Kept native for cost:** draws that write no depth (`PerPixelCostAdmits`;
  the blend flag alone does not exclude a draw: the game blends most solid geometry, and the first
  version of this rule left per-pixel 0 lit draws in logs14), and, in both modes, draws whose base
  fragment program spills (none in the pack now; their variants would spill too). The Scene lights tab says so. Every
  optional term (native lights, points, directionals, rim) sits behind its own uniform branch, and the
  shipped profiles' blocks use a specialized path variant (see "Cheaper per-pixel lighting").
- **Lighting > Scene lights under per-pixel (0.1.2-test.2 said "needs the Modern lighting model"):**
  active by one rule (`SceneEnvironmentActive`: capability, Modern model, not Authentic, no native
  override). Live through the published environment: Use scene environment, Preserve scene
  brightness, ambient colour and intensity, native scene direct, local and ambient contributions,
  native fog contribution, rim light (all its controls), key and fill lights (colour, intensity,
  direction modes), and the Navi / fairy / flame / architecture point-light groups (on, intensity,
  radius, tint; at most 4 per surface, said in the tab). Marked unavailable on the Switch renderer:
  placed lights (kept in the profile, not drawn) and shadows (caster faces). No other reachable Switch
  menu names the lighting model (the built-in Lighting & GI / Shadows panels are not in L3+R3).
- **Variants come from the pack:** a variant is the deterministic mutation of the text the native draw
  uses (specialized or parametric), looked up by its own `ShaderTextKey`. Offline,
  `oot3d_deko3d_lighting_variants` (the console's own mutation code) writes the toon and per-pixel
  variants (the three per-pixel paths) of every known fragment text and `tools/oot3d/deko3d_shader_pack.py --lighting-variants
  <tool>` compiles them with the other texts. A variant the pack lacks is recorded like any missing
  stage (`logs/deko3d_shaders/{vert,frag}_<key>.glsl`) and that draw keeps both native shaders (never
  a mixed pair). The per-pixel **vertex** variant depends on the draw's vertex hooks, which exist only
  at run time: the first per-pixel run records it (one title vertex program, a few skinning layouts),
  `deko3d_shader_pack.py --glsl logs/deko3d_shaders --pack deko3d.pack -o deko3d.pack` adds it, and the
  second run draws per-pixel. Both mutations are idempotent, so recorded variant texts can go back
  through the tool unchanged.
- **Pack (06/11, lighting3: reference p7b + six bases + console-missing):** 288 fragment texts, 288 toon
  and 288 per-pixel variants, 0 uam failures, **no stage uses scratch** (before: 3 per kind at 4096
  bytes per warp, 105-106 GPRs); 14.5 MB merged. Bases: the parametric (uber) programs no longer copy
  their TEV and lighting programs into locals (the lighting copy's light table, indexed by the light
  loop, went to scratch) and pick TEV sources/operands with if chains; lit 105 → 73 GPRs, unlit 97 →
  65; same output (lavapipe comparison of both texts over random uniform blocks, bit for bit). The
  sixth base is the lit family without lookup tables, which the console recorded in the Forest Temple
  as `frag_08743ed174a0fce1` (now `frag_251773f35edd5102`, dumped by `oot3d_deko3d_parametric_glsl_dump`):
  drop the old file from `console-missing/`. Specialized texts are unchanged (their keys are the
  reference pack's; fingerprints in `oot3d_native_pica_program_descriptor_tests`).
- **Log (`logs/deko3d.txt`):** `lighting: mode <off|toon|perpixel> (config/lighting)` at start and on
  each change, and with every `programs (frame)` line while a mode is on (or was):
  `lighting (frame): mode=… variants found=… missing=… unsupported=…; draws lit=… native=…
  ineligible=… (spilling base=… no depth write=…)` (variants: distinct shaders resolved; draws: cumulative),
  then `per frame lit opaque=… translucent=… points 0/1/2/3/4=… rim=… paths g/e/b=… px=…M | avg of N …` (lit draws of the
  last frame, then the average since the previous line; points: per-pixel draws by scene point lights; paths:
  per-pixel draws and their pixels by fragment variant path, generic/environment/baked).
  `programs (frame)` ends with `per frame spill=D/L px=P/LP M parametric=… normal=… | avg of N …`: draws
  and lit draws by base fragment program class (spilling, parametric, specialized) and their pixels
  (viewport inside scissor, millions; an upper bound). `missing` > 0 means the pack did not cover the
  scene: the texts are in `logs/deko3d_shaders/`.
- **Expected cost [not measured]:** game thread 0 (the per-draw work runs in `SubmitPicaDraw` on the
  render thread; only the rare dependency flushes run it on core 0). Toon: about 0.2-0.5 ms of GPU per
  frame at the 2.25x floor. Per-pixel: about 1-2.5 ms of GPU per frame, plus about 0.3-0.8 ms of render
  thread (the block per lit draw, 3 KB streamed each). Measure: `busy_ms` in the `dkperf` lines of
  `logs/perf.txt` and `rt_ms_*`, same scene and save, mode off against on.
- **Forest Temple room 8 (lighting3) [expected, not measured]:** before, 16.7 ms `busy_ms` off and 38.4
  ms per-pixel at 60 fps / 2.25x, against 10-11 ms off in rooms 0-2: its large surfaces are drawn by the
  parametric lit program (105 GPRs, scratch), and per-pixel added its spilling variant. Now that program
  has 73 GPRs and no scratch (about 1.4x the warps per SM): off should fall toward the 11-13 ms of
  other rooms, per-pixel to off + 1-4 ms (the rooms 0-2 increment). Check in room 8, off then per-pixel:
  `programs (frame)` `parametric=` draws and `px=` (who pays), `spill=0/0`, `missing=0` once the pack
  holds the new texts, `lighting (frame)` `translucent=0` and `spilling base=0`, then `busy_ms`.
- Code: `gfx_deko3d_lighting.{h,cpp}` (mode, mutations, lookup and fallback), `gfx_deko3d.cpp`
  (`ApplyPicaLighting`, `BuildSurfaceLighting`), `gfx_deko3d_pica_draw.cpp` (binding),
  `tools/oot3d/deko3d_lighting_variants.cpp`. Host tests `oot3d_deko3d_lighting_tests` (config, both
  mutations stable/idempotent/only the intended lines, off = native, lookup and fallback, block layout
  against upstream's and binding); guards in `tests/switch/test_bug_guards.py`.

### ZCULL on deko3d (`config/deko3d_zcull`, 07/11, off by default)
- **Default unchanged:** the queue keeps `DkQueueFlags_DisableZcull` ("lost tiles of decals in Aurora's
  renderer"). With `sdmc:/switch/oot3dre/config/deko3d_zcull` present (content ignored) it is created without
  it; deko3d then allocates the channel's ZCULL context and configures the region per depth target
  (`gfx_deko3d_device.cpp`). Log at start: `deko3d: zcull on (config/deko3d_zcull)` / `deko3d: zcull off`.
- **How the game resets depth:** it never clears or fills it (`fills=0 clears=0` in every logs15 line), so the
  reset is a draw. Which draw cannot be read from the code; a new `logs/deko3d.txt` line, written with the
  `programs` line in both modes, measures it:
  `depth (frame): zcull=0 per frame always=N full=N fragdepth=N first none/full/partial/tested=... | avg of N ...`
  - `always`: depth-writing draws that pass every fragment (compare ALWAYS or test off); `full`: those
    covering the whole target.
  - `first`: frames by the kind of their first depth-writing draw. `first ... full` near 1.00 means the reset
    is a full-target ALWAYS draw; then turning it into a real `dkCmdBufClearDepthStencil` (what ZCULL needs to
    start from a known state: deko3d updates ZCULL on `ZcullClearDepth` in its clear) is the next step. It is
    not done yet: a clear is only identical if that draw writes one constant depth everywhere, which the
    counters cannot show.
  - `fragdepth`: depth-tested draws whose program writes `gl_FragDepth`
    (`PicaDrawWritesFragDepth`, the generator's rule: anything but z-buffering with scale -1 and offset 0;
    decals with a depth offset). **Likely cause of the lost decal tiles** [inference]: deko3d 0.5 writes
    `ZcullTestMask = 0` once and never changes it, while Mesa's nvc0 sets `ZCULL_TEST_MASK = 0x11` for
    programs that write depth; ZCULL then tests such a decal at its raster depth, not the depth it writes.
- **Stats:** with ZCULL on, `dkperf` gains `zcull_stats=a/b/c/d`: deko3d's `DkCounter_ZcullStats` (the four
  raw hardware counters ZcullStats0-3), reset at each frame top and reported at frame end, averaged per
  frame. deko3d does not document their meaning: compare runs; all zero means ZCULL rejected nothing.
  Every `dkperf` line also has `zcull=0|1`.
- Saving: 0 to −1 ms per frame [estimate; nfsmw-nx measured about 2 ms]. Without a clear, ZCULL may start
  each frame unknown and cull little.
- Tests: `oot3d_deko3d_backend_tests` (`TestDepthBreakdown`; `TestFrameBreakdown` was never called and now
  is); guard "ZCULL: off unless config/deko3d_zcull exists".
- Console check (1 minute each): (a) without the file, Forest Temple room 8: `logs/deko3d.txt` `depth` line
  says how depth is reset. (b) Create the file, restart, same room: `deko3d: zcull on` in deko3d.txt, `dkperf
  zcull=1 zcull_stats=...` and `scene_ms` against (a); look at decals (floor markings, shadows' blobs,
  Deku Tree spider webs) for missing square tiles. Delete the file if any tile is missing.
- **Decals with ZCULL on (07/11, depthclear):** since deko3d cannot set `ZcullTestMask`, every draw whose
  program writes `gl_FragDepth` and tests or writes depth is wrapped in two ZCULL invalidations
  (`dkCmdBufBarrier(None, DkInvalidateFlags_Zcull)` = `InvalidateZcullNoWfi`, in order, no wait): ZCULL
  cannot reject the decal by its raster depth, nor keep a view of the tiles that the decal's written depth
  made wrong. The cost [inference]: ZCULL culls little or nothing after the frame's first such draw until the
  next depth clear (`config/deko3d_clear_draw`). `dkperf zcull_invalidates=` counts them per frame. Restored save states
  already invalidated ZCULL (`WriteTarget`); a change of depth target invalidates it in deko3d itself.
  Grass drawn with `gl_FragDepth` (w-buffered meshes; `grass: ... depth-writing 0` in every log so far) is
  not wrapped. **Stays opt-in:** ZCULL needs a depth clear to start from a known state, and the clear only
  exists where `reset draw:` says `plan depth_clear`/`full_clear`; the decal guard is correct by
  construction [inference from deko3d 0.5's source] but untested on the console.

### Depth reset as fast clears, overdraw counters (`config/deko3d_clear_draw`, 07/11, off by default)
The game never clears depth; every frame starts with a full-target draw with depth test ALWAYS (`depth (frame):
... first ... full=1.00`). `gfx_deko3d_clear_draw.h` decides, per such draw, whether it provably writes one
depth and one colour; `config/deko3d_clear_draw` (present, content not `0`) applies the decision. Absent, the
draw is only described. Log at start: `pica: clear draw on (config/deko3d_clear_draw)` / `pica: clear draw off`.
- **Plans:** `depth_clear`: the draw provably writes depth 0 or 1 at every pixel of the target, so the depth
  is cleared to it (`dkCmdBufClearDepthStencil`, deko3d's fast ZCULL clear) and the draw runs with depth
  writes off. `full_clear`: its colour is provably one constant too (or it writes no colour), so the colour
  is cleared to it under the draw's colour mask and the draw is skipped. The target's version moves once,
  as for the draw. Anything not provable keeps the draw unchanged.
- **What "provable" means** (all host-tested on synthetic draw states, `oot3d_deko3d_clear_draw_tests`):
  - Depth: only the explicit form of the specialized program (`gl_FragDepth = -z * depth_scale +
    depth_offset`) with scale 0 is constant (w-buffered: only offset 0). The raster form (scale -1, offset 0)
    writes the rasterized depth, which the CPU cannot reproduce bit for bit: never converted. Only 0 and 1
    are cleared to (exact in any depth conversion).
  - Colour: every TEV input the program reads is a constant (TEV constants, buffer colour; disabled
    textures, lighting and procedural texture read 0). The six stages run in double precision and every
    8-bit rounding must be at least 0.01 (in 1/255 steps) from a boundary, else "rounding boundary". Checked
    against 200,000 random constant programs in eight float32 models (division or reciprocal, fused or
    separate fma, two mix forms) and, in `tests/switch/test_clear_draw_tev.py`, by running the generator's own
    TEV expressions through `glsl_eval.py` in binary32: every proven program stores the predicted bytes.
  - Coverage: the frontend's typed model-view-projection transform projects the vertices (with a float32
    error bound); one triangle or two forming a convex quad must contain every pixel centre of the viewport
    (a quarter pixel inside its edges); no vertex within rounding of a clip plane (the vertex wrapper's
    near/far snapping included: a quad exactly on the far plane is **not** provable, float32 may clip it);
    with culling, the covering triangles front-facing by the GL rule `PlanPicaCull` reproduces.
  - No discard (alpha test off, ALWAYS, or provably passing on the constant alpha, with `int(a * 255)`
    truncation), no fog, no fragment lighting, no Toon/Per-pixel variant, the specialized program; for the
    colour clear also no blending (or ONE/ZERO add), logic op COPY, no stencil writes.
- **`reset draw:` line** (`logs/deko3d.txt`, at each change of plan, at most 64, and once per periodic log):
  vertices, topology, triangles, culling, `coverage proven|...`; `depth raster|frag <constant>|frag scale ..
  offset ..`, `test always|off`, the vertices' raster depth range `z a..b` and NDC `x`/`y` ranges; `colour
  constant rrggbbaa|vertex_colour|texture|lighting|proctex at stage N`, mask, blend, logic op, fog, lighting,
  `alpha_test enabled/function passes|discards|unknown`, stencil; `plan draw (reason)|depth_clear[, colour
  kept (reason)]|full_clear`. The plan is computed (and logged) with the file absent too.
- **dkperf (perf.txt):** `fs_px=` fragment shader runs and `passed_px=` samples passing depth/stencil, frame top
  to scene end, per pixel of the presented top scene (`top_w x top_h`; every target of the scene counts);
  `opaque_ms=` frame top to the first translucent world draw (before grass); `clear_draw=0|1 resets= depth_clears=
  full_clears=` per frame (`resets` counts the depth resets whether converted or not).
- **Saving** [estimate]: the reset covers 921,600 pixels at 3.0. Skipped (`full_clear`): its constant-TEV
  shader (six byte-rounded stages) and the colour/depth writes, about 0.5–1.2 ms at 307 MHz (less once
  pass-through stages are skipped in the pack), and a known ZCULL state each frame. `depth_clear` alone: the
  depth writes, about 0.1 ms, and the ZCULL state. Nothing if the console's `reset draw:` says `plan draw`.
- **Risks:** the coverage proof trusts the frontend's typed transform (as the widescreen coverage does) and
  the GL culling model of `PlanPicaCull`; the clears trust deko3d's clear to respect the scissor (the extent)
  as `ApplyPicaFill` does. Nothing changes the generated game code; `deko3d.pack` is unchanged.
- Guards: "clear draw: the depth reset becomes clears only with config/deko3d_clear_draw", "clear draw: a
  constant colour only away from 8-bit rounding boundaries", "ZCULL: draws whose program writes
  gl_FragDepth invalidate ZCULL around them".
- Console check (one minute each, full resolution, 60 FPS: the Temple of Time, Forest Temple room 8, Sacred
  Forest Meadow): (a) without the file, read `reset draw:` (its `plan` and reason) and `dkperf fs_px passed_px
  opaque_ms busy_ms`; (b) with `config/deko3d_clear_draw`, restart, same places: `pica: clear draw on`,
  `dkperf clear_draw=1 depth_clears full_clears`, `busy_ms`/`scene_ms`/`fs_px` against (a), and the picture
  unchanged (sky, background colour, far geometry); (c) only if (b) shows `full_clears` or `depth_clears`
  near 1: add `config/deko3d_zcull`, compare `zcull_stats`, `zcull_invalidates`, `scene_ms`, and look at decals.

### Fewer deko3d barriers (07/11)
Checked against deko3d 0.5.0's source (`source/maxwell/gpu_base.cpp`, `dk_queue.cpp`, `gpu_transfer.cpp`):
- `dkCmdBufBarrier(Full)` = semaphore WFI + a gpfifo entry with NoPrefetch: everything before finishes and
  command prefetch stops. `DkInvalidateFlags_Shader` invalidates the shader *instruction, global-data and
  constant (uniform)* caches. Every `dkQueueFlush` (the backend flushes at each `Device::Submit`, twice a
  frame) appends `postSubmitFlush`: a barrier with no wait that invalidates image, shader, descriptor and
  L2 caches ahead of the next submitted commands.
1. **No barrier at every frame's first draw.** `PicaResources::BeginFrame` set the upload flag every frame,
   so the first draw paid a Full barrier with Image|Descriptors|Shader. The cache part is what
   `postSubmitFlush` already does (CPU writes of the new frame, uniforms in recycled stream memory included:
   the slot's fence was waited and an invalidation follows that frame's work). The wait part guards
   nothing: the GPU-written images read later (the display copy, save-state/target copies) have their own
   Full barriers before and after, uploads set the flag themselves. `config/deko3d_barrier` gains the word
   `frame` to bring it back (A/B; e.g. `full image descriptors shader frame` = the old behaviour).
2. **Uploads no longer invalidate shader caches.** Default flags are Image|Descriptors (was |Shader): texture
   uploads and descriptor writes touch no code, global memory or uniform (those are fresh stream memory per
   draw and in steady state never had a per-draw invalidation anyway); new shader code still adds Shader
   through `ShaderStore::TakeCodeWritten`. The barrier stays Full: the texture copy runs on the copy engine
   (`BlitCopyEngine`, `LaunchDma` non-pipelined) and must land before the 3D engine samples it.
3. **Pack images batched at frame top.** The flag is consumed lazily at the next draw, so all uploads
   recorded before one draw already shared one barrier; the per-texture drains came from pack images
   finishing while a frame draws (each polled and uploaded at the draw that used it). Now a native texture
   whose pack image is still decoding is not polled mid-frame: `UploadReadyReplacements` (end of
   `BeginFrame`) polls every such texture drawn last frame and uploads all finished ones together, under the
   16 MiB per-frame cap (the rest stays pending). The HD image appears one frame later at most. Natives seen
   for the first time (and pack images already decoded then) still upload at their draw.
- Logs: `dkperf image_barriers_per_frame=` (Full barriers issued for uploads/descriptors, per frame),
  deko3d.txt `resources` line `pack images=N (again N, at frame top N)`.
- Saving: −0.05 to −0.2 ms per frame steady (one Full barrier and the Shader invalidation per frame),
  more while pack images stream [estimate].
- Guards: the Full + Image|Descriptors default, every submit flushed, pack images only at frame top.
- Console check (1 minute): HD textures on, walk into a new area: `image_barriers_per_frame` well under 1 in
  steady windows (was at least 1), `at frame top` grows while loading; no texture briefly showing stale or
  garbage contents. If anything looks wrong, `config/deko3d_barrier` with `full image descriptors shader
  frame` restores the old barriers for comparison.

### Colour compression on deko3d (`config/deko3d_compression`, 07/11, on by default)
- The scene's colour target, the display images and the mailbox images now get `DkImageFlags_HwCompression`
  (deko3d then maps them with a compressible memory kind); depth already had it. `config/deko3d_compression`
  whose first character is `0` turns it off (A/B). Log at start: `deko3d: colour compression on|off`.
- Every reader is a GPU engine, which reads through the compression-aware L2:
  - the display copy (2D engine `dkCmdBufBlitImage`) and the edge bands; the scanout samples the display image;
  - save-state capture/restore: copy engine (`dkCmdBufCopyImageToBuffer`/`dkCmdBufCopyBufferToImage`), the
    same calls that already read and write the compressed depth; the CPU only touches the linear staging
    buffers (dynamic resolution copies nothing: it clears and redraws);
  - the mailbox: the present queue's `dkCmdBufCopyImage` into the window image.
  - Screenshots capture the window images, which stay as before: compressed only without the present
    thread, where `dkQueuePresentImage` decompresses them (`SurfaceDecompress`); with it they go to nwindow
    uncompressed (the display controller cannot read compressed memory).
- Saving: −0.1 to −0.4 ms per frame [estimate]: less memory traffic for blending (most scene draws blend),
  the 2D copy and the scanout.
- Guards: window images keep the `!mPresentThreaded` rule; targets and displays follow the setting.
- Console check (1 minute): `deko3d: colour compression on` in deko3d.txt; the picture looks as before
  (no blocks, no garbage after a room change or a DRS step); save a state and load it: the restored frame is
  right; take an album screenshot. `dkperf busy_ms`/`scene_ms` against a run with `deko3d_compression` = `0`.

### Pass-through TEV stages skipped in the pack (`config/deko3d_pack`, 07/11, on by default)
- **Why:** the fragment generator writes all six combiner stages, each rounded to 8 bits (`byteround3`/
  `byteround1`: four `floor` = `F2F` per stage, a quarter-rate instruction on Maxwell). Most stages only pass
  the previous result on: of the 1412 fragment texts the pack compiles (271 recorded `.src`, 11 console-missing,
  1130 toon/per-pixel variants), every one has 4-10 pass-through channels (colour or alpha of stages 1-5);
  315 have all five stages pass-through.
- **What:** `tools/oot3d/deko3d_shader_pack.py` (`skip_tev_passthrough`), just before uam: a stage N ≥ 1 whose
  colour is `byteround3(clamp(combiner_output.rgb, vec3(0.0), vec3(1.0)))` (alpha: `byteround1(clamp(
  combiner_output.a, 0.0, 1.0))`) with scale 1 becomes `vec3 color_output_N = combiner_output.rgb;` and its
  `combiner_output` line uses it unclamped. Only when the line before is the previous stage's generator line
  (or its buffer lines): anything a variant inserts between stages ends the chain. Stage 0 is never touched.
  The DKSH is stored under the key of the **unmodified** text, so the console's lookup, the lighting variants
  and the texts consoles record as missing are unchanged; no generator change, no shard-visible change.
  Same rule as the fork author's light uber shader (`6884c113`, trailing stages), credited in the tool.
- **Exact:** a stage's input is the previous stage's 8-bit result `fl(k/255)` (uam emits `* 0x3B808081`,
  `fl(1/255)`) times 1, 2 or 4, clamped; rounding and clamping it again gives the same float.
  `tests/switch/test_tev_passthrough.py`: every byte × scale × (division, reciprocal) × (separate, fused
  multiply-add) in binary32 (a reciprocal one ulp off would fail: checked); and the stages of real texts
  (12 in `tests/switch/data/tev_passthrough/`, the 137 boot/title/Kokiri profile texts, and with
  `TEV_PASSTHROUGH_TEXTS=<dir>` all 1412 pack texts, 237 distinct changed chains) run by `glsl_eval.py`
  before/after with four byteround forms on random inputs: the same bits for every stage's output and the
  combiner buffer. The copies are deliberately **not** `precise`: a precise copy made the earlier stages'
  clamps exact through `combiner_output`, uam then stopped folding them into the multiply's `.sat` (two
  `FMNMX` per channel, +24 instructions on some texts); what remains for the compiler (`x * 1.0`, clamp →
  `.sat`, powers of two times 1/255) is exact [INFERENCE: from the SASS, not a GPU run].
- **Measured offline** (uam `-r` + envydis gm107 of all 1412 texts, `~/oot3d-work/tevskip`): static
  instructions per program, median (the TEV code is straight-line):

  | texts | instructions | `F2F` floor | registers |
  |---|---|---|---|
  | recorded (271) | 206 → 138 (−67, −32%; min −16, max −81) | 32 → 15 | 13 → 13 |
  | console-missing (11) | 200 → 131 (−75) | 32 → 13 | 12 → 12 |
  | toon/per-pixel variants (1130) | 632 → 567 (−67, −11%) | 34 → 17 | 22 → 22 |

  By pass-through channels: 4 → −16, 6 → −48, 8 → −63, 10 → −80 instructions (median).
- **Expected saving** [INFERENCE, not measured]: counting `F2F` at 4 issue slots, a Native-lit pixel costs
  ~39% fewer shader cycles, a Per-pixel one ~23%; if shader ALU is 30-60% of `scene_ms`, that is about
  1.5-4 ms per frame at 3.0 and 307 MHz (8-20% of the scene).
- **A/B:** the build makes `deko3d.pack` (rewrite) and `deko3d_reference.pack` (same keys, `--keep-tev-
  passthrough`). `config/deko3d_pack`, first line a file name in `shadercache/` (`[A-Za-z0-9._-]`, ending
  `.pack`), picks the pack at start; missing or bad falls back to `deko3d.pack`. Pack header word 3 and each
  entry's last word carry flag 1 (`kShaderPackFlagTevPassthroughSkip`; 0 in older packs). `deko3d.txt` at start:
  `shaders: pack deko3d.pack flags=1 tev_passthrough=skip (1412 stages compiled with pass-through TEV stages
  skipped)` or `... deko3d_reference.pack flags=0 tev_passthrough=keep (0 ...)`. The pack is read in one
  call (fork `a7424d3b`, credited) instead of byte by byte.
- **Console check (1 minute, full resolution, 60 FPS):** Temple of Time, Forest Temple room 8, Sacred Forest
  Meadow, 20 s still each; then write `deko3d_reference.pack` into `config/deko3d_pack`, restart, repeat. Compare
  `dkperf busy_ms`/`scene_ms` and `perf.txt gpu_ms_step`; the picture must not change (screenshot the same
  spot with each pack).
- Code: `deko3d_shader_pack.py`, `gfx_deko3d_shader_pack.h`, `gfx_deko3d_shaders.cpp`; tests
  `test_tev_passthrough.py`, `oot3d_deko3d_backend_tests` (TestShaderPack); guards "tevskip: ...".

### Per-pixel dots: 8x4-pixel tiles of wrong lighting (0.1.2-test.7, Per-pixel only)
- **Seen:** dark, white or coloured (cyan, blue, magenta) dashes on Link's trousers and face, the Temple
  of Time platform, the Forest Temple and Hyrule Field's ground; gone on Native; flickering.
- **They are warps, not pixels:** the dashes sit on a screen grid of 8 render pixels horizontally
  (10.67 display pixels at the 2.25x scale of the Temple of Time, Hyrule Field and face shots, 8.0 at the
  3.0x scale of the Forest Temple shot; phase-locked fit over ~700 detected dashes) and 4 render rows
  (2.67 display rows in the Hyrule Field shot): 32 pixels, one Maxwell fragment warp. A numeric fault
  (NaN, Inf, `pow`, a division) is per pixel. CPU evaluation of the test.7 text (`tests/switch/glsl_eval.py`,
  binary32, Maxwell min/max) over zero/opposite/infinite/NaN normals, the eye at the fragment, a light at
  the surface, a zero radius and grazing angles: every output finite and in [0, 1]. The math is upstream's
  (no `saturate`, `max` or epsilon lost in the trim).
- **What the compiled variants do that native shaders do not** (uam `-r` + envytools `envydis -m gm107`
  over the 271 p7b texts, the console-missing texts and the six parametric bases): scoreboard RAW/WAR,
  predicate latency (13 cycles) and register counts are all correct; no local memory; sync-stack depth 5-7
  (16 on chip). But the per-pixel code interpolated its inputs at every use: 184 `IPA` per variant against
  20-25 in the base (`pica_view` 52 times, inside the light loops; the tint ~3000 instructions in), and it
  had 8 lane-dependent branches (the rim's `dot(n, n) < 0.5`/`dot(l, l) < 0.5` early returns, the rim
  side ternary); specialized native texts have none. [INFERENCE] the warp-level fault comes from those
  late/lane-dependent interpolations or branches (nothing else in the variant differs from native code;
  uniform-buffer streaming is fenced and identical in reuse rules to the PICA uniforms).
- **Fix (shader text, `deko3d.pack` must be rebuilt):** `pica_surface_fragment()` reads the normal, view
  and tint once at the top of the body, before any branch (a clamp to the finite range keeps them computed
  values: a plain copy is propagated back to every use) and passes them down; the per-fragment early
  returns and the rim side ternary are selects. Same bytes as test.7 on valid inputs and on finite degenerate
  inputs. A variant now has 22 `IPA`, none after a branch, no lane-dependent control flow, ~15% fewer
  instructions (3469 -> 2975 on `frag_3555e8fa561f9ca5`), 43 GPRs instead of 37.
- **Tests:** `tests/switch/test_perpixel_lighting.py` (run.sh): the current text against
  `tests/switch/data/perpixel_lighting_test7.glsl` on 300 random blocks over every path, degenerate inputs,
  interpolants read only in `pica_surface_fragment()`, every branch condition uniform. Guard "per-pixel dots".
- **Check (one minute):** after the pack rebuild, Per-pixel, stand on the Temple of Time platform (or look
  at Link's legs up close): no dashes. If they remain, the cause is outside the shader structure (next
  suspects: the per-draw surface block's memory, texture uploads racing slow draws).

### Per-pixel blink on fps60 in-between frames (0.1.2-test.7 videos, `logs15`, 07/11)
- **Seen:** Temple of Time, Per-pixel, HD pack, scale 3.00, `exact_step=1 carry_unmatched=1`: every 2-3 s
  one frame shades differently. Geometry is right (leg silhouettes lie between the neighbours'); Link's
  trousers go from cool grey with rim and the per-pixel dots (RGB 69,73,70) to warm beige without rim or
  dots (99,90,74); windows ~1.4x and far surfaces brighter. Crops: `~/oot3d-work/blink/evidence/`.
- **Which frames:** the scene costs ~20.5 ms of GPU per step, so the pacing governor sits at
  `level=dropped` and draws one probe in-between frame every couple of seconds (`probes=1-2` per 150
  frames); the transients match that rate. In a scene that holds 60 FPS every other frame is an
  in-between frame [INFERENCE: a 30 Hz shading flicker].
- **What is ruled out (host test `oot3d_deko3d_inbetween_lighting_tests`):** the backend's per-pixel
  decision now lives in `Deko3d::BuildPerPixelSurface` / `AdmitPerPixelDraw`
  (`gfx_deko3d_lighting_draw.cpp`, the same code `ApplyPicaLighting` runs). Two real frames of a lit CMB
  draw (camera turning, Navi moving: the light binding is interpolated) go through the prepared
  transition, its samples at 0, 1/2 (and 1/4) and the next real frame, the bridge's draw view and that
  decision: every frame kind is admitted, keeps the rim, the environment and the branches (`control`),
  and the in-between block lies between the two real blocks. The same test built with `-ffast-math` on
  the visual-frame, bridge, surface-lighting and decision sources (as the Switch builds them,
  `~/oot3d-work/blink/fastmath/run.sh`) passes too. And `logs15` measures the probe in-between frames at the
  step's GPU cost (`gpu_ms_full` ~20.5 = `gpu_ms_step`) while per-pixel covers ~49 of 64 Mpx per step: the
  in-between frames do bind the per-pixel variant. So it is not the draw selection falling back to native
  (H1). Open: what the in-between frame's variant receives (a block with the environment or rim off, or
  the variant's own inputs) [not proved on the host].
- **Counters (next console run decides it):** after each `lighting (frame)` line, `logs/deko3d.txt` has
  `lighting real (frame): ...` and `lighting in-between (frame): ...` since the previous line: `frames`
  (temporal samples of that kind; deko3d learns the kind from `PublishPicaFrameTemporalSample`'s
  `Synthetic`), `lit=L (no env E, no rim R)` (lit draws whose block has the scene environment off or no
  rim), `native`, `ineligible`, `first=<reason>` and the refusals by reason (`not_scene_world`,
  `no_surface_program`, `no_depth_write`, `fragment_operation`, `no_uniforms`, `not_perspective`,
  `not_relightable`, `native_lights`, `no_camera`, `block`), then `block control=a/b/c/d env=E rim=R baked=B`
  of the first lit draw. Any change of the scene environment the blocks are built with (on/off, scope,
  rim) is logged as `lighting: scene environment on|off, scope S, rim on|off (frame N, after an
  in-between|real sample)` (at most 64 lines).
- **Reading them:** in-between `ineligible` with a `first` reason = H1 (that rule); in-between `no env`/`no
  rim` close to its `lit` while real ones are 0, or a `scene environment off` line at the blink = the block
  (environment) side; both like the real line = the GPU side (what the variant reads at run time).
- **Check (one minute):** Per-pixel, 60 FPS, Temple of Time, Link close up for ~30 s, then
  `logs/deko3d.txt`: compare the `lighting real` and `lighting in-between` lines (and any `scene
  environment` line). No `deko3d.pack` rebuild needed for this change.
- Code: `gfx_deko3d_lighting_draw.cpp`, `gfx_deko3d_lighting.h` (`PerPixelRefusal`, `FrameKindLighting`),
  `gfx_deko3d.cpp` (`ApplyPicaLighting`, `ApplyLightingSettings`, `LogLightingCounters`),
  `gfx_deko3d_grass.cpp` (`PublishPicaFrameTemporalSample`). Guard "lighting by frame kind".

### Cheaper per-pixel lighting: path variants (0.1.2-test.8 report, `logs16`, 07/11)
- **Seen:** Per-pixel at 60 FPS is the GPU limit in busy rooms. Forest Temple room 8 (`bmori1 room=8`):
  `dkperf busy_ms` median 14.0 ms at `drs_scale` 2.25, whole windows saturated at 16.7 ms (host frames
  32400-34200), 25.7 ms per frame at 30 FPS / 3.00; 305 of its 380 late frames are render-caused. Its lit
  draws (38-55 per frame, every one blended) cover 63% of the frame's viewport pixels (`px=25.92/40.82M`,
  an upper bound), all with the same block shape: `control=1/x/0/0 env=1 rim=0.4 baked=0`, 0, 1 or 3 points.
  The audit's "+32 ms" was test.7 (184 interpolations per variant, fixed in test.8) at two samples per line
  [INFERENCE: not measurable again, no Native run of room 8 since].
- **What a pixel executed** (uam `-r` + envydis of the per-pixel variants of 29 pack texts, and a partial
  evaluator of the SASS that follows the warp-uniform branches for a given block, `~/oot3d-work/ppcost/tools`):
  test.8 added 269 issued instructions per pixel on room 8's path to a 198-instruction base (median; 340 with
  1 point, 486 with 3), and 43 registers (13 in the base). About 85 of the 269 were branch bookkeeping
  (`SSY`/`SYNC`/`BRA` and their tests) of block tests whose answer never changes in the shipped profiles:
  every profile in `environments/` has native direct 0, only the key directional, rim `independent`. The
  loops (points, directionals, native and environment lights) also loaded their members by computed index.
- **Fix (shader text and one hook; output the same floats as test.8):**
  - loops written out with constant slots (no loop control, no indexed loads), count tests nested; the
    rim's view and Fresnel terms once per pixel behind the rim's branch (were per light); each shared term
    computed in the one path that uses it; a baked fill light without gain (all profiles) skipped (it added
    zeros).
  - `const uint pica_surface_path` in the text: 0 (generic, any block), 1 (environment: environment on, no
    native direct light, no scene directionals, no scene-light rim, not baked, lit through the environment
    model), 2 (the same, baked). The constant settles those tests at compile time. `Deko3d::
    ClassifyPerPixelSurface` picks the path from the draw's built block; `ApplyPicaLighting` then uses that
    path's variant when the pack has it, else the generic one (a missing path variant is recorded like any
    missing stage). The offline tool writes all three per text.
- **Numbers (median over the pack texts; added issued instructions per pixel on each path):**

  | | test.8 | now, generic | now, path |
  |---|---|---|---|
  | static instructions / GPRs | 2803 / 43 | 1827 / 38 | 595 / 22 (environment), 667 / 28 (baked) |
  | branches (`BRA` / `SSY`) | 42 / 51 | 12 / 43 | 3 / 8, 4 / 8 |
  | room 8, 0 / 1 / 3 points | 269 / 340 / 486 | 243 / 295 / 405 | 159 / 206 / 306 |
  | baked (Hyrule Field), 0 / 1 point | 308 / 379 | 249 / 301 | 214 / 261 |
  | rim from scene lights, 3 native lights, 2 points (generic only) | 967 | 727 | — |

  `frag_3555e8fa561f9ca5` (Link): 516 → 402 issued per pixel in room 8, 43 → 22 GPRs.
- **Expected [estimate, not measured]:** room 8's lit pixel 575 → 430 instructions (its 0/1/3-point mix),
  about −21% of the frame's fragment ALU; 22 registers instead of 43 allow 64 warps per SM instead of 42.
  `busy_ms` in room 8 at 2.25: 14.0 → about 11.5-12.5 ms, and the saturated windows below 16.7 ms.
- **Tests:** `tests/switch/test_perpixel_lighting.py` evaluates the text against the frozen test.8 text
  (`tests/switch/data/perpixel_lighting_test8.glsl`): the same floats on 1500 random blocks over every path,
  on 1500 blocks through the path variant their classification picks (the Python mirror of
  `ClassifyPerPixelSurface`), and on the degenerate inputs; still the same bytes as test.7.
  `oot3d_deko3d_lighting_tests` (`TestPerPixel`, `TestPerPixelPaths`: classification and path texts),
  `oot3d_deko3d_backend_tests` (`TestFrameBreakdown`: the per-path counters). Guards "per-pixel cost".
- **`deko3d.pack` must be rebuilt** (`deko3d_shader_pack.py --lighting-variants`): 864 per-pixel texts
  (three per fragment text); the path variants are small, so the pack grows about 10% [estimate].
- **Risk:** the evaluator checks GLSL semantics; the GPU compiler may fuse multiply-adds differently in
  the generic and the path texts (folded constants), so a channel may differ by one step of 255 now and
  then [INFERENCE]. Draws whose block leaves the shipped shape (Scene lights tab: native direct light, rim
  from scene lights, environment off) use the generic variant, which is still cheaper than test.8.
- **Check (one minute):** after the pack rebuild, Per-pixel, 60 FPS, Forest Temple room 8 for about a
  minute. `logs/deko3d.txt`: `lighting (frame)` shows `paths g/e/b=0/N/0` (the environment path for the
  lit draws) and `missing=0`; `logs/perf.txt`: `dkperf busy_ms` in the `bmori1 room=8` windows against
  logs16 (14.0 median at 2.25, windows stuck at 16.7), and fewer render-late frames in `hitches.txt`. The
  picture must look the same.
- Code: `gfx_deko3d_lighting.cpp` (text, `ClassifyPerPixelSurface`, `PerPixelFragmentVariant(text, path)`),
  `gfx_deko3d_lighting.h`, `gfx_deko3d.cpp` (`ResolveLightingFragment`, `ApplyPicaLighting`),
  `gfx_deko3d_programs.h` (counters), `tools/oot3d/deko3d_lighting_variants.cpp`.
### Per-pixel blink at 30 and 60 FPS: what `log16` says, and the per-frame watch (0.1.2-test.8, 07/11)
- **Seen (test.8):** with Per-pixel, models still "blink" now and then, at 30 FPS too: one frame shaded like
  native lighting (warm, no rim).
- **What the `log16` counters show** (`deko3d.txt`, 21 windows of 1800 frames; scenes from `perf.txt`):
  - **Title screen (scene 0x6B, frames 300-3600):** `no env 8616` of 44731 and `no env 15300` of 57777 lit draws,
    11-17 per frame in both frame kinds. These are the title logo's draws: the composition tracker marks the
    logo actor `IsolatedLighting` (`IsTitleLogoActor`, `oot3d_native_pica_composition.cpp:345/482`), and
    `BuildPerPixelSurface` lights it with its own lights, no scene environment and no rim, by design (upstream's
    rule). Not a blink.
  - **The 32-146 `no env` draws of the other windows** are the area changes: every `scene environment off`
    line is followed by `on` two device frames later, and each window's count is the number of changes times one
    frame's lit draws, the same in both kinds (5400-7200: changes at 6362 and 7016, 88 = 2 x 44; 7200-9000: 90 =
    2 x 45; 18000-19800: three changes, 146). For those two frames the scene-view capture has no scene
    (`NativeA32SceneViewProbe::Capture`: the new PlayState's View is not set yet), so the environment publishes
    none. [INFERENCE] the new scene is still under its fade from black then.
  - **`native=`** (1386/237, 4853/4362, 4889/2358) appears only in the windows through the Castle Town ruins and
    its entrance, after `shaders: fragment de28aff122da114a not in the pack` (frame ~7018): `variants missing=1`.
    The draws of that program keep native shaders every frame they appear (a steady fallback of one material, not a
    flip). The text is already in `~/workshop-OOT/shaders/console-missing/`: the next `deko3d.pack` build covers it.
  - **Steady windows** (Temple of Time at 30 FPS, 21600-25200; Hyrule Field; Forest Temple room 8): `no env 0, no
    rim 0, native=0` in both kinds, lit draws per frame equal across kinds (47.0 vs 47.2). So in the frames that
    blink, the per-draw decision either refuses Link's draws (`ineligible`, which the window totals cannot place in
    one frame) or the GPU draws something other than what the CPU decided.
- **One mechanism the code allows** (not yet seen on the console): a world model is refused as `not_scene_world`
  when its draw arrives without its composition. The frontend tags every draw of a command list submitted in the
  HUD phase as UI (`oot3d_native_pica_frontend.cpp`, `resolveComposition` returns nothing when the list's domain is
  `Ui`; the domain flips at `kTopScreenGameplayCompositionPrefix` and returns to Scene at each guest refresh). A
  scene piece the game's graphics library submits late (a speculative or completion-driven split) would then lose
  its world layer, its light binding and per-pixel lighting for that frame. Spans can also be dropped when they
  cross a submission (`PartiallyOverlappingSubmissions`) or rejected over an unconsumed span
  (`OverlappingPacketSpans`).
- **New per-frame watch (decides it on the next run):** `Deko3d::LightingTransientWatch`
  (`gfx_deko3d_lighting_draw.cpp`) keeps the last three presented samples (one per
  `PublishPicaFrameTemporalSample`; draws of another render-target namespace, the dependency flushes, are not
  counted). A sample that differs from both neighbours, where both light at least 4 draws, is a transient: under
  3/4 of their lit draws, or more lit draws without environment or rim, more native draws, or more world models lost
  to their composition. `logs/deko3d.txt` gets one line per transient (at most 48):
  `lighting transient: frame F (real|in-between) lit b/s/a, no env b/s/a, no rim b/s/a, native b/s/a, lost world
  b/s/a (class=count ...; first domain=D layer=L provenance=P pc=X actor=X list=X+N)` (before/sample/after).
  - Lost world model: refused `not_scene_world`, yet with a typed surface program and depth write
    (`ClassifyLostWorldComposition`). Classes: `ui_list` (a list submitted in the HUD phase: UI lifecycle, no source
    PC), `ui_span` (a UI scope's span), `no_span` (no span at all), `control_flow` (a primitive packet outside a CMB
    pass), `atmosphere`, `other`. Domain 0 unknown, 1 scene, 2 UI; layer 0 unknown, 1 opaque, 2 transparent, 3
    atmosphere, 4 UI; provenance 0 unknown, 1 CMB pass, 2 control flow, 3 UI lifecycle.
  - `lighting real/in-between` lines gain `lost world=W (class=count ...) transients=T`.
  - `perf.txt` gains `composition: spans_consumed spans_partial spans_overlapping spans_unattributed spans_evicted
    cmb_invalid` (per window): the composition tracker's losses.
- **Reading the next run:** a blink with a `lighting transient` line at that moment and `lost world` > 0 = the
  composition (the class says which path; `ui_list` = the HUD-phase submission); with `no env`/`no rim` = the
  environment; with `native` = a variant; with only `lit` lower = draws missing from the frame. A blink with no
  transient line = the GPU side (the variant drew with a block it did not receive, or the presented image is not the
  one drawn).
- **Cost:** a few counters per lit draw and a three-sample comparison per frame; nothing on the game thread except
  six subtractions per `perf.txt` window.
- **Check (one minute):** Per-pixel, Temple of Time, Link close up: 30 s at 60 FPS, then 30 s at 30 FPS. Note
  roughly when a blink is seen. Then `logs/deko3d.txt`: `lighting transient` lines (frame, class), the `lost world` and
  `transients` counts of the `lighting real/in-between` lines; `logs/perf.txt`: the `composition:` group in the
  same windows. No `deko3d.pack` rebuild needed for this change (one is pending for `frag_de28aff122da114a`).
- Code: `gfx_deko3d_lighting.h` (`LostWorldComposition`, `LightingSample`, `LightingTransientWatch`),
  `gfx_deko3d_lighting_draw.cpp`, `gfx_deko3d.cpp` (`ApplyPicaLighting`, `BeginLightingSample`),
  `gfx_deko3d_grass.cpp` (`PublishPicaFrameTemporalSample`), `oot3d_native_a32_window_frame.cpp` (perf line).
  Tests: `oot3d_deko3d_inbetween_lighting_tests` (`TestLostWorldComposition`, `TestLightingTransients`). Guards
  "per-pixel blink (test.8)".

### Per-pixel blink in test.9: late command lists and a watch that missed 60 FPS (`logs17`, 07/11)
- **Seen (test.9):** "the general flickers were still happening", with and without upscaling and OC.
- **What `logs17` says** (`deko3d.txt`, `perf.txt`; device frames mapped to wall time by `hitches.txt` `t=`):
  - The 48 `lighting transient` lines were used up by device frame 19203, before both videos. All are real
    frames with `lit N/N-1/N` and `lost world 1/2/1 (no_span=1 atmosphere=1 ...)`: one lit draw without any span
    for one game frame. Their `first` was the atmosphere draw every frame has (kankyo primitive, pc `003fb5ec`),
    not the lost draw.
  - `no_span` costs 16-45 world draws per window of ~900 real frames in every scene, and the same number in the
    in-between kind (18/18, 45/45, 25/25). In 60 FPS windows `transients` stays 0-2: an in-between frame repeats
    its real frame's draws, so the loss sits in both presented frames of a game frame and each had a neighbour
    that agreed. The watch was blind to every 60 FPS blink; the 30 FPS windows show 9-19.
  - `ui_list` (world models in a list the frontend tagged UI) appears only in the Forest Temple: 11, then 98/49,
    88/85 and 32/30 (real/in-between) in the windows 25200-27000, 27000-28800, 37800-39600 and 39600-41400.
    About two whole frames of lit world draws each (~50 per frame). The two videos (13:00:53 and 13:04:26) are
    device frames 25560-27339 and 37674-39452: both inside those windows. Second video, frame ~640 (21.4 s,
    device ~38950): the large pillar in front of Link is drawn light and textured for one frame between two dark
    ones.
  - `composition:` is zero for partial, overlapping, unattributed, evicted and invalid CMB spans in every window.
- **Cause of `ui_list`:** the frontend tags every draw of a list submitted while the domain is `Ui` (UI
  lifecycle, spans ignored). The domain turns `Ui` at `0x002E278C` inside Gameplay_Draw (and at the GameplayHud
  presentation entries) and back to Scene/Unknown at the guest refresh; the scene lists are normally kicked after
  the refresh [INFERENCE: world draws are Scene-domain in every steady window]. A list kicked after the next
  Gameplay_Draw passed `0x002E278C` (when the GPU queue runs a queued request on the previous one's completion
  [INFERENCE]) came through as UI: its world models lose per-pixel
  lighting, grass (`gfx_deko3d_pica_draw.cpp` keeps grass to Scene-domain draws) and get the UI widescreen
  placement evidence, for one game frame.
- **Fix:** `Oot3dNativePicaCompositionTracker` numbers each Gameplay_Draw (`CurrentWriteGeneration`) and stamps
  every span with the generation its bytes were written in (`WriteGeneration`). At a SubmitCommandList the GSP
  glue gives the frontend the last refresh's domain and the current generation (`SetCommandListTiming`). A list
  submitted in the `Ui` domain whose spans all predate the current Gameplay_Draw (`IsLateCompositionCommandList`)
  is an earlier frame's scene: it gets the on-time domain, so its spans and draws resolve exactly as when it is
  kicked on time. A list with any span of the current Gameplay_Draw (this frame's HUD, the pause screen) or
  without spans is unchanged. `config/late_lists_as_ui` (file present) restores the old attribution (A/B).
- **Not settled: `no_span`.** `CmbRenderer_SubmitDrawHandle` (`0x0030F4D0`) tail-calls `0x00452808`, which
  issues every visible mesh through `MeshCommandPacket_Submit` (`0x00466E2C`, a copy of the packet's `+0x10`
  bytes at `0x0054CC4C`; EUR code.bin, objdump), so a CMB draw's trigger lies in its mesh span; the draw without
  span is something else. New counters decide it next run (below).
- **Diagnostics (for the next run):**
  - `LightingTransientWatch` compares each frame kind with its own kind: a real frame with the real frames
    around it, an in-between frame with the in-between frames around it (at 60 FPS both report a lost game
    frame). `first` names the first draw lost to its composition (`ui_list`, `ui_span`, `no_span`,
    `control_flow`), never the atmosphere draw, with `vertices=` and `geometry=`. At most 96 lines.
  - `perf.txt` `composition:` gains `spans_invalid` (mesh/primitive packets whose range could not be recorded:
    misaligned, read failures, nested/unmatched primitive packets, invalid atmosphere ranges), `late_lists`,
    `late_draws`, `ui_world_draws` (draws left UI inside a world span: HUD-phase lists that are not late),
    `no_span_draws` (scene draws in no span of a list that has spans), `owner_holes` (of them, between two spans of
    one CMB renderer) and `bare_list_draws` (scene draws of a list without spans).
  - `perf.txt` lines `composition note: window_end=F <kind> list=X+size draw=+N domain=a->b spans=S
    generations=oldest..newest/current draws=D world=W` (at most 64): `late_list`, `ui_list_world` (a HUD-phase
    list with world spans that is not late) and `same_owner_hole` (with the spans before and after).
- **Reading the next run:** `late_lists` > 0 and `ui_list` gone from `lighting real/in-between` = fixed; with
  `config/late_lists_as_ui` the `ui_list` counts come back. `ui_world_draws` > 0 with `ui_list_world` notes whose
  generations equal the current one = a mid-frame kick within one Gameplay_Draw (not covered). For `no_span`: the
  transient's `list=X+N` against `bare_list_draws`/`no_span_draws`/`owner_holes`/`spans_invalid` in the same
  window says whether the draw's list had no spans, fell between two spans of one model, or lost a packet range.
- **Check (one minute):** Per-pixel, 60 FPS, Forest Temple (the rooms of `logs17`) for a minute, then 30 s at
  30 FPS in Hyrule Field. `logs/deko3d.txt`: `ui_list` in `lighting real/in-between` and the `lighting transient`
  lines; `logs/perf.txt`: the `composition:` group and `composition note:` lines. For the A/B, create
  `config/late_lists_as_ui` and repeat. No `deko3d.pack` rebuild needed.
- Code: `oot3d_native_pica_frontend.{h,cpp}` (`IsLateCompositionCommandList`, `SetCommandListTiming`,
  `Oot3dPicaCompositionResolveStats`, notes), `oot3d_native_pica_composition.{h,cpp}` (`WriteGeneration`),
  `oot3d_native_a32_ctr_host_gsp.cpp`, `oot3d_native_a32_window_{setup,frame}.cpp`, `gfx_deko3d_lighting{.h,_draw.cpp}`,
  `gfx_deko3d.cpp`. Tests: `oot3d_native_pica_frontend_tests`, `oot3d_native_pica_composition_tests`
  (`TestWriteGeneration`), `oot3d_deko3d_inbetween_lighting_tests` (`TestLightingTransients`). Guards "blink17".

### Per-pixel blink in test.10: draws a CMB renderer writes outside its mesh packets (`logs18`, 07/11)
- **Seen (test.10 video `2026110714542300`, Temple of Time, logs18a):** frame 102 of 363 (about 14:54:14.4) differs
  from both neighbours while they agree: the room's pillar and walls lighter, the left windows brighter and warmer,
  the chandelier darker; Link unchanged. Crops: `~/oot3d-work/flicker18/vid/` (`crop3b.png`, `win3b.png`). The
  other three test.10 videos show no such frame in the scene (fast camera turns, pause menu and file select only).
- **What `logs18` says:**
  - `ui_list` is gone: every `lighting transient` line in 18a/b/c (96 each) is `lost world 1/2/1 (no_span=1
    atmosphere=1)`, one scene draw in no span for one game frame, in both frame kinds. `late_lists` is about 75 per
    150 frames, one per game frame (13 draws in gameplay, 4 on the title screen): the fix moves them back every frame.
  - The video's blink maps (hitches.txt `t=` between frames 3597 and 3893) to host frame about 3750 ± 60; the
    transients there are frames 3739/3740 (`list=1458fa80+17712 vertices=1161`) and 3805/3806
    (`list=144af360+7540 vertices=8700`, a whole room mesh), each `lit 48/47/48`: one lit draw falls back to native
    lighting, which is what the frame shows.
  - The no-span draws are game-deterministic: the title demo loses the same draws at the same frames in all three
    sessions (frames 207/261/337/375/387/397: offsets +15894, +936, +2280, +5416, +4968, +19110).
  - `perf.txt`: `no_span_draws` 0-12 per window in every scene, `owner_holes` about half of them; `bare_list_draws`,
    `ui_world_draws`, `spans_partial/overlapping/unattributed/evicted` 0; `spans_invalid` only at scene loads. The 64
    `composition note:` lines were all `late_list` of the first 300 frames, so no `same_owner_hole` note was logged.
- **Ruled out (EUR code.bin):** the mesh span is exact (`0x00466E2C` reads the cursor through `0x002F9CA0` case
  `0x208` = `[0x0054CC4C]`, copies `[packet+0x10]` bytes and commits them through `0x002F9C88`); the only draw
  trigger the code emits (`0x000F022F` at `0x003135A0`) is in the primitive builder `0x00313444`, whose three callers
  are all tracked. The product build runs every slice with `skipFirstBlockEntry=false` (`DispatchWholeAotProduct`),
  so the mesh hook is not skipped. Between two mesh packets `0x00452808` calls `0x0032471C` (a command writer on
  the cursor), the material vcalls `+0x24/+0x20/+0x14` and `0x002F9C74`: bytes inside the pass that no span covers.
  Which of them carries the lost draw is not proved [INFERENCE: an owner hole lies between two mesh spans of one
  renderer and pass, so inside that pass].
- **Fix:** `Oot3dNativePicaCompositionTracker` records each scene CMB pass, from `CmbRenderer_SubmitDrawHandle`'s
  entry to its return (`[0x0054CC4C]` at both), as a pass span: the pass attribution (layer, actor, projected
  shadow), the light binding of its last mesh span, no temporal identity. Passes inside a UI or atmosphere scope are
  not recorded. `TakeCommandListPassSpans` hands the passes of a submitted list to the frontend
  (`SetNextCommandListPassSpans`); a scene draw in none of the list's spans but inside a pass resolves to the pass.
  `config/no_pass_span_fallback` (file present) restores the old behaviour (A/B). A draw outside any pass is
  unchanged.
- **Diagnostics:** `perf.txt` `composition:` gains `passes passes_consumed passes_lost` (tracker) and `pass_draws`
  (no-span draws resolved to their pass). Every no-span draw is noted (`no_span` or `same_owner_hole`) with
  `vertices=`, the spans before and after it and `pass=` (the pass holding it, or `none`); `late_list` notes are
  capped at 8 so these reach the log.
- **Reading the next run:** `pass_draws` equal to `no_span_draws` and no `lighting transient` lines (or only with
  `pass=none` notes) = fixed. `pass=none` notes name the remaining writers (the spans around the draw and the
  renderer). With `config/no_pass_span_fallback` the transients come back.
- **Check (one minute):** Per-pixel, 60 FPS, Temple of Time, stand still and turn the camera slowly over the pillars
  and windows for 30 s. No one-frame brightening of the room. `logs/perf.txt`: `pass_draws` against `no_span_draws`,
  the `composition note: ... no_span|same_owner_hole ... pass=` lines; `logs/deko3d.txt`: `lighting transient` lines.
  No `deko3d.pack` rebuild needed.
- **Risk:** a resolved draw has no temporal identity, so in an in-between frame it is drawn like a draw that has no
  match [INFERENCE: at most a one-frame position step of that draw instead of a lighting change]. One more cursor
  read per CMB pass entry and exit and a vector of passes per list.
- Code: `oot3d_native_pica_composition.{h,cpp}` (pass spans), `oot3d_native_pica_frontend.{h,cpp}`
  (`SetNextCommandListPassSpans`, `SetPassSpanFallback`, notes), `oot3d_native_a32_ctr_host_gsp.cpp`,
  `oot3d_native_a32_window_{setup,frame}.cpp`. Tests: `oot3d_native_pica_composition_tests` (`TestPassSpans`),
  `oot3d_native_pica_frontend_tests`. Guards "flicker18".

### Landing roll (N64): L3+R3 > Game > Gameplay (`config/no_landing_roll`, 07/11, on by default)
- **Seen (OoT3D since 2011, GameFAQs "Link no longer rolls after jumping off a high cliff"):** after a run
  jump off a ledge, Link never rolls on landing, even with the stick held forward; he still rolls after
  walking or dropping off a ledge.
- **Cause (EUR code.bin, Capstone):** the N64 roll branch is still in the fall action `FUN_004bba4c`
  (`Player_Action_8084411C`): `0x004BC0E8..0x004BC160` (fallDistance `<= 80` short landing, `< 800` with
  stick forward and nothing carried → `Player_SetupAction(Player_Action_Roll 0x00492A5C)` + the landing-roll
  animation at 1.25, then the exit, without the fall-damage call `FUN_002bc420` at `0x004BC16C`). What
  OoT3D dropped is the mid-air `LinkAnimation_Change(normal_landing)` (N64 `z_player.c:9604`): at
  `0x004BBE10` it stores only actionVar2 = -1. A run jump keeps its animation (id `0x6B`) to the ground and
  the run-jump-end check at `0x004BC080` (`ldr r0,[r4,#0x284]; cmp r0,#0x6B`) wins over lock-on, short
  landing and roll. Player fields: animation `+0x284`, stateFlags1 `+0x1710` (carrying `0x800`, hostile
  lock-on `0x10`, = `Player_CheckHostileLockOn` `0x003518CC`), stick ring index `+0x222A` (u8) and
  directions `+0x2231` (s8[4], forward 0), actionVar2 `+0x2238`, fallDistance `+0x2282` (s16).
- **Fix (no generated-code change, shards and PGO profile untouched):** `0x004BC080` is a block-entry hook
  (`oot3d_native_a32_window_setup.cpp`); the policy (`oot3d_native_a32_projection_policy.cpp`) reads the
  player at r4 and, when `DecideLandingRoll` (`oot3d_landing_roll.h`) says Roll, leaves the whole-AOT graph
  with the block's registers and resumes at `0x004BC120`, the game's own roll setup in the same function
  and stack frame. Roll = run-jump animation, actionVar2 < 0 (the fall passed the point where N64 changed
  the animation; a jump that lands before it keeps the run-jump end on both), no hostile lock-on,
  80 < fallDistance < 800, stick forward, not carrying. Everything else runs the game's own landing; falls
  of 800 or more still hurt. Both PCs are block starts and dispatch entries of the title module
  (`fork-gen-7baf1fde8b-pgo`, shard 033); the hook only diverts under whole-AOT execution (always on in
  the product).
- **Option:** L3+R3 > Game > Gameplay > "Landing roll (N64)", applied at once; off writes
  `config/no_landing_roll` (`oot3d_gameplay_settings_panel.cpp`). Cost: one hook per landing.
- **Check (one minute) [INFERENCE: heights not measured]:** Kakariko Village, climb the lookout tower ladder
  and run off the top platform (Link auto-jumps): stick held forward → roll, no damage, no stun; stick
  released in the air → the game's landing with the small fall damage (N64: half a heart) and the stun.
  Any drop that costs the small damage without the stick is in the 400-800 range (800 or more costs the
  big one, and never rolls). With the option off, holding forward gives the old OoT3D landing (no roll,
  damage).
- Tests: `oot3d_landing_roll_tests` (falls 80/81/400/799/800, stick forward/released/back/side, carrying,
  lock-on, run-jump end, walked off; the same cases through guest reads at each stick-ring slot; the
  config file). Guards "Landing roll".

### Version, Exit game and OTA updates: L3+R3 > Updates
- Version: CMake cache variables `OOT3D_RELEASE_VERSION` (semver, at most 15 characters, the NACP
  field; default `0.1.0-dev`) and `OOT3D_RELEASE_BUILD` (monotonic integer; default 0). Both go
  into `oot3d_build_version.cpp` only. Players see the version alone (NACP `VERSION`, the first
  line of the L3+R3 menu, the Updates tab); the build number orders releases across channels for
  the updater and appears only in `release.json` and `logs/update.txt`.
- "Exit game" (end of the menu's first line) asks for confirmation ("Progress since your last save
  will be lost.") and then closes the window from the game thread, the path HOME > Close takes:
  the render stream discards queued frames and `Finish` runs. `main` then sets libnx's
  `__nx_applet_exit_mode = 1` and returns, so libnx's exit closes the application through AM
  (`ISelfController::Exit`) instead of returning to the homebrew loader: on return hbl maps the
  next NRO into memory this process's services still hold and aborts (crash report on the console:
  `hbl + 0x3810`, 2001-0106 InvalidCurrentMemory). Ending the process with `svcExitProcess`
  (1.0.0-test.2) avoided that, but the console still reported a crash on exit.
- Updates tab: installed version; channel Stable (releases only) or Test (also
  pre-releases), saved in `config/update_channel` (default: Test for a pre-release build);
  repository `JohnUzoka/TriAevum-Switch`, or `config/update_repo` (`owner/name`). "Check for
  updates" reads `GET /repos/{repo}/releases` (unauthenticated: 60 requests an hour per address)
  and offers the highest-version release tagged `v<version>` whose `release.json` is valid and
  whose files are all assets of the listed size (and of GitHub's `digest`, when listed). A newer
  build is "Download and install", the same build "Reinstall (repair)", an older one "Install this
  older version". Release notes are the release body.
- `release.json`: `{"format": "oot3dre_switch_release_v1", "version", "build" (> 0), "channel"
  ("stable" for a full release, "test" for a pre-release), "files": [{"path", "size", "sha256"}]}`.
  Paths are relative to `sdmc:/switch/oot3dre/`, `[A-Za-z0-9._-]` components, never under
  `game/`, `savedata/`, `logs/`, `config/`, `update/`, `lab/`, `bench/`, never
  `game_language.json`, `.new`/`.old`/`.tmp`; `oot3d_native_game.nro` is required; each file's
  asset is named after its last component.
- Install (worker thread; sockets and libcurl only while it runs; libcurl follows the redirect to
  GitHub's CDN, HTTPS only, certificates checked by the system's SSL service): checks free space,
  downloads each file to `<file>.new` with its SHA-256 (mbedcrypto) and size, checks the NRO's NACP
  version, then journals the files other than the NRO in `update/pending.json` and swaps the NRO
  (`.nro` → `.nro.old`, `.nro.new` → `.nro`; Horizon cannot rename over a file). The others may
  be open (`resources.o2r` is), so the next start swaps them before anything opens them
  (`FinishPendingUpdate` in `main`; a journal for another build is dropped). Cancel or any failure
  removes the `.new` files and leaves the installed version as it was. Log: `logs/update.txt`.
- After an install the tab offers "Close game" (confirmation), which exits like Exit game; the new
  version runs at the next launch. The loader cannot start it directly (see Exit game). Rollback:
  rename `oot3d_native_game.nro.old` back to `oot3d_native_game.nro`.
- The first-launch screens (`oot3d_switch_console_screen.h`) put stdout and stderr back after
  `consoleExit`: libnx left them on the freed console, so the game's first log line crashed
  (`ConsoleSwRenderer_drawChar`, null write) right after the import.
- Without `config/atlas_overrides.o3tu` (built on the console from the third-party TopScreen mod,
  CC BY-NC-ND, not shippable; see "HUD textures from the TopScreen mod") the game starts without
  TopScreen texture overrides instead of refusing.
- Logic and install steps: `oot3d_update_release.{h,cpp}`, tests `oot3d_update_release_tests`.

### Stable heap and less phantom work per frame (2026-10-17)
Measured (`switch-logs/interp1`, fps60, scale 2, 8.5 min): `heap_arena_mb` 631 → 1659 MB with
`heap_used_mb` ~700 MB (fragmentation of newlib's malloc, which Mesa/nouveau also use: devkitPro's
`libdrm_nouveau` allocates each BO with `memalign`).
- **Plan vector per visual frame**: a draw plan is ~16 KiB; each game frame built
  a new vector of ~200 plans (3–4 MB) for the visual frame and another for pending draws (which
  still grew by doubling), and freed those of old frames on the render thread: large blocks of
  varying sizes at 30/s. Now `Oot3dPicaVisualFrame::Draws` is an `Oot3dPicaPlanVector`, whose
  memory returns cleared to a pool (up to 6) when the frame dies; the accumulator takes from there and the
  pending vector keeps its capacity.
- **Interpolation palettes** (`ShareVisualPaletteMotions`, ~1.7 KiB per draw) were allocated on every
  sample: now a scratch buffer in the prepared transition, reused.
- **Sample views**: vertex bindings are no longer cleared between frames (without
  `HasVertexBindingOverrides` they are ignored), so the interpolated copy reuses the bytes.
- Long-running host test: `oot3d_native_pica_visual_frame_soak_tests` (1800 frames,
  180–220 draws, matching + 2 samples per frame, counted `operator new`): before 12 MB and 4.8
  allocations ≥ 256 KiB per frame; now ~0.24 MB and 0 large, constant over time.
- **Visual frame preparation (fps60) without maps or per-draw hashing**: matching between the previous
  and the new frame uses sorted lists (identity, index) with a merge join instead of
  `unordered_map`/`std::map` (compact native key of 13 × u64, no padding), and all
  scratch (marks, keys, monotonic DP table, identities) lives in the prepared transition.
  `ShareVisualPaletteMotions` uses groups and bones in reused flat vectors and runs once
  per transition (does not depend on alpha). `CmbPaletteSlots` validates the bone attribute once and
  remembers the result of the per-draw vertex scan, keyed by the shared bytes
  (immutable, held in the memo) and by the layout: in steady state the scan does not run.
  `NormalizedVertexUniformDelta` and uniform interpolation skip equal rows (exact,
  including -0.0). `perf.txt` gained `rt_prepare_ms` (matching + analysis), `rt_match_ms` and
  `rt_sample_ms` per host frame. Proof of equivalence:
  `oot3d_native_pica_visual_frame_equivalence_tests` compares against a frozen copy of the previous
  code (`..._reference.inc`) matched pairs, statistics and samples bit for bit over thousands of
  random frames (also with `-O3 -ffast-math`, ASan/UBSan and TSan `--threads`); `--bench`
  measures the cost per game frame (Prepare + samples at 0 and 1/2).
- **PICA upload rings ≤ 2 MiB** (were 8/4 MiB): nouveau suballocates buffers up to 2 MiB from slabs
  it maintains (`nouveau_mm`); above that each ring orphan was a new 8/4 MiB `memalign`
  (1610 orphans in the session).
- **ImGui without a GL context**: with the render thread, `ImGui_ImplOpenGL3_NewFrame/RenderDrawData`
  ran on the game thread, where the context is not current: everything went to Mesa's null dispatch,
  and the backend rebuilt shaders, buffers and the font atlas upload (~1 MB) every frame. That was the
  `tex_upload_kb` ≈ 1 MB and part of `gen_delete`/`gets`/`buf_reallocs` even on hold frames (the
  glperf hooks count calls from any thread). Now without a current context only the atlas is
  kept built on the CPU; nothing from ImGui appeared on screen before and that remains so.
- **Legacy UI (TopScreen HUD)**: triangles go to a 1 MiB ring via unsynchronized map
  (orphan only on wrap) instead of one `glBufferData` per draw (~29 per frame);
  the UI renderer no longer allocates per frame (insertion sort instead of `stable_sort`, quad
  in `std::array`, texture key and scratch vectors retained).

### Per-GL-program heap on Mesa 26 (2026-10-17)
- With Mesa 26 the heap grows ~6–8 MB per new PICA program (20.1: ~1.5 MB). Mesa 26 converts the
  GLSL to NIR already in `glCompileShader` and keeps that NIR in the shader object; since the shaders
  stayed attached to the program, `glDeleteShader` only flagged them and the NIR lived along with the program.
  `LinkPicaGlProgram` and the scanout program now do `glDetachShader` before deleting.
  Measured on the host (Mesa 26.2.2, iris and llvmpipe, real 30 KB PICA VS): −2.0 MB per program
  (iris 8.6 → 6.6 MB).
- The rest is the driver's: Mesa 26's nvc0 keeps the whole NIR (`nvc0_program::nir`, only used to
  retranslate when the number of clip planes changes) in addition to st/mesa's serialized NIR; 20.1
  kept only TGSI tokens. Reducing this requires a Mesa patch (serialize `prog->nir` after
  `nvc0_program_translate` and deserialize only on retranslation).

### GPU: ZCULL/early-Z on nvc0 and exact savings `config/gl_gpu_lean` (2026-10-17)
**What Mesa 20.1 (nvc0) does** (source in `mesa-cache/work/mesa-20.1.0-rc3`):
- **ZCULL never turns on.** `nvc0_screen.c` writes `ZCULL_REGION = 0x3f` ("deactivate ZCULL") at screen
  creation and `nvc0_validate_zcull` is under `#if 0`; no path reactivates it. devkitPro's
  `libdrm_nouveau` even does `nvGpuChannelZcullBind`, but with no region configured the hardware doesn't use it. Depth
  format, clears or state don't change that: ZCULL (nfsmw-nx's ~2 ms) only with a Mesa patch
  (port deko3d's region/storage/invalidation configuration, which enables ZCULL by default)
  or with deko3d/NVK.
- **Per-pixel early-Z is automatic** on Maxwell, unless the fragment program header says
  it kills pixels (`discard` anywhere in the GLSL → `usesDiscard` → `hdr[0] |= 0x8000`),
  writes depth (`gl_FragDepth` → `hdr[19] |= 0x2`, and Mesa also sets `ZCULL_TEST_MASK = 0x11`) or
  writes memory (image/atomics → `hdr[0]` bit 26). Depth function, blend or logic op don't disable
  early-Z.
- Depth compression: devkitPro's libdrm reports version `0x01000202` (≥ `0x01000101`), so
  nvc0 picks compressed memory types for Z24S8; clearing depth **and** stencil of the whole
  buffer is the case the hardware can resolve by just marking the tiles.
- The PICA programs were already right for early-Z: in specialized mode (the only one used on GL; the
  parametric/uber one is Vulkan-only) `discard` only exists with PICA alpha test enabled (function ≠
  ALWAYS) and `gl_FragDepth` only outside the "raster depth" form (`Oot3dPicaFragmentDepthIsRasterDepth`:
  scale -1, offset 0, z-buffering). That's why the shaders were **not** touched (changing the source
  invalidates Mesa's shader cache with no proven gain). The remaining cases are counted (below).

**`config/gl_gpu_lean`** (on by default; `0` disables for A/B; desktop `TRIAEVUM_GL_GPU_LEAN=0`),
`gfx_opengl_pica_gpu_lean.h` (no GL, tested in `gfx_opengl_pica_gpu_lean_tests`):
1. **Logic op COPY with blend off → logic op off.** Every opaque PICA draw reached the
   hardware with `LOGIC_OP_ENABLE = 1` (Mesa doesn't filter COPY), a ROP operation that may read the
   destination. COPY = s per the spec.
2. **Blend ADD with source ONE and destination ZERO (color and alpha) → blend off** (s·1 + d·0 = s).
3. **A draw with no effect is not emitted**: empty color mask, or blend that keeps the destination
   (ZERO/ONE/ADD), or logic op NOOP; and no depth write, no stencil that changes, no image
   (Shadow2D producer is never skipped). Prior state and uploads stay the same.
4. **Depth fill on a target without stencil in PICA (D16/D24)** also clears the stencil of the D24S8
   renderbuffer: the frontend only enables the stencil test with format 3 (D24S8), so that plane is never read
   or written; the clear becomes a full depth+stencil one instead of masked.
- Proof of identical image: the unit test compares the GL per-fragment operations model
  (all 256 destination colors × dense source sweep, including outside [0,1]); and a pixel
  check on real GL (windowless EGL, Intel UHD 620 and llvmpipe, 20 random scenes of 300
  triangles per case, 257×193): 0 color bytes and 0 depth values differing in all four cases (the
  same check flags millions of differences with XOR instead of COPY or without the depth clear).
- **On the console**: same still scene (market, fixed camera, frame paused via the menu), one album
  screenshot with `gl_gpu_lean` absent and another with `0`; compare pixel by pixel on the PC (the
  album capture is JPEG: compare with tolerance or use the homebrew's PNG captures, if any).
  To measure: `config/gpu_timing` with and without `gl_gpu_lean`, same scene, `step_gpu_pica_top`.
- Expected gain: uncertain without measuring. (1) hits nearly all opaque draws in the scene — if the Maxwell
  ROP reads the destination with logic op on, that's ~4 B per opaque pixel drawn (≈ 3.3 MB per overdraw
  layer at 960×853), something like 0.2–0.6 ms on the full frame; if it doesn't read, ~0. (2)/(3) depend on
  how many draws the game sends that way (counted). (4) ≤ 0.1–0.2 ms per clear.

**New counters in glperf** (`config/gpu_timing`, per host frame and type): `n_fs_discard`,
`n_fs_depth`, `n_fs_image` (draws whose program disables early-Z), `n_blended`, `n_logic_copy_off`,
`n_blend_identity_off`, `n_noeffect_skipped`, `n_full_depth_clears`. With them we can decide whether it's worth
specializing further (e.g. replacing `gl_FragDepth` with `glDepthRange` — not done: depth rounding
in hardware × shader may differ in the last bit and change LEQUAL/EQUAL tests).

**glperf sampling fixed**: with fps60 step and intermediate frames alternate, and
`FrameSerial % 30 == 0` always landed on the same type — no `perf.txt` had `step_gpu_*`. Now the
sample advances one frame every block of N (one per block, alternating parity).

**Evaluated and not done (no proof of identical image or no gain):**
- Reordering draws / "sky last": without ZCULL, sorting only helps per-pixel early-Z when
  opaque comes front to back; PICA requires the game's order for blend, depth EQUAL and stencil, and the
  backend has no safe marker in PICA state saying "this draw is the sky" (only heuristics by
  texture/state, which can be wrong and change the image). High risk, uncertain gain.
- "Copy-less resolve" of the display transfer: the copy (0.2–0.8 ms measured) would have to be deferred until the
  target is rewritten, and it is rewritten every frame (the display image is re-presented on
  hold frames/reused EASU): the copy would just move elsewhere.
- Full-screen passes: the window clear and the repeated copy are already skipped (`gl_lean`); RCAS
  (~1.9 ms) and the UI (~1.5 ms) change the image if removed.
- Shaders: the specialized one already has branches resolved at generation; without per-program measurement it's
  not worth invalidating the shader cache.

### Game Over without the "Continue?/Save?" prompt (2026-10-17)
- Symptom: on dying, "GAME OVER" appears (with the fade running too fast) and the choices never
  appear (one frame at most); the Game Over controller layer disappears. It didn't depend on Mesa.
- Cause: OoT3D's Game Over is drawn by the bottom-screen controller; TopScreen migrates the
  choices to the top screen after 60 draws of delay + 24 fade steps (2.1.1
  `005CF75C`/`005CFB38`) and composites them into the frame (viewport 0,40,480,320, auxiliary `PauseUi_Draw`, then
  the native draw `0x00427A3C`; `docs/TRIAEVUM_ISSUES_45_46.md`). In the product this is done by the hook
  at the `0x00427A3C` entry. Since the upstream commit that enabled the whole source port in
  product builds (80ef0f6), the `0x00300544` call also ran the mod route with the SAME state: delay and
  fade advanced twice per frame and, once the fade finished, the call suppressed the native draw, so the
  `0x00427A3C` hook (which draws the choices) never ran again.
- Fix: with the `0x00427A3C` hook active (product), the `0x00300544` call only diverts to the
  native draw in scene 3 (Game Over); scene 7 and development builds keep the mod route.
  Policy in `TopScreenRendererVisibilityCallDefersToGameOverDraw`
  (`oot3d_top_screen_mod_profile_pause.cpp`); test in `oot3d_top_screen_mod_profile_tests`
  (simulates the frames: before ≤ 1 composited frame, now every frame from the 84th on).
- **Console test** (`config/debug_force_game_over`, off without the file): zeroes health once
  after N seconds of normal play (file text = N; empty = 10; `0`/`off` disables),
  via SaveContext memory (`0x0058799C`); death, Game Over and the choices are the native ones.
  Desktop: `TRIAEVUM_DEBUG_FORCE_GAME_OVER=10`. Load a save, wait, and check: "GAME OVER",
  then "Save?" Yes/No centered, then "Continue?"; No/Yes must return to the game with 3
  hearts or go to the title. Delete the file afterwards (fires once per session).

### Giant polygon in 1st-person aim: clipping at the near plane (2026-10-17)
- Symptom (since the 1st Switch version): when aiming the slingshot, a huge brown polygon came out of the
  top-left corner and covered half the screen for an instant ("the textures jump and come back").
  It persisted with `config/no_guest_overrides`, with `fps60_mode=legacy` and didn't depend on interpolation.
- Cause: `GfxRenderingAPIOGL::Init` enables `GL_DEPTH_CLAMP` (for the legacy renderer) and never
  disables it; the PICA path drew with it. With clamp GL doesn't clip at the near/far planes: a
  triangle with a vertex between the eye and the near plane (arm, sleeve or slingshot stuck to the camera in
  1st person; PICA z > 0 with w > 0) was rasterized whole, with depth pinned to near, instead of
  clipped as on PICA (and on the PC's Vulkan, `depthClampEnable = false`; the GL dialect
  `z_gl = -2z - w` already assumes clipping).
- Fix: PICA draws run with `GL_DEPTH_CLAMP` off (`PicaGlStateCache::DepthClamp`, one
  toggle per PICA → legacy transition, not per draw); legacy gets it back in
  `RestoreLegacyVertexArray` (before every legacy draw) and in `ShutdownNativePica`.
- Tests: `gfx_opengl_pica_near_clip_tests` (real GL context via surfaceless EGL, skipped without it):
  shader through the real GL dialect, "sleeve" triangle with one vertex 0.2 from the eye (near = 1);
  with clamp on it covers 9295 px versus 4334 px of the PICA clip (CPU reference), and a
  triangle entirely inside near covers 17280 px instead of 0; with the backend sequence, 4325 px and 0.
  `gfx_opengl_pica_state_cache_tests` (`PicaGlDepthClamp`): every PICA draw without clamp, every legacy one with.

### File select in widescreen: BACK tab, transition fade (2026-10-17)
- Symptoms (console screenshots, widescreen and `widescreen_menus` on): the "B BACK" tab sits at the
  menu corner (x=128 instead of 160, of 1280); the card sliding in appears up to x=1152; the
  black fade when picking the save (and the darkening of the Normal/Master Quest choice) covers only the
  menu's 4:3 rectangle and the sky stays on the sides.
- Evidence: Linux product build with OpenGL on the host (Xvfb/llvmpipe, 1280x720, TopScreen, scale
  2, `TRIAEVUM_GL_WIDESCREEN=1`) reproduces the console screenshot; the diagnostic log below showed the
  classification of each draw.
- Cause 1 (BACK/cards): the draws of the relocated bottom menu (viewport 0,40,480,320, orthographic
  320x240, square pixels) come attributed to the atmosphere layer (Scene domain, layer 3: the menu
  runs inside the environment queue of the title screen's `Gameplay_Draw`), and the 111c86e policy
  left atmosphere stretched with the scene. The whole menu stretched 6.7% (viewport 40..360 -> x
  128..1152): the cards only looked in-frame; the BACK tab (flush with the bottom
  screen's corner) and the sliding card, near the edges, gave it away. Fix: in the relocated viewport the
  atmosphere layer doesn't count (no environment effect uses Y 40..360); on the full screen it stays
  tied to the scene.
- Cause 2 (fade): the black is a Ui-domain quad, untextured, with the top screen's 400x240 orthographic
  drawn in the menu viewport; it went to the frame and covered only the 4:3 (x 160..1120), with the scene
  (the sky, which the frontend composition keeps behind the menu, `TRIAEVUM_TOPSCREEN_FRONTEND_SKY.md`)
  in the 16:9. Fix: a draw in the relocated viewport, untextured, without game scissor and whose geometry
  provably covers the native viewport (coverage proof now computed for it too; with a
  widened frustum the native edge sits at 1/factor, `PicaDrawGeometryNdcBounds`) covers the whole widened
  target. Textures, buttons, bars and the Game Over panels (Ui domain, same viewport)
  stay in the frame.
- Compensated perspective draws (viewport widened around the center) are now clipped to
  their native viewport: the widened viewport only restores proportions, it doesn't show what on the 3DS is
  off-screen.
- **Bottom corners of the menu at the screen corners** (user request: BACK at the corner of the 16:9 screen,
  not at the corner of the menu's 4:3): the BACK tab comes in the SAME draw as cards, header and text (1104
  vertices), so the relocated menu draw is emitted in cutouts: the middle in place and each bottom
  corner (64 px x 40 lines of the native menu) shifted by the margin to the edge of the widened target
  (`ResolveGlWidescreenCornerPasses`). Touch follows: a touch in a bottom corner of the window outside the
  menu goes to the equivalent corner of the menu (`RemapGlWidescreenCornerPointer`, only with the
  TopScreen frontend active in GL widescreen; the tab's old spot still touches BACK). Disable with
  `config/widescreen_menu_corners` = `0` (desktop `TRIAEVUM_GL_WIDESCREEN_MENU_CORNERS=0`). Ui domain
  (Game Over), full-screen fades and compensated models are not split.
- The presented axis (viewport/scissor: frame, stretched, fade, compensation) moved out of the backend to
  `ResolveGlWidescreenPresentedAxis` (`gl_widescreen_menu_policy.h`), tested on the host
  (`gl_widescreen_menu_policy_tests`, 21/21, also with ASan/UBSan).
- **Console diagnostics** (`config/widescreen_menus_log`, off without the file; desktop
  `TRIAEVUM_GL_WIDESCREEN_MENUS_LOG=<file>`): `logs/widescreen_menus.txt` with one line per
  distinct draw on the widened target: decision (`FRAME`, `FRAME+COMP`, `STRETCH`, `SCREEN`), viewport, scissor,
  domain/layer, projection c0-c3, anisotropy, coverage, texture. Delete the file afterwards.
- The 3D Link, the "...adventure through time itself!" box and the logo behind the cards and the thin
  header lines: see the next section.

### Relocated menus over their own background; atlas seams (2026-10-17)
- Symptoms (console screenshots, MQ unlocked): in file select the top screen's 3D Link and its
  text box appear behind and through the cards; in the Normal/Master Quest choice the title
  logo and the "© 1998-2011 Nintendo / Codeveloped..." appear between the cards and through the
  Master Quest card (semi-transparent); thin light vertical lines in the "Please select a file." header
  (and in the name-entry one), ~28 px from each edge.
- Host reproduction: Linux product build on the Ryzen (`/root/agentes/menus-visual`, Xvfb/llvmpipe,
  1280x720, TopScreen, scale 2, widescreen) with `TRIAEVUM_UNLOCK_MASTER_QUEST=1`: the path through the version
  choice shows exactly the screenshots (without it the Link and the box don't appear in file select).
- Cause 1 (scene behind the menu): the port's frontend composition (`TRIAEVUM_TOPSCREEN_FRONTEND_SKY.md`)
  removed the menu's own background (CommonBackground00 of file select/name entry, common_bg01 of the version
  choice) to show the top scene behind. On the 3DS the menu and that background (brown stone with ornaments)
  make up the bottom screen, which the mod moves whole to the top screen (swap of targets 0x400/0x401); the
  top scene (logo, copyright, 3D Link and the file-select box) is not behind the menu. Fix: the menu
  background is drawn again; only the opaque clear of the menu canvas stays removed, so the scene's sky
  stays on the sides of the 4:3 in widescreen (as on the sides of the 320 menu on the 400 screen).
  `OmitTopScreenFrontendMenuDraw` (`oot3d_top_screen_frontend_composition.h`); `config/topscreen_menu_backdrop`
  = `0` restores the old composition (desktop `TRIAEVUM_TOPSCREEN_MENU_BACKDROP=0`).
- Cause 2 (header lines): seam between menu atlas pieces at N× internal resolution. The menu is
  assembled from pieces drawn texel per native pixel; on the 3DS each pixel lands at the center of a texel and
  linear returns the exact texel, but at 2× or 3× the fragments at a piece's edge sit 1/(2N) of a
  texel from the boundary and blend in the neighboring atlas texel. Proof: at scale 1 the line vanishes, at 2 and 3
  it appears; with nearest it vanishes. Fix: the pieces of the relocated bottom screen (viewport that doesn't cover the
  presented width, square-pixel orthographic, textured; in both domains, which includes
  name entry and the Game Over panels) use a GL_NEAREST sampler for magnification when the scale is an
  integer ≥ 2: reproduces the native result pixel by pixel and doesn't read outside the piece. Minification keeps
  the texture's filter; fractional scale (1.5×) stays linear. HUD, Items, Map and System (full
  width) don't change. `GlRelocatedMenuNearestSampling` (`gl_widescreen_menu_policy.h`), sampler in
  `gfx_opengl_pica.cpp`; `config/menu_nearest` = `0` disables (desktop `TRIAEVUM_GL_MENU_NEAREST=0`).
- Known residue: after the version choice, the top scene's 3D text box is wider than
  the 4:3 and a strip of it (~25 px) shows on the sides, next to the sky, at the bottom of file select.
- Tests: `gl_widescreen_menu_policy_tests` (22/22, ASan/UBSan) and
  `oot3d_top_screen_frontend_composition_tests` (compile together with `oot3d_native_pica_visual_frame.cpp`).
  Host captures checked: version choice, file select, file 1, name entry, copy, fade until
  entering the game, BACK tab in the corner.

### Grass on deko3d: `config/grass` (05/11)

Off by default: without `config/grass` nothing below runs (each hook returns at its first test,
the game thread does not change). L3+R3 > Video > "Grass" writes the file; it applies at the next start.

| `config/grass` | What happens |
|---|---|
| absent, `0`, `off` | nothing (default) |
| empty or `placements` | as before: sources and placements are built and logged, nothing is drawn |
| `draw` | the tufts are drawn too |

- **Placements** (`grass_surface_extractor.cpp`): a cap that bites no longer depends on the index
  buffer order. Camera-independent placements sample up to 4x the cap, then keep the anchors with the
  smallest rank key (a hash of the stable id, not of the position): the density drops evenly and the
  cap is filled even when the mask rejects most candidates (`ThinGrassAnchorsToLimit`).
  View-focused placements (PC) keep their exact quotas.
- **Switch profile** (`grass_switch_profile.h`): tufts (a 4-vertex card cut into 5 blades in the
  fragment shader) at 40/m2, 14-30 units tall; within 4 m (400 units) the tuft is drawn as its 5
  blades, one segment each. At most 96k tufts per placement and 12 MiB of placements kept (the least
  recently drawn go first; Hyrule Field at 40/m2 is ~11 MB).
  - 60 FPS: at most 8000 visible; full density to 700 units, then thinning with distance
    (a tuft stays while its stable visibility value is below (700 / distance)², about the same count
    per screen area) to none at 3000, shrinking from 2400.
  - 30 fps: at most 22000 visible; full density to 1200, thinning to 4000, shrinking from 3200.
    The FPS in L3+R3 > Video switches the profile at once (the stream holds 22000 at both rates).
  - (0.1.2-test.2 cut at 1200/2000 units: the title demo's far camera drew nothing at 60 fps.)
- **Threads:** sources are read on the render thread (as before). Placements build on one service
  worker. Culling per cluster and writing the 32-byte instances straight into GPU memory runs on
  a second service worker (`grass_instance_stream.cpp`; cores 1|2). Its result is drawn from the next
  frame on, with the current frame's projection; the cluster spheres grow by 64 units + 6% of the
  distance so the one frame of lag never shows an edge. The game thread reads only Link's and Epona's
  colliders, and only with `draw` (`CollectNativeGrassColliders`: the player list head and the first 16
  background actors).
- **Drawing** (`gfx_deko3d_grass.cpp`): instanced draws into the scene target, before its first
  translucent world draw after this frame's grass sources were drawn into it (or before its display
  transfer). In 0.1.2-test.2 any non-opaque scene draw (the sky, unknown layers) triggered it, often
  before the ground was drawn: the placements of that frame were matched while still empty and the
  frame drew nothing (`drawn 0/600` at 60 fps, 374/600 at 30). A placement is now also kept per render
  target, so the same ground drawn into two targets keeps both cameras.
- **Hand-off** (`grass_stream_handoff.h`): every draw point draws the newest finished cull; a late or
  skipped worker reuses the previous one (counted as `stale`), never an empty frame. The worker writes
  only a slot no frame in flight reads. They use the viewport, scissor
  (dynamic resolution, widescreen) and depth compare of that target's opaque world draws.
  The camera is each source draw's own model-to-clip, so interpolated frames match. Fog comes
  from the draw's PICA fog LUT and light from its decoded PICA vertex lights, per vertex.
  Depth follows the source draw's PICA mapping: without w-buffering it is linear in z/w, so the vertex
  program puts it into clip z and the rasterizer produces it (early depth testing stays on). Only
  w-buffered draws use the fragment variant that writes `gl_FragDepth`. (0.1.2-test.2 used that
  variant whenever the mapping was not the identity, and the fragment program hashed every blade with
  `sin()` per pixel: 5.2 ms of GPU for ~3.4k tufts at 3.0x. The blade cut now reads per-tuft values
  computed in the vertex program.) Wind and the bend away from Link and Epona are computed in the vertex shader.
- **Round 3 (after test.3):**
  - *Foreign grass:* in the temple entrance, grass from the title demo was drawn. Every cull now
    carries the area (scene, room) it was made in; the game thread hands the area over once per host
    frame (`Deko3dSetGrassArea`, only with `draw`). A scene or room change drops the drawn cull, the
    sources' memo and the frame's placements. A cull of another area, or of none of this frame's
    placements, is dropped before drawing (`GrassCullFitsFrame`), counted as `foreign culls dropped`.
    (After test.8 only a scene change drops them, and only a cull of another scene is forgotten; see
    below.)
  - *60 fps flicker:* in-between frames interpolate the view in the vertex uniforms but not the
    light binding's camera, so the world transform the source memo checks drifted and those frames lost
    their sources (no draw point: ~40% of frames). A memoized source now keeps its world transform and
    takes only the frame's model-to-clip (`GrassRigidModelToClip`), so every frame draws, with its own
    camera.
  - *Look:* full density to 1000 units at 60 fps (1600 at 30), thinning to 4000 (5000). Thinned tufts
    are drawn wider, by 1/sqrt(the share kept), at most 3x, so a field reads as covered at distance
    with about the same fill cost.
  - *Log:* the third `grass:` line ends with `area drawn scene S room R, changes C, foreign culls dropped
    F | empty placements E` (built placements with no tuft: mask, slope or size left nothing).
- **Round 4 (after test.5):**
  - *60 fps:* round 3's memo still lost sources on in-between frames (`drawn` well under 600/600).
    An in-between (synthetic) frame now publishes no sources at all: the deko3d backend learns which
    frames are in-between from the temporal sample (`PublishPicaFrameTemporalSample` ->
    `GrassNoteSample`), and each draw that was a source of the last real frame gives its target's
    camera (`GrassInBetweenCameras`, `grass_stream_handoff.h`, at most `kGrassInBetweenReach` frames
    on). The frame draws the last real frame's placements with that camera. Placements are found by
    identity on every frame (`GrassFramePlacements`), not by target.
  - *Grass distance* (`config/grass_distance`, L3+R3 > Video, applies at once): Near (default; the
    round-3 distances, at most 8k visible at 60 fps, 22k at 30), Far (full density to twice that,
    thinning to 6000 (8000), at most 16k (40k)) and Whole area (every tuft of the area, no thinning,
    at most 65,536: the stream holds that cap for every mode, 4 slots of 32 bytes each, 8 MiB). The
    log's profile line and the third `grass:` line show `distance=near|far|area`.
  - *Tufts:* blade triangles, no `discard` (early depth stays on for every fragment variant).
  - *Log:* the second line splits `gpu` into `blades` and `tufts`; the third ends with
    `foreign culls dropped F (other area A, no shared placement P)` and
    `in-between frames I (without a camera W)`.
  - *GPU cost:* test.5 measured ~0.5 µs per tuft at 3.0x (5.5 ms for ~10k at 30 fps); cost is per
    tuft drawn, so the visible cap bounds it: Near ≤4 ms (60 fps) / ≤11 ms (30 fps), Far ≤8 / ≤20 ms,
    Whole area ≤33 ms `[INF]`. Whole area in Hyrule Field exceeds a 60 fps frame: the menu says so
    and recommends 30 FPS.
- **After test.6 (a save that starts in an area):** a ground without a rule of its own takes the rule of
  its twin texture (`GrassTwinRules`), which needs the rule texture's signature. deko3d kept none, so
  a save that starts in the Forest Temple drew no grass at its entrance until Sacred Forest Meadow
  had been visited in that session (logs15/15sc: `rules_seen` 4, 0 sources published, then 6 after
  the meadow). Rule textures' signatures are now kept in
  `shadercache/grass_texture_signatures.bin` (`SetSignatureStore`, as on PC; one ~4 KiB record per
  rule texture, written once) and the log says `grass: N rule texture signatures kept from earlier
  sessions`. The first session after an update still needs one visit to the area that draws the
  rule's texture. deko3d decodes a grass texture once per session even when its signature is kept
  (`Contains`, not `Find`): the placement samples its pixels and deko3d decodes them nowhere else.
- **After test.7 (a first load-in, nothing kept yet):** the user still had no grass at the Forest
  Temple entrance on loading a save there: the kept signature needs the rule texture drawn once,
  in that session or an earlier one. The twins of the preset's 25 rule textures are now built in
  (`GrassKnownTwins`, generated `grass_texture_twin_table.cpp`): 11 rows for 5 grounds, among them the
  Forest Temple entrance's (`d885db352c997c7b`, bmori1 room 0) taking rule 10 (Sacred Forest
  Meadow's ground, score 0.97); the others are grottos (kakusiana), Zora's River (spot03), a Lost
  Woods room (spot10_1) and Ganon's Castle room 16. `GrassTwinRules::Resolve` uses a row when
  either signature is missing, and the signatures when both exist (the same result: the rows are
  made with the same code). A row counts only for a configured rule with that texture and channel.
  - *Made offline:* `tools/oot3d/deko3d_grass/make_twin_table.py <romfs.bin> <work dir>` (in the host-tests
    container) hashes every CMB texture of the RomFS as the runtime does (FNV-1a of the encoded base
    level; all 25 rule hashes are found), then `twin_table_tool.cpp` decodes them with
    `DecodePicaTextureRgba8` and scores them with `ComputeGrassTextureSignature`/`GrassTextureTwinScore`
    against the preset's rules (`triaevum_product_graphics.inc`). Only hashes, the channel, the score
    and the sample mapping are written: no pixels, no signatures. The EUR and USA (rev 1) RomFS have
    the same 4009 textures. Rerun it when the preset's rules or the signature/score code change.
  - *Memo:* the sources' memo keeps negatives (`cacheable` at `Resolve`'s early returns in
    `pica_grass_source.cpp`) only under a key that holds the source cache's `Generation()` (bumped
    by every new texture and by the store) and the rules' version; the other early returns depend
    only on keyed draw state. A rule found later re-resolves the draw; nothing else to fix there.
  - *Log:* `grass: N rule texture signatures kept from earlier sessions (...), 11 built-in twins of the
    rule textures`.
  - Tests: `oot3d_grass_switch_tests` (`Oot3dGrassTwinsAtLoad`: a fresh process, no store, no area
    before, with the preset's rules: the entrance's ground has rule 10 at the first ask; every row is
    of a preset rule with its channel and the best row wins) and two guards.
- **After test.8 (07/11, a flicker on entering the Forest Temple):** the user saw the grass flicker
  once on walking from Sacred Forest Meadow into the temple. log16's window of the entry (frames
  27000-27599): `area drawn scene 3 room 1, changes 5, foreign culls dropped 4 (other area 2, no
  shared placement 2)`, `no result 15`, worker max 5.51 ms; `hitches.txt` puts room 1 at frame
  27573 (`scene=0x03:bmori1 room=1`), and the next window still draws the entrance's grass in room 1
  (`drawn 273/600`, `area drawn scene 3 room 1`).
  - *Cause:* round 3 treated a room change like a scene change: it dropped the drawn cull, the
    sources' memo and the placements of this and the last real frame. The entrance's ground (room 0)
    stays drawn from room 1 (both rooms are loaded across a room transition), so the frame of the
    change had no cull; a cull still in flight from room 0 was dropped on arrival as another area's
    (`other area`), so the next frame had none either, and an fps60 in-between frame right after the
    change lost its placements and camera. A drawn frame, one or two without grass, drawn again: the
    flicker. A cull without a shared placement (`no shared placement`, frames drawn before the
    temple's own culls, render lag) was also forgotten, costing the frame after it its grass.
  - *Fix* (`grass_stream_handoff.h`): `ClassifyGrassAreaChange` tells a room change from a scene
    change; `CheckGrassArea` drops everything only on a scene change (leaving gameplay counts as one).
    A room change keeps the cull, the memo and the placements: the memo's key holds the geometry's
    content version, so a room loaded into another room's memory never finds an old entry, and
    the placements are found by identity, so a cull draws only the grounds the frame drew.
    `GrassCullFitsFrame` compares scenes: a cull of another scene is dropped and forgotten (the title
    demo's grass in the temple entrance stays fixed). `GrassCullForDrawPoint` leaves a cull without
    any of the frame's placements out of that frame only and keeps it for the next. So once the
    grass has appeared in a scene, every draw point has a cull: `no result` counts only the start
    of a scene, before its first cull, where the grass appears once.
  - *Log:* the third `grass:` line now ends with `area drawn scene S room R, changes C (scene N),
    foreign culls dropped F (other scene), without a shared placement P, gaps G, ...`. `gaps` counts
    draw points without a cull right after one that drew grass, in the same scene (a flicker); it
    should be 0.
  - Tests: `oot3d_grass_switch_tests` (`Oot3dGrassAreaChange`: the entry as a frame sequence, the
    meadow, its fade-out outside gameplay, the load, the entrance's placement being built, room 0
    -> 1 -> 0 -> 1 with a late worker at each change. test.8's rules give frames without a cull after
    the grass appeared, the new ones none, with every frame from each scene's first cull drawn and no
    cull of another scene drawn) and guards.
- **After test.8 (07/11, less grass around Link in Hyrule Field):** at Near the grass reached the
  whole field, but the user saw less of it near Link than a few metres further out, changing as he
  walked (videos `2026110709434700`/`09402400`, frames 150-750: sparse tufts around Link, a dense carpet
  from a few hundred units past him).
  - *Cause:* the placements keep one density in the world: Hyrule Field's ground placement is capped
    and thinned evenly (`logs10a/deko3d.txt:32`: 3 placements of 34886 tufts, then 130886 with the 4th,
    which is the cap of 96000). Past `FullDensityDistance` the thinning keeps the screen coverage
    (fewer, wider tufts), but nearer than that the same tufts per m2 cover less of the screen the
    nearer they are (coverage ~ density x distance): around Link, ~250 units from the camera, about a
    quarter of the band at 1000. Host reproduction (`AroundLinkTheGrassCoversAsMuchAsFurtherOut`, a
    field of the profile's tufts at ~11 per m2, the field camera): coverage 0.87 at 180-320 units
    against 2.60 at 600-1000 and 3.21 at 1000-1600. Two changes as Link walks: tufts crossing 400
    units switched at once from camera-facing tufts to single blades of random facing (~3/4 of the
    screen width), and a tuft the distance thinning keeps appeared at full height (696 tufts changed
    by more than a quarter of their size in one 4-unit step over 400 units of walking, host test).
  - *Fix* (`grass_instance_stream.cpp`): nearer than `FillDistance` (Near and Far: their
    `FullDensityDistance`; Whole area: Near's) each kept tuft on screen brings up to 8 fill tufts on
    its ground plane within `FillRadius` (18 units), FillDistance / distance tufts in all
    (`GrassStreamFillMultiplier`, at most 9x). A fill tuft has a stable spot, height, facing and phase
    of its own (`PackGrassFillInstance`), the tuft's ground colour, and grows out of the ground over
    25% of its distance; a tuft the thinning keeps grows over the last 10% of its distance
    (`kGrassThinGrowth`). Single blades turn into tufts between 400 and 600 units in the vertex shader
    (every vertex moves from its blade place to its tuft place; a single blade is 1.32x wider, the
    average screen width of a tuft's blade). The stream's kinds only split the draws (GPU split).
    Per tuft, the cluster's frustum test leaves out the tufts of a visible cluster that are off the
    screen (about a quarter of the instances in the host field), which pays for most of the fill.
  - *After:* coverage 3.10 around Link, 3.04 at 600-1000, 2.95 at 1000-1600 (30 fps: 4.89, 4.78,
    4.99); walking 400 units, no tuft changes by more than 12% of its size in a step. Host field at
    60 fps: 5020 instances before, 5250 after (1547 fill, 1317 off-screen tufts left out).
  - *Cost* `[INF]`: the log16 Hyrule windows spend ~0.16 us of GPU per tuft (far tufts, a few pixels)
    and ~0.23 us per near blade, so the cost is per instance; with about as many instances and twice
    the grass screen area near the camera, Near's 0.55-0.63 ms should become ~0.6-0.75 ms. Worker: one
    frustum test per kept tuft. Whole area draws fewer instances (off-screen tufts left out).
  - *Log:* the profile line adds `(tufts from 600), fill to F`; the third `grass:` line adds
    `fill F off screen O` after `thinned` and `fill F` after `max`. `visible` counts the fill tufts:
    `visible - fill + off screen` compares with earlier logs.
  - Tests: `oot3d_grass_switch_tests` (`Oot3dGrassNearDensity`: coverage around Link against the
    full-density and thinned bands, walking without pops, the multiplier, a fill tuft's place, fill
    only near and on screen).
- **Shaders:** `tools/oot3d/deko3d_grass/gen.py` compiles `grass_vsh.glsl` and the two fragment
  variants with uam into `gfx_deko3d_grass_dksh.h` (not in `deko3d.pack`). Rerun it after editing a
  shader.
- **Measuring** (`logs/deko3d.txt`, every 600 frames):
  - `grass: frame N draw fps60 | ... placements P queued Q tufts T (max) retained X MiB | sources ms
    placements ms`: the render-thread cost of finding sources and matching placements.
  - `grass: frame N drawn D/600 frames | visible V (max, budget) tufts blades draws clusters
    visible/tested over_budget | worker W ms/cull | render draw R ms/frame | gpu G ms/frame (max)`.
    `gpu` is the time between GPU timestamps around the grass draws, approximate; `blades` and
    `tufts` split it (the instances nearer than the single-blade distance first, then the rest).
  - `grass: frame N fragment raster R draws, depth-writing D | culls fresh F stale S (frames that
    redrew an older cull), no result X, nothing drawn E | cull instances I (max) thinned T fill L off
    screen O | distance full A max B fill C | area ... gaps G`. `fill` counts the fill tufts among the
    instances, `off screen` the kept tufts of visible clusters left out by the per-tuft frustum test.
    `depth-writing` should be 0 unless the scene uses w-buffering;
    `nothing drawn` counts draw points that had a cull result but drew none of it (should be 0);
    `no result` counts draw points before a scene's first cull and `gaps` draw points without a cull
    right after grass (should be 0).
  - The game thread's cost shows in `perf.txt` (`guest`/`game_pair_ms`) as before; the GPU's in
    `dkperf` `busy_ms`.
- Budget estimates `[INF]`: GPU 0.6-1.2 ms per frame at the 2.25 floor at 60 fps, 1.5-3 ms at 30 fps
  (first console run, before the depth and fragment fixes: 5.2 ms at 3.0x for ~3.4k tufts).
  Worker 0.3-1 ms per cull. Render thread well under 0.1 ms per frame for the draw plus the existing
  `sources`/`placements` cost. Game thread: a few guest reads per step.
- Tests: `oot3d_grass_switch_tests` (thinning, profile budgets, config file, culling and the instance
  stream) and the `grass` guards in `test_bug_guards.py`.
- To check on the console (Kokiri Forest, Hyrule Field, Lon Lon Ranch, Kakariko Village): grass
  stands on the grass textures and sinks into the ground at the same depth as the native ground, in
  front of and behind Link correctly; fog colours it like the ground at a distance; it bends away
  from Link and Epona; no popping at the screen edges when the camera turns; DRS and widescreen keep
  it aligned; the two `grass:` lines appear.

### Grass on deko3d: cost per tuft, `config/grass_lod` (07/11)

Test.9 drew every tuft as 15 unindexed vertices (5 blade triangles, one instance per tuft) and each
vertex repeated the whole tuft's work: wind and gust (2 `sin`), the bend away from Link and Epona
(a loop over up to 4 capsules), the camera-facing card, 3 `sin` hashes, the ground normal decode,
3 lights and fog. log16/logs16b: about 0.17 us of GPU per tuft at 307 MHz, near and far alike, so
the cost is per vertex, not per pixel (Whole area 3.3/7.0/10.6 ms for 20.6k/41.8k/63.1k tufts).

| `config/grass_lod` | What happens |
|---|---|
| absent, any other word (default) | on: a tuft pass, then the blades, far tufts with fewer blades |
| `0`, `off`, `false` | test.9's grass exactly (`grass_vsh.glsl`, instanced, 5 blades everywhere) |

L3+R3 > Video > "Grass detail" writes the file and publishes the choice: the next cull job culls for
it and every cull is drawn the way it was culled, so it switches live (one session, A/B).

- **Tuft pass** (`grass_tuft_vsh.glsl`): one point per tuft, the rasterizer off; it does the shared
  work of `grass_vsh.glsl` once and writes a 64-byte `GrassTuft` (base, height, the card's
  camera-facing half width, the bend, the lit root and tip colours, the blades' seed, the level
  coordinate). A `DkBarrier_Primitives` (plus the texture/L1 invalidate) then orders it before the
  blade draws. Every frame, in-between frames included, at the draw point, with this frame's
  camera, wind time and colliders. *Why not the cull worker:* its instances are drawn for several
  frames (stale culls, fps60 in-between frames with another camera), so wind, Link's bend, the
  camera-facing card and fog written by it would move at the cull rate, one frame late.
  *Why not a compute dispatch:* the device's queue is created graphics-only
  (`gfx_deko3d_device.cpp`); a vertex program with storage writes needs no queue change and runs in
  the same place. Two tuft buffers (2 x 65536 x 64 B = 8 MiB, GPU only, made at the first cull
  with levels) are taken in turn by the draw points that run the pass: the one written was last
  read before the previous pass's barrier.
- **Blade draws** (`grass_lod_vsh.glsl`): no instancing; vertex v is corner v % 3 of blade
  (v / 3) % n of tuft v / 3n, so the vertices of several tufts fill a warp (an instance of 15, 9 or
  3 vertices would otherwise leave lanes idle if the hardware gives each instance its own warp).
  `v / 3n` is one float multiply, exact for every vertex of a 65536-tuft draw (checked
  exhaustively). The tuft's colour is `mix(root, tip, y)` (test.9's colour is linear in the height
  y, checked by running its lines); fog stays per vertex.
  - *Near draw* (`GRASS_LOD_NEAR`): single blades within 400 units and their turn into tufts to
    600, as `grass_vsh.glsl` (per-blade facing and light per vertex), with the cull margin (the
    current camera's distance decides the turn). Near Link the grass looks as in test.9.
  - *Far draws:* 5, 3 or 1 blades per tuft. Between levels the blades that stay move to and widen
    into their slot of the next layout and the others narrow to nothing where they stand; the card
    keeps its width and the blades' summed width (so the covered area) stays 0.84 of the card at
    every step. At the end of a morph the layout is the next level's, so the draw a tuft is in
    changes where both draw the same triangles (no pop; `test_grass_lod.py`).
- **Levels:** the level coordinate runs 0 (5 blades) -> 1 (3) -> 2 (1) as the projected width of a
  5-blade slot (2 x the packed half width, so the thinning's widening counts) on a 1200x720 target
  falls from 2.4 to 1.8 px, then from 1.2 to 0.9 px (`kGrassLodPixels`; `GrassLodFocal` takes the
  focal length from the frame's world-to-clip matrix, so it follows the camera's field of view, not
  the dynamic resolution). It is 0 up to 800 units (`GrassLodMinimumDistance`: past the single-blade
  turn and twice the cull margin there) and reaches 2 at most at 1200. The tuft pass computes it
  per frame; the cull worker (`GrassLodKind`) puts a tuft in the draw its coordinate needs at its
  distance less the cull margin (64 + 6%), so a camera that comes that much nearer before the next
  cull still finds every blade it needs. Host field (`Oot3dGrassLod`), instances near / 5 / 3 / 1
  blades: with the game's 60 degrees at 5:3 (focal 624 px), Near 5958 -> 2377/2331/1250/0, Far
  16000 -> 4285/2179/6559/2977, Whole area 34273 -> 2377/1124/3228/27544 (blade vertices 514095 ->
  164199); with a 45 degree 16:9 camera (focal 815 px) Near 5250 -> 2172/2947/131/0.
- **Integer hash:** lowbias32 (Chris Wellons, hash-prospector, public domain), the same mix as
  `MixGrassBits`; one hash per blade gives its 3 random values (10, 11 and 11 bits put into a
  float's mantissa, no conversion). The blades' jitter therefore differs from test.9's (another
  random layout, the same distribution).
- **Cost per tuft** `[INF]` (uam + envydis, gm107 instructions per program; quarter-rate =
  `mufu`/conversions, weighted x4): test.9 536 instructions (45 quarter-rate) x 15 vertices ~ 10.1k;
  with levels: tuft pass 336 (26) once ~ 0.41k, far blade program 193 (6) per vertex ~ 0.21k, near
  program 456 (27) ~ 0.54k. Per tuft: near ~ 8.5k (0.84x test.9), 5 blades ~ 3.6k (0.36x),
  3 blades ~ 2.3k (0.23x), 1 blade ~ 1.0k (0.10x), plus 5/3/1 triangles. Scaled from 0.17 us per
  tuft: about 0.14 / 0.06 / 0.04 / 0.02 us. Host field, 60 degrees: Near ~ 1.0 -> 0.5 ms (the near
  draw is two thirds of what is left); Whole area ~ 5.8 -> 1.0 ms for 34k tufts (63k tufts: ~10.6
  -> ~2 ms). If test.9 was limited by instances of 15 vertices per 32-lane warp (an instance per
  warp), the saving is larger.
- **What changes on screen** (60 degrees, 1200x720): from ~9-12 m from the camera (where a test.9
  blade is under ~2 px wide) tufts turn into 3 blades, 5/3 wider; in Far and Whole area from ~14-19 m
  into 1 blade, 5x wider (the same covered area, fewer sub-pixel slivers). Thinned tufts at Near
  (already wider) stay at 1.4-2.2 px slots: 3 blades or part way there. Every blade's random jitter
  differs from test.9's (another layout, the same distribution); colours may differ by under 1/255
  from rounding. Single blades and their turn into tufts (within ~6-7 m), so the grass around Link,
  are test.9's.
- **Log** (`deko3d.txt`): at start and on each switch `grass: lod on (config/grass_lod): ...` or
  `grass: lod off (config/grass_lod = 0): ...`; the second `grass:` line ends with `gpu G ms/frame
  (max, samples; blades B tufts T pass P) | lod on|off (L/D drawn frames) near N l5 A l3 B l1 C
  vertices V pass_points Q`: instances per drawn frame per draw (off: near = test.9's blades, l5 =
  its tufts), blade vertices per drawn frame (3 x blades x instances) and tuft-pass points; `pass`
  is the tuft pass's GPU time, `blades`/`tufts` the near and far draws. `LOD OFF (setup failed)`
  means the tuft memory or a shader was missing: test.9's grass is drawn then.
- **Shaders:** `gen.py` builds `grass_tuft_vsh.glsl` and the two variants of `grass_lod_vsh.glsl`
  (pasting `grass_lod_common.glsl` at their `#include` lines) into `gfx_deko3d_grass_dksh.h`; test.9's
  three stages come out byte for byte as before. `deko3d.pack` is not involved (no rebuild).
- Tests: `tests/switch/test_grass_lod.py` (`glsl_eval.py` runs `grass_lod_common.glsl`: level
  changes show the same blades, the covered width through a morph, 5 blades unmorphed are test.9's
  tuft, the coordinate equals the cull worker's, lowbias32 and the blade randoms, the vertex index
  division for every vertex, the colour ramp); `oot3d_grass_switch_tests` (`Oot3dGrassLod`: config
  file, coordinate samples, focal length, the levels regroup exactly the instances of test.9 and
  each draw holds for a camera up to the cull margin nearer); `grass_lod` guards.
- Console A/B (full resolution, 60 FPS, Grass On): in Hyrule Field at Near and Whole area, Kokiri
  Forest and Sacred Forest Meadow, switch "Grass detail" every ~20 s and compare the second
  `grass:` line's `gpu` (and `pass`/`tufts`) and `l3`/`l1`/`vertices`, and `dkperf` `busy_ms`.
  Watch for pops 8-20 m away while walking and riding.

### Grass on deko3d: nothing grows near Link, `config/grass_fill`; the temple entry flicker (07/11, after test.9)

The user after test.9: "now the grass grows around Link, which we don't want" and "grass still
flickered when I went in the temple" (Whole area, 60 FPS; logs17, videos `2026110713005300` and
`2026110713042600`).

**What grows** (video `2026110713005300`, 30 fps capture of the 60 fps output): frames 112-140, on
the ledge at the top of Sacred Forest Meadow's stairs, the patch in front of Link fills with new
short tufts along its near edge that grow taller as he walks to the temple door; frames 0-8, the
camera low in the meadow's grass, a wall of tall blades fills the screen (test.9's fill is 9x the
placement's density right in front of the camera).

- *Cause:* test.9's fill (GrassFull) put FillDistance / distance tufts per tuft near the camera:
  fill tuft c of a tuft appears at FillDistance / (c + v) and grows out of the ground over a quarter
  of that distance, so at Near (1000) children 2-4 grow in between 250 and 500 units, right around
  Link (~250 units from the field camera), and children 5-8 in front of the camera, as he walks.
  A tuft the thinning keeps grows over the last 10% (past FullDensityDistance; Near and Far only).
  GrassLod's level morphs (from 800 units) and the single blade -> tuft turn (400-600) slide blades;
  nothing there grows.
- *Fixed fill* (`grass_instance_stream.cpp`, `GrassFixedFillRank`): every kept tuft nearer than
  `kGrassFixedFillNear` (450 units) brings the same fill tufts wherever the camera is: test.9's
  multiplier at `kGrassLinkDistance` (250 units: Near and Whole area 3.56 at 60 fps, 5.69 at 30;
  Far 7.1 / 9). The multiplier then falls to test.9's (or 1) by `GrassFixedFillFar` (one fill tuft
  per `kGrassFixedFillPerRank` = 120 units: 757 at Near 60 fps, 1183 at Far), never past
  FullDensityDistance. Fill tuft c with stable value v is drawn at width clamp(rank - (c - 1 + v), 0,
  1) of its own (the widths add up to the multiplier less one), always at its full height: a
  transition narrows or widens a tuft where it stands (`PackGrassFillInstance`'s width scale), far
  from Link, about 3% of a tuft's width per 4 units walked. A tuft the thinning keeps widens in
  over the same 10% instead of growing. Nearer than 250 units there are fewer fill tufts than test.9
  had (no wall of blades in front of a low camera). CPU only: no shader change.
- *Host field* (`Oot3dGrassNearDensity`, `Oot3dGrassNearLink`; Near, 45 degrees 16:9):
  coverage around Link 2.97 (test.9 3.10), 600-1000 units 3.52 (3.04), 1000-1600 2.95 (2.95);
  instances 5633 (5250; fill 1930 against 1547), drawn screen area +16%. 30 fps: 4.70 (4.89),
  12499 instances (11085), screen area +23%. With levels and the game's 60 degrees: 6447 instances
  (test.9's fill 5958), 86523 blade vertices. Walking 400 units in steps of 4: within 450 units no
  instance changes at all in 35283 tuft-steps (test.9: 5687 of 27559 changed), no instance below
  full height (test.9: 92397), past 450 at most 6% of a tuft per step. Cost `[INF]`: about +8% grass
  instances at Near; the near draw's single blades are within 600 units either way.

| `config/grass_fill` | What happens |
|---|---|
| absent, any other word (default) | fixed: the same fill tufts near Link at full height, narrowing away past 450 units |
| `0`, `off`, `false` | test.9's fill exactly (fill tufts growing in, thinned tufts growing in) |

L3+R3 > Video > "Grass near Link" writes the file and publishes the choice: the next cull job takes
it (A/B in one session). `deko3d.txt`: `grass: fill fixed (config/grass_fill): 3.56 tufts per tuft to
450 units wherever the camera is, narrowing to test.9's by 757; nothing grows` (or `grass: fill grows
(config/grass_fill = 0) ...`) at start and on each switch of fill, FPS or distance; the third
`grass:` line's `distance=... fill F` ends with `fixed` or `grows`.

**The temple entry flicker** (logs17 `deko3d.txt`): the window of the entry from the meadow (frames
25800-26399; `hitches.txt` puts `scene=0x03:bmori1 room=0` at frame 25883) reads `area drawn scene 3
room 0, changes 4 (scene 2), foreign culls dropped 2 (other scene), without a shared placement 6,
gaps 2`; the room change after it (26400-26999, room 1 at 26661) is clean (`changes 1 (scene 0)`,
`gaps 0`): test.9's room-change fix holds. No other scene entry of the session (Hyrule Field,
Lost Woods, Kokiri Forest, the meadow) has a draw point without a shared placement; logs16b's
temple entry had `no shared placement 2` too.

- *Cause:* the entrance's ground is the only grass grown from an inherited rule (a twin of the
  meadow's rule 10, `GrassTwinRules`). Its entry stays incomplete while some rule textures have no
  signature (15 of 25 in this session), and `GrassTwinRules::Sync` erased incomplete entries
  whenever any texture was first decoded (the source cache's generation). A texture first drawn
  after the ground in a frame (a Wolfos', an effect's) erased it between the ground's draw (a memo
  hit: the rules are not asked) and the draw point, where `PicaGrassPlacements::Update` lists
  `AllInherited`: the ground matched no rule, the frame had no placement, the cull none of the
  frame's placements, and that frame and the fps60 in-between frame after it drew no grass. The
  next frame's memo missed (its key holds the generation) and resolved the ground again: two
  frames without grass, once per new texture seen. `rules_seen` 11 -> 10 in the next window is the
  entry erased while the ground was not drawn (room 1). The 30 fps videos show no missing grass
  frame (the darker whole frames 215 and 227-228 of `2026110713005300` keep their grass: lighting).
- *Fix* (`grass_texture_twins.cpp`): new signatures mark incomplete entries stale instead of erasing
  them; a stale entry keeps its rule (`AllInherited` lists it) and is resolved again at the
  texture's next `Resolve` (its next memo miss), as before.
- *Not this:* world draws attributed to UI lists (`lighting real ... lost world ... ui_list=11` in
  the 25200-26999 lighting window, 88-98 later in bmori1) lose the ground as a grass source for a
  frame, which makes no draw point and is not counted as a gap; that is fixed on its own
  (`config/late_lists_as_ui`).

- Tests: `oot3d_grass_switch_tests`: `Oot3dGrassTempleEntry` (the preset's rules, a fresh session,
  the entrance's ground and textures first seen after it, as frames with fps60 in-between frames:
  12 gaps in 240 frames before the fix, none after), `Oot3dGrassNearLink` (fill config file, the
  multiplier and widths, nothing within 450 units changes and nothing grows walking at Near 60/30,
  Far and Whole area, test.9's off exactly), `Oot3dGrassNearDensity` (both fills); guards.
- `deko3d.pack`: not involved (no rebuild); the grass shaders are unchanged.
- Console check: Hyrule Field at Near and Whole area, walk and run with the camera behind Link,
  then look down at the grass around him: nothing should grow or appear within a few metres; switch
  "Grass near Link" to "Grows in (test.9)" to compare. Then Sacred Forest Meadow -> Forest Temple:
  the entrance's grass stays through the Wolfos fight; the third `grass:` line of the entry window
  should read `without a shared placement 0, gaps 0`.

### Grass at a scene's start: stored grounds reused, then a hold (`config/grass_placement_reuse`, `config/grass_hold`, 08/11, after test.12)
- **Report (test.12):** the Hyrule Field grass still briefly pops in.
- **What the logs and the video show** (logs20a-e, the same route every session: scenes 107, 67, 37,
  34, 29, then Hyrule Field 81): the pop is the scene's first frames. It is not the LOD (`lod off`),
  and it is not the edge at 4000 units (`distance=area`; after the arrival window every window has `no
  result 0` and `over_budget 0`).
  - Arrival window (the first `grass:` line, then the third line's `no result`): 20a `loaded 4 in 327
    ms`, 8 draw points without grass; 20b `built 4 in 1086 ms ... written 4`, 44 draw points; 20c
    `built 4 in 1194 ms`, 53; 20d `loaded 4 in 391 ms`, 8; 20e `loaded 4 in 374 ms`, 8. Builds and
    loads both run on the one builder worker while the fade-in plays. The first cull then takes 26-28
    ms (65536 visible looking across the field).
  - Video `2026110807130400` (logs20c) ends on the fade-in with a bare ground. In
    `2026110807131300`, frames 0-7 (30 fps) show bare ground with the HUD already up, and the grass
    appears at frame 8.
- **Cause 1: the disk cache missed in 2 of 5 sessions.** The file name (`GrassPlacementPersistentKey`)
  held the exact bits of the ground's model-to-world. `pica_grass_source.cpp` derives that matrix from
  the scene's first drawn frame as inverse(light-binding world-to-view) x view x model, in float.
  Different first cameras round it differently: Link walks in, so the first resolved frame varies.
  `GrassSceneBridge` keeps the matrix for the session, but not across sessions. The name also held
  the geometry identity, which contains guest physical addresses. Which of the two differed in 20b
  and 20c is not logged `[INFERENCE: the transform]`; the new counters below will show it.
- **Cause 2: even a hit is late.** The load (330-390 ms for 4 files) and the first cull run while
  the fade-in plays. A build takes about 1.1 s.
- **Fix 1: `config/grass_placement_reuse`** (on by default; `0`/`off` gives the exact matching of
  test.11, which the PC also uses):
  - Disk format v2 (`kGrassPlacementDiskVersion`; v1 files are rejected, built again once, and
    evicted as least recently used). The file name holds the ground's payload and bake settings, with
    the model-to-world rounded: rotation to 1/64, translation to 16 units, with offsets that keep
    round values off the step edges. The header holds the geometry identity and the exact
    model-to-world, both in the checksum.
  - `GrassPersistentPlacementCache::Load` with a `GrassPlacementDiskMatch` accepts a transform at
    most `1e-4` per rotation element and 0.5 units per translation element away. It also accepts the
    same ground under another geometry identity. Otherwise it reports `OtherTransform` or
    `OtherGeometry`, and the ground is built again.
  - The loaded placement keeps the transform it was made with (`GrassWorldPlacement::ModelToWorld`),
    and `PicaGrassPlacements::Update` draws with it. The anchors therefore go back to model space
    exactly, and the tufts sit where they were made on the ground.
  - **What changes visually:** with a reused ground, the world frame used for culling distances and
    for Link's collider is off by the rounding (at most ~0.5 units). A ground reused from other
    addresses shows the tufts of that earlier build: the geometry identity seeds them, so the pattern
    differs, but the density and look are the same. Exact matching (switch off) keeps test.11's
    behaviour.
- **Fix 2: `config/grass_hold`** (ms per scene, default 1500, `0`/`off` = never, at most 2500;
  `grass_scene_hold.h`). At frame start, after the texture hold, `Deko3d::GrassBeginFrame` waits in
  the scene's first 120 frames:
  - for placements the last frame requested and not made yet (`GrassAsyncPlacementBuilder::WaitForPending`;
    writes left aside);
  - then for their first cull (`GrassCullWorker::WaitIdle`);
  - within what is left of the budget, until the scene's first grass draw.
  As with the texture hold, the game follows once the frame queue is full, so the fade-in starts that
  much later: about 0.35 s with a stored ground, up to 1.2 s for a first build of Hyrule Field. The
  first frames of the fade-in then show the grass.
- **Log** (`deko3d.txt`):
  - Start: `grass: placement reuse on|off (...), hold N ms per scene for its first grass`.
  - First `grass:` line: `loaded L in Y ms (reused R of another transform, up to r / t; G of another
    address), written W | not loaded: no file M, other transform T (nearest r / t), other address A,
    damaged D`. Offsets are the largest or nearest of the session.
  - Third `grass:` line: `... | hold H ms over F frames (X timed out)`.
  - At each scene's first grass: `grass: scene S first drawn at its frame F (draw points without grass
    before: N) | hold W ms over H frames (placements P, cull C, timed out T; budget B ms)`.
    With the fix, Hyrule Field should read `draw points without grass before: 0`.
- Tests: `oot3d_grass_switch_tests`:
  - `Oot3dGrassPlacementCache` (`TwoFirstCamerasRoundTheGroundsTransformWithinReuse`,
    `TheNextSessionReusesTheGroundOfAnotherFirstCamera`: exact builds again with `other transform 1`;
    reuse loads the same anchors with the stored transform; another address; `ReuseConfigFileMeaning`);
  - `Oot3dGrassSceneHold` (the logs20c arrival as a frame sequence, budget, window, config).
  The target now builds the placement builder and the disk cache. `oot3d_graphics_foundation_tests`
  (`StoredGroundMatchesItsTransformAndIdentityExactlyOrWithReuse`,
  `BuilderReusesAGroundOfAnotherFirstCameraAndCountsWhyNot`, `WaitForPendingEndsWhenTheRequestedPlacementsAreMade`;
  the old key tests moved to the v2 name) is not built in the host image (no gtest).
- No shader change: `deko3d.pack` is unchanged.
- Console check: enter Hyrule Field from the market twice (two sessions). The second time the first
  `grass:` window should say `loaded 4` (possibly `reused 4 of another transform`), and the scene line
  should say `draw points without grass before: 0`. The grass should be there as the picture fades
  in, with no pop. For an A/B, write `0` to `config/grass_hold` and `config/grass_placement_reuse`.

### Mirror natives for the render loops (Profile18 items 2 and 5, 11/07)
Guest replacements for the hottest loops of the game's CPU renderer, in the existing mechanism
(`guest_overrides.json`, `apply_guest_overrides.py`, `oot3d_native_guest_overrides.cpp`), all with
`"inline": true` single-line prologues, so only the functions that receive one lose their PGO profile.
- **Kind: "mirror".** Each one is the translated blocks of its range transcribed one for one: the same
  `ReadFast`/`WriteFast`/`ReadWordsFast`/`WriteWordsFast` calls in the same order, the same lazy-flag
  updates with the same liveness masks, one budget unit per block. The guest registers, flags and
  budget live in locals for the whole loop (the translation writes them back and reloads them around
  every guest store through the window's byte pointer and every budget decrement, which may alias
  them), and the per-block hook gate is a single `Hooked()` test. The exits are the translation's own,
  at the same point and with the same state: budget exhausted at block X → block limit at X; a failing
  access at instruction P → memory fault at P with the registers the earlier instructions left; leaving
  the range → resume at the translated block. Nothing is validated ahead and, after the hook test,
  nothing declines. Blocks that call translated functions stay translated (the replacement resumes
  there). The body lambdas are `always_inline`: out of line, the registers went back to the stack and
  the gain disappeared.
- **Enabled** (logs18b block entries; real time per 30 Hz step with the profile's ~200 ns per entry
  removed, over the profiled session):

  | Block | Function | Entries | Translated ms/step | Loop bench |
  |---|---|---:|---:|---:|
  | 0x00452DA4 | `CmbRenderer_UpdateTextureTransforms`, coordinator loop (call-free path) | 3.11 M | 0.43 | 1.44–1.67x |
  | 0x0046E140 | 0x0046C204 nngx state validation: emit dirty registers | 2.04 M | 0.24 | 1.33–1.53x |
  | 0x0046E048 | same loop, second register list | 0.13 M | 0.02 | 1.35x |
  | 0x0046D734 | same function: mark registers dirty | 0.68 M | 0.08 | 1.26x |
  | 0x0046DA6C | same function: runs of uniform words | 0.48 M | 0.06 | 1.28x |

- **Written, proven and disabled** (no gain in the bench; they stay in the differential test and a
  guard keeps them off): 0x00477D60 arena free-list walk (0.98x), 0x003FCA08 quad corner copy
  (0.74x), 0x00466F78 `MeshCommandPacket_PatchBoneUniforms` whole (0.83x: 17.7 blocks per call, the
  state copy in and out costs more than the block entries it saves) and 0x00368D94 unsigned divide
  (1.03x, 4 blocks per typical call). Like the five math natives of 2026-10-18: short bodies lose.
- **Left translated:** 0x003F96BC (straight line; its cost is three external `Mtx3x4_Multiply` calls
  per call, 1.4–2.0 µs each in the profile, which a replacement would still make the same way),
  0x003F9D9C (a translated call in each of its 6 iterations), 0x0047FE44 (one block and a tail call),
  0x00452934 (calls in every mesh iteration; the skip path is 2 blocks), 0x00350820 `Array_Construct`
  (an indirect constructor call per element), 0x002DC370 (double-precision `pow`, 1,921 calls in the
  whole session from `AudioPitchTable_Generate`, software binary64 in the translation).
- **Exactness:** `run_guest_override_equivalence.py`, 200 thousand trials per replacement, zero
  differences: host with the fastmem window on (all 33 entries) and off (the nine new ones), and
  qemu-aarch64 (devkitA64, Switch flags, native VFP) for the five enabled ones (`--only` now takes a
  comma-separated list). The five do no floating point: in the Switch build (`-ffp-contract=off`, as
  the shards) four use no FP/SIMD register at all and 0x0046DA6C only moves integer state through
  SIMD registers (no arithmetic, no FPCR). Under qemu, the older 0x0046700C case takes hours at 200
  thousand trials (1,106 s on the host: two thirds of its trials run to the 1 M-block budget), so a
  full 33-entry qemu run needs a lower trial count for that case. The harness gained scenarios for the nine new entries, a stub for indirect
  calls in the call closure (both sides take the same exit), the renamed translation in patched copies
  that call a function with its own override, and a loop-only benchmark.
- **Benchmark:** `GUEST_OVERRIDE_BENCH=loop TRIAEVUM_FASTMEM_WINDOW=1`: both sides on the window
  (the console's memory path); the replacement runs alone and the translation runs from the same block
  with a budget of exactly the blocks the replacement consumed, so it stops at the same resume block;
  TSC cycles per guest block on the Ryzen. (`GUEST_OVERRIDE_BENCH=1`, the old mode, measures the
  replacement on a heap image, the slow path, against the translation on the window: with the window
  on it is not a fair comparison. In the loop bench the older 0x0046700C copy measures 0.82x and
  0x003563E0 1.03x.)
- **Expected effect** [INFERENCE: x86 ratios applied to the console profile]: −0.2 to −0.3 ms per 30 Hz
  step on the profiled mix, more where draws rise (room 8 attacks, bush cutting), since both functions
  run per draw.
- **A/B on the console:** `config/guest_overrides_disable` (text: hex entries and/or `mirror` for this
  round's replacements); `config/no_guest_overrides` still turns every replacement off.
  `logs/perf.txt` gets a `natives:` line per window: `overrides=on|off calls= declined= disabled=` and
  `entry=calls/declines` for each mirror replacement that ran.
- **Build:** `build-fork.sh` with `PGO=use` regenerates the `-pgo` C++ in place with the checkout's
  `generate_whole_aot_cpp.py --guest-overrides` and `apply_guest_overrides.py`, both reading the
  checkout's `guest_overrides.json`; with `AOT_GENERATOR=7baf1fde8b` the path (and so the `.gcda`
  names) is unchanged. Checked on a copy of `fork-gen-7baf1fde8b-pgo/cpp`: only shards 091 and 180
  change, both keep their line count (only the five block-label lines change; both already had the
  include), and both compile for the Switch. The functions that receive a prologue lose their
  profile: 0x0046C204 (3,528 instructions, still in the hot list) and 0x00452B40. Note for later
  rounds: a function prologue and a block prologue cannot share a function (`find_definition` needs
  the unmodified signature line), so a whole-function replacement of a function that already has a
  block replacement goes on its entry block (as the disabled 0x00466F78 does).

## Next steps

- Future ideas (user request, 2026-10-17):
  - "Light grass" for the Switch: done on deko3d (see "Grass on deko3d" above).
  - Other PC effects (toon shading, outlines, wider FOV) adapted to GL, if they fit on the GPU.
  - Mods: test community HD texture packs (Citra/Azahar format) in the texture
    replacement system (`.o3tu`/PICA), after the video memory fix; replacement of data files
    (models/text) by the runtime; code mods require regenerating the AOT per mod.

### Frame generation: options evaluated (2026-10-22, notes only, nothing implemented)
1. **Eden on PC with LSFG.** The code already exists in Eden (`src/video_core/frame_gen` and
   `renderer_vulkan/present/lsfg_*`), but only compiles on Android (`ENABLE_LSFG` via
   `cmake_dependent_option "ANDROID"`), and the Qt UI hides the options (`IsAndroidOnly` in
   `src/yuzu/configuration/shared_widget.cpp`). There is a local patch, not yet compiled or tested
   (`~/projetos/eden-framegen-pc/framegen-pc.patch`, based on `BASE_COMMIT`). It enables `ENABLE_LSFG` on
   all platforms, shows frame_gen, multiplier, target_rate and flow_scale in the UI and adds the
   translations in `src/qt_common/config/shared_translation.cpp`. It needs the user's own `Lossless.dll`
   in `%APPDATA%\eden\lossless\`; Eden only reads the file, no login.
   - Pending on the Ryzen: update Eden to Nightly PGO v3, store `Lossless.dll` in
     `D:\Ferramentas`, build with the patch and test. The GPU is the integrated Vega, so the
     cost must be measured.
2. **Frame gen in the port (Switch): not recommended for now.** Eden's LSFG is Vulkan (shaders
   DXBC→SPIR-V), and the port is OpenGL/Mesa. The Switch GPU is already the bottleneck: 3× scale stalled and we went back to
   2×. The preferred path is the native 60 FPS interpolation already on the roadmap
   (`native30_interpolated`). A simplified version should only be reconsidered if the cost measured in Eden
   on the PC (Vega) is low. The repository is public and Eden's code is GPL-3: first, check
   license compatibility and require the user to bring their own `Lossless.dll`.
3. **Frame gen directly on the Switch (sysmodule): nothing known exists.** The alternative is Switch Frame
   Tap, a homebrew that streams over USB to the PC at 1080p60 (https://gbatemp.net/posts/10917720/),
   combined with Lossless Scaling on the PC. It works with any game, including the port, but adds
   latency. Compatibility with AMS 1.11.1 / FW 22.1.0 has not been verified yet.
### Cheaper, paced 60 FPS interpolation (2026-10-17)
- **Native replay skipped on GL** (`config/native_replay` = `1` restores the old behavior). Each step frame
  was drawn twice: in the native namespace (1), which exists to return pixels to guest memory
  (read-backs, savestate), and in the visual one (2), which is what gets presented. The GL backend never reads namespace 1
  (textures always come from the guest bytes; no read-back nor savestate capture), so the whole
  scene was drawn for nothing: half the GPU at 30 FPS, a third with `config/fps60`. The backend reports this
  via `PicaNativeNamespaceObservable()` (Vulkan stays `true`). The only thing crossing namespaces: the
  shadow consumer (Shadow2D) took the image from namespace 1 (the first in the map); now it prefers the
  one from its own namespace. At 30 FPS the image is the same; with fps60 the shadow no longer runs one step ahead
  of the geometry (a fix). `perf.txt`: `native_replay_skipped`.
- **"Keep game speed" pacing** (`config/interp_pacing`, on; `0` draws every
  intermediate as before). A governor on the render thread
  (`oot3d_native_interpolation_pacing.h`) learns the recent cost of each frame type (max of
  GPU time and render thread time) and, for each intermediate, picks the best level whose pair
  (step + intermediate) fits in two refreshes (33.3 ms): full, reduced or **dropped**
  (re-presents the step image: only the already-made copy + RCAS, no scene replay). Starts at full and only
  goes down on real measurement: the current level must have been measured and the pair exceed 100% of the budget in
  3 consecutive decisions; goes up when the better level's pair fits in 97%. Probes every 60 decisions below
  the preferred one. (1st version, bug on console: 95%/90%, probes every 150 and, before the GPU clock
  calibration, cost = render thread + swap — swap includes the vsync wait —; the first seconds
  dropped due to inflated costs and a ~16+16 ms pair, which fits, never got back below 90%: stuck
  in `dropped`, ~30 real frames presented irregularly. Without a GPU clock the cost is now only
  the render thread's.) The cost of an undrawn level is estimated
  as the step cost × the last measured ratio (both are a replay of the same scene), so it
  comes back by itself when the scene gets light; probes (one at a time, never-measured level or every 150
  intermediates below the preferred one) refresh the ratio. Spikes count at most 2× the average
  (SwitchWakerHD `interp_pacing.h`). The step frame never changes; logic never depends on the
  intermediates (the scheduler still coalesces refreshes).
- **Reduced resolution for the intermediate** (`config/interp_scale`, e.g. `1.5` with scale 2; multiples
  of 0.25 below the scale; absent = off). An intermediate that doesn't fit at full is drawn in its own
  namespace (3) with targets at the smaller scale (`SetPicaNamespaceResolutionScale`; viewport,
  scissor, copies and presentation use the target's scale) and FSR EASU upscales as always.
  `config/interp_scale_mode` = `always` reduces every intermediate. Only applies if the sample clears (memory
  fill) each target before the first draw (color; depth/stencil if used), because namespace 3
  only sees some frames; otherwise goes full (`reduced_refused`). Refused with `antialiasing` on (the
  AA images would have to be reallocated every frame).
- Console crash with `interp_scale=1.5` (2026-10-17, mallinfo found the heap corrupted at the 21st window):
  in the session's perf.txt the reduced level never drew (`interp_reduced=0` in all windows,
  `gl_targets=2`: no namespace 3 targets); only in the last window were there 11 "reduced" decisions, all
  refused (`reduced_refused=11`) by the CPU check. That check had function-`static` vectors;
  it now uses session scratch (`oot3d_native_reduced_replay.h`) and
  `oot3d_native_reduced_replay_stress_tests` (worker generating real frames, render thread with the
  governor alternating full/reduced/dropped ~200 times in 6000 frames, guest thread reading the
  counters) passes clean under ASan and TSan. `SetPicaNamespaceResolutionScale` no longer deletes live images
  (refuses if the namespace already has targets). `config/heap_check_windows` (1–1000, default 20) controls the
  mallinfo frequency: `1` finds a corruption ~5 s after it happens.
- **Stall-free GPU time** (`oot3d_native_gpu_frame_timer.h`): `glQueryCounter(GL_TIMESTAMP)` at the
  start and end of each frame, read only when available (ring of 8). The switch-mesa GPU clock
  is not in ns (SwitchWakerHD measured 1.63 real ns per "ns"): the factor is calibrated against the
  CPU clock over a base of ≥ 3 s and nothing is used before that (`gpu_timer=0`; the governor uses render
  thread + swap time).
- **Reused EASU** (part of `gl_lean`): re-presenting the same image (dropped intermediate,
  presentation without a new frame) skips EASU and only runs RCAS from the stored upscaled image.
- Bottom screen/HUD: in the TopScreen profile the bottom screen is already recomposed onto the top one and the HUD (N64 UI) is
  cheap and may change every refresh; nothing to reuse. Shadows: depend on actor matrices
  (interpolated), can't be reused in the intermediate.
- `logs/perf.txt`, `interp:` block (per 150-frame window): `pacing interp_scale level
  step_frames interp_frames interp_reduced interp_dropped other_frames reduced_refused repeat_fallbacks
  native_replay_skipped probes level_changes gpu_timer gpu_ns_per_tick`, `gpu_ms_<type>` and `rt_ms_<type>`
  (average GPU and render thread ms per type: step/full/reduced/dropped/other) and the estimates
  `est_step_ms est_full_ms est_reduced_ms pair_budget_ms`.
- Test: `oot3d_native_interpolation_pacing_tests` (governor, measurement latency, hysteresis, spikes,
  GPU queue simulation: with dropping the steps stay on time, without dropping >100 late).
- A/B on console: `native_replay` = `1`; `interp_pacing` = `0`; `interp_scale` = `1.5` (and
  `interp_scale_mode` = `always` to see the cost/quality of reduced on every intermediate).

### Texture/object flicker when turning the camera fast (fps60, 2026-10-24)
- **Symptom** (pgo5o, `config/fps60`, `dynamic_resolution`, `fps60_mode=adaptive`): when turning the
  camera fast, a texture/object "flickers" for under 1 s. At pure 30 FPS it practically doesn't happen.
- **Cause** (`oot3d_native_pica_visual_frame.cpp`): fps60 presentation stays one step behind and,
  for each game step, shows the step frame (alpha 0, instant of the previous step) and the
  intermediate (alpha 1/2). Both were sampled over the **draw list of the new frame**. The game
  culls with each step's camera; turning fast, each step removes pieces at one screen edge and
  adds pieces at the other. So: (1) a piece the game culled in the new step vanished in both refreshes
  (~33 ms) — a hole at the trailing edge; (2) a piece the game added was drawn with the new step's
  camera: a whole step out of place in the step frame, half a step in the intermediate. At 30 FPS
  the game frame is drawn as-is, so it doesn't show. Paired draws only swap uniforms and
  stay correct.
- **Fix** (on by default; `0` in the file restores the old behavior for A/B):
  1. `config/interp_exact_step`: the step frame (alpha 0) is exactly the previous game frame
     (`ViewOot3dPicaVisualFrame(previous)`: its draws, uniforms and textures).
  2. `config/interp_carry_unmatched`: in the intermediate, the camera is measured on paired world draws
     (static, no skinning, top target): Δ = VM_prev × VM_cur⁻¹, by voting (moving actors
     disagree and lose). A draw only in the new frame starts where the previous camera saw it (view
     rows × Δ, interpolated like the paired ones); a draw only in the previous frame is kept (right after
     the previous neighbor's pair, same target, after the clear) and moves to the new camera (Δ⁻¹). Works
     with the camera in the view rows or embedded in the model. Without a camera measurement: old behavior.
- **Diagnostics in perf.txt** (`interp:` block, per window): `exact_step carry_unmatched transitions
  trans_unmatched unmatched_prev unmatched_cur carried_cur inserted_prev unmatched_no_camera
  camera_measured discontinuities` (averages per transition). If `unmatched_no_camera` is high and
  `camera_measured` low, the camera wasn't measured and the intermediate fix didn't act.
- **Ruled out** (`[decision]` guards): DRS (the depth fill clears the whole attachment without
  scissor; the scale changes at most once per FrameSerial, so step and intermediate use the same one);
  texture cache (key by content hash; an evicted entry is decoded again from the
  plan's bytes: delay, never a wrong pixel — and it would apply at 30 FPS too); hold (the dropped intermediate
  re-presents the whole last presented frame).
- Test: `oot3d_native_pica_visual_cull_tests` (camera turning 9°/step in a ring of pieces culled
  as in the game, camera in the view and embedded in the model): old 288 missing + 144 displaced + 144
  extra in 160 samples; with the fix 0. Still camera: sample identical to the old one.

### Floor shimmer on deko3d: the PICA LOD bias applied twice (2026-11-06)
- **Symptom** (0.1.2-test.6, `logs15`/`logs15sc`, Forest Temple; reported as "some flicker" on
  deko3d since test.3): "square pixels flickering on the floor texture", with the HD pack on, scale
  3.00, fps60. Not on PC (Vulkan).
- **Cause:** the generated fragment shaders apply the PICA LOD bias themselves:
  `textureLod(pica_textureN, coord, lod + fragment_uniforms.texture_lod_bias[N])`
  (`oot3d_native_pica_fragment_shader_gen.cpp`, `pica_sample_texture0..2`). The deko3d sampler
  carried the same bias again (`sampler.lodBias = LodBiasRaw / 256`, `PicaResources::SamplerSlot`),
  copied from the GL backend's `GL_TEXTURE_LOD_BIAS`; a sampler bias adds to an explicit LOD too
  (Vulkan spec, LOD operation: λ' = λbase + sampler.bias + shaderOp.bias, λbase = the shader's Lod;
  GL: `TEXTURE_LOD_BIAS` likewise; deko3d writes it into the TSC). The Vulkan backend keeps
  `mipLodBias` 0 (`gfx_vulkan_pica_textures.cpp`, `SamplerState.LodBias = 0`). The Forest Temple's
  room models (RomFS `scene/bmori1_*_info.zsi`, texture bindings at material + 0x10, bias float at
  + 0x10 of each) use −1.7 on 372 of 424 mipmapped bindings (−1.0 on 48), floors included
  (`frs_yuka_02`, `frs_k_yuka_01/02` in room 8, `frs_jutan_*`). deko3d sampled them at −3.4: level 0
  stays until a pixel covers 2^3.4 ≈ 10.6 texels (3.2 on PC and on the 3DS), so a floor seen at a
  distance or a grazing angle undersamples its full-size level, and each texel square blinks as the
  camera (or an fps60 in-between frame) moves. The pack's larger images make it worse.
- **Fix:** the sampler has no LOD bias (`dkSamplerDefaults`' 0), and the bias left
  `PicaSamplerKey` and `PicaTextureKey`: one sampler, one texture and one pack upload per image
  whatever the bias (room 8's `frs_water_01` is sampled with −1.0 and −1.7 by one material: two
  textures and two pack uploads before). No shader change, no `deko3d.pack` rebuild.
- **Expected on the console** (estimate, not measured): far and grazing floors and walls as stable
  as on PC; distant surfaces read smaller levels, a little less texture bandwidth. `resources
  (frame)` in `deko3d.txt`: `samplers=` lower than test.6's 24–27 for the same route, `textures
  created=` slightly lower.
- **Other leads, checked against the code and logs15:** in-flight reuse (evictions and slot frees only
  for entries idle 2 frames, right after the fence wait of a 2-frame ring; uploads before a full
  barrier with image/descriptor invalidation), block decode (the shared `DecodePicaTextureRgba8`,
  like Vulkan; a decode error would not blink), fps60 (`exact_step=1 carry_unmatched=1` in every
  `interp:` window; in-between frames draw the same textures), depth (Z24S8, the PICA's 24 bits;
  z-fighting makes bands, not texel squares). Native/pack swaps: `live=` never passed the 96 MiB
  budget at the log points (max 68 MiB) and `created=` grew 13–100 per 30 s, so each swap is a
  one-time pop, not a flicker. Two counters say it directly in the next logs:
  `evicted=N (budget M)` (M: textures evicted for the budget, not after 30 s idle) and
  `pack images=N (again M)` (M: pack images uploaded again under the same settings, evicted
  meanwhile; the surface showed its native texture for the frames in between). Both should stay 0
  in one area at the default budget; if they grow, raise `config/gl_texture_budget_mb`.
- The GL backend (`gfx_opengl_pica.cpp`, not used on the Switch) still sets `GL_TEXTURE_LOD_BIAS` on
  top of the shader's; Vulkan's pack images are uploaded single-level (no mips).
- Tests: `oot3d_deko3d_backend_tests` (`TestSamplerAndTextureKeys`: a −1.7 bias gives the same
  sampler and texture key as none); guards "the PICA LOD bias is applied once" (no `.lodBias =` in
  `gfx_deko3d_pica_resources.cpp`) and "the shader is where the PICA LOD bias is applied".

### Top screen at one sample per presented line on deko3d: `config/deko3d_line_samples` (07/11)
- **Waste** (GpuScout audit, Forest Temple room 8): the game renders its 240 presented lines
  twice (the 480 columns of its 480x400 top target) and its display transfer averages each pair.
  deko3d kept both samples at host resolution: 1440x1280 at 3x for 720 lines; at the DRS floor
  2.25, 1080x960 averaged down to 540x960, then stretched to 720p. The PC Vulkan backend already
  renders one sample per presented line (`render_resolution_policy.h`).
- **Now** (default): the same policy on deko3d. `PicaTargetSamplingX` asks
  `Oot3d::NativeTopScreenStorageSamplingX` with the extent the allocation scale presents: 0.5 for
  the top target from scale 2 (at least the game's 480 samples), 1 below it, for the bottom screen
  and every other target. A target's X (the presented vertical axis) scales by
  `ScaleX = Scale * SamplingX`; Y (presented width, widescreen) is unchanged. The display
  transfer's 2:1 filter becomes a 1:1 copy (`PlanPicaTransfer`, as Vulkan's
  `SourceAtPresentedDensity`); the display image, its content and the scanout keep their extents.

  | scale | test.7 (two samples) | now (one) | lines |
  | --- | --- | --- | --- |
  | 3.00 | 1440x1280, 1.84 Mpx | 720x1280, 0.92 Mpx | 720 |
  | 2.75 | 1320x1173, 1.55 Mpx | 660x1173, 0.77 Mpx | 660 |
  | 2.50 | 1200x1067, 1.28 Mpx | 600x1067, 0.64 Mpx | 600 |
  | 2.25 | 1080x960, 1.04 Mpx | 540x960, 0.52 Mpx | 540 |

- **What follows the target:** viewport and scissor scale X by `ScaleX` (`PlanPicaViewport`,
  `PlanPicaScissor`, the widened path's X edges); placed UI and the widescreen canvas keep their
  Y-axis rules (`Scale`, `ScaleY`), so proportions hold. Dynamic resolution keeps the
  allocation's sampling at every level (`PlanPicaDynamicExtent`), the copy fills the content
  corner and the two-texel edge bands work as before. Grass draws with the scene's viewport and
  scissor (`RememberGrassScene`); fps60 in-between frames replay into the same target. Save states
  resample each axis to the guest extent and back (`MakeGuestExtentSnapshot`, `PlanRestorePixels`),
  so a state loads under either setting.
- **Scale range, chosen on purpose:** unchanged (`config/resolution_scale` 3, DRS floor three
  steps below: 2.25). The ceiling 3 is now a true 720p, one rendered line per panel line; the floor
  is test.7's heavy-room sharpness (540 lines) at half its pixels, so no level is blurrier than
  test.7. The controller's pixel model still holds (area ∝ scale², both axes linear in the scale);
  its fixed part (`FixedFraction` 0.35) is now relatively larger, which only makes up-steps more
  cautious. (test.8 showed the controller never climbed at 60 FPS at all, for another reason: see
  "Dynamic resolution on deko3d: its own calibration" below.) Every level spends less GPU than
  test.7 at the same level. Lost: the game's own vertical
  2:1 averaging (Vulkan already drops it).
- **Expected** (estimates, not measured; scene time taken as 60-75% pixel work): room 8,
  lighting off, 60 FPS: `busy_ms` 14.9 → about 9-11 at 2.25; the controller should settle at 2.5
  (600 lines) at about 11-11.5 ms, with the in-between frames fitting. At 30 FPS (scale 3) about
  13-14 ms where test.7 would draw 1.84 Mpx. Per-pixel lighting (47 ms at 2.25) roughly halves
  its pixel part: about 25-27 ms.
- **Log:** `deko3d.txt` at start: `targets: resolution scale 3.00, widescreen factor 1.067, top
  screen one sample per presented line (config/deko3d_line_samples 1)`, and the created target
  `targets: 720x1280 (native 480x400 …, wide)`. Every `perf.txt` window's `dkperf` line ends with
  `top_w= top_h= top_line_samples=`: the drawn extent of the last presented top scene (DRS level
  included) and 1 or 2.
- **Back to test.7:** `config/deko3d_line_samples` containing `2` (read at start).
- **Console check (1 minute):** room 8, 60 FPS, lighting off, 30 s standing still: `dkperf`
  `top_w` 540-720 with `top_line_samples=1`, `top_h` 960-1280, and `busy_ms` against test.7's 14.9
  at the window's `drs_scale`; the picture no softer than test.7. With the `2` file: `top_w` twice as
  large, as test.7.
- Tests: `oot3d_deko3d_backend_tests` (`TestTopScreenLineSamples`: config parse, policy agreement
  with Vulkan, both settings at 3 to 1.5 with and without widescreen, transfer copy against
  average, display extents, edge bands, scanout, viewport tiling, area ∝ scale²;
  `TestSavestateRoundTrip`: a one-sample target saved and restored), `oot3d_native_a32_input_tests`
  (touch on the 720x1280 target); five guards "deko3d top screen at one sample per line". No shader
  change, no `deko3d.pack` rebuild.

### Dynamic resolution on deko3d: its own calibration (07/11)
- **Seen** (0.1.2-test.8, `log16`, Per-pixel, HD pack, grass Near): "30 FPS looks sharper than 60".
  In `perf.txt` 232 of the 234 windows at 60 FPS show `drs_scale=2.25` (540 lines; the other two
  2.5), title screen and Temple of Time included; all 25 at 30 FPS show 3.00 (720 lines).
- **Cause: thresholds from the GL timer.** The controller (`oot3d_native_dynamic_resolution.h`) was
  calibrated in tour17/tour19 on GL: 3 ms of GPU per pair the GL timer misses (`HiddenGpuMs`), down
  when the pair + 3 passes 85% of 33.3 ms (a step frame over 12.7 ms), up when the pair predicted one
  level up + 3 fits 80% (from 2.25: a step frame of at most 10.1 ms). On deko3d the device times its
  own frames (busy = scene + tail) and nothing is hidden: room 8 held 60.0 FPS with no in-between frame
  dropped and no late step at `busy_ms` 16.8 (windows 32400-34200, `drs_pair_ms` 36.2 = 2 x 16.6 + 3),
  and the governor drops exactly where the timed pair passes 33.3. The title (step frame 10.4-11.4 ms,
  windows 300-900) and the Temple of Time (10.5-11.9, windows 4950-6150 and 22500-23250) use 67-71% of
  the pair and never met the up test. Neither CPU nor GPU held them there: the controller's own
  thresholds did. The one climb (window 24750, step 9.7) fell back at 12.6 (window 25050).
- **What 3.00 costs at 60:** the same spots at 30 FPS give the real cost per scale. The Temple of Time
  at 3.00 takes 16.0-20.3 ms of GPU a frame (windows 21600-22200, 23700-24300; `scene_ms` 10.8 → 19.5
  against 2.25: x1.81 for x1.78 the area, all pixel work with Per-pixel lighting); room 8 25.8
  (x1.55, about 0.3 fixed). A 60 Hz frame has 16.7 ms, so 3.00 cannot hold 60 FPS in those places
  at stock clocks with Per-pixel; the reachable level there is 2.5 (600 lines), 2.75 in lighter shots.
- **Fix:** `UseDeko3dDynamicResolutionTiming` when `config/renderer` selects deko3d
  (`EnsureInterpolationPacing`): no hidden part, down when the pair passes 92% (30.7 ms, 2.7 ms before
  the governor drops in-between frames), up when the pair predicted at the next level fits 82%
  (27.3 ms; from 2.25 a step frame of at most 11.9 ms, from 2.5 about 12.0 at 2.5). The cost model
  keeps its 0.35 fixed part: against the measured 0-0.3 it predicts one step up at most ~7% low,
  inside the 82 → 92% band. GL keeps the old calibration. No change at 30 FPS (the pair is one frame
  in 66.7 ms: always 3.00).
- **Expected** (replay of log16's windows, not measured on the console): title and Temple of Time
  at 2.5 (2.75 when the step frame at 2.25 is under ~10.4 ms, 3.00 under ~8.5), room 8 and the other
  heavy stretches (13-21 ms at 2.25) at 2.25 as before; whole session 2.31-2.33 mean instead of 2.25;
  about 40 scale changes in 11 minutes (most in the title's changing shots); pairs over budget (an
  in-between frame the governor drops) 29-40 more step frames than at the fixed 2.25, of 17839.
- **Switching 30 → 60 (L3+R3 > Video > FPS):** the controller used to start over at 3.00 on every
  switch, as at a session start, and step down from there. In room 8 (25.8 ms a frame at 3.00) that
  descent took 3 downs and dropped 16 and 23 in-between frames, with 12 and 6 late steps (windows 32250
  and 34800, `drs_downs=3`). Now `RatePacingResolutionMemory` (`oot3d_native_rate_pacing.h`) keeps the
  scale 60 FPS used when the session left it, and a switch to 60 begins there
  (`DynamicResolutionController::Configure` start scale, rounded down to a level). If the session never
  ran at 60, the switch begins at the floor. From there the controller climbs as usual when there is
  room. A switch to 30 and the session's first start (at either rate) still begin at the top.
- **Log:** `drs_pair_ms` in `perf.txt` no longer includes the 3 ms on deko3d: about twice
  `gpu_ms_step` (log16: twice plus 3). The stderr start line (not in the console's log files)
  `TRIAEVUM_DYNAMIC_RESOLUTION ... timing=deko3d down=0.92 up=0.82 hidden_ms=0.0`.
- Tests: `oot3d_native_dynamic_resolution_tests`: `TestLog16GlCalibrationStaysAtFloor` (the bug,
  replayed), `TestLog16Deko3dReplay` (every 60 FPS window of log16, `oot3d_native_dynamic_resolution_log16.inc`,
  at its step frames with jitter and its `busy_max`, real fixed share 0 and 0.3: title and Temple
  of Time ≥ 2.45 mean, room 8 at 2.25, at most 50 more over-budget steps, at most 6 bounces),
  `TestDeko3dSteadyAreasHoldOneLevel` (steady areas 8-17 ms, fixed share 0-0.5, ±4-8% jitter and
  spikes: at most one change after 20 s, no bounce, the level held fits);
  `oot3d_native_cadence_governor_tests` `TestLiveRateSwitchKeepsSixtyScale` (room 8 standing at
  15 ms, 60 → 30 → 60: back at 2.25 at once with no pair over budget, where the old start at 3.00
  gave at least 10; an area left at 2.5 comes back at 2.5 and still climbs to 3.00 when light; a
  session started at 30 enters 60 at the floor; start scales round down). Three guards "log16
  (test.8)". No shader change, no `deko3d.pack` rebuild.
- **Console check (1 minute, 60 FPS, Per-pixel):** title screen 20 s,
  the Temple of Time standing 20 s, Forest Temple room 8 20 s. `perf.txt`: Temple of Time windows
  `drs_scale` 2.5 (2.75 at times) with `dkperf top_w=600` (660), `level=full`, `interp_dropped` 0-2;
  title the same in its light shots (its heavier shots, 13-18 ms at 2.25 in log16, stay at 2.25);
  room 8 `drs_scale=2.25`, `interp_dropped`/`late_steps` no higher than log16's room 8 windows.
  Then, still in room 8, switch to 30, wait 10 s and switch back to 60: the window with the switch
  back shows `drs_downs=0` (log16: 3), `drs_scale=2.25`, and few `interp_dropped` (log16: 16 and 23).

### Resolution at 60 FPS: Dynamic or Full, `config/drs_60` (11/07)
- **Why:** to compare the GPU clock options (Standard 307 MHz, High 460.8 MHz) at the full
  resolution at 60 FPS, where dynamic resolution would otherwise hide the difference by stepping down.
- **Option:** L3+R3 > Video > "Resolution at 60 FPS", next to FPS. Dynamic (default) is the
  controller above. Full: at 60 FPS the controller is configured with `MinScale == MaxScale`
  (`ApplyDynamicResolutionSixtyMode`, `oot3d_native_dynamic_resolution.h`, called by
  `ConfigureRatePacing`), so it holds the boot's full scale (handheld 3.0, docked the docked setting)
  and never steps down. 30 FPS is unchanged. The cadence governor still drops in-between frames
  whose pair does not fit; `new_fps` in `perf.txt` (and the overlay) then shows the real rate.
- **Persistence and live switch:** `config/drs_60` = `full` (absent = Dynamic), written by the panel
  through `VideoSettings` like `fps60`; read at start (`TRIAEVUM_DRS_60` on the PC). The panel
  publishes `LiveDynamicResolutionSixty()`; `EnsureInterpolationPacing` sees the new revision at the
  next frame and reconfigures the pacing as a rate change does. A 30 → 60 switch under Full starts
  at the full scale (the remembered 60 FPS scale only applies under Dynamic), and a held full scale
  is not remembered (`RatePacingResolutionMemory::Leave` skips a controller with no levels).
- **Logs:** start-up `TRIAEVUM_DRS_60 mode=`, and `drs_60=`/`held_full=` on the
  `TRIAEVUM_DYNAMIC_RESOLUTION` line, `drs_60=` on `TRIAEVUM_FRAME_RATE`; every `perf.txt` window
  has `drs_mode=full|dynamic` after `drs_min` (under Full at 60, `drs_min` equals `drs_scale`).
- Tests: `oot3d_native_dynamic_resolution_tests` `TestFullAtSixty` (3.0 and 4.5 held under a pair
  1.5x the budget, shedding or not, no change; Dynamic goes to its floor; 30 untouched; the parser);
  `oot3d_native_cadence_governor_tests` `TestLiveRateSwitchFullStartsAtFull` (heavy room 8 at 60:
  3.0, no downs, the cadence governor sheds; 60 → 30 → 60 and Dynamic → Full switches start at 3.0;
  back to Dynamic resumes at the remembered scale); `oot3d_video_settings_tests` (`drs_60` round trip).
- **Console check (1 minute):** 60 FPS, Resolution at 60 FPS = Full, GPU clock Standard: stand in
  the Temple of Time 20 s; switch GPU clock to High, 20 s more. `perf.txt`: `drs_scale=3`
  `drs_min=3` `drs_mode=full` in every window at 60; compare `new_fps` between the Standard and High
  windows. Switch FPS to 30 and back to 60: the window after shows `drs_scale=3`.

### fps60 cadence: regularity before average, `config/fps60_mode` (2026-10-17)
- **Diagnosis** (`switch-logs/integ2b`, CPU 1785, 4.5 min at 60, 52 FPS average): in the
  heavy stretches (host_frames 9450–14250) the game thread spends 27–31 ms of each 33.3 ms step pair
  (`game_cpu` 80–95%) and the GPU ~16.5 ms per replay (`est_step`≈`est_full`≈16.5: pair 33.0–33.2 of 33.3).
  Frame by frame, window 12150 (fps 54.6, 2.75 s) had 165 refreshes in 150 host frames:
  ~15 host frames with **two refreshes** (the game was late: the step goes out with no intermediate frame) and,
  in the others, step + dropped intermediate (81 steps, 67 drops, 1 intermediate). That is, ~30
  new images/s with the "fps" counting the repeats, and ~5 steps per second holding the image 3
  refreshes instead of 2. In window 10650 (fps 45, 3.33 s): 200 refreshes in 150 frames → 50 frames
  of 2 refreshes (step alone) + 50 pairs (26 with intermediate, 24 dropped): images of 1 and 2
  refreshes mixed several times per second. Mechanism (reproduced in the simulator): a delay puts
  two refreshes in one host frame — a whole step (~23 ms) at once — which delays the next one;
  the loop falls into an irregular rhythm of 1- and 2-refresh frames and stays there; at the same time the per-frame
  governor alternates draw/drop with the GPU pair at the limit (33.2 > 97% of 33.3).
- **New policy** (`oot3d_native_cadence_governor.h`, default `adaptive`):
  1. at 60, one refresh per host frame: a late frame carries **one refresh of debt** instead of
     running two (the image stays one refresh behind; a second debt is discarded; stalls of 2.5+
     refreshes — loading — recover as before); the intermediate is drawn (full/reduced),
     never dropped here and there;
  2. 3 presentations of 2+ refreshes among the last 60 images → **locks at 30**: the game runs both
     refreshes of the step in one host frame (the intermediate frame and its cost on the game
     thread disappear), the pacer waits 2 periods and a late step re-anchors the clock (hold 3,2,2… and not 3,1);
  3. stays at 30 for at least 2 s (doubles up to 16 s if 60 fails again within 10 s) and only returns with
     proven headroom: game thread pair p90 ≤ 90% of 33.3 ms, estimated GPU/render pair ≤ 90% and
     no late step in the last 2 s.
- **A/B** (`config/fps60_mode`, desktop `TRIAEVUM_FPS60_MODE`): `legacy` (default, see below), `adaptive`, `strict60` (every
  intermediate drawn, old loop; = `interp_pacing=0`, which without the new file becomes `strict60`),
  `legacy` (exactly the previous behavior). Options after the mode: `adaptive misses=3 window=60
  hold=2 max_hold=16 relapse=10 game=0.9 gpu=0.9`.
- **perf.txt**, new `cadence:` block (at the end of the line before `gl_textures_live`, without touching the
  existing fields): `fps60_mode lock present_1/2/3/4p` (presentations N refreshes after the previous one;
  since 06/11 `fps60_mode` is the mode in effect, legacy at 30, followed by `rate_hz`)
  `hold_1/2/3/4p` (refreshes each new image stayed on screen) `repeats cadence_switches` (hold
  changes between consecutive images) `late_steps superseded_frames` (steps never shown) `lock_changes
  lock30_ms hold_s game_pair_ms game_pair_p90_ms`. `perf_report.py` summarizes the cadence and shows
  step/interp/drop, hold and lock per window in `--windows`.
- **Host simulation** (`oot3d_native_cadence_sim.h`; `oot3d_native_cadence_governor_tests --report`):
  console pipeline (game → render thread → GPU → pacer → swap on vblank) with the production
  governors and perf.txt costs. Blocking swap (as the console measures) / with a 1-frame queue:
  | scenario | legacy: hold 1/2/3, switches/s, judder | adaptive: hold 1/2/3, switches/s, judder |
  |---|---|---|
  | heavy (game 21±3 ms, GPU 16.5) | 332/1624/6, 11.2/s, 8.6 ms | 1/1799/0, 0.03/s, 0.7 ms (locked 30) |
  | v. heavy (24 ms, GPU 17) | 75/1661/66, 4.6/s, 4.9 ms | 0/1799/0, 0/s, 0.6 ms |
  | normal (18 ms, GPU 14) | 3408/95/0, 3.2/s, 2.9 ms | 3584/7/0, 0.23/s, 0.7 ms (60) |
  | 220 s session (like integ2b) | 5.2 switches/s, judder 4.1 ms, 95 holds of 3 | 0.22/s, 0.9 ms, 0 holds of 3 |
  | session, queued swap | 0.36/s, deviation 5.7 ms | 0.07/s, deviation 3.4 ms |
  (judder = RMS of shown interval − content interval between consecutive images; game at 60
  refreshes/s in all). The average drops little or not at all; what disappears are the cadence switches.
- Tests: `oot3d_native_cadence_governor_tests` (parser, refresh plans, hold histogram,
  lock/return/relapse, game pair estimate, per-frame governor without dropping, simulation; clean
  under ASan/UBSan) and `triaevum_presentation_clock_tests` (`AdvancePlanned`, borrowing outside the saved
  state, 2-period pacer with re-anchoring).
- What to watch on console: `cadence_switches` and `hold_3` per window (should drop to ~0 in the heavy
  stretches), `lock`/`lock30_ms`, `game_pair_p90_ms` (near 30 = the game is the limit; below 28 with
  `lock=30` = the GPU is the limit) and `guest_fps` ~60 (game speed).

- **Console result (integ4, CPU 1785) and fix**: the 1st `adaptive` stayed locked at 30 83.7%
  of the time (18 switches), FPS 53.7 → 38.5, and even locked it wasn't regular (hold 1/2/3 =
  28/63/8.7%, 983 late steps). Logic didn't get worse (12.25 vs 11.9 ms/step); the game pair
  (30.6 ms) stayed above the return threshold (30) and never unlocked, and the 30 loop (two refreshes per
  host frame) serialized the game thread and the GPU. Simulating with the integ4 numbers, locked 30
  with the 60 loop is also no more regular than legacy, and the debt at 60 (`PlanSixtyRefreshes`)
  delayed the game (52 of 60 refreshes/s). Now: **default `legacy`**; `adaptive` no longer uses the 30
  loop nor the debt (the scheduler plans like legacy in every mode), doesn't drop intermediates one by one and
  only locks at 30 under real overload (average game pair or GPU pair > 1.15 × 33.3 ms, option `lock=`);
  returns with average pair ≤ 92%. In practice, today `adaptive` ≈ `strict60`. The `cadence:` counters stay.
  "Textures jump and come back" defect: not reproduced; candidates were the 18 lock switches and the host
  frames with two refreshes or with zero (repeat) from the 30 loop, which no longer exist.

### Lighter core 0: `config/offload_*` (2026-10-17)
Diagnosis (`switch-logs/integ5`, session 67, CPU 1785, fps60): the game thread is the bottleneck
(`game_cpu` 68/81/85% in stretches at 57–60 / 45–57 / <45 fps; render 39%, plans 20%). Outside the
guest it spent per host frame `pica_submit` 1.62 ms (which **includes** the guest resumed inside
the drain: `guest_in_pica` ≈ 0.6 ms/frame in the session), `visual_present` 0.91, `dsp_mix` 0.50,
`pica_plan` 0.28 (inside `pica_submit`) and `frame_start` 0.25.

Where the time went: a PICA plan is **15.7 KiB** (`sizeof(Oot3dPicaVulkanDrawPlan)`: fragment shader
and uber uniforms with the fog LUT, 6 KiB each) and a draw packet 11 KiB. With the deferred plan,
each draw's *placeholder* was still constructed zeroed twice (the loop's `plan` variable and the
return of `MakeDeferredPlan`), copied by assignment and moved three times (`Capture` →
`Append` → `push_back`); closing the frame (`FinishFrames`, in `visual_present`) moved each plan once
more. Moving a `std::array` is copying it: ~6 copies of 15.7 KiB per draw on the game thread. The
packet was assembled in the frontend and copied again into the queue (2 × 11 KiB per draw, inside the guest).

What changed (each part can be disabled, `0` in the file restores the old path; no file = on
on Switch; desktop: `TRIAEVUM_OFFLOAD=1` or `TRIAEVUM_OFFLOAD_<PART>=0/1`, off by default):
- **`config/offload_plan`**: the placeholder is written in place where the accumulator stores it
  (`CaptureInPlace` + `FillDeferredPlan`): one construction per draw, no copy. `FinishFrames`
  hands the whole pending vector to the frame when a single game frame closes (the common case) instead
  of moving plan by plan; with several frames (host late) it follows the old path. Same frames,
  same order, same resolved plans.
- **`config/offload_capture`** (inside the guest, in GX commands): the packet is built directly in the
  queue submission from the frontend state (`SubmitDrawPacketSource`, one 11 KiB copy
  less); texture hashes in a single pass (FNV-1a is sequential: the base mip hash is the state after
  its bytes, and without transformation the source hash is the content's — it used to be up to 3 passes); geometry
  hash only when a savestate asks (`CaptureState` computes it; the savestate is the same document);
  min/max index range remembered with the cached index bytes (same bytes, same range).
- **`config/offload_perf`**: the `perf.txt` line is assembled in memory and written by a service
  thread (`AsyncLineWriter`, priority 0x3B, cores 1|2): opening/writing to SD leaves the game
  thread. Queue of 32 lines; when full, drops (never waits) and counts in `perf_dropped`.
- **`config/offload_actors`**: the actor scan for the grass colliders
  (`CollectNativeActorInteractions`, up to 255 actors per category read from guest memory + one
  `unordered_set` per frame) is only done when the backend can use it: GL never reads the grass bridge
  (only Vulkan/NRI). On Switch the image doesn't change.
- No key: the DSP sample vector of each refresh is reused (no `malloc` per refresh).

What did **not** leave core 0, and why:
- `dsp_mix`: each DSP frame reads the configuration the guest wrote in response to the previous
  frame and writes status, aux buffers and `final_samples` to guest memory before the signal; the
  guest stays stopped waiting for that signal. Nothing to overlap without speculating (or changing the
  observable order); resampling/stretch is already on the audio thread. Only the buffer was reused.
- State decoding and vertex/index/texture copies: must be done at the moment of
  submission (the guest rewrites the memory right after); the per-write-generation cache already avoids
  repeated copies.
- Input (SDL polling): would change which frame the game sees the button in.

`perf.txt`, new `offload:` block (before `mesa:`, without touching existing fields):
`offload_capture/plan/perf/actors` (modes), `pica_capture_ms` (game thread time inside
draw submissions: part of `guest`), `pica_capture_draws`, `capture_copy_kb`, `capture_hash_kb`,
`index_range_reuses` (per host frame), `pica_submit_host_ms` (`pica_submit` without the guest
resumed inside it), `perf_line_ms` (cost of the previous line on the game thread),
`perf_writer_ms`/`perf_dropped` (service thread). Nothing new waits on a lock: the perf queue only
holds the mutex to push/pop. `perf_report.py` summarizes the block.

Equivalence proof (`oot3d_native_offload_tests`, also clean under ASan/UBSan and TSan): old ×
lean capture (+ packet from source) on thousands of random draws over synthetic memory with
guest writes (with and without write generations, rewrites with the same value, with payload
transformation): submissions, snapshots, hashes, index ranges and the savestate (`CaptureState`, also
after restoring) identical; frontend copying × building from source on random command
lists (with composition spans and light binding): identical packets; accumulator moving ×
handing over the vector with draws, fills, transfers, raw copies, superseded frames, draws of the next
frame and continuation screens: identical frames and pending items; placeholders moved × in place,
with the plan worker on and dependency flushes: identical frames before and after
resolving; perf writer: all lines, in order. Run on the Ryzen (g++ 15.2, -O2, ASan/UBSan,
TSan): offload, deferred plan (unit and soak), visual frame (soak and equivalence, also
`--threads`), vulkan plan, submission, frontend, command stream (including the stress) and shutdown
pass. The deferred plan soak in worker mode measures heap traffic/live batches and failed once with the
machine loaded by another build; alone it passed 4/4 with `TRIAEVUM_SOAK_MOVE_FREE=1` and 4/4 with `=0`
(old path), base and ASan. The 8 changed TUs compile with the Switch flags (devkitA64 16.1).

Estimated gain on the game thread. `oot3d_native_offload_tests --bench` (Ryzen 5 5600G, -O2, 200
draws per frame, median of 3): old path 2.08 µs/draw in the placeholder (of which 0.68 µs in the
stretch `pica_plan` measures) + 0.78 µs/draw in `FinishFrames` = 2.86 µs/draw; new path
1.20 + 0.006 = 1.21 µs/draw. Scaling by the `pica_plan` measured on console (0.28 ms/frame ↔ 0.68
µs/draw): before ~0.86 ms in `pica_submit` (fits in the ~1.0 ms of `pica_submit` without guest) + ~0.32 ms in
`visual_present`; after ~0.49 ms. **Estimated gain ≈ 0.7 ms per host frame** (~4% of the
16.7 ms budget), plus the capture (one fewer 11 KiB copy per draw and ~75% fewer hashed bytes in the
tests; the real console cost shows up in `pica_capture_ms`, which wasn't measured) and the
perf.txt write (one SD open+write every 150 frames, which could stall the game thread: see the
freeze analyzed in the watchdog). The `bench capture` is not usable as an estimate: synthetic memory
with random addresses fills the geometry cache (4096 entries) and the cost is dominated by
LRU eviction via linear scan (`std::min_element`), which already existed and doesn't change here.

A/B on console: create `config/offload_plan`, `config/offload_capture`, `config/offload_perf` or
`config/offload_actors` with `0` (one at a time) and compare `pica_submit`, `pica_plan`,
`visual_present`, `pica_submit_host_ms`, `pica_capture_ms` and `game_cpu` in the same scene and clock.
With `offload_perf` on, the perf.txt line is written whole at once (no longer cut off in the
middle); to find where the game thread stopped, use the watchdog's `logs/hang.txt`.

### Game-thread host work outside the guest (2026-11-06)
Logs `logs15`/`logs15sc` (test.6, CPU 1020, Forest Temple room 8): in each `perf.txt` window, the
`ms/frame:` fields nest as follows. `frame_start` = `host_frame_start` + `input` + `renderer_frame_start`
(`oot3d_native_a32_window_frame.cpp`, RunFrame). `pica_submit` (RunGuestRefresh) contains `pica_plan`
(placeholders, `FillDeferredPlan`) and `guest_in_pica`: the guest resumed after each completion
signal. That time is also counted in `guest`, so `pica_submit_host_ms` = `pica_submit` − `guest_in_pica`.
`pica_capture_ms` (the GX command's packet build, decode and snapshots) is inside `guest`. In room 8
with the second core on, the game thread's work outside the guest is about 0.9 ms
(`pica_submit_host_ms`, of which 0.56 is `pica_plan`) + 0.83 ms (`frame_start`, of which 0.66 is
`input`) + 0.48 ms (`visual_present`), plus 2.44 ms of capture inside `guest` (24.7 µs per draw).

- **Second core: no lock handoff at each render submission**
  (`oot3d_native_render_command_stream.cpp`, Enqueue). Every `renderStream.Submit` from the frame loop
  released and retook the kernel lock. With the second core waiting, Horizon's unlock hands the lock
  to the waiting thread, so the game thread then waited for that core's whole SVC or DSP frame. This
  happened in `visual_present` (two submits), in each drain pass's `PublishNativeSceneView`, and in
  the two submits at the start of RunFrame. The lock is now released only when the submit actually
  waits for a frame slot. Test: `oot3d_native_render_command_stream_tests` (TestKernelLockKeptUnlessWaiting).
  A probe thread never gets the lock across 20000 submits with free slots, and does get it while a
  submit waits for a slot. The old code fails the test.
- **Geometry snapshot cache: O(1) LRU eviction** (`oot3d_native_pica_submission.cpp`). A miss
  with the cache full (4096 entries) scanned every entry to find the oldest one (`std::min_element`
  on an `unordered_map`). The cache now keeps an intrusive list in its own nodes and evicts the same
  entry (the smallest `last_used_sequence`). The list is rebuilt after a savestate restore. Host -O2
  bench, miss-heavy: 53.7 → 5.2 µs per draw, about 85 µs per eviction removed. Test:
  `oot3d_native_offload_tests` (TestGeometryLru: across a draw, the evicted keys are exactly the oldest
  of those the draw did not touch, also after a savestate round trip). New `offload:` fields:
  `geometry_misses`, `geometry_evictions` per host frame.
- **Procedural and fog tables shared copy-on-write** (`oot3d_native_pica_frontend.h`). Every draw
  packet copied the 3.5 KiB procedural texture tables and the 0.5 KiB fog table, 4 of its ~10.6 KiB.
  They are now `Oot3dPicaSharedArray` objects, like the shader programs: draws share one copy until
  the game writes a table, and a write never reaches a packet already submitted. Test:
  `oot3d_native_pica_frontend_tests` (draws share the tables; a later write leaves the earlier
  snapshot intact). Host steady-state bench (`oot3d_native_offload_tests --bench-steady`, the same
  100 draws every frame): building the packet is 32% of the capture.
- **`perf.txt` `untimed:` block**: game-thread time outside every phase, ms per host frame:
  `frame_head` (RunFrame before `host_frame_start`), `refresh_prep` (per refresh: input sampling and
  service update), `ui_submit` (HUD/touch UI build and submit), `kernel_reacquire` (taking the kernel
  lock back after the frame start and the presentation hand-off, which is a wait for the second core),
  and `input_poll` (PollNativeA32Input alone, inside `input`). In logs15, `total_ms` exceeds the sum of
  the phases by 0.7 ms (second core on) and 1.1 ms (second core off) per frame; this block names where
  that time goes.

Estimates for the console (not measured; scale factor ×5–10 from host to A57 at 1020 MHz,
calibrated on `pica_plan` and `pica_capture_ms`): no lock handoff, 0.2–0.5 ms per step with the second
core on, in `visual_present`, `pica_submit_host_ms` and `total_ms`; shared tables, ~0.3 ms per step
in `pica_capture_ms`/`guest`; the LRU change saves ~0.4 ms per eviction, which shows up only where
`geometry_evictions` is non-zero (room changes, effects at new addresses).

### ui_submit and frame_head split, `config/kernel_ui_lock` (08/11)
From test.10 to test.11, `untimed:` `frame_head` / `ui_submit` grew 0.2–0.75 ms per host frame
(Kokiri 0.48/0.55 → 0.76/0.90). What the logs show (`logs19`, `logs19b`, `logs18a`):
- `ui_submit` grew where the HUD is drawn, also in calm 60 FPS windows: Temple of Time
  0.5 → 0.8–1.1 ms, Hyrule Field 0.5 → 0.6–0.75. Without the HUD (title, room changes) it stays at
  0.04–0.07 in both builds. The second start (HUD pack decoded ahead) shows the same values, so it is
  not the HUD pack's provisional polling. The HUD code itself only lost work in test.11.
- `frame_head` and `input` grew only in game-bound windows (Kokiri at 42 FPS: 0.48 → 0.76 and
  0.27 → 0.44), where `render_idle_ms` also rose (8–12 → 12–14): the render thread waits more, so
  more submissions wake it. Calm windows are unchanged.
- The UI submission reads guest memory with the kernel lock released (`HostWaitRelease`), so each of
  its reads is one lock acquisition. On Horizon an unlock with the second core waiting hands it
  the lock, and the next read waits for that core's whole SVC or DSP frame.

The logs cannot split the two fields further, so `perf.txt` now does (`untimed:` block, ms or
counts per host frame):
- `head_policy` (PrepareNextWindowDemoFrame, a frame-rate change), `head_area` (the pacer's
  presentation submit, the area read, the pack's `NoteScene`), `head_cadence` (cadence report,
  refresh plan), `head_ui` (`ApplyTopScreenConfigSnapshot` .. `UpdateScenePresentation`);
- `ui_ahead` (HUD textures asked for ahead), `ui_build` (the presentation build: guest memory reads),
  `ui_prepare` (`PrepareTextures`: keys, provider resolves), `ui_queue` (the render-stream submit);
- `ui_lock_takes` / `ui_lock_waits` / `ui_lock_wait`: game-thread kernel-lock acquisitions inside
  `ui_submit`, those that found the lock taken, and their wait; `start_lock_*` the same for the
  frame start's released region (host frame start, input). The game thread's acquisitions now
  `try_lock` first and time only a failed one (`SecondCore::gCore0Lock*`);
- `sync:` `render_wakes`: submissions that woke the idle render thread.

**`config/kernel_ui_lock`** (absent = on, `0` = the previous path; desktop
`TRIAEVUM_KERNEL_UI_LOCK`; logged in the `kernel_fast:` start line and as `ui_lock=` in each
window): the UI submission keeps the kernel lock the frame loop holds, and the release starts at the
presentation hand-off. The second core then waits at most the submission instead of the game thread
waiting at every read. Guest-visible behaviour is unchanged: the reads were each atomic before and
now read one consistent state; the second core's SVCs only move later within the frame, which the
release never ordered. Test: `oot3d_native_a32_second_core_tests` (TestUiLockHandOff: 600 reads
against a core that keeps taking the lock for 20 µs units; released, one acquisition per read,
4.3 of them waited, 2.38 ms per submission on the host; kept, no acquisition, 3.3 µs, same values).
The host's 80 % lock duty is a worst case; the console's share is what `ui_lock_wait` shows with
the switch off.

Expected on the console [not measured]: `ui_submit` loses the `ui_lock_wait` it shows with the
switch off. Up to the 0.25–0.5 ms per host frame it grew by where the second core waits for the lock
(Temple of Time, Kokiri); nothing where `ui_lock_waits` is 0.

**A/B (one minute, Kokiri Forest, walking in the grass):** run 30 s as is, then create
`config/kernel_ui_lock` containing `0`, restart, run 30 s on the same path. Compare `ui_submit`,
`ui_lock_wait`, `ui_lock_waits`, `ui_build`, `game_pair_ms` and `fps` (`perf_report.py` prints the
split under "outside the phases"). If `frame_head` stays high, `head_*` names the part and
`render_wakes` shows whether wake-ups follow it; `start_lock_wait` shows whether the frame start's
reads (input) wait for the second core as well.

### Kernel-lock retake: the DSP mix without the lock, `config/kernel_retake` (08/11)
Perf19 item 6: in test.11 the game thread spends 0.8–1.6 ms per step retaking the kernel lock, and
core 1 waits 1.5–7 ms per host frame for it.

**Where the game thread drops and retakes the lock** (second core on):
- `Run()` releases it around each guest dispatch and retakes it at each exit (SVC, block limit,
  fallback). This is `guest_kernel:` `lock=`. In Kokiri (`logs19/perf.txt:1041`), thread 0 spends
  1.10 ms per host frame there (1.58 ms per step) over 13.5 SVCs. Threads 4 and 7 make 19.8 and 9.6
  SVCs but spend only 0.008 and 0.013 ms.
- The frame loop's released regions (`HostWaitRelease`: host frame start and input, presentation
  hand-off) show `kernel_reacquire` 0.0013 ms per host frame, so nothing waits there. The UI
  submission keeps the lock since `kernel_ui_lock`, and a render-stream submit releases it only
  when it waits for a slot.
- Out-of-line guest memory paths inside guest code each take the lock. Their waits fall inside
  `guest`; there was no field for them until `core0_lock_wait_ms` below.

**Why core 1 needs it:**
- the SVC exits of the sound thread (thread 6): 45 per host frame, 0.23 ms;
- its scheduling;
- the DSP frames: 4.8 per host frame, 1.36 ms (`dsp_core1_ms`). Each frame mixes, signals the
  interrupt, updates the statistics and submits to the host audio stream, all with the lock held.
  The submit wakes the audio worker (0x2B, on core 1, above core 1's 0x2D), which then ran while
  the guest-core thread still held the lock.

Core 1 itself waits for the game thread's long holds (GSP IPC: 8.5 calls × 595 µs per host frame).

**The pattern:**
1. Core 1 queues for the lock during a GSP SVC.
2. When the game thread re-enters guest code, Horizon's unlock hands the lock to core 1.
3. Core 1 handles the sound thread's SVC. The sound thread is then parked, so core 1 runs the whole
   DSP frame without releasing the lock.
4. The game thread's next exit waits for the rest of that frame.

Evidence from the 200 test.11 windows: thread 0's `lock` follows `dsp_core1_ms` (correlation 0.96,
median ratio 0.72). Threads 4 and 7 stay at about 0.01 ms, because a DSP frame never starts while
either is ready or running (`AuxiliaryWorkPending`).

**`config/kernel_retake`** (absent = on, `0` = the previous path; desktop `TRIAEVUM_KERNEL_RETAKE`;
logged as `retake=` in the `kernel_fast:` start line) changes the DSP frame on the second core
(`DeliverDspAudioFrameOnSecondCore`):
1. With the lock held, it claims the frame and resolves both shared regions (`BeginFrame`, the only
   step that can fail, still before anything the guest can see). Then it signals the DSP
   interrupt.
2. The mix (`MixFrame`) and the host-audio submit then run with the lock released
   (`NativeA32Process::RunSecondCoreIdleWorkAfterEffect`). The sample reads take the lock
   themselves.
3. With the lock held again, it accounts the regions as written back (`EndFrame`) and updates the
   statistics.

`ProcessFrame` is now these three steps in a row: same bytes, samples, memory traffic and order.

The mix keeps the lock (the previous timing, with the interrupt first) when the interrupt readied
a game-thread thread (`Core0ThreadWakes`), or when it stayed signaled so that one could take it
later (`DspAudioInterruptPending`).

While the mix runs released:
- the second core counts as inside an SVC (`gInKernel`), so a world stop (savestate, page-table
  rebuild) waits for the frame;
- the game thread does not dispatch a thread above the main thread's priority
  (`WaitOutReleasedIdleWork`, then it chooses again), because no such thread ever ran during a DSP
  frame;
- the game thread's SVCs, memory paths and host work go on.

**Why the guest sees the same:**
- **The interrupt's position is invisible.** Before, no kernel operation ran between the mix and
  the interrupt (the lock was held across both), so signaling before the mix changes nothing a
  thread can observe.
- **The woken thread runs after the mix.** The thread the interrupt wakes is the sound thread, on
  this same host thread, so it runs only once the mix is done. A game-thread thread woken by the
  interrupt, or one that could still take it, keeps the held path.
- **SVCs see the state after the whole frame.** The game thread's SVCs during the mix see the
  kernel exactly as after the whole frame (the effect is already applied), which is what they saw
  when they waited for it. Its guest code already ran alongside the mix before.
- **The region pointers stay valid.** They are only replaced under a world stop, which waits.

As with `kernel_ui_lock`, what moves is when the game thread's SVCs run relative to the mix's
memory traffic, which nothing ordered. The DSP regions and sample buffers are touched only by the
sound thread (parked) and the mix [INFERENCE for the game's own SVC targets: no SVC of the game
writes DSP RAM or a playing sound buffer].

**`perf.txt` `second_core:`** gains, per host frame:
- `dsp_released`: frames mixed released;
- `dsp_held_ms`: the rest of `dsp_core1_ms`, the part the game thread can still wait for;
- `core0_gated`: game-thread waits for a released frame before a thread above the main thread's
  priority;
- `core0_lock_waits` / `core0_lock_wait_ms`: every game-thread acquisition of the kernel lock that
  found it taken, and the time waited. This covers the retakes, the guest memory paths and the
  released regions; minus `guest_kernel:` `lock_ms`, it gives the memory paths' share.

**Tests:**
- `oot3d_native_audio_dsp_mix_tests`: every other frame goes through the three steps, with the
  same digests.
- `oot3d_native_a32_ctr_host_tests`: `DspAudioInterruptPending` after clear, signal and a
  manual-reset wait.
- `oot3d_native_a32_second_core_tests` (both `kernel_wake` modes):
  - `TestReleasedIdleWork` checks that the work stays held after a game-thread wake or a pending
    effect, that a world stop waits for the released work, that the main thread runs during it, and
    that a thread above the main thread's priority runs only after it.
  - `TestRetakeSchedule` runs 100 DSP frames of 300 µs against a game thread making an SVC every
    ~400 blocks. The sound thread sees the same frames in the same order, never a half-mixed one
    (same checksum, `dsp_released` 0 and 1.00 per frame).
  - Host (WSL2, RelWithDebInfo), game-thread waits for the lock per frame: 344 → 20 µs with
    `kernel_wake` on, 362 → 23 µs with it off. Retake time (`lock`, uncontended cost included):
    368 → 44 µs and 423 → 83 µs.
  - The suite is also run under TSan.

**Expected on the console [not measured]:**
- Thread 0's `lock` in `guest_kernel:` falls from 0.5–1.4 ms per host frame to the share left by
  core 1's short holds (SVC exits, frame claim and accounting, sample reads): host 20 of 344 µs per
  frame.
- In Kokiri at 42 FPS that is −0.8 to −1.4 ms per step of the 1.58 ms. A little more can come off
  `guest` where the memory paths waited for a frame.
- Core 1's mix may now wait for the lock at its sample reads (`core1_lock_wait_ms`), behind the
  game thread's GSP SVCs. That is time on core 1, which has the slack: a frame every 4.9 ms against
  ~0.3 ms of mix.

**A/B (one minute, Kokiri Forest, the same path):**
1. Run 30 s as is.
2. Create `config/kernel_retake` containing `0`, restart, and run 30 s again.
3. Compare `guest_kernel:` `lock_ms` and thread 0's `lock=`, `second_core:` `dsp_held_ms`,
   `core0_lock_wait_ms`, `dsp_released`, `core1_lock_wait_ms`, then `guest`, `game_pair_ms` and
   `fps`.
4. With it on, `dsp_released` should equal `dsp_core1`. If it is lower, either a game-thread thread
   waits on the DSP interrupt, or the game's interrupt event stays signaled after the wake (manual
   reset). In both cases those frames keep the previous timing; nothing is lost but the gain.

### Plan build cost per draw: `config/plan_*` (07/11)
When Link cuts bushes (`logs18a` `perf.txt` window 27300, 133 draws per step), the render thread
used 27.2 ms per step frame (`rt_ms_step`; 8.3 ms in the calm window just after). The plan fields
explain most of it: `plan_late_draws=10392` in 150 host frames (78 per step) built on the render
thread at `plan_late_ms` 6.41 ms per host frame, `plan_lock_wait_render_ms` 1.81 and
`plan_lock_wait_worker_ms` 7.59, with `plan_resolve_locks` ≈ `plan_late_draws` and the worker idle 55% of
the window (`plan_worker_idle_ms` 19.1). The whole step's draws arrive in one batch with the frame;
the worker and the render thread then walk the same slots in the same order under one build mutex,
taking turns: the builds are serial and each turn adds a mutex handoff. A plan build costs ~90–110 µs
per draw on the console (worker 6.59 ms per host frame, CPU-time from `plan_cpu`).

Host profile (callgrind on a warm cache, `oot3d_native_pica_plan_build_tests --bench` stream): 60% of
a build's instructions are `BuildOot3dPicaCanonicalDrawIdentity`, seven byte-serial FNV hashes over the
register file, 96 float uniforms, the fog table and the lighting registers; 14% is zeroing (`plan = {}`
builds a zeroed 9.5 KB temporary and moves it over the plan); 9% the fragment uniform state (256 float
divisions for the fog table). On the A57 an FNV byte is a dependent 64-bit multiply (~5 cycles).

- **Canonical identity only where it is read** (`Oot3dPicaPlanBuildOptions::CanonicalIdentity`). The
  identity is read by the Vulkan backend, the semantic trace and the scene-frame statistics; deko3d never
  reads it (guard), and a plan restored from a visual savestate already has none. On deko3d without the
  semantic trace or extended diagnostics it is left empty. The shader cache learns exactly what it
  learned before (its structural entries still carry the vertex program id). Kill switch:
  `config/plan_full_identity`.
- **In-place build** (`InPlace`): the plan is destroyed and value-initialised in place (what `plan = {}`
  leaves, one zeroing instead of a temporary and a move), both uniform states are written into the plan
  (`BuildOot3dPica{Vertex,Fragment}UniformStateInto`), and the converted fog table is reused from the
  thread's previous draw when the 128 raw words are equal. Kill switch: `config/plan_copy_build`.
- **Parallel builds** (`oot3d_native_pica_deferred_plan.cpp`). The build mutex now only claims a slot
  (`Building`) and publishes it; the build itself runs outside it, with a second mutex held around each
  shader-cache access (structural, vertex, fragment and uber, fixed binding). A frame resolve builds every
  pending slot itself while the worker builds others, then takes the slots the worker was building,
  waiting for them only at the end (`plan_slot_waits`). Each slot is still built exactly once.
  Kill switch: `config/plan_one_lock` (the previous single mutex and runs of 32).
- Cache order: with parallel builds the shader cache fills in the order builds reach it, not draw order,
  so a key must determine its program. `TestKeysDetermineSources` checks it over random streams
  (vertex, specialized and parametric fragment programs).
- **`perf.txt`**: `plan_identity`, `plan_in_place`, `plan_parallel` (options in use), `plan_worker_draws`
  (worker builds per host frame), `plan_us_draw` (mean build time per draw, worker and late builds),
  `plan_slot_waits` and `plan_slot_wait_ms` (resolve waited for a slot the worker was building). Startup
  line `TRIAEVUM_PLAN_BUILD canonical_identity= in_place= parallel=`. Desktop: `TRIAEVUM_PLAN_BUILD=legacy`
  turns all three off.
- **Exactness**: `oot3d_native_pica_plan_build_tests` builds random draw streams (TEV materials, lighting
  setups, fog changes, program uploads, textures with and without capture hashes or copy producers,
  refused draws) with a frozen copy of the previous builder (`oot3d_native_pica_vulkan_plan_reference.inc`)
  and with each of the 8 option sets, plan objects reused draw after draw, and compares every field of every
  plan, the build result and error, the consumed resources and the cache counters. Parallel resolves with
  the worker and 1–3 resolving threads must give the old plans (also under TSan). The deferred-plan and
  soak tests run the console options too (`parallel` in the soak test: heap bytes per draw 2084 vs 2071).
- Host bench (-O2, warm caches): build 12.3 → 5.4 µs per draw (identity −54%, in-place −5% more); a
  133-draw frame published at once and resolved beside the worker: 4.51 → 0.75 ms (85 draws: 3.02 → 0.46).
  Console estimate [inference, ×7–8 host → A57 from `plan_us_draw` 90–110]: ~50 µs less per plan build on
  core 1 and core 2, and in the bush case 6–7.5 ms less render-thread time per step frame
  (`rt_ms_step` 27 → ~20–21; `plan_late_ms` and the lock waits shrink, `plan_slot_wait_ms` stays small).
- Other per-draw render-thread costs found while tracing (not changed here): the deko3d backend streams
  every draw's vertices and indices again each frame (no geometry cache), packs, compares and copies the
  1.6 + 6.1 KB uniform blocks per draw (3.6 KB of the fragment block is unused by specialized programs),
  re-emits all vertex state, and the scheduler inserts each draw in two `unordered_set`s (two heap
  allocations per draw).

### Capture delta and capture sub-stages: `config/capture_delta` (2026-11-07)
logs18 (test.10): capturing each draw on the game thread (`pica_capture_ms`) costs 19–25 µs per draw,
2.1–3.4 ms per step, and grows with the draw count, which is what rises in Kokiri Forest, Forest Temple
room 8 and bush cutting. The bytes copied into snapshots are small (`capture_copy_kb` ≈ 8 KiB per
frame, `capture_hash_kb` ≈ 0), so the cost is per draw, not per byte of guest data.

**`perf.txt` `capture:` group** (same line, after `offload:`). µs per draw of the queue's part of
`pica_capture_ms`:
- `cap_regs_us`: the register file into the packet, or into the delta log.
- `cap_uniforms_us`: float uniforms and default attributes.
- `cap_packet_us`: the rest of the packet (shared tables, composition).
- `cap_decode_us`, `cap_vertex_us`, `cap_texture_us`, `cap_enqueue_us`: the other stages.
- `cap_hash_us`: content hashing inside the vertex and texture stages.

Also: `cap_state_kb` (KiB of register/uniform/attribute state written on the game thread per frame),
`cap_delta_draws`/`cap_keyframes` per frame, `cap_materialize_us`/`cap_materialized` (the replay off
the game thread, µs per draw and draws per frame), `cap_plan_us` (the placeholder plan, `pica_plan`, per
draw), `list_ms`/`lists`/`list_kwords` (the frontend's whole command-list walk, captures included),
`lut_ms`/`lut_hashes`/`lut_reuses`/`lut_copies` (lighting LUT: 24 KiB hashes and 24 KiB copy-on-write
copies), `table_copies` and `same_value_writes`. On the Switch the stages are timed with the system
counter (`cntpct_el0`, `oot3d_native_capture_clock.h`), seven reads per draw. `perf_report.py`
prints them under `offload`.

**Capture delta** (`config/capture_delta`: no file = on, `0` = off; desktop `TRIAEVUM_CAPTURE_DELTA=1`):
- The frontend marks the 64-byte lines it writes: 48 lines of registers, 24 per float-uniform file,
  4 of default attributes (`Oot3dPicaStateDirtyLines`). The queue builds the packet without those
  three arrays and appends only the dirty lines to a log (`oot3d_native_pica_capture_delta.{h,cpp}`).
  The first draw of every drain pass is a keyframe with all 100 lines. The draw state is decoded from
  the frontend's registers.
- The 6.4 KiB copy per draw leaves the game thread. Whoever consumes the draws first replays the log
  in draw order and writes the arrays: the plan builder under its build lock, before the slot it
  builds (worker on core 1, or the render thread for a late plan); the serial path at the drain; and
  every other take of the queue (`TakePendingDraws`, `TakePendingBatch`).
- Table writes of the value already present are skipped: shader program and swizzle words, fog and
  procedural tables, lighting LUT entries. There is no copy-on-write, and the LUT hash stays valid.
- A lighting LUT whose uploads restored an earlier content takes that table object and its hash.
  The check is an incremental XOR fingerprint per entry plus a byte compare, against the last 8
  hashed tables, instead of hashing 24 KiB byte by byte again.
- Every packet keeps the same bytes and hashes as with the switch off. Only shared-table identities
  (`MutationIdentity`, LUT object) may be older objects with the same content.

**Exactness proof:** `oot3d_native_capture_delta_tests` (in `run.sh`).
- *Streams:* seeded command streams shaped like the game's. They contain:
  - material registers with byte masks and grouped bursts, and vertex-uniform bursts in float24 and
    float32;
  - geometry-shader mirroring toggles and default attributes;
  - lighting LUT uploads that repeat and alternate contents;
  - fog, procedural, program and swizzle uploads;
  - composition spans and failing draws;
  - guest memory writes between lists, through the store cards.
- *Runs:* each stream is replayed through the real frontend, queue and guest memory three times: off,
  on, and on with copied packets.
- *Compared byte for byte:* register file, both uniform files, default attributes, shader words,
  every table, LUT content and hash, decoded state, every snapshot with its hashes and versions. The
  frontend and queue savestates are also compared, and then restored mid-stream.
- *Delivery paths:* the queue's take, log replay in random prefixes, and the deferred plan builder.
  The builder runs with the worker on and off and resolves plans in random order. Its plans are
  compared with serial plans of the reference draws: visual identity, uniforms, shader sources, and
  identical errors.
- *Result:* 4 seeds × 400 lists, about 20,000 draws and 10,000 plans (about 90 % build), all identical.

**Host bench** (`oot3d_native_capture_delta_tests --bench`, Release, Ryzen, 87 draws per frame, steady
room). With the switch off → on:
- capture: 3.11 → 2.10–2.37 µs per draw;
- registers + uniforms: 0.58 → 0.11 µs;
- rest of the packet: 0.32 → 0.14–0.18 µs (fewer zeroed arrays);
- state written: 6.25 → 1.23 KiB per draw;
- the command-list walk: 11.8 → 4.2–4.6 µs per draw, nearly all of it the LUT hash (6.1 → 0.09 µs).
  This assumes the synthetic stream's LUT uploads: one draw in 5 uploads a table, from 3 contents per
  table.
- The replay costs 0.37–0.42 µs per draw on the consumer's thread.

Vertex and texture snapshots (about 0.9 + 0.5 µs) are now the largest stages.

**Estimate for the console** (×5–10 host → A57, not measured): about 3–6 µs per draw of the 20–25 µs
capture, so 0.3–0.5 ms per host frame or 0.6–1.0 ms per step at 90–130 draws. If the game re-uploads
lighting LUTs as the synthetic stream does, that saves another 30+ µs per hashed table, and `lut_ms`
shows whether it does.

**Console A/B (one minute):** create `config/capture_delta` with `0`, then remove it, at the same
place and clock: Kokiri Forest, room 8 with the Deku Babas, cutting bushes. Compare `pica_capture_ms`,
the `cap_*` stages, `list_ms`, `lut_ms`/`lut_hashes`, `game_pair_ms` and `guest`. With it on,
`cap_state_kb` falls about fivefold and `cap_materialize_us` appears; the image must be identical.

### Command-list walk: `config/list_walk_fast` (2026-11-08)
Perf19 (test.11, Kokiri Forest, `logs19/perf.txt:1039`): the GSP command-list walk outside the captures
(`list_ms` − `pica_capture_ms`) costs 1.81 ms per host frame, about 2.6 ms per step: 34.7 K words per
frame, about 450 words and 23 µs per draw, about 52 ns per word. The per-word walk ran every word
through a lambda that stored the register, marked its dirty line, called
`ApplyShaderRegisterSideEffects` (eight range checks and the mirror condition before it finds that
most registers have no side effect), checked the diagnostic history, both draw triggers and the
interrupt request. A float uniform word also went through the stage's offset checks and reloaded
the setup register.

**Fast walk** (`config/list_walk_fast`: no file = on, `0` = off; desktop `TRIAEVUM_LIST_WALK_FAST=1`).
Used when diagnostic history is off (always on the console). The writes keep their order:
- **Dispatch table** (`kRegisterKinds`, 0x300 bytes, built at compile time from the same constants):
  plain, side effect (LUT/fog/procedural data, default attributes, boolean/integer uniforms, program
  and swizzle data), trigger (0x22E/0x22F draws, 0x010 interrupt request), vertex/geometry float
  uniform data. A plain register is a store and a dirty bit, and a trigger or side-effect register
  goes to the same code as before.
- **Consecutive mode:** a plain register repeated by one command keeps the masked bits of the last
  word, (old & ~mask) | (last & mask), in one store. A float uniform burst goes through
  `WriteFloatUniformBurst`: one loop queues the words and decodes each vector. The setup register, the
  mode and the geometry mirror are read once, the queue count stays in a register, and the setup is
  written back at the end with its dirty line. If the queue already holds a whole vector for the
  current mode, the burst takes the per-word path. That only happens when the setup changes from
  float32 to float24 with 3 words queued, where both walks overrun the 4-word queue the same way.
- **Dirty register lines** are collected in a register and handed to `Oot3dPicaStateDirtyLines`
  before each trigger (the sink consumes them at a draw) and on every exit.
- The draw and interrupt code is shared by both walks (one `trigger` lambda). The per-word walk is
  unchanged and checks the three trigger ids inline before calling it.

**`perf.txt` `capture:` group:**
- `list_walk_fast`: whether the fast walk is on.
- `list_walk_ms`: the walk without the sink's draw and interrupt submissions, in ms per host frame.
  The frontend times those submissions (`SinkTicks`, lock included), so this is `list_ms` minus the
  sink. It is a little less than `list_ms` − `pica_capture_ms`, which also holds the queue lock.
- `list_walk_ns_word`: the same, per command word.
- `list_batched_kwords`: the K words per frame taken by a batched loop.

`perf_report.py` prints them under `offload`.

**Exactness proof:** `oot3d_native_pica_list_walk_tests` (in `run.sh`). Two frontends, per-word and
fast, get the same lists. Compared after every list:
- success and error text;
- every sink event: each packet with its register file, both shader states (float uniforms bit for
  bit), every table, LUT content and hash, default attributes, composition and light binding; the
  dirty lines handed along; interrupts after GPU work;
- the whole frontend savestate (registers, shader state, tables, the three packed queues);
- the pending interrupts, the composition statistics and notes, and the table/LUT counters.

What it runs:
- *Every register:* 777 ids (0–0x2FF, and ids past the register file and near 0xFFFF that wrap), each
  with all 16 byte masks, written alone, repeated in consecutive mode, and in a grouped run, each
  followed by a probe draw: 12,432 lists.
- *Streams:* 16 seeds × 400 lists, all 16 combinations of capture delta, in-place packets, pass-span
  fallback and late lists on time.
  - Game-shaped draws: material registers, TEV and light runs, texture units, float32/float24
    uniform bursts, bone matrices, attribute loaders.
  - Random commands over the whole register space, with masks, padding, bursts up to 256 words, draws
    in bursts and interrupt requests that stop the list.
  - Truncated lists, draws the sink refuses, and random composition and pass spans, domains and
    generations.
  - About 13 million words and 38,000 draws in all, identical.
- *Diagnostic history* keeps the per-word walk.

`oot3d_native_capture_delta_tests` now also walks its two capture-delta runs with the fast walk
against the per-word reference, through the real queue, guest memory, delta log and plan builder.

**Host bench** (`oot3d_native_pica_list_walk_tests --bench`, Release, Ryzen, shared machine, 100 draws
per frame in 4 lists). It times the walk without the sink and interleaves runs with the english
`af991bd7` walk (`base`). That walk measures the same as the per-word column, within ±10 % noise:

| Shape | Words/draw | Per-word walk, ns/word | Fast walk, ns/word | Batched words |
|---|---:|---:|---:|---:|
| game (material singles, runs, uniforms, skinning) | 390 | 9.7–16.5 | 3.1–4.8 | 20 % |
| single writes only | 402 | 7.3–11.2 | 2.6–4.2 | 0 % |
| uniform-heavy (60-vector float24 bursts) | 242 | 15.9–24.6 | 5.8–9.4 | 79 % |

Within each run the fast walk takes 2.3–3.9× less time (−56 to −79 %), and the game shape about 3×.

**Estimate for the console** [INFERENCE: the game's real register mix was not recorded, and the
A57/Ryzen ratio is assumed to be the same for both walks]:
- Kokiri's 2.6 ms per step outside the captures falls to about 0.8–1.3 ms;
- that is −1.2 to −1.8 ms per step at about 109 draws per step, and −0.7 to −1.2 ms in the 60 FPS
  places (1.7–2.4 ms there).
- `list_walk_ns_word` should fall from about 50 to about 15–25.

**Console A/B (one minute):** in Kokiri Forest, at the same place and clock, create
`config/list_walk_fast` with `0`, then remove it. Compare `list_walk_ms`, `list_walk_ns_word`,
`list_ms`, `guest`, `game_pair_ms` (mean and p90) and the GSP IPC part of `guest_kernel:`.
`pica_capture_ms` and the `cap_*` stages must not move, and the image must be identical.

### Vertex snapshot hit path: `config/vertex_snapshot_fast` (11/08)
Perf19 found that the index/vertex snapshot stage (`cap_vertex_us`) costs 10–18 µs per draw on core 0,
55–69 % of the capture. That stage copies only 2.5–18 KiB per frame and makes 1–12 new snapshots
per frame, so the time goes into the per-draw hit path. For the index buffer and every vertex loader
the hit path does the following:
- views the memory through three callbacks and two alias walks;
- takes the range's write certificate (`RangeWriteGeneration`): one atomic and-fetch per page, with
  acquire/release ordering on the A57;
- looks the range up in an `unordered_map` and relinks the LRU;
- runs a full `memcmp` whenever the certificate changed.

**`perf.txt` `vsnap:` group** (same line, after `capture:`). Fields per host frame:
- `vs_resources`: geometry resources looked up.
- `vs_version_hits` / `vs_version_misses`: resources proven unchanged by their certificate, and the
  rest (these are the queue's `GeometrySnapshotVersionHits/Misses`, now always counted).
- `vs_uncertified`: resources with no certificate (an escaped write pointer, or an alias that cannot
  be observed).
- `vs_unproven_kb`: KiB of the resources that had to be compared.
- `vs_compared_kb`: KiB actually compared.
- `vs_pages`: pages of certified multi-page ranges.
- `vs_dirty_pages`: pages whose certificate changed on a compared entry.
- `vs_fallbacks`: resources left to the previous path (aliased ranges).

Then µs per draw of `cap_vertex_us`:
- `vs_view_us`: the view and the certificate;
- `vs_compare_us`: page certificates and the comparison;
- `vs_copy_us`: new snapshot bytes;
- `vs_rest_us`: the rest (map lookup, LRU, index range, the snapshot list).

`perf_report.py` prints the group under `offload`. The three steps are timed in both modes, with two
system-counter reads per resource plus two per comparison or copy.

**`config/vertex_snapshot_fast`** (no file = on, `0` = off): the four changes below, all switched
together.
- **Direct view and certificate** (`Oot3dPicaPhysicalMemoryView::ViewCertified`). When a range lies in
  exactly one region, the view calls `NativeA32Memory::SynchronousReadView` and `RangeWriteGeneration`
  directly. It skips the `std::function` callbacks, their `IsMapped` checks and the two alias walks;
  the memory calls and their order are the same. An aliased range keeps the previous path
  (`vs_fallbacks`).
- **Page certificates.** When the certificate of a multi-page range changed, the certificate of each
  page is taken too (`GuestPageGenerations`) and kept with the cache entry. They are taken after the
  range's certificate and before any byte is compared or copied. On the next change, only the pages
  whose own certificate changed are compared with the cached bytes, adjacent ones in one `memcmp`.
  The other pages are proven unchanged by the same argument as a whole-range version hit: a new
  certificate is larger than every earlier one.
- **Front table of the cache.** A direct-mapped table of 1024 slots sits in front of the
  `unordered_map` and holds the node where a key was last found. A hit skips the bucket walk (modulo,
  bucket array, predecessor node and hash recomputation, each likely a cache miss). An eviction
  clears its slot; a restore clears them all.
- **Card peek** (`oot3d_native_a32_memory_peek.h`). `RangeWriteGeneration` reads each page's card
  before taking its bit, and the atomic and-fetch runs only when the bit is set. A clean card is not
  written, and a set bit is still taken by the same and-fetch. The setter is declared outside
  `oot3d_native_a32_memory.h`, so the whole-AOT product hash and the generated code stay unchanged;
  `oot3d_native_a32_memory.cpp` is runtime code with no profile.

The switch gives the same version hits, misses, snapshot bytes, content versions, evictions and
savestates. Only the bytes compared, and the time spent, differ.

**Exactness proof:** `oot3d_native_vertex_snapshot_tests` (in `run.sh`).
- *Runs:* three queues over three copies of the same guest-memory history: off, on, and the switch
  (with the card peek) toggled at random between draws.
- *Memory changes:* content changes, same-value rewrites, and same-value bytes in one page of many
  ranges. Escaped write pointers write raw bytes the cards never see. Deferred device writes and an
  aliased physical range (the fallback) are included too.
- *Checks on every draw:* the same success or error, index range and snapshots (bytes, versions,
  hashes) in all three runs. The bytes must also equal guest memory at the draw; a mirror kept by the
  test provides it.
- *Checks per frame:* the same counters. Savestates must be equal and are restored mid-stream. The
  write observation is rebound, and the cache overflows its 4096 entries.
- *Result:* 4 seeds × 400 frames, about 136,000 draws and 470,000 resources.
- *Mutation check:* ignoring the last dirty page of a range fails the test at the first affected
  draw.

**Host bench** (`oot3d_native_vertex_snapshot_tests --bench 400`, Release, Ryzen, shared machine,
4 runs). The scene has 90 indexed draws, each with an index buffer and 3 loaders of 0.5–32 KiB
(9 pages per draw). Vertex stage in µs per draw, off → on:

| Write pattern between frames | Vertex stage | of which view + certificate | compare | KiB compared per frame |
|---|---|---|---|---|
| none (steady) | 2.19–2.54 → 1.48–1.98 | 1.12–1.25 → 0.58–0.72 | 0 | 0 |
| one same-value byte in every vertex range | 6.24–7.84 → 4.11–5.54 | 1.15–1.44 → 0.66–0.90 | 3.3–4.7 → 2.1–2.8 | 3013 → 932 |
| every byte of every range rewritten (same values) | 6.40–7.83 → 5.90–8.29 | 1.11–1.47 → 0.65–0.95 | 3.3–4.5 → 3.6–5.3 | 3013 → 3013 |

In the worst case, every page of every range rewritten, the page certificates cost about 0.3–0.8 µs
per draw more in the compare. The other savings roughly cancel that. The card peek cannot be
measured on x86: a `lock and` on a cached line is cheap there. On the A57 it removes an exclusive
load/store pair per clean page.

**Estimate for the console** [INFERENCE, not measured]. Perf19's Kokiri window: 17.7 µs per draw,
about 109 draws per step, 1.93 ms per step in this stage.
- If the console is in the steady regime (host 2.2–2.5 µs ↔ console 10–18 µs, a factor of 5–8):
  −25 %, so −3 to −5 µs per draw, or −0.3 to −0.55 ms per step.
- If ranges share pages with data the game writes every frame: −30–35 %, so −0.55 to −0.7 ms per
  step.
- Add the card peek, worth roughly 30–40 cycles per clean page (several pages per resource).
- The kernel lock is held that much less in the GSP SVC, so core 1 waits less (`core1_lock_ms`).

**Console A/B (one minute):** create `config/vertex_snapshot_fast` with `0`, then remove it, at the
same place and clock (Kokiri Forest, near the shop and the bushes). Compare:
- `cap_vertex_us`, `pica_capture_ms`, `list_ms`, `guest`, `game_pair_ms` and `core1_lock_ms`;
- the `vsnap:` fields. With the switch off, `vs_compared_kb` equals `vs_unproven_kb`. With it on,
  `vs_compared_kb` falls by the share of clean pages.

`vs_version_misses`, `vs_uncertified` and `vs_unproven_kb` show which regime the game is in. The image
must be identical.

**Risks:**
- If a range is rewritten whole every frame, the page certificates only add cost.
- `vs_uncertified` shows ranges an escaped write pointer covers; those still compare every byte.

### Texture snapshot and packet stages: `config/texture_snapshot_fast` (11/08)
Perf19 candidate 5. On core 0, inside the GSP SVC, the texture stage (`cap_texture_us`) costs 2.6–3.7 µs
per draw and the packet stage (`cap_packet_us`) 2.1–2.6 µs. In Kokiri Forest (109 draws per step) that is
0.34 and 0.26 ms per step.

For every enabled texture, the texture stage did the following:
- views the memory through the callbacks and two alias walks (`View`, then `SnapshotWriteGeneration`);
- hashes a 13-field key and walks the bucket of an `unordered_map`;
- runs a full `memcmp` of the texture (often tens of KiB) whenever its certificate changed.

The packet stage builds the draw's packet in the queue. With capture delta, the register file, the float
uniforms and the default attributes are already left to the log. What is left still copies the eleven shared
tables, with one atomic reference increment each per draw: two shader programs, two swizzle tables, the fog
table, five procedural texture tables and the lighting LUTs. The slot it writes was last written frames ago,
often on another core (the plan worker frees the drained draws).

**`config/texture_snapshot_fast`** (no file = on, `0` = off). It switches the following together:
- **Direct view and certificate.** A texture that lies in exactly one region goes through
  `Oot3dPicaPhysicalMemoryView::ViewCertified`, the vertex path's direct calls. The memory calls and their
  order are the same. An aliased texture keeps the previous path (`tx_fallbacks`).
- **Front table** of the texture cache: 512 direct-mapped slots, each holding the map node where a key was
  last found. The cache only loses nodes when the whole map is replaced (a payload transform is set, or a
  restore), and both of those clear the table.
- **Page certificates.** When a multi-page texture's certificate changed, the certificate of each page is
  taken. That happens after the texture's own certificate and before any byte is compared or copied, and the
  page certificates are kept with the cache entry. Only the pages whose certificate changed are compared with
  the cached source bytes. This is the vertex path's proof, and the compare is now one function for both
  (`ComparePageRuns`). Observation resets clear the page certificates.
- **Deferred tables.** A capture-delta draw's packet leaves its shared tables empty
  (`Oot3dPicaStateTablesDeferred`; `Oot3dPicaSharedArray` gains a storage-less constructor). The delta log
  takes one reference to each set of tables, and only when one of them is a different storage than at the
  previous recorded draw (`SharesStorageWith`). Its replay sets them together with the arrays.
  - A storage the log holds is shared, so the frontend's copy-on-write never writes it in place. The
    replayed tables are therefore the draw's.
  - Everything that reads the tables (plan builds, shader generation, diagnostics) also needs the register
    file, so it already runs after the replay. The placeholders read only identity, composition and decoded
    state.
  - The comparison and recording are counted in `cap_packet_us`. The increments move to the replay on the
    plan worker (`cap_materialize_us`).
- **Prefetch.** After each draw, the parts of the next packet slot that the constructor and the decoder will
  write are prefetched for writing. The register file, the uniforms and the attributes are not.

The switch gives the same snapshots, hashes, base-level hashes, misses, copied and hashed bytes, materialized
packets and savestates. Only the bytes compared, the references taken on core 0, and the time spent differ.

**`perf.txt` `tsnap:` group** (after `vsnap:`). Fields per host frame:
- `texture_snapshot_fast`;
- `tx_resources`: textures looked up;
- `tx_copy_referenced`: textures with GPU copy references (their CPU gaps are read every draw; unchanged);
- `tx_version_hits` / `tx_version_misses`;
- `tx_uncertified`;
- `tx_unproven_kb`, `tx_compared_kb`;
- `tx_pages`, `tx_dirty_pages`;
- `tx_fallbacks`;
- `tx_misses`: new snapshots.

Then µs per draw of `cap_texture_us`:
- `tx_view_us`: copy-reference check, view, certificate, lookup;
- `tx_compare_us`: page certificates and comparison;
- `tx_copy_us`: new snapshot with its copy, payload transform and hashes;
- `tx_rest_us`: snapshot list, HD-font resolve and the copy-referenced textures.

`perf_report.py` prints the group under `offload`.

**Exactness proof**
- **`oot3d_native_texture_snapshot_tests`** (in `run.sh`)
  - *Runs:* three queues over three copies of one guest-memory history: off, on, and toggled at random
    between draws.
  - *Textures:* every format, 8–256 texels per side, mip chains and type 2. They live in the heap, in VRAM
    and behind a second alias (the fallback). Invalid formats and unmapped textures produce the same errors.
  - *Memory changes:* content changes, same-value rewrites, escaped write pointers and deferred device
    writes.
  - *Mid-stream events:* a payload transform is set and removed, the write observation is rebound, and
    savestates are compared and restored.
  - *Checks on every draw:*
    - the same success or error, packet and snapshots in all three runs;
    - the bytes and both hashes equal guest memory at the draw, through the transform when one is set.
  - *Result:* 4 seeds × 400 frames, about 133,000 draws and 267,000 textures. The fast run compares
    28–57 % of the bytes the reference compares.
- **`oot3d_native_capture_delta_tests`.** The two capture-delta runs now start with the switch on and toggle
  it between lists, so the logs mix deferred and copied tables.
  - Before the replay, a deferred draw must have all eleven tables empty.
  - After every consumer path (queue takes, prefix replays, the plan builder with its worker and concurrent
    resolvers), every table must equal the reference's.
  - About 960 deferred draws are checked per seed.
- **Mutation checks:** treating a texture's last page as clean fails the texture test at the first affected
  draw. Not recording a lighting-LUT change fails the delta test with "lighting LUT".

**Host bench** (`oot3d_native_texture_snapshot_tests --bench`, Release, Ryzen, shared machine).
- *Scene:* 110 draws per frame from the frontend state with capture delta, 1–2 of 120 textures of 4–64 KiB
  each, a 390-word walk between draws, and a consumer thread that replays and frees the drained draws through
  4 recycled vectors.
- *Texture stage, µs per draw, off → on:*

| Write pattern between frames | Texture stage | of which view + lookup | KiB compared per frame |
|---|---|---|---|
| none (steady) | 0.64–0.89 → 0.51–0.61 | 0.43–0.63 → 0.29–0.36 | 0 |
| one same-value byte in every texture | 1.11–1.38 → 0.90–1.11 | 0.49–0.59 → 0.32–0.40 | 799 → 308 |
| every byte of every texture rewritten | 1.05–1.33 → 0.94–1.24 | 0.47–0.61 → 0.30–0.38 | 799 → 799 |

- *Packet constructor alone* (hot, single thread), ns per draw: 158–161 → 86–89. Destruction: 78 → 23.
- *Full packet stage:* 0.30–0.53 → 0.31–0.45 µs. That is within the noise: on the Ryzen the slot's cache
  lines come from the 32 MiB L3 and a `lock xadd` costs about 6 ns.

**Estimate for the console** [INFERENCE, not measured]
- *Texture stage:* if Kokiri is in the steady regime, −25 to −30 %, so −0.8 to −0.95 µs per draw, or
  about −0.1 ms per step.
- *Packet stage:*
  - Each of the eleven increments is an exclusive load/store pair with acquire/release ordering on the A57
    (no LSE). That is 25–40 cycles in L1 and more when the plan worker on core 1 last wrote the line:
    −0.3 to −1.1 µs per draw, or −0.03 to −0.12 ms per step.
  - The prefetch is worth something only if the slot misses the 2 MiB L2. The drained-draw ring is about
    3 MiB, so it probably does.
- *Total:* about −0.1 to −0.25 ms per step on core 0, all inside the GSP SVC, so the kernel lock is held
  that much less.
- *Cost on core 1:* `cap_materialize_us` rises by the moved increments, about 0.1–0.5 µs per draw.

**Console A/B (one minute):** in Kokiri Forest, at the same place and clock, create
`config/texture_snapshot_fast` with `0`, then remove it. Compare:
- `cap_texture_us`, `cap_packet_us`, `pica_capture_ms`, `list_ms`, `guest`, `game_pair_ms` (mean and p90),
  `core1_lock_ms` and `cap_materialize_us`;
- the `tsnap:` fields. With the switch off, `tx_compared_kb` equals `tx_unproven_kb`.

The image must be identical.

**Risks:**
- A texture rewritten whole every frame only pays for its page certificates.
- A table read from an unreplayed capture-delta packet would dereference an empty storage. Every reader
  found also reads the register file, which is already under the replay rule.

### Status as of 2026-10-17 (best version so far)
- PGO (2nd collection) + lean mode + PICA plans on core 1 + render thread on core 2 + audio on its own thread.
- Measured at 30 FPS, 2× scale: 29.9 FPS average, 93% of the time at 30, logic ~11 ms per step.
- Audio without audible glitches (stretch only on real drops; audren buffer 1024). 720p + 60 FPS (`config/fps60`) reaches ~96% GPU.
- Pending: aliasing in generated code and native hot functions (agents in progress, require a new PGO collection);
  other ways to relieve the GPU for 720p/60 (2.5× scale, linear upscaler at 3×, lower anisotropic);
  `config/audio_interpolation` (`sinc`/`cubic`) not yet tested on console; unlock Master Quest.

- Master Quest without beating the game: file `config/unlock_master_quest` (content `0` disables; desktop:
  `TRIAEVUM_UNLOCK_MASTER_QUEST`). The unlock is byte 0 = `0xBE` of `savedata/system.dat`
  (34 bytes, CRC-16/ARC at +0x20); the host returns that byte to the game on read and never writes it to
  disk (`oot3d_native_master_quest_unlock.h`). Saves and system.dat stay intact; without the option everything
  goes back to how it was (the MQ saves, slots 3–5, remain on the card). Not yet tested on console.

Reference measurement (2026-10-16, console, CPU 1785 MHz, render thread on): game logic
costs ~17.5 ms per refresh (~35 ms per game step); the game runs at ~72% speed in heavy
scenes. Goal: ≤ 33 ms per step (real speed at 30 FPS), then 60 FPS via interpolation.

Performance (priority):
1. Validate on console the recompilation with hot/cold split, per-span LDM/STM and folded literals.
2. Native ARM64 VFP (generator option, exact against the ~86 M case oracle) — in progress.
3. PGO with GCC (instrumented build → play → .gcda via FTP → final build) — in preparation.
4. Partial decompilation: lightweight profiler on Switch + hot functions rewritten in native C with
   equivalence testing (overrides) — in progress.
5. "Lean mode": count write generations only on pages relevant to GPU/savestate, remove
   trace/statistics from the hot path.
6. Guest registers in locals: **evaluated and discarded** (2026-10-17). The old `--local-state`
   regressed because it saves/reloads the 15 GPRs on every call and at each of the thousands
   of exits (2× larger code, 318 MB module) and passes lambdas capturing the locals to
   non-inline functions (the locals escape and live on the stack). The new `--promote-registers`
   (exact dirty set per block, shared exit labels, local budget, nothing escapes;
   exact per the differential `run_register_promotion_equivalence.py`) also gets worse on the A57:
   shard 243 with +36% instructions, loads +77% and stores +89% (stack spills 2.4×), because
   with references `state` itself is each register's home and the cold calls of the
   slow memory paths force the locals into callee-saved/stack. It stays off.
7. Mesa glthread (compiled; enable behind config and measure).
8. Fastmem (reservation of the 3DS address space, unchecked access) — big.
9. Long term: deko3d instead of OpenGL/Mesa.

Image (GPU only, no CPU cost; each item behind a file in `config/`):
- Internal resolution 1×/2×/3× — done (`config/resolution_scale`).
- FSR 1 (EASU+RCAS) as upscaler, FXAA/SMAA, anisotropic filtering, sharpening — in progress.
- Interpolation to 60 FPS (`native30_interpolated`) once logic fits in 33 ms.
- Lightweight ambient occlusion only if there's GPU to spare in handheld.

Tools and distribution:
- ccache in builds (installed); use a stronger PC (Ryzen 5 5600G) as the build machine.
- "Forge for Switch": a single script that, from the user's ROM, extracts, adapts, generates the C++,
  compiles with devkitPro and assembles `switch/oot3dre/` (the `.nro` is never distributed).
- Long term: generic 3DS→Switch recompiler combining the function discovery of
  fearkov/3dsrecomp with TriAevum's Switch runtime.

## References and credits

- [coccofresco/TriAevum](https://github.com/coccofresco/TriAevum) — original project (GPL-3.0-or-later);
  [GustavoHenriqueRocha/TriAevum-Switch](https://github.com/GustavoHenriqueRocha/TriAevum-Switch) —
  the Switch port this English fork comes from (branch `deko3d`, base `311d8fd7`). Upstream work
  ported later names its commit in the source (`68d527c6`, `14edc1eb`).
- TopScreen / Single Screen Experience by M-1 — [GameBanana 695893](https://gamebanana.com/mods/695893)
  (CC BY-NC-ND 4.0): layouts and rules reimplemented from its `code.ips`, no code or art copied; the
  player's console downloads it.
- [sa2-nx](https://github.com/ChanseyIsTheBest/sa2-nx) (ChanseyIsTheBest) — code adapted: the 2 MB code
  pages (`sdk/src/ui/switch_code_blocks.cpp`, BSD-3-Clause, from kameo-nx and the Torchlight port's
  findings), the PGO stream dump (`app/src/sa2_pgo.cpp`, GPL-3.0) and the profile summary
  (`tools/pgo_summary.py`, GPL-3.0). Findings only: store cards, the flat dispatch table, the
  fastmem slack, the shard compiler settings.
- [nfsmw-nx](https://github.com/StevensND/nfsmw-nx) (StevensND), [usf4-nx](https://github.com/Thorhax/usf4-nx)
  and [masseffect-nx](https://github.com/nebadasSwifty/masseffect-nx) — findings only: the console PGO
  method, the fastmem window recipe (`guest_memory_switch`), the 460.8 MHz apm configuration, ZCULL
  and thread-placement measurements.
- [zeldaret/oot](https://github.com/zeldaret/oot) (no license) — the N64 landing-roll rule and the
  actor overlay names, behaviour and names only.
- Azahar (GPL-2.0-or-later) — the on-console dump import (`oot3d_rom_import.cpp`) ports TriAevum's
  `ctr_rom.py`/`oot3d_region_assets.py`, which adapt Azahar's NCCH LZSS and RomFS hash; the Switch
  changes to the Azahar audio files are listed in `docs/AZAHAR_AUDIO_PROVENANCE.md`.
- [centollOS/SwitchWaker](https://github.com/centollOS/SwitchWaker) and
  [SwitchWakerHD](https://github.com/centollOS/SwitchWakerHD) — Mesa cache patches,
  thread placement and the Switch performance study that guided several changes.
- [fearkov/3dsrecomp](https://github.com/fearkov/3dsrecomp) — reference for 3DS recompilation techniques.
- [TheMoonPeople/Chronicle PR #205](https://github.com/TheMoonPeople/Chronicle/pull/205) — guard
  pages with `svcSetMemoryPermission` (idea behind `config/guard_alloc`).
- devkitPro/libnx (ISC), deko3d (zlib) and the sys-clk IPC (Beer-Ware): see
  [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- Zelda3drecomp, Azahar, Ship of Harkinian / libultraship, NVIDIA NRI, devkitPro/libnx — via the original project.
