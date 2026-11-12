# Grama em cima do cimento no castelo (deko3d) × como o PC coloca a grama

Data: 2026-11-12 · Título: OoT3D · Branch: `integra-john-2` (f0ce6d09)

## Pergunta
O Gustavo viu, no tour da build "john" (grama do John no deko3d), grama nascendo em cima de piso de
cimento/pedra no castelo (a área em que o tour só anda para a esquerda). Como o mod original (a
grama do TriAevum de PC, Vulkan) decide onde pôr grama ali, e o que o Switch faz diferente?

## Fontes
- Log da rodada 2 (`~/oot3d-work/bench/integra-john/john-r2/deko3d.txt`): grama desenhada nas cenas
  95 (Hyrule Castle) e 69 (pátio dos guardas), `area drawn scene 69 room 0` e
  `grass: scene 69 first drawn at its frame 34`.
- Regras: `tools/oot3d/native_game_runtime/triaevum_product_graphics.inc` (25 `Sources`, as mesmas
  no PC e no Switch: textura por hash, canal verde, níveis, inclinação máxima 65°).
- PC: `gfx_vulkan_pica_draw.cpp:3890-3990` (desenhos `masksOnly` viram `GrassOverlayAlpha`: programa
  TEV, texturas, alpha test/blend, `FindGrassMaskRule`), `grass_scene_bridge.h` (`OverlaysFor`,
  `kGrassOverlaySettleFrames` = 20), `grass_surface_extractor.cpp:564-630`
  (`GrassOverlaysForTriangle` e `sample *= GrassOverlaySurvival(...)`), `grass_overlay_mask.cpp:432`.
- Switch: `pica_grass_source.cpp`, `gfx_deko3d_grass.cpp`, `gfx_deko3d_pica_draw.cpp`: nenhuma
  referência a `GrassOverlayAlpha`/`OverlaysFor` (`grep -c -i overlay`: 0 no source; as 11 do
  deko3d são o HUD/minimapa, outra coisa).

## Conclusão
- O PC põe grama em duas etapas: (1) a regra da textura do chão dá a densidade pelo canal verde de
  cada ponto; (2) **máscara de sobreposição**: todo desenho coplanar por cima do chão (caminho,
  piso, decalque com alpha) é guardado com seu programa TEV e texturas, e cada ponto de grama
  sobrevive só na parte que esse desenho não cobre (`GrassOverlaySurvival`), com regra de máscara
  por textura (Automatic/Custom/Off) e 20 quadros para assentar.
- No Switch só existe a etapa (1): o caminho deko3d nunca coleta as sobreposições, então
  `surface.Overlays` fica vazio e a grama nasce embaixo de caminhos e pisos desenhados por cima do
  gramado. Explica o cimento com grama [hipótese forte: confirmar com um print no lugar e a textura
  daquele desenho].

## Decisão
O Gustavo pediu o port da etapa 2 (12/11). Feito em c66c97b0/a78e1862: o `GrassObserveDraw` publica
como sobreposição todo desenho do mundo (opaco ou translúcido) feito depois de um chão no mesmo
alvo; como o deko3d põe a grama no ponto de desenho, antes dos translúcidos, o `GrassEndFrame`
junta os conjuntos (`GrassLateOverlays`) e o quadro seguinte pede a posição com eles (um chão novo
espera um quadro). A chave do cache do builder já tinha a `PlacementVersion` do conjunto.
Na mesma build (0d83b373) os tufos de longe ficaram curvos (pedido do mesmo dia).

## Alternativas
- Portar a coleta de sobreposições para o deko3d (`gfx_deko3d_pica_draw.cpp` → `GrassSceneBridge`):
  corrige todas as áreas de uma vez; custo de CPU só na montagem das posições (que já fica no
  cache, cuja versão muda). Recomendada.
- Regras de máscara/`ActiveRects` só para as texturas do castelo: conserta um lugar e deixa os outros.

## Em aberto
- Conferir no tour da build 0d83b373: cimento do castelo e canteiros da cidade sem grama; contadores
  `overlays`/`waiting`/`overlaid` na linha `grass:` do `deko3d.txt`; custo de CPU das sobreposições
  (`sources ... ms/frame`).
- Qual desenho/textura é o cimento (print do Switch).
- Quantas outras áreas têm o mesmo problema.
