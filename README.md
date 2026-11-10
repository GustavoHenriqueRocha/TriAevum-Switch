> **Nintendo Switch port, in English.** This fork of
> [GustavoHenriqueRocha/TriAevum-Switch](https://github.com/GustavoHenriqueRocha/TriAevum-Switch), the Switch port of
> [coccofresco/TriAevum](https://github.com/coccofresco/TriAevum), translates the port into English and adds gyro aiming,
> touchscreen controls, a performance overlay, in-game updates and a first-launch import of your own cartridge dump.
> All credit for the recompilation, runtime and renderer goes to those projects and the ones credited below.
> No ROM or game data is distributed. Port notes: [SWITCH_PORT.md](SWITCH_PORT.md); 3DS → Switch porting
> guide: [docs/PORTING_GUIDE_3DS_SWITCH.md](docs/PORTING_GUIDE_3DS_SWITCH.md).

## Nintendo Switch

**You need** a Switch running Atmosphère and a decrypted dump of your own *Ocarina of Time 3D* cartridge
(`.3ds` or `.cci`): USA Rev 1 (En, Fr, Es) or European (original or Rev 1). GodMode9 on a modded 3DS can
dump and decrypt it. eShop copies (`.cia`) and other versions are not supported.

1. Download `oot3dre-<version>-switch.zip` from [Releases](https://github.com/JohnUzoka/TriAevum-Switch/releases)
   and extract it to the root of the SD card. It creates `switch/oot3dre/`.
2. Copy your dump into `switch/oot3dre/`.
3. Start the game with a game's memory: hold R while starting any game, then open it from the homebrew menu.
   The first start turns your dump into the game files (about a minute), then asks whether to keep (A) or
   delete (Y) the dump. A forwarder on the HOME menu made with [Sphaira](https://github.com/NaGaa95/sphaira)
   works too; forwarders from the nsp-forwarder website have been reported to close the game, so if you try
   one, please send `switch/oot3dre/boot-status.txt` and the `logs` folder.

- **Menu:** press both sticks in (L3+R3) for settings, the performance overlay, updates and Exit game.
  Touch works in the menu too.
- **Controls:** gyro aiming with the console, a Pro Controller or a pair of Joy-Cons; tap the on-screen
  item, X/Y and D-pad icons; drag a finger on empty screen to aim. L3+R3 → Controls → Camera → Free
  camera → Smoothing Off makes the free camera stop as soon as you let go of the stick.
- **Updates:** L3+R3 → Updates → Check for updates. Saves, settings and game files are kept. The game
  also checks when it starts and shows a notice for a few seconds when a newer version is out; L3+R3 →
  Updates → Check at launch turns that off.
- **Performance:** at stock clocks lighter areas run at full speed and busy ones slow down (the CPU is
  the limit). L3+R3 → Video → **Performance mode** On raises the CPU to 1224 MHz, the GPU to 460.8 MHz
  and the memory to 1600 MHz (each also on its own row; off by default, more battery use); the clocks
  go back to normal when you turn it off, press HOME, sleep or exit. sys-clk still works and takes
  precedence.
- **Defaults:** 60 fps interpolation (delete `switch/oot3dre/config/fps60` for 30 fps) and Master Quest
  unlocked from the start (delete `switch/oot3dre/config/unlock_master_quest`).
- **Gameplay (L3+R3 → Game → Gameplay):** *Landing roll (N64)* (on by default) rolls after a run jump off a
  ledge as on N64; *Roll dash* (off by default) is Single Screen Experience's Roll Dash: keep A held after
  a roll and Link dashes. Its dash animations come with the HUD art download below; a game set up with an
  earlier version is offered the download again at the next start once Roll dash is on.
- **HUD art, downloaded on the console:** the single-screen HUD's art comes from the TopScreen mod
  ([Single Screen Experience](https://gamebanana.com/mods/695893) by M-1, CC BY-NC-ND 4.0), whose license
  lets you download and install it but not share it. The first start offers to download it from
  GameBanana (56 MB) and builds the HUD textures, the HD HUD art and the HD text font from your game
  files: A downloads, B asks again next time, X never asks (L3+R3 → Updates turns the offer back on).
  Without internet, download `singlescreen_322.zip` from that page on a PC and copy it into
  `switch/oot3dre/`. Without the mod the game still runs, with the game's own HUD art.

### HD textures (optional)

HenrikoMagnifico's texture pack replaces the world, character and HUD textures with sharper ones.

1. Download the **1080p** version of the
   [Ocarina of Time 3D 4K texture pack](https://www.henrikomagnifico.com/zelda-ocarina-of-time-3d-4k)
   (`OOT3D 4K 5.0a (1080p).zip`, 1.9 GB). If Google Drive says the download quota is exceeded, use the
   MediaFire mirror on the same page.
2. Make a copy at the sizes the Switch uses, on a PC: install [Python 3](https://www.python.org/downloads/)
   and Pillow (`pip install pillow`), download
   [`shrink_texture_pack.py`](tools/switch/shrink_texture_pack.py) into the same folder as the zip, and run

   ```
   python shrink_texture_pack.py "OOT3D 4K 5.0a (1080p).zip" textures
   ```

   It writes a `textures` folder of about 350 MB instead of 1.9 GB (world 2x the original, HUD 3x), so
   the Switch also loads it faster. Add `--parts hud,models` for the HUD and characters only (65 MB).
   `--models 3` keeps characters, creatures and Link at 3x (38 MB instead of 21 MB); then set
   "HD texture size" to 3x in the game, or it draws them at 2x.
   Skipping this step works too: copy the contents of the zip's `user/load/textures/0004000000033500/`
   folder instead, and the game reduces each texture as it loads it.
3. Copy the contents of `textures` (the folders and `pack.json`) to `switch/oot3dre/textures/` on the SD
   card, whatever your dump's region.
4. In the game, L3+R3 → Video → **HD textures**. It applies at once and turns off the same way.
   "HD texture size" (default 2x) is the most the world textures are drawn at. Once the game has
   checked your pack (a few seconds after HD textures come on, the first time), the menu offers only
   sizes your pack has: 1x and 2x for the copy from step 2, up to 3x with `--models 3`, all of them for
   the 1080p pack.

Higher resolution costs some GPU time and memory (about 10–50 MB of textures per area at 2x): if a busy
area slows down, compare the same spot with HD textures off, using the performance overlay.

### Mods: Link models and other game files (optional)

Mods made for the 3DS that replace the game's files (Luma's `romfs` folder) work on the Switch
too. Get them yourself from the mod's page; the port includes and downloads none.

1. Make a folder per mod in `switch/oot3dre/mods/` and put the mod's `romfs` folder in it, e.g.
   [Vanilla+ Link remodel](https://gamebanana.com/mods/705890) →
   `switch/oot3dre/mods/vanilla_plus_link/romfs/actor/zelda_link_boy_new.zar` (its `actor` and `misc`
   folders). For Better Link v3, use its romfs-only variant (without `code.ips`), the same way.
2. Start the game. The first start with a new or changed mod takes a moment ("Applying the
   mods..."); `switch/oot3dre/logs/oot3d_native_game.log` lists what was applied, e.g.
   `romfs mods: 26 files from 1 mod (vanilla_plus_link), 0 ignored`.

- Mods apply in folder name order; when two replace the same file, the later folder wins.
- **Turn them off:** L3+R3 → Game → Gameplay → **Mods (RomFS)**. It applies at the next start.
- Texture-only mods (e.g. Art-Accurate Link, PNG files for Citra/Azahar) are texture packs: copy
  them into `switch/oot3dre/textures/` instead (see HD textures above). With the HD pack installed
  too, the first file in name order wins, so the pack's `LINK` folder beats a mod's loose files:
  put the mod in a folder whose name starts with `0`, e.g. `textures/0-art-accurate-link/`.
- **N64 models** (ModLoader64 `.pak`/`.zobj`, e.g. Skilar's TP Link): convert them with
  [Ocarina ReTuned](https://github.com/OTPR26/Ocarina-ReTuned/blob/main/documentation/Player-models.md)
  on a PC (Settings → Mods → PAK Importer, after preparing your dump there). It writes
  `player-models/<id>/converted.zar` in its data folder (`%LOCALAPPDATA%\Ocarina ReTuned` on Windows,
  `~/Library/Application Support/Ocarina ReTuned` on macOS, `~/.local/share/Ocarina ReTuned` on
  Linux; the most recently modified `<id>` folder is your last import). Copy it as
  `switch/oot3dre/mods/<name>/romfs/actor/zelda_link_boy_new.zar` for adult Link or
  `zelda_link_child_new.zar` for child Link.
- **What does not work:** code patches (`code.ips`, `exefs/`: the game's code is compiled ahead of
  time) and mods that add new files the game does not have. They are skipped and the log says why.
- **Save states** made with other mods (or with none) do not load; in-game saves are not affected.
- A mod's new materials may draw a little slower at first: the console cannot compile shaders, so
  missing ones use a slower general shader (they are saved in `logs/deko3d_shaders/` for the next
  shader pack).

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
