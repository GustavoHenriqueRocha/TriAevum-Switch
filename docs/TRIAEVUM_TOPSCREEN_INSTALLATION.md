# TopScreen textures in the base installation

Implemented in the `release/triaevum-precompiled` worktree. This extends the
default TopScreen behavior with the mod's modified native textures; it does not
replace the title adapter with the mod's executable patch.

## User flow

1. Select the supported decrypted `.3ds` or `.cci` ROM in Forge as before.
2. Forge verifies the precompiled module and the original game data.
3. Forge imports the official TopScreen 2.1.1 texture changes automatically.
   It also prepares the archive's HD Latin font coverage for the ROM's region,
   without a second download, selector or installation step.
4. The launch profile receives the verified local pack and the game is ready.

Only the ROM is requested through the normal UI. First installation needs
internet access unless `topscreen211.zip` is already beside `TriAevumForge.exe`,
under `mods/` beside it, or in `data/downloads/`. Archive size and SHA-256 must
match the recipe; a same-name archive is not automatically trusted. Later
installations reuse the verified local pack without another download.

The existing installation remains active if acquisition/import fails. Forge
reports a retry/offline-archive instruction; it does not silently activate a
texture-incomplete replacement. Saves and valid user configurations are preserved.

## Exact payload and boundary

- Official source: [Single Screen Experience by M-1](https://gamebanana.com/mods/695893).
- Version: 2.1.1, download `https://gamebanana.com/dl/1772020`.
- Archive: 179,430,508 bytes; SHA-256
  `e0c143c872ccf4ad72033caab768753b147a4033d260a701204a63ee6bc16df4`.
- Supported revision: the existing EUR recipe. Other revisions do not inherit
  this import implicitly.
- Inputs: 18 localized CTXB files (`menu_top_parts00` and `menu_cursor00`,
  nine languages), plus `custom_menu00` and `custom_font00` profile textures.
- The existing native CTXB decoder/pack builder deduplicates identical original
  texture identities. The verified real pack occupies 3,211,552 bytes.
- The optional 4K PNG pack is **not** imported by this base path.
- No IPS, ExHeader replacement or injected ARM code is extracted/applied.
- No ROM, mod archive or derived texture pack is included in the public release.
  These are private installation products downloaded/derived on the user's PC.

## Ownership and files

- `tools/triaevum_release/topscreen_assets.py`: bounded HTTPS acquisition,
  pinned integrity checks, atomic import and content-addressed cache.
- `supported_revisions.json`: revision-specific source URL, size and hash.
- `tools/oot3d/decomp_support/scripts/build_topscreen_texture_override_pack.py`:
  reusable `build_texture_pack`, native CTXB/RomFS decoding and O3TUv2 output.
- `precompiled_titles.py`: prepares the pack before transactional activation.
- `forge.py`: portable `--topscreen-texture-overrides` launch argument and
  texture hash/path in the `forge-state.json` runtime receipt.
- `installed_runtime.py`: validates the file, hash and unique launch routing.
- Existing `tools/oot3d/ui_topscreen/oot3d_top_screen_texture_overrides.*` and
  `tools/oot3d/native_game_runtime/oot3d_native_ui_texture_provider.*` remain
  the consumers. No title-specific behavior is added to the generic renderer.

Private outputs live in `data/mods/topscreen/<source-key>/`: `atlas_overrides.o3tu`
and `import.json`. The latter records archive/RomFS identities, importer version,
pack hash and attribution. Download temporaries are removed on failure.

### HD font preparation

The same directory also contains `font_coverage.zip` and `fonts_import.json`.
The latter has an independent font-import version and pack hash. Existing
texture receipts remain valid: reinstalling/preparing with the updated Forge
adds missing fonts without rebuilding valid CTXB packs, reusing the verified
download/local archive. If the archive was removed, it must be acquired again
once to extract the previously unused fonts. Once both packs are valid, neither
the archive nor network access is needed for cache reuse. Missing, modified or
outdated font packs are regenerated; failed imports do not get a success receipt.

This currently prepares `message/eu/ltn16.qbf` or `message/us/ltn16.qbf` from
the archive's `4K Textures` RomFS subtree, preserving the native ROM's layout
metrics. It does not import the whole 4K texture pack or apply executable patches.
Font assets remain private installation data, never bundled release content.

Font import version 2 also stores a native QBF provenance reference in this local
pack. The runtime automatically uses its HD coverage from 480 output pixels high;
below that threshold it uses native coverage. The launch adapter finds the font
pack beside `atlas_overrides.o3tu`, without a second user-selected path. Old
font receipts trigger reimport from the existing verified archive. Missing or
incompatible packs retain native text with a diagnostic requesting a Forge
refresh. See [implementation and verification](TRIAEVUM_ISSUE44_HD_FONTS.md).

2026-09-17 source-path verification on Windows with the real EU RomFS and verified
official archive: textures plus fonts prepared in 3.089 s; warm reuse 0.010 s.
These timings exclude network download and ROM extraction. Focused Forge suite:
91 tests passed. This check did not rebuild the distributed frozen Forge binary.

## Verification

Unit tests cover verified archive reuse, offline reuse of the imported pack,
corruption rejection, bounded/atomic downloads, other-title exclusion, actual
CTXB import without executable extraction, and installed launch/hash validation.

The frozen Forge executable was rebuilt and used against the real ROM in
`I:/TriAevum-0.6.0-candidate-r1`. Reinstallation with the verified pack cached
took **16.574 seconds**, with **zero compiled game objects**. This does not
include a fresh 179 MB network download and is not a modest-PC timing claim.

A second frozen-Forge proof used an empty private installation, the real ROM
and the verified official archive, with no imported pack or generated data.
It completed extraction, texture import and activation in **16.124 seconds**,
again with zero compiled objects. The worker log and generated receipt are in
`I:/oot3dre_work/grass-outline-verification/forge-cold-import/`. This verifies
the importer inside the packaged executable, not only the Python source path.
The release suite passes **153 tests, one skipped**.

A real Vulkan/NRI run loaded the pack and reported actual PICA texture
substitutions (`applied=2`), not only successful parsing. Framebuffer captures
under `I:/oot3dre_work/grass-outline-verification/native-fog-topscreen-02` and
`native-fog-point-guides` show the HUD in Kokiri. Test processes exited normally.

The local candidate's Forge and private install have been updated. A new public
release archive has **not** been promoted or audited by this change; use the
normal allowlist release workflow, never distribute that installed directory.

## TopScreen 3.0 (Switch/developer path)

TopScreen 2.1.1 is no longer downloadable from GameBanana; only "Single Screen
Experience" 3.0 is. The pack builder also accepts that archive, verified exactly:

- Archive: 28,883,512 bytes; SHA-256
  `5668640fec8dc2b47b4516fd0b871c4d7d90ca1c2203acc2a22adb59aaab5993`.
- Layout: `SingleScreen_3.0/<EUR|USA|JPN>/<Emulator|3DS Hardware>/...`.
  Each RomFS contains `menu/custom_ui01.ctxb` (byte-identical everywhere,
  SHA-256 `20290e41...6d07d8`), `actor/zelda_link_ext.zar`, `stage_grid.bin`;
  the Emulator variant adds HD `message/<eu|us>/ltn16.qbf` and
  `message/sys8.qbf`; every variant has `code.ips` and `exheader.bin`.

### What changed relative to 2.1.1

- No localized `menu_top_parts00`/`menu_cursor00` CTXBs: 3.0 leaves the native
  menu textures untouched (its own HUD draws from `custom_ui01`). 2.1.1 had
  painted HUD sprites into texels the original leaves transparent, and the
  native consumers still sample them, so the builder recreates those
  replacements (see below).
- `custom_menu00` + `custom_font00` became one 512x256 ETC1A4 atlas,
  `custom_ui01.ctxb`, which also holds the payload's own glyph font, buttons,
  the "Mod Options" panel, SELECT/START labels and the cycle arrows.
- The 3.0 payload (Emulator `code.ips`, EUR and USA identical) keeps the same
  sprite sizes in its float rect table: SELECT `(277,222,64,16)` at
  0x0060A6F8, the round "?" `(344,222,16,16)` at 0x0060A708 and the cycle mark
  `(320,3,40,40)` at 0x0060A718. The native consumers sample the 2.1.1
  rectangles `(434,410,64,16)`, `(492,104,16,16)` and `(458,2,40,40)`.

### Pack produced from 3.0

No C++ changes. The builder emits O3TU v2 with hash-matched entries for every
distinct original `menu_top_parts00` / `menu_cursor00` payload (USA RomFS: 6 +
2 = 8 entries, the same count 2.1.1 produced). Each entry is the original
RGBA4 payload, bit-exact outside the 2.1.1 rectangles, with the 3.0 sprite
painted in; the builder refuses an original that is not fully transparent in a
rectangle, so native draws never see a changed texel. Nothing is scaled; only
transparent borders are cropped:

| 2.1.1 rect (consumer) | Element | 3.0 `custom_ui01` source |
|---|---|---|
| `menu_top_parts00 (432,218,30,30)` TouchCluster quad 5 | D-pad cross | `(385,108,30,30)`, the 3.0 remap of 2.1.1 `(434,219)` (literal pool 0x005E52D4) |
| `menu_top_parts00 (386,201,48,48)` TouchCluster quads 0..4 | X/Y/I/II action circles | `(98,3,46,48)` of `(97,3,48,48)`, one texel left of centre so the D-pad columns 432..433 stay clear |
| `menu_top_parts00 (440,190,17,11)` TouchLabels quad 0 | `ZR` label (Item I) | `(47,234,16,11)` of the `(47,231,16,16)` cell, at +1,0 |
| `menu_top_parts00 (440,201,17,11)` TouchLabels quad 1 | `ZL` label (Item II) | `(245,230,16,11)` of the `(245,227,16,16)` cell, at +1,0 |
| `menu_top_parts00 (486,230,26,26)` Ocarina navigation (left mirrored) | arrow | `(47,124,26,26)` of `(46,124,28,26)` |
| `menu_cursor00 (72,89,28,36)` tab hint, quarter turn | `R` glyph | `(240,117,36,28)` of `(240,117,37,28)`, rotated |
| `menu_cursor00 (100,89,28,36)` tab hint, quarter turn | `L` glyph | `(462,108,36,28)` of `(462,108,37,28)`, rotated |

Everything else the TopScreen C++ samples from `menu_top_parts00` (view eye,
Navi, camera, hearts, magic meter, X/Y/I/B/II labels, native touch copies) is
present in the original texture. Profile textures:

| Semantic | Content |
|---|---|
| `oot3d/topscreen/2.1.1/menu_atlas` | 512x512 RGBA8; the three 3.0 sprites (plus one texel of their 3.0 neighbourhood) copied to the 2.1.1 rectangles, transparent elsewhere |
| `oot3d/topscreen/2.1.1/font_atlas` | `custom_ui01` verbatim (3.0 merged the custom font into it; the runtime only requires the semantic) |
| `oot3d/topscreen/3.0/ui_atlas` | `custom_ui01` verbatim, for future native 3.0 consumers |

The Switch gate (`topscreen_211_assets=1`) therefore holds. The region is
detected from the original RomFS (`message/us` or `message/eu`) or given with
`--region`. Executable patches, `exheader.bin`, `zelda_link_ext.zar`,
`stage_grid.bin` and the 4K PNG are never read.

```sh
python3 tools/oot3d/decomp_support/scripts/build_topscreen_texture_override_pack.py \
    --archive singlescreen_30.zip --original-romfs-image romfs.bin \
    --output topscreen_3_0.o3tu
# or: OOT3D_TOPSCREEN_30_ARCHIVE=singlescreen_30.zip scripts/switch/stage-product.sh ...
```

`prepare_qbf_font_pack.py` also accepts the verified 3.0 archive and pairs its
Emulator `ltn16.qbf` (64x64 HD, same contract as 2.1.1). Its `sys8.qbf`
(1,575,184 bytes vs 26,896 native) uses an encoding the QBF pairing does not
support and is not imported. Forge recipes still pin 2.1.1 only.

Not yet verified on hardware: the Items hint, the D-pad cross/cycle marks,
action circles, labels, Ocarina arrows and L/R tab hints with 3.0 art. The
pack replaces the old one in place (`sdmc:/switch/oot3dre/config/
atlas_overrides.o3tu`); no rebuild is needed.
