# SM3DL: por que a W1-1 roda lenta no PC

## Pergunta

O SM3DL funciona no PC, mas abaixo da velocidade do 3DS. Onde vai o tempo na W1-1?

## Fontes

- Bench fixo: savestate `~/oot3d-work/conv/sm3dl-o1/w11-b6cdb4d5.oot3dsav` (W1-1, salvo no quadro 3300
  da timeline `fase_pula.json`), timeline sem botões, 75 s, `--profile-a32-runtime`, build
  `~/oot3d-work/conv/sm3dl-o1` (Debug com `-O1`, sem NDEBUG).
- `summary.json`: `benchmark_window`, `a32_runtime_profile` (dispatch, svc, gsp_trigger_*).
- Perfil de pobre com gdb (o gdb é pai do jogo por causa do `ptrace_scope=1`; SIGINT 1x/s, 25 amostras):
  15 amostras na thread do guest dentro do GSP, 8 delas em `ExecuteOot3dPicaMemoryFill`, 5 em
  `ComputeOot3dPicaLightingLutContentHash`.
- Instrumentação temporária (não commitada) de tempo por comando GSP no `TriggerCmdReqQueue`.
- Log de GX: o fill é sempre `p0=0x1f000080..0x1f05dc80` e `0x1f300080..`, 2 × 384000 bytes.

## Conclusão

1. O código do jogo (AOT) não é o gargalo: de 37,9 s de dispatch, 28,8 s eram SVC, quase tudo no
   `TriggerCmdReqQueue` do GSP.
2. Por comando, antes: MemoryFill 4239× a 7,05 ms = 29,9 s; lista de comandos 9891× a 1,75 ms = 17,3 s;
   DisplayTransfer e Flush desprezíveis. São 3 fills por quadro.
3. O fill montava o padrão byte a byte com `offset % elementSize` (divisão por byte) em -O1.
   Montando um elemento e copiando por dobramento: 0,127 ms por fill (5802×, 0,73 s).
4. Bench W1-1 (uma rodada de cada, com o agente do discover usando CPU ao mesmo tempo):
   18,95 → 26,43 quadros/s (1400 → 1963 quadros medidos em ~74 s).

## Decisão

- Fill por dobramento (`ExecuteOot3dPicaMemoryFill`), com teste de 16/24/32 bits terminando no meio
  de um elemento; `oot3d_native_pica_transfer_tests` entrou na suíte.

## Alternativas rejeitadas

- Pular o fill na CPU quando ele também vai para a GPU: muda quem é dono da memória do alvo; o ganho
  já veio de fazer o fill barato.
- Build Release completo: recompila os 256 shards (~1 h no notebook) e não ataca o fill.

## O que já existia (procurado depois do fill — devia ter sido antes)

- O port OoT3D→Switch roda em Release `-O2 -DNDEBUG` (`~/oot3d-work/build-switch`, ThinLTO/PGO no
  produto); este bench media um Debug `-O1` com asserts. Build `-O2 -DNDEBUG` do SM3DL em
  `~/oot3d-work/conv/sm3dl-o2` (o mesmo `build_title_boot_probe.sh`, `OPT="-O2 -DNDEBUG"`).
- `config/offload_*` (fbdc81d9, SWITCH_PORT.md "Núcleo 0 mais leve"): captura enxuta dos draws,
  hash de textura numa passada, placeholder do plano no lugar; equivalência provada em
  `oot3d_native_offload_tests` (ASan/UBSan/TSan). Ligado no Switch; no desktop só com
  `TRIAEVUM_OFFLOAD=1`. A medir no bench da W1-1.
- LUT de iluminação: o Azahar só marca a tabela suja quando o valor muda
  (`src/video_core/pica/pica_core.cpp`, `lighting.lut_dirty |= (prev != value) << lut_config.type`,
  o mesmo para fog e proctex). Aqui toda escrita copia os 24 KiB do estado (se um draw o segura) e
  invalida o hash FNV de 24 KiB recalculado no próximo draw
  (`oot3d_native_pica_frontend.cpp`, `ApplyShaderRegisterSideEffects`). Ignorar a escrita de valor
  igual não muda conteúdo nem hash (savestates antigos seguem válidos). Falta medir quantas escritas
  do SM3DL repetem o valor.

