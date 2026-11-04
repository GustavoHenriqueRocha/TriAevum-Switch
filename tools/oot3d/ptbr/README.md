Versão em português: [README.pt-BR.md](README.pt-BR.md)

# OoT3D PT-BR patch (TriAevum)

Replaces Spanish (slots `06_EU_SPANISH`/`07_US_SPANISH` of `message/eu/eu.qm` and the textures in `menu/06_*`, `menu/07_*`)
with Portuguese. In the game, `game_language.json` → `"selected": "es"`.

Nothing from the game or the translation lives in the repository: everything lives in `~/oot3d-work/ptbr` (`TRIAEVUM_PTBR`), and the
original romfs comes from `TRIAEVUM_ROMFS` (see `paths.py`).

## Text sources
- Ship of Harkinian PT-BR mod by **SolidOne and LordeJabiraca** (`.otr` read with `otr.py`), converted from the N64
  format to the 3DS one by `conv.py`. Personal use; do not distribute.
- `traducao.json`: the messages that only exist on 3DS (exported by `export.py`, codes such as `⟦7f..⟧`).

## Steps
1. `export.py` → pending messages + glossary; translate into `traducao.json`.
2. `python -I assemble.py` → `romfs-ptbr.bin` (the new eu.qm fits in the original's space; the romfs is patched in place)
   and redraws the menus (`menu_batch.py`, `menus_ptbr.py`, `swap.py`, `labels.py`, `ctxb.py`) and the area
   titles of each `scene/*.zar` (`titles.py`, LA8 textures `name/*_eusp`/`_ussp`).
3. Copy `romfs-ptbr.bin` to `/switch/oot3dre/game/romfs.bin` (with the game closed).
4. Rebuild the TopScreen pack with the PT-BR romfs included (the Spanish `menu_top_parts00` changes and, without this, the HUD loses
   the X/Y/I/II frames/labels and the D-pad cross — loose items on the top screen):
   `build_topscreen_texture_override_pack.py --archive singlescreen_30.zip --original-romfs-image <original romfs>
   --extra-romfs-image romfs-ptbr.bin --output atlas_overrides.o3tu` → `/switch/oot3dre/config/atlas_overrides.o3tu`.
   The `menu_top_parts00` with the sprites has a different hash in the Azahar pack (`EA217158888A9410` today): its upscaled texture
   is the PT-BR one with the 2.1.1 rectangles copied from the composite version.

Dependencies: `mpyq`, `Pillow`, `etcpak`; fonts M PLUS Rounded 1c and Belleza (OFL) in `$TRIAEVUM_PTBR/fontes`.

Test: `python -I test_ptbr.py`.
