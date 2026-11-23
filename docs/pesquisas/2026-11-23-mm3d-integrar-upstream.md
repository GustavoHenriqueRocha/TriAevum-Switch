# MM3D: trazer o port do upstream para a linha Switch/deko3d

Data: 2026-11-23 · Título(s): MM3D (EUR Rev 1), genérico · Commits: deko3d `1c22eabd`, origin/main `672d13f1`, base comum `23eac6b0`

## Pergunta

O `ctr_convert.py` do MM3D cai no generate com `instruction 0xFA06D22F at 0x0014EBC0 is unsupported`
(BLX imediato ARM→Thumb; o gerador do deko3d não tem Thumb). O upstream (coccofresco/TriAevum,
origin/main) já portou o MM3D para a mesma ROM e tem Thumb no gerador compartilhado. Qual é o
caminho mais barato para ter o MM3D na nossa linha Switch/deko3d: (A) trazer para o deko3d só os
commits neutros do gerador/runtime e os dados de `titles/mm3d`, ou (B) partir do origin/main e
levar a nossa camada Switch/deko3d para lá? E o que o produto Switch precisa que o upstream não tem?

## Fontes

- Mesma ROM: `sha256sum ~/oot3d-work/conv/mm3d/extract/{code,exheader}.bin` = `6eec27b1…` e
  `d14ad800…`, iguais a `origin/main:titles/mm3d/a32/whole_aot_product_manifest.json` (`source_image`).
- Divergência: `git merge-base HEAD origin/main` = `23eac6b0`; `git rev-list --count` dá 350 commits
  nossos (342 sem merges) e 896 deles.
- Conflito do merge inteiro: `git merge-tree --write-tree --name-only HEAD origin/main`; blocos e
  linhas contados pelos marcadores `<<<<<<<`/`>>>>>>>` na árvore resultante (`git cat-file -p <árvore>:<arquivo>`).
- Rota A medida num worktree temporário (`git worktree add ~/oot3d-work/conv/integra-mm3d -b analise/mm3d-integra deko3d`)
  com cherry-pick em sequência: em cada commit, `--no-commit`, conta os conflitos e segue com
  `-X theirs` **só para medir o próximo** (não é resolução de verdade; script no scratchpad da sessão, `picar.sh`).
- Commits do upstream: `85503360` (G3 Thumb decoding and lowering), `757f90ae` (G3 zero residual incl. Thumb),
  `716b0ba6`, `1dfe3c47`, `5d8b549a` (BLX #imm como chamada a fronteira Thumb), `3e27183b` (execução Thumb
  no runtime A32), `e2af9560` (serviços que o boot do MM3D usa), `c7479893` (PICA segue jumps do command buffer),
  `419bff1a`; `docs/MOONFALL_CURSE.md` e `titles/mm3d/a32/` no origin/main.
- Medição própria do Thumb (antes da mudança de plano; script no scratchpad da sessão, `medir_thumb.py`, sobre o
  `aot_program.json` do nosso discover): 1680 BLX imediatos no `.text`, 1536 em código ARM descoberto, 24 alvos;
  fechando o CFG Thumb (BL entre funções Thumb) dá 37 funções e 892 instruções, 862 T16 + 30 T32 (só BL/BLX),
  nenhuma IT/CBZ/Thumb-2 (ARMv6K), 4 chamadas Thumb→ARM. Rotinas: `0x303484` (1223 chamadas, guarda tipo
  `__cxa_guard_acquire`), `0x1007B0` (113, laço de construtor de vetor tipo `__aeabi_vec_ctor_nocookie_nodtor`),
  strcat, wcscpy/wcslen de 16 bits, wrappers de printf variádico. O nosso discover decodifica 20 dessas
  entradas Thumb como ARM (117 PCs) porque o `_walk_cfg` enfileira alvo de BLX alinhado a 4 como ARM.

## Conclusão

**Rota A tem bem menos conflito real.** Números:

| | Rota A (gerador + boot do MM3D no deko3d) | Rota B (deko3d em cima do origin/main) |
|---|---|---|
| Commits a trazer | 11 do gerador + 3 de serviços/PICA (+ dados `titles/mm3d/a32`, 21 arquivos) | 342 nossos (134 só Switch, 208 em arquivos compartilhados) |
| Arquivos em conflito | 20 conflitos em 14 commits; 6 dos 11 do gerador limpos | 104 (92 de conteúdo, 7 de localização, 5 add/add) |
| Blocos / linhas em conflito | gerador: 18 blocos, ~856 linhas; serviços/PICA: 22 blocos, ~2330 linhas (1813 num só arquivo de teste) | 265 blocos, ~10 400 linhas |

Rota A, commit a commit (arquivos/blocos/linhas em conflito): `b77f12a7` limpo, `3e27183b` limpo,
`3cd558e7` 1/2/185 (`oot3d_native_a32_window_setup.cpp`), `b1878f7a` limpo, `5d8b549a` limpo,
`af9860df` 2/11/633 (`whole_aot_cpp.py`, `test_whole_aot_media_execution.py`), `407a4b43` 8/3/17
(quase tudo localização: `tools/mm3d` → `titles/mm3d`, `tools/ctr_title` que não temos), `85503360`
1/2/21 (`whole_aot_program.py`), `757f90ae` 7/0/0 (localização), `0ccc7184` limpo, `716b0ba6` 1/0/0;
`e2af9560` 7/11/264 (`ctr_host*`), `c7479893` 5/8/253 (`pica_frontend`, `window_setup`), `419bff1a` 3/3/1813.

