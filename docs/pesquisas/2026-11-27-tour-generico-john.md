# Tour no console: NRO generico-john contra a 1.0.0

Data: 2026-11-27 · Título: OoT3D · Branch `generico-john` (9b0daafb), NRO 152528447 B, md5 b46b8823.

## Pergunta
A NRO do `generico-john` (serviços, fills, shaders lidos do SD, IR) quebra alguma área ou piora o desempenho?

## Fontes
- Mesmo roteiro do tour28 (`~/oot3d-work/bench/tour28-john/script.jsonl`, 59 passos), mesmo dia, save normal,
  CPU 1785 / GPU 614 / mem 1600, portátil.
- Rodada A: `~/oot3d-work/bench/oot3d-generico-john`; rodada B (referência): `~/oot3d-work/bench/oot3d-1.0.2-ref`.
  A reserva no SD era a **1.0.0** (152434239 B), não a 1.0.2 (152438335 B): a 1.0.2 nunca foi instalada no
  console. O arquivo foi renomeado para `oot3d_native_game.1.0.0.nro.old`.
- `tools/oot3d/perf_report.py <pasta>/perf.txt --session -1` e
  `tools/oot3d/lab_tour_report.py <pasta> tools/oot3d/native_game_runtime/oot3d_dev_warp_catalog.inc`.

## Conclusão
- As duas completaram 59/59 passos; nenhum crash.
- Equivalentes, dentro do ruído de uma rodada:

| | generico-john | 1.0.0 |
|---|---|---|
| guest (perf_report, ms) | 9,38 | 9,91 |
| guest médio das 13 áreas (ms) | 10,25 | 10,27 |
| fps global / p10 / mín | 59,3 / 56,7 / 45,1 | 59,1 / 56,6 / 45,4 |
| fps mínimo médio das áreas | 58,4 | 58,7 |
| refresh perdido (soma das áreas) | 15,4 | 12,9 |
| quadros que ficaram 1 refresh | 99,1% | 99,0% |

- A maior diferença por área é Hyrule Castle Courtyard: perdidas 3,8 contra 0,5, com o mesmo guest (13,62 ms).
  Uma rodada só, então não dá para chamar de regressão.
- As duas gravaram os mesmos 2 shaders fora do pack (`frag_594c87e2a8e1e325`, `vert_ec5498476104c169`):
  falta antiga do pack, não desta NRO.

## Em aberto
- Segunda rodada de cada uma para confirmar Courtyard.
- Pôr os 2 shaders no pack (`tools/oot3d/deko3d_shader_pack.py`).
