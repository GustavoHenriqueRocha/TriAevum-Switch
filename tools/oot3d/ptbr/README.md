# Patch PT-BR do OoT3D (TriAevum)

Troca o espanhol (slots `06_EU_SPANISH`/`07_US_SPANISH` do `message/eu/eu.qm` e as texturas de `menu/06_*`, `menu/07_*`)
por português. No jogo, `game_language.json` → `"selected": "es"`.

Nada do jogo nem da tradução fica no repositório: tudo mora em `~/oot3d-work/ptbr` (`TRIAEVUM_PTBR`), e a romfs
original vem de `TRIAEVUM_ROMFS` (veja `caminhos.py`).

## Fontes do texto
- Mod PT-BR do Ship of Harkinian por **SolidOne e LordeJabiraca** (`.otr` lido com `otr.py`), convertido do formato
  N64 para o do 3DS por `conv.py`. Uso pessoal; não distribuir.
- `traducao.json`: as mensagens que só existem no 3DS (exportadas por `exportar.py`, códigos como `⟦7f..⟧`).

## Passos
1. `exportar.py` → mensagens pendentes + glossário; traduzir em `traducao.json`.
2. `python -I montar.py` → `romfs-ptbr.bin` (o eu.qm novo cabe no espaço do original; a romfs é corrigida no lugar)
   e redesenha os menus (`lote_menus.py`, `menus_ptbr.py`, `trocar.py`, `rotulos.py`, `ctxb.py`) e os títulos de
   área de cada `scene/*.zar` (`titulos.py`, texturas LA8 `name/*_eusp`/`_ussp`).
3. Enviar `romfs-ptbr.bin` para `/switch/oot3dre/game/romfs.bin` (com o jogo fechado).
4. Refazer o pack do TopScreen com a romfs PT-BR junto (o `menu_top_parts00` espanhol muda e, sem isso, o HUD perde
   as molduras/rótulos de X/Y/I/II e a cruz do D-pad — itens soltos na tela de cima):
   `build_topscreen_texture_override_pack.py --archive singlescreen_30.zip --original-romfs-image <romfs original>
   --extra-romfs-image romfs-ptbr.bin --output atlas_overrides.o3tu` → `/switch/oot3dre/config/atlas_overrides.o3tu`.
   O `menu_top_parts00` com os sprites tem outro hash no pack Azahar (`EA217158888A9410` hoje): a textura ampliada dele
   é a do PT-BR com os retângulos do 2.1.1 copiados da versão composta.

Dependências: `mpyq`, `Pillow`, `etcpak`; fontes M PLUS Rounded 1c e Belleza (OFL) em `$TRIAEVUM_PTBR/fontes`.

Teste: `python -I test_ptbr.py`.