O conflito do gerador é **sobreposição real**, não ruído de texto:
- `af9860df` (deles) implementa as mesmas instruções de mídia ARMv6 (SIMD 8/16, SSAT16/USAT16,
  xTAB16, SMLAD/SMUAD) que o nosso trabalho do SM3DL. A resolução certa é ficar com a nossa
  (testada pelo nosso `test_whole_aot_media_execution.py`) e descartar a deles.
- `whole_aot_program.py`: o nosso `extra_roots`/`seed_roots` (descoberta sem inventário do
  `ctr_convert`) contra o Thumb deles.
- Prova: com a resolução mecânica `-X theirs`, no worktree temporário falham 1 de 11 testes de
  `test_whole_aot_thumb`, 20 de 24 de `test_whole_aot_program` (`UnboundLocalError: extra_roots`) e 3 de
  50 de `test_whole_aot_cpp`. No deko3d puro os dois últimos passam. Resolver à mão é obrigatório e
  pequeno (4 arquivos).

Rota B é cara pelo tamanho e pela arquitetura: no origin/main o produto é multi-título (`triaevum_title_adapter.h`
por hash do code.bin, sessão de título, módulo/plugin de título no Linux, Forge, Android), com renderer NRI e
um grafo de extensões de PICA (439 arquivos em `fast/extension`, nenhum no deko3d). As 104 colisões caem nas
áreas que a camada Switch mexeu: serviços CTR (8 arquivos, 30 blocos, ~4400 linhas), janela/PICA (22, 103,
~2500), TopScreen/UI (11, 28, ~600), renderer do `three_ds_recomp` (24, 24, ~330), CMake (3, 7, ~290).

**O que o produto Switch do MM3D precisa e o upstream não tem:**
1. Backend deko3d (16 arquivos `gfx_deko3d*` nossos; o upstream só tem GL, Vulkan, D3D11, Metal) e o pack de
   shaders DKSH do MM3D (o nosso `deko3d.pack` é do OoT3D).
2. Runtime Switch: threads/núcleos do Horizon, áudio 48 kHz em thread própria, ritmo/fps60, heap, caminhos
   `sdmc:/switch/<jogo>/`, build devkitA64/GCC com PGO e ligação estática (o upstream liga o C++ gerado como
   plugin do título no desktop).
3. Perfil de título do MM3D no nosso `ctr_title_profile.h` (program id `0004000000125600`); o upstream decide o
   título pelo hash em `triaevum_title_adapter.h`, outra camada.
4. Os extras do adaptador `titles/mm3d/adapter` (55 arquivos: single-screen do dekui, Project Restoration,
   iluminação, câmera de foto) dependem de 23 cabeçalhos que o deko3d não tem (9 do grafo de extensões do
   renderer, 7 da camada de título, 3 de `titles/oot3d/environment`...). Para jogar no Switch eles não são
   pré-requisito: o mínimo é o gerador com Thumb, os dados de `titles/mm3d/a32` e os serviços de boot.

## Decisão

Recomendação: **rota A**, em duas levas:
1. Gerador: os 11 commits (`b77f12a7 3e27183b 3cd558e7 b1878f7a 5d8b549a af9860df 407a4b43 85503360 757f90ae
   0ccc7184 716b0ba6`), resolvendo à mão `whole_aot_cpp.py`, `whole_aot_program.py`, `test_whole_aot_media_execution.py`
   (ficar com o nosso de mídia) e o `window_setup.cpp` do `3cd558e7`; os caminhos `tools/mm3d` vão direto para
   `titles/mm3d`. Critério: os testes do gerador passam e o C++ gerado do OoT3D sai byte a byte igual.
2. Boot: `e2af9560 c7479893 419bff1a` (+ `1dfe3c47`), resolvendo contra os nossos serviços genéricos de título (MK7/SM3DL).
Depois, a parte Switch específica do MM3D (perfil, pack deko3d, caminho no SD) é trabalho nosso.

## Alternativas rejeitadas

- **Rota B (rebase/merge do deko3d no origin/main):** 104 arquivos e ~10 400 linhas em conflito, mais portar o
  grafo de extensões e a camada de título para o deko3d antes de qualquer ganho no MM3D.
- **Implementar Thumb do zero no nosso gerador** (o que comecei): duplicaria o trabalho já pronto e testado no
  upstream (`whole_aot_thumb.py`, 534 linhas, mais `a32_thumb.cpp` no runtime). Parei antes de alterar qualquer
  arquivo do repositório.
- **Substituir as rotinas Thumb por nativas escritas à mão:** específico por jogo e sem tradução para onde
  recusar (o contrato das substituições exige a tradução intacta).

## Em aberto

- O upstream conta 223 funções Thumb (~21 KB, `thumb_ranges.csv` com 400 linhas); a minha medida a partir dos
  BLX chega a 37. As outras devem vir do inventário revisado (Ghidra, ponteiros); não conferi.
- A seleção do upstream tem 18 751 funções; o nosso discover sem inventário dá 20 764 (com as entradas Thumb
  lidas como ARM). Falta decidir se o MM3D no deko3d usa o inventário revisado do upstream ou o nosso discover.
- Conflitos semânticos fora do texto (ex.: comportamento dos serviços CTR para MK7/SM3DL depois do `e2af9560`)
  só aparecem rodando os testes de host e o boot probe; nada foi compilado nesta análise.
- O `-X theirs` serviu só para medir os commits seguintes; os números da rota A depois do primeiro conflito
  podem mudar um pouco com a resolução de verdade.
