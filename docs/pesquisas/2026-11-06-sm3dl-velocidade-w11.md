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

## Em aberto

- Repetir o bench sem carga concorrente (medida única de cada lado).
- Próximo gargalo: lista de comandos PICA, ~2 ms cada (27,5 s em 75 s depois do fill), com
  `ComputeOot3dPicaLightingLutContentHash` nas amostras.