## Medidas repetidas (06/11, notebook sem build concorrente, janela na ws2)

Bench W1-1, 60 s, savestate `w11-b6cdb4d5`, `benchmark_window.frames_per_second`, rodadas
alternadas (pastas `~/oot3d-work/conv/sm3dl/progresso/*bench-w11-*`):

| build | rodadas (quadros/s) |
|---|---|
| Debug `-O1` (com o fill novo) | 22,05 · 21,77 |
| `-O2 -DNDEBUG` | 22,19 · 22,13 · 22,09 · 21,59 |
| `-O2 -DNDEBUG` + `TRIAEVUM_OFFLOAD=1` | 23,48 · 27,27 · 22,15 · 23,35 |

- O -O2 quase não muda nada (~1 %), o que indica que o gargalo não está no código gerado.
- O offload parece ajudar um pouco, mas oscila demais (22 a 27): ainda não dá para concluir.
- Os 26,43 do commit do fill eram uma medida única e não se repetiram. O valor estável depois do
  fill fica em ~22 quadros/s, contra 18,95 antes.
- Perfil com offload (`--profile-a32-runtime`, 60 s): `gsp_trigger_command_queue_seconds`
  18,1–19,9 s, dos quais `gsp_pica_frontend_submit_seconds` 17,4–19,1 s. `pica_submission_total_seconds`
  fica em só 2,7–3,0 s, então o resto é o frontend PICA (registradores, LUT/hash). Despacho do
  código: 11,1–12,7 s.

## LUT de iluminação: escrita de valor igual (06/11)

Contagem temporária na W1-1 (60 s, -O2): 139.151.104 escritas de entrada da LUT, 109.105.661 (78 %)
com o valor que já estava lá; 543.559 cargas de tabela, 305.353 inteiras iguais; 182.098 cópias de
24 KiB do estado e 182.099 hashes FNV de 24 KiB. Correção como o Azahar: valor igual não toca a
tabela, nem copia, nem invalida o hash (conteúdo idêntico, savestates compatíveis).

| build -O2, sem offload | quadros/s |
|---|---|
| antes | 22,19 · 22,13 · 22,09 · 21,59 |
| depois | 25,52 · 26,92 · 28,02 |

## Gargalo depois da LUT: VSync com a janela escondida (06/11)

Amostragem com gdb (40 amostras, -O2, W1-1):
- thread do jogo: 40 % em `pthread_cond_wait` dentro de `NativeRenderCommandStream::Enqueue`, ou
  seja, esperando a thread de render;
- thread de render: 32,5 % em `drmSyncobjTimelineWait` dentro de `GfxRenderingAPIVulkan::StartFrame`,
  ou seja, a fence do quadro em voo (`gfx_vulkan_frame.cpp:642`).

GPU do notebook: Intel UHD 620. Efeitos extras desligados, `RenderScale` 1,0, VSync ligado.

A janela das rodadas abre escondida no workspace 2. Com VSync (FIFO) no Wayland, o compositor
segura o ritmo de uma janela que não está na tela. Teste com `Graphics.Presentation.VSync=false` na
config da janela, -O2 com a correção da LUT:

| VSync | quadros/s | vblanks no bench |
|---|---|---|
| ligado | 25,52 · 26,92 · 28,02 | 3268 em 59,4 s |
| desligado | 29,83 · 29,81 | 3478 em 59,4 s (58,6/s) |

- O SM3DL desenha um quadro a cada 2 vblanks (30 quadros/s no 3DS). Sem VSync, a W1-1 roda em
  velocidade cheia: 29,8 quadros/s e ~59 vblanks/s, com variação quase zero (1770 quadros nas duas).
- A oscilação e o "lento" que sobravam vinham do VSync com a janela escondida, não do jogo.
- Regra de bench daqui em diante: janela na ws2 só com VSync desligado. O FPS satura em 30, então
  folga se mede por tempo de CPU e GPU, não por FPS.

## Em aberto

- Próximo gargalo: lista de comandos PICA, ~2 ms cada (27,5 s em 75 s depois do fill), com
  `ComputeOot3dPicaLightingLutContentHash` nas amostras.
