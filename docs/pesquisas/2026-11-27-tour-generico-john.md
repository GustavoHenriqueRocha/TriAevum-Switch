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
- Os 2 shaders baixados de `logs/deko3d_shaders/` (`frag_594c87e2a8e1e325`, `vert_ec5498476104c169`) NÃO
  faltaram nesta sessão: `missing=0` em todas as linhas `programs` do `deko3d.txt` das duas rodadas. São
  arquivos antigos que ficam na pasta do SD e voltam em todo download (ver "Por que os 2 shaders" abaixo).

## Por que os 2 shaders "não entraram" e se há outros (pergunta do Gustavo, 27/11)
- O `vert_ec5498476104c169` apareceu pela primeira vez em `~/oot3d-work/bench/integra-john/primeira`
  (12/11 16:01, `missing=1` no `deko3d.txt`), 24 min depois do pack v10 (12/11 15:37). Sessões de 12 e 13/11
  da linha do John (john-r1, john-r2, mascara, nospan, grama-metade) têm o mesmo `missing=1`; depois de 13/11
  nenhuma sessão do OoT3D tem `missing` > 0.
- O `frag_594c87e2a8e1e325` já estava na pasta em 24/11 15:12 (`bench/oot3d-faltantes-20261124`), gravado
  numa sessão que não foi baixada.
- Os dois JÁ ESTÃO no pack junto com o do John (24/11, 4795 shaders, md5 8247b5bb, o que o console usa):
  `deko3d_shader_pack.py --pack <junto> --glsl <os 2> --lighting-variants ...` deu "0 compiled now" e o
  mesmo md5. Por isso as sessões desde 24/11 têm `missing=0`.
- O jogo nunca limpa `logs/deko3d_shaders/`; o download de cada tour pega os arquivos antigos de novo.
- Outros do mesmo tipo: nenhum. Desde 24/11 todas as sessões do OoT3D baixadas (tour28-john, as duas desta
  pesquisa) têm `missing=0` e `uber=0`; a pasta só tem esses 2. No MM3D os tours desde 25/11 baixaram 0.
- O problema real era outro: a release 1.0.2 (17/11) saiu com o pack v10 (2255 shaders), sem os 2540 do
  John. Quem instalou pela release não tinha esses shaders. A 1.0.3 sai com o pack junto.

## Em aberto
- Segunda rodada de cada uma para confirmar Courtyard.
- Limpar `logs/deko3d_shaders/` no SD (ou o jogo apagar o arquivo quando o shader entra no pack) para o
  download mostrar só faltantes novos.
