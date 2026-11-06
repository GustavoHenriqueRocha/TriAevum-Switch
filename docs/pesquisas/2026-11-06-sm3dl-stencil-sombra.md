# SM3DL sem chão e sem céu: depth do Vulkan sem stencil

Data: 2026-11-06 · Título(s): SM3DL (0004000000053F00), genérico (renderer Vulkan do PC) ·
Continuação de `2026-11-05-sm3dl-opendirectory.md`.

## Pergunta

Depois do OpenDirectory, o SM3DL chegou ao mapa do Mundo 1 e à W1-1 jogável (Mario corre, o
cronômetro desce, aparecem Goomba e moedas), mas o fundo era um cinza único (141,141,153) e não
havia chão nem céu. Por quê?

Repro: `janela-progresso.sh` (scratchpad da sessão) = `tools/ctr/build_title_window.sh --work
~/oot3d-work/conv/sm3dl --run --seconds 560 --shots <pasta> -- --input-timeline
~/oot3d-work/conv/sm3dl/mapa_e_fase.json` (A na abertura, direita no mapa, A para entrar, correr e
pular). Trace: `--pica-semantic-trace <arq>` com `OOT3D_PICA_TRACE_FIRST_FRAME=3120
OOT3D_PICA_TRACE_LAST_FRAME=3121`.

## Fontes

- Trace semântico do quadro 3120 (295 draws por quadro, todos `fully_supported`).
- Captura `quadro_003120.png`: os pixels do fundo são todos (141,141,153).
- `runtime/three_ds_recomp/src/fast/oot3d/pica_nri_pipeline_state.cpp` (`FindPicaDepthFormat`) e o
  log `TRIAEVUM_VULKAN_DEPTH_FORMAT format=126` (`VK_FORMAT_D32_SFLOAT`).
- Backend deko3d: `DkImageFormat_Z24S8` (único formato de depth usado).

## Conclusão

1. **O cinza não é a cor de limpeza (confirmado).** O memory fill da cor é `0x0000FFFF` (azul em
   RGBA8), e nenhum registrador de TEV ou de fog tem 0x8D8D99.
2. **O SM3DL faz sombra por volumes de stencil (confirmado no trace).** A ordem do quadro é: terreno
   e céu (draws 839714–839767); volumes de sombra com stencil (cor com blend Zero/One, sem efeito);
   três quads de tela cheia com stencil test (839788 escurece com 1−const, **839789 escreve cor
   com blend One/Zero**, 839790 com depth Never limpa o stencil); depois Mario, objetos e HUD.
3. **O depth do Vulkan não tinha stencil (confirmado).** `FindPicaDepthFormat` preferia
   `D32_SFLOAT`; sem aspecto de stencil o teste sempre passa, e o quad 839789 pintava a tela inteira,
   cobrindo o que veio antes (chão e céu). Tudo o que é desenhado depois aparecia.
4. **Com `D32_SFLOAT_S8_UINT` (formato 130 no log), a rodada de 560 s mostra o render completo
   (confirmado):** céu, castelo e árvore na abertura, mapa xadrez do Mundo 1, W1-1 com terreno,
   blocos, caixas e riacho (`progresso/2026-11-05_2357_5e78939f_stencil-fase/folha.png`).

O Switch não tinha o problema: o deko3d usa Z24S8.

## Decisão

- `ChoosePicaDepthFormat` (função pura em `pica_vulkan_device_profile.h`) escolhe
  D32S8 → D24S8 → D32. O D32S8 mantém a precisão de depth float que o renderer usava; o D32 sem
  stencil fica como último recurso. O resto do renderer já tratava S8 (aspecto nas views e barreiras,
  `stencilAttachmentFormat`, captura de depth+stencil, clear com `HasStencil`).
- Teste `TestPicaDepthFormatKeepsStencil` em `oot3d_native_pica_vulkan_bridge_tests` (visto falhando
  com a ordem antiga), agora na suíte `tests/switch/run.sh`; guarda vista falhando por mutação
  (172/173).

## Alternativas rejeitadas

- Ligar o stencil só para títulos que não são o OoT3D: o PICA usa D24S8 em qualquer título, e o
  OoT3D no Switch (deko3d) já roda com stencil.
- D24S8 primeiro (igual ao 3DS): mudaria a precisão de depth do OoT3D no PC, que foi ajustado com D32
  float.

## Em aberto

- **Savestate na fase quebra (confirmado, não corrigido):** salvar no quadro 3100 e carregar, até
  dentro da mesma execução, leva a `MemoryFault` no pc 0x3361CC (chamada virtual com vtable
  0x29042104). No logo (quadro 600) salvar e carregar funciona, também em processo novo.
  Investigação em `2026-11-06-sm3dl-savestate-alvo-render.md`.
- A tela de baixo aparece preta em todas as capturas (ainda não verificado se o host apresenta).
- Custo em memória e banda do D32S8 no PC (não medido).
