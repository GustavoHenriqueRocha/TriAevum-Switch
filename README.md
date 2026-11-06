> **Fork com port para Nintendo Switch.** Este fork de [coccofresco/TriAevum](https://github.com/coccofresco/TriAevum)
> adiciona correções e otimizações para o homebrew de Switch — veja [SWITCH_PORT.md](SWITCH_PORT.md).
> Para portar outros jogos de 3DS reaproveitando essas técnicas, veja o [guia de port 3DS → Switch](docs/GUIA_PORT_3DS_SWITCH.md).
> Todo o crédito do projeto é do autor original e dos projetos que ele credita abaixo. Nenhuma ROM é distribuída.

# TriAevum — Echoes of Time

A native PC AOT recompilation of *The Legend of Zelda: Ocarina of Time 3D*,
with a modern NRI/Vulkan renderer, single-screen UI and customizable graphics.
*Echoes of Time* is TriAevum's Ocarina of Time 3D project; executables, data
folders and package names keep the TriAevum name.

## Credit Where It Belongs

Before the shaders, settings and excessive grass: **Echoes of Time would not exist in
its current form without [TopScreen](https://gamebanana.com/mods/695893),
developed by rlgcarrot**. Its single-screen experience is reimplemented here for
our runtime, but the vision, care and tremendous work behind it deserve a
special thank-you. Please consider [buying rlgcarrot a coffee](https://ko-fi.com/rlgcarrot)
to support that fantastic work.

Just as essential are **Azahar, Ship of Harkinian, libultraship, NVIDIA NRI**,
and the SDL controller-mapping community. And, of course, **Nintendo and
Grezzo**, who created the original game. [Full credits and licenses](THIRD_PARTY_NOTICES.md).

> **alpha.3 and alpha.3b are BROKEN and withdrawn. Do not use these builds.**
> Downloads have been removed pending further verification. Back up existing
> saves/settings. [Release status and earlier releases](https://github.com/coccofresco/TriAevum/releases).

## About

Echoes of Time is an AOT recomp in the same general spirit as the Xbox 360 recomp
projects. I just ended up spending most of my time on the renderer instead of,
you know, actually playing the game. The goal is to push the visuals further
without completely losing the original look.

**Development of this project is entirely AI-assisted, under human direction.**
That describes the TriAevum-specific work, not the mature donor projects and
libraries it builds on: Azahar, Ship of Harkinian, NRI and others. Aside from
those foundations, you could fairly call this concentrated AI slop.
Apparently, even AI slop can still consume several months of your life.

## Install

Requires Windows or Linux x64, a Vulkan-capable GPU with current drivers, and your own
supported decrypted ROM. Forge accepts `.3ds` and `.cci` and checks the contents,
not just the filename.

It should work with any personal EUR or USA ROM dump, provided it is decrypted
and in `.cci` or `.3ds` format.
See [ROM compatibility details](docs/TRIAEVUM_CONTENT_FAMILY_IMPORT.md).

### Windows

1. Download the **Windows-x64 ZIP**, not GitHub's source-code archive, and
   extract it into a writable folder.
2. Run `TriAevumForge.exe` and select your decrypted ROM.
3. When setup finishes, choose **Launch game**. Afterwards, use `TriAevum.exe`.

### Linux and Steam Deck

The alpha.3/alpha.3b AppImages are withdrawn; the instructions below describe
the intended packaging, not a currently recommended download.

1. Download the **Linux-x86_64.AppImage** and make it executable in your file
   manager, or with `chmod +x TriAevum-*.AppImage`.
2. Open the AppImage and select your decrypted ROM in Forge.
3. Choose **Launch game** when preparation finishes.

On Steam Deck, prepare in Desktop Mode, then add the same AppImage as a non-Steam
game. Use Steam Input's Gamepad layout without Proton. Physical Steam Deck and
Gaming Mode testing have not yet been performed. `--forge` reopens preparation;
`--appimage-extract-and-run` is available on systems without FUSE.

No compiler or SDK is needed. Forge downloads the official TopScreen texture
package during setup; an Internet connection is required unless that package
is [provided locally](docs/TRIAEVUM_TOPSCREEN_INSTALLATION.md). No ROM or original
game assets are included. Forge also prepares the bundled shaders for your GPU
automatically; shaders encountered later are cached while playing.
Back up your saves, settings and prepared data: `data/` beside the Windows
application, or `$XDG_DATA_HOME/TriAevum` (normally `~/.local/share/TriAevum`)
on Linux. Old Flatpak data remains in `~/.var/app/io.github.coccofresco.TriAevum/`
and is not migrated automatically.

## Graphics and controls

- Cel/toon shading and outlines.
- Ambient occlusion, plus optional reflections, shadows and anti-aliasing.
- Real widescreen framing and configurable field of view.
- **60/90 FPS visual interpolation** of the original 30 Hz game state.
  Game logic and gameplay speed remain unchanged; this is not 60/90 Hz physics.
- Azahar-compatible custom textures and texture dumping.
- Reimplemented **TopScreen** single-screen HUD and menus.
- Configurable gameplay free camera, keyboard/mouse and controller inputs.
- Named settings profiles, audio and save states.
- Procedural grass. A lot of grass. Possibly too much grass.

Press **F1** for settings. **F2** temporarily disables the main added graphics
effects, then restores them without losing your configuration. Most of the
presentation is configurable, so if my artistic decisions offend you
personally, you can probably undo them. Effects can be expensive; adjust them
to suit your GPU.

## Converting other 3DS games (experimental, this fork)

`tools/ctr/ctr_convert.py` turns any decrypted 3DS game into native code with the
same runtime, without a per-game function list:

1. extracts the NCCH (`code.bin`, ExHeader, RomFS);
2. discovers the functions in `code.bin` (`discover_functions.py`);
3. generates the whole-AOT C++ (256 shards);
4. builds and runs the boot probe, or the windowed game with `--window`.

Everything that differs between games lives in one title profile
(`ctr_title_profile.h`). OoT3D keeps its own behaviour; any other title gets the
generic profile, with the OoT3D-only PC features switched off.

Requirements: Linux x64, CMake, Ninja, a Vulkan GPU, and Python 3 with
`capstone==5.0.7` (a venv is fine). Bring your own decrypted `.3ds`/`.cci`: no
game data, generated code or binaries are ever published here.

```sh
python3 tools/ctr/ctr_convert.py "/path/to/Game.3ds" --work ~/conv/game --jobs 2 --run
python3 tools/ctr/ctr_convert.py "/path/to/Game.3ds" --work ~/conv/game --window --run
```

- `--run` writes `<work>/build/title/report.json`. Its stderr lists the services
  and SVCs the game asked for that the HLE does not have yet.
- `--window` builds `oot3d_native_game` in the same tree and saves screenshots
  of the presented frame.
- The first build compiles the 256 shards at `-O0`, which takes a while.
- `tools/ctr/build_title_window.sh` rebuilds the window with other flags, for
  example `OPT="-O2 -DNDEBUG"`.

Status:

| title | where | result |
|---|---|---|
| Super Mario 3D Land | Linux | World 1 map and 1-1 playable: full render, sound, savestates. Full speed (30 fps) in 1-1 on an Intel UHD 620. |
| Ocarina of Time 3D | Linux, Switch | Boots the same with discovered functions as with the hand-reviewed list. On the Switch it costs about 4% more CPU per frame. |

In the window, the bottom screen is shown as a picture-in-picture. A mouse
click on it is a touch, and F7 hides or shows it. Other games will probably hit
services that are not implemented yet; the probe tells you which. Building a
Switch NRO is only wired up for OoT3D. Notes, traps and measurements:
[the port guide](docs/GUIA_PORT_3DS_SWITCH.md) (§8, in Portuguese) and `docs/pesquisas/`.

## Testing and contributing

This is an **experimental alpha**, not a fully tested port. Gameplay testing
currently covers the early parts of the game. The USA adapter has been checked
through installation, the title intro and file selection, not a full playthrough.
I find tweaking shaders considerably more entertaining than finishing Zelda
again, so people willing to actually play through it are especially welcome.

Please [report issues](https://github.com/coccofresco/TriAevum/issues) with the
version, ROM input/revision, GPU/driver, settings and steps to reproduce.
**Normal in-game save files are particularly useful for problems later in the
game.** Review logs for personal information before sharing; do not upload
ROMs, extracted assets or memory savestates containing game code.

Contributions, experiments and forks taking the renderer in other directions
are welcome. [Community contributors and PR credits](docs/TRIAEVUM_CONTRIBUTIONS.md)
are recorded separately from the original AI-assisted development. Longer term,
I would like to explore a proper ray-tracing path
and finish features still in the traditional state known as "technically
implemented". Ray tracing is a wishlist item, not a feature of this release.

[Release notes and known limitations](docs/releases/v0.6.0-alpha.2b.md).

## Screenshots

Captured directly from the alpha.2 framebuffer on Windows and Linux with the
default profile: toon shading, outlines, grass, 1.10x FOV, x2 interpolation and
TopScreen. No additional HD texture pack.

![Windows: Kokiri Forest outside Link's house, with the TopScreen HUD](docs/images/alpha2-kokiri.png)

![Windows: Link and Epona in the title intro](docs/images/alpha2-title.png)

![Linux Flatpak: mounted gameplay with the TopScreen HUD](docs/images/alpha2-mounted-linux.png)

## Source and license

Original contributions are licensed under **GPL-3.0-or-later**. Third-party
components retain their own licenses and notices:
[license scope](LICENSE_SCOPE.md) and [third-party notices](THIRD_PARTY_NOTICES.md).
See the [release and build contract](docs/TRIAEVUM_PRECOMPILED_RELEASE.md) for
source availability and developer packaging, and the [source map](docs/TRIAEVUM_SOURCE_MAP.md)
for where the main components live.

TriAevum is independent and is not affiliated with or endorsed by Nintendo.

Worst case scenario, at least we got more grass.
