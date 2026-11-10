# MK7: custo de suportar o geometry shader da PICA

## Pergunta
O MK7 cai no meio da 1ª volta com "native PICA Vulkan draw uses a geometry shader"
(`oot3d_native_pica_vulkan_plan.cpp:238`). Já sabemos que a queda não vem do `-O2` (ver
2026-11-10-mk7-release-o2.md). Aqui a pergunta é outra: qual desenho usa o GS, como o Azahar
faz e quanto custa suportar. Nesta rodada só a estimativa foi aprovada, nada de implementação.

Estados das alegações: **[C]** confirmado, **[P]** parcial, **[S]** sem suporte (estimativa ou
palpite).

## Fontes
- **Captura do draw.** `~/oot3d-work/mk7-o2/rodar-gdb-gs.sh` roda a janela O2 com a timeline da
  pista e `--disable-audio` sob o gdb.
  - O binário é PIE e sem DWARF. Por isso o breakpoint vai pelo símbolo mangled das três
    instâncias de `BuildOot3dPicaVulkanDrawPlanImpl`, com a condição
    `(*(unsigned*)($rdi+2624) & 3) == 2`, que é o reg 0x229 do pacote.
  - Os offsets de `Oot3dPicaDrawSubmission` foram medidos com `offsetof` compilando os headers
    reais: Registers +412, VertexShader +3488, GeometryShader +5104 e sizeof 11680.
  - Parou no 1º draw com GS, chamado por `Oot3dPicaDeferredPlanBuilder::Build` dentro de
    `RunGuestRefresh`. O despejo ficou em `~/oot3d-work/mk7-o2/gs-dump/` e fica fora do git,
    porque é código do jogo.
- **Programa.** Foi decompilado com o decompilador do Azahar que já está em
  `tools/oot3d/third_party/azahar_pica`, por um driver de 15 linhas no scratchpad:
  `decomp gs_prog.bin gs_swz.bin 0x94`.
- **Azahar.** Commit 6b4e7f2 (06/11/2026), em `src/video_core`.

## O draw do MK7
- **[C] Registradores:**
  - 0x229 = 2: `use_gs` ligado, sem primitiva variável.
  - 0x252 = 0: modo **Point**.
  - 0x25E bits 8-9 = 3: topologia "Shader".
  - 0x244 = 1: a unidade GS tem configuração exclusiva.
  - 0x228 = 30 vértices.
  - GS em 0x289 = 0x08000003 (4 entradas, modo GS), main 0x28A = 0x94, máscara de saída
    0x28D = 7, ou seja, 3 saídas.
  - Mapas de saída 0x50–0x52: o0 = posição, o1 = UV0 e o2 = cor.
- **[C] Programa:**
  - Tem 0x1EC palavras, com 8 SETEMIT e 8 EMIT, em dois caminhos de 4 vértices. Em cada
    caminho, os vértices 0, 1 e 2 fecham a 1ª primitiva e o 4º, com `winding`, fecha a 2ª.
  - Cada ponto de entrada vira um **quad**. O GS soma ±tamanho·c67 a `in_v0.xy`, que já está em
    clip space; o UV sai de c76 e a cor é repassada de `in_v3`.
  - É um sprite de partícula: 30 pontos viram 30 quads.
- **[C] Entradas:**
  - O VS (512 palavras) produz 4 saídas cruas que viram `in_v0..3` do GS. Elas não passam
    pelos mapas semânticos.
  - Os uniforms do GS têm dados por partícula com passo de 9 vec4: posições em c8, c17, c26,
    c35… e matrizes junto.
- **[S] Qual efeito é:**
  - O quadro anterior mostra o kart encostado na grama com a estrela no quadro de item. Pode ser
    poeira ou grama, ou o brilho do item.
  - Também não sei quantos draws com GS acontecem por quadro: a captura parou no primeiro.

## Como o Azahar faz
- **[C] Qualquer draw com GS cai no software.**
  - `AccelerateDrawBatch` recusa o GS fora do modo Point e da topologia Shader
    (`renderer_vulkan/vk_rasterizer.cpp:448-456`, `renderer_opengl/gl_rasterizer.cpp:473-481`).
  - Logo em seguida, `SetupGeometryShader` recusa qualquer `use_gs != No`, com
    "Accelerate draw doesn't support geometry shader" (`vk_rasterizer.cpp:429-435`,
    `gl_rasterizer.cpp:453-459`).
  - Então nem o modo Point, que é o do MK7, tem caminho de hardware.
