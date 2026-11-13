# Portar a grama do PC (o "mod original") para o deko3d: viabilidade

Data: 2026-11-13 · Título: OoT3D · Branch: `integra-john-2` (7826c82d)

## Pergunta
O Gustavo prefere a grama do PC ("fica bem mais bonita e original; queria portar o mod mesmo"). Dá para
rodar o renderizador de grama do PC no Switch em vez da grama própria do deko3d? A que custo?

## Fontes
- PC: `runtime/three_ds_recomp/src/fast/oot3d/interactive_grass_pass.cpp` (1698 linhas, Vulkan),
  `grass_shader_sources.cpp` (976 linhas de GLSL montado em C++), `grass_gpu_instance_compactor.cpp`,
  `grass_shadow_grid_pass.cpp`, preset em `graphics_settings_persistence.cpp:1397-1480`.
- Switch: `gfx_deko3d_grass*.cpp`, `tools/oot3d/deko3d_grass/*.glsl`, `grass_switch_profile.h`;
  a fila do deko3d do jogo é só gráfica (`gfx_deko3d_device.cpp:361`), por isso o Switch troca compute
  por um passe de vértice que grava SSBO (`SWITCH_PORT.md` "Grass on deko3d: cost per tuft").
- Compilação: GLSL do PC extraído refazendo a concatenação do C++ (script de um agente, fora do repo) e
  compilado com `/opt/devkitpro/tools/bin/uam`. Conferido por mim: o vértice original falha
  (`unrecognized layout identifier 'set'`); o adaptado compila (25 088 B, aviso de divisão inteira
  emulada em ponto flutuante); fragmento aux0 3328 B e compute de compactação 6656 B também compilam.
- Custo medido: PC RTX 3060 1440p Kokiri, grama 3,3 ms de GPU com ~199 mil lâminas
  (`docs/TRIAEVUM_GRASS_PRESET_RENDER_COST.md:80-87`); Switch Lite, tour grama-pc2 (740353e4):
  grama 2,3-4,2 ms/quadro nas janelas lidas (`~/oot3d-work/bench/grama-pc2/deko3d.txt`, linhas
  `gpu ... ms/frame (... blades ... tufts ...)`), até ~20 mil tufos e 1,27 M vértices (relatório do agente).

## Conclusão
- **Shaders do PC compilam no deko3d** com 4 trocas mecânicas (tirar `set=0`, push constant → UBO,
  `gl_VertexIndex` → `gl_VertexID`, `gl_InstanceIndex` → `gl_InstanceID` + primeira instância).
  Confirmado (3 estágios recompilados por mim; o agente relata os 6).
- **O lado da CPU já é compartilhado** (posições, visibilidade, regras, cache de texturas, máscara de
  sobreposição). Falta escrever o caminho de desenho deko3d do PC e a compactação como passe de vértice.
- **Ficam de fora no Switch:** grade de sombra (o Switch não tem VSM), iluminação nativa do chão (exige
  reexecutar a PICA no deko3d), vetores de movimento (sem TAA).
- **GPU:** o vértice do PC usa 53-75 registradores e 14-25 KB de código, contra 25-30 e 3-6 KB no Switch.
  Estimativa (não medida): 2-4× o custo por vértice. Com a densidade do PC (~1049 lâminas/m²,
  até 500 mil por sala) daria 14-44 ms só de grama: **inviável a 60 fps**. No orçamento de hoje
  (~4-6 ms) cabem 1/2 a 1/3 das lâminas que o Switch desenha agora.

## Decisão
Pendente do Gustavo. Recomendação: não portar o preset do PC; se quiser o visual do PC, o caminho é
"lâmina e sombreado do PC dentro do orçamento do Switch" em etapas com tour A/B (duas rodadas):
1. vértice/fragmento do PC (sem movimento, sem iluminação nativa, sem sombra) sobre a seleção do Switch,
   2-3 sessões; go se a GPU por lâmina ≤ 2× a de hoje e nenhum quadro novo > 33 ms;
2. memória da dobra (rastro atrás do Link), 1-2 sessões;
3. toon e rim, meia sessão.
Muito do visual do PC já foi trazido (forma em 3 segmentos f0ce6d09, curva longe 0d83b373, máscara
c66c97b0, tamanho e→ metade 7826c82d, cor/vento/LOD 740353e4).

## Alternativas rejeitadas
- Port inteiro com o preset do PC: GPU (estimativa acima), memória (~32 MB de âncoras + 34 MB de
  instâncias contra 12 MiB retidos, processo a 3185 de 3189 MB num tour) e CPU do A57.
- Iluminação nativa (reexecutar a PICA no deko3d, 3-5 sessões): alto risco; as luzes PICA por vértice já existem.

## Em aberto
- `gl_BaseInstanceARB` no deko3d: compilou, não visto funcionando (usar uniform com a primeira instância).
- Custo real por vértice do shader do PC no Switch: só medindo a etapa 1.
- Clock da GPU nos tours recentes não gravado (`clk_src=none`).
