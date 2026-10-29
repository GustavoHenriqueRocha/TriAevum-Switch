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
   e redesenha os menus (`lote_menus.py`, `menus_ptbr.py`, `trocar.py`, `rotulos.py`, `ctxb.py`).
3. Enviar `romfs-ptbr.bin` para `/switch/oot3dre/game/romfs.bin` (com o jogo fechado).

Dependências: `mpyq`, `Pillow`, `etcpak`; fonte M PLUS Rounded 1c (OFL) em `$TRIAEVUM_PTBR/fontes`.

Teste: `python -I test_ptbr.py`.