- **[C] Caminho de software:** `PicaCore::DrawArrays` (`pica/pica_core.cpp:1047`).
  - Com `use_hw_shader` ligado, ele tenta acelerar (linhas 1068-1082). Se não der, roda o VS
    na CPU por vértice, com cache de 64 entradas (linhas 1112-1140).
  - Depois `GeometryPipeline` faz a reconfiguração e o setup (linhas 1124-1126). Os modos
    estão em `pica/geometry_pipeline.cpp`:
    - Point (46-52): as entradas vão para os registradores de entrada do GS;
    - VariablePrimitive (131-134) e FixedPrimitive (229-232): os vértices vão para os uniforms;
    - escolha do modo (340-349).
  - O GS roda no interpretador (`shader/shader_interpreter.cpp:666-677`, EMIT e SETEMIT) ou no
    JIT (`shader_jit_x64_compiler.cpp:934`, `shader_jit_a64_compiler.cpp:868`).
  - Cada vértice emitido passa pelo `PrimitiveAssembler` e vai para
    `rasterizer->AddTriangle` (`pica_core.cpp:77-87`, `rasterizer_accelerated.cpp:86`).
  - Ou seja, **o fragmento continua na GPU**; só VS e GS rodam na CPU.
- **[C] O GS "fixo" do Azahar não é o GS da PICA.**
  - `PicaFixedGSConfig` (`shader/generator/shader_gen.h:170`) e `GenerateFixedGeometryShader`
    (`glsl_shader_gen.cpp:427`) existem para corrigir quaternions da iluminação por fragmento
    (comentário em `vk_rasterizer.cpp:437-441`).
  - Não servem de base para traduzir o programa do jogo.

## O que já temos
- **[C] Decompilador de VS:** `third_party/azahar_pica/.../glsl_shader_decompiler.cpp`. Ele já
  decompila o GS do MK7 inteiro, menos EMIT/SETEMIT (linhas 792-795: "Geometry shader operation
  detected in vertex shader").
- **[C] O frontend já guarda tudo o que o GS precisa.** Programa, swizzles e uniforms do GS
  ficam em `Oot3dPicaDrawPacket::GeometryShader`, e os regs 0x280–0x2AF são roteados para lá
  (`oot3d_native_pica_frontend.cpp:409-413`). Os valores despejados são coerentes.
- **[C] Não há interpretador de shader PICA na CPU** no nosso código. Não existe opcode
  executado em nenhum lugar fora do decompilador.
- **[C] Vulkan:** o perfil de dispositivo já liga `geometryShader` quando a GPU suporta
  (`pica_vulkan_device_profile.h:39`). Mas o pipeline PICA não tem estágio de GS, e o recurso
  só é usado para `mPbrSeamsAvailable`.
- **[C] deko3d:** tem `DkStage_Geometry` (`/opt/devkitpro/libnx/include/deko3d.h:314`) e o `uam`
  compila `--stage=geom`. Já `DekoPrimitive` (`gfx_deko3d_pica_pipeline.h:127`) trata
  `GeometryShader` como lista de triângulos, sem estágio de GS.

## Opções e custo
Todas as horas são **[S] estimativa**, sem medição.

| opção | o que é | horas | risco OoT3D | Switch |
|---|---|---|---|---|
| (c) pular o draw | draw com GS não é fatal: é descartado, com contador no summary e uma linha de log por programa novo. Atrás de flag no `ctr_title_profile.h`, com teste sem ROM e guarda | 2–3 | nenhum: o OoT3D nunca liga o GS e a flag fica desligada nele | igual |
| (a) GS na CPU, como o Azahar | portar o interpretador do Azahar (≈770 linhas) + `GeometryPipeline` (≈400) + montagem de primitivas (≈50), só para draws com GS. A saída vira uma lista de triângulos com VS de passagem no plano Vulkan, e o fragmento continua o nosso | 20–30 | baixo: só os draws com GS mudam de caminho; o resto do pipeline fica igual | roda (CPU ARM). Custo baixo para 30 pontos; sem JIT, risco de desempenho se outro jogo usar GS em massa |
| (b) GS traduzido para GS do host | EMIT/SETEMIT no decompilador, VS com saídas cruas, bloco de uniforms do GS e estágio de GS na criação de pipeline (Vulkan; deko3d e OpenGL depois). Primeiro só o modo Point | 25–40 (só Point); variable/fixed à parte | médio: mexe na criação de pipeline e nas chaves de cache que o OoT3D usa | deko3d e uam suportam GS, mas precisa de um caminho novo no backend deko3d |

## Recomendação
1. **(c) agora.** Destrava a corrida em poucas horas sem risco para o OoT3D. O contador mostra
   quantos draws e quantos programas de GS o MK7 usa numa corrida inteira. As partículas somem.
2. **(a) depois, como correção de verdade.** É o mesmo caminho do Azahar, então temos referência
   linha a linha para conferir. Cobre os três modos. O draw medido é pequeno (30 pontos).
3. **(b) só se** o contador da (c) ou o desempenho da (a) mostrarem muitos draws com GS por
   quadro. Não há referência no Azahar e o risco para o pipeline compartilhado é maior.

## Em aberto
- Qual efeito é e quantos draws com GS acontecem por quadro e por corrida. A (c) responde isso.
- Se há GS nos modos VariablePrimitive ou FixedPrimitive em outro ponto do MK7 ou no SM3DL.
