# Queda de desempenho depois do merge do english: span VFP, grama ou iluminação?

Data: 2026-11-13 · Branch: `integra-john-2` (f613f88e) · Build de teste: `nospan` (NRO md5 daeddfb7)

## Pergunta
Depois da mescla do `english` do John, o guest ficou ~1-2 ms mais lento em todas as áreas do tour (Market: média
10,7 ms na p7e → 12,8-12,9 ms). O que causou, e como recuperar?

## Fontes
- Tours de 12/11 (`~/oot3d-work/bench/mascara`, `grama-pc-crash`, `john-off`), comparados por área com
  `scratchpad/area_campos.py` (lógica do `lab_tour_report.py`: janelas do perf.txt → área pelos acks).
- C++ do John no Ryzen: `/root/agentes/deko3d/gen/cpp-lean` (é o que o build monta; o `gen/cpp-lean` do caminho do
  notebook dentro do WSL é o antigo da p7e). Guardado lá como `gen/cpp-john-12nov`.
- Commit d0c42b4d (John, 05/11): VLDM/VSTM/VPUSH/VPOP de 2+ palavras tentam `ReadWordsFast/WriteWordsFast` antes da
  gravação palavra por palavra.

## Conclusão (até aqui)
- Descartados, com medida:
  - grama (roda na thread de render, não no guest);
  - as otimizações do John com chave no config (`capture_delta`, `list_walk_fast`, `vertex_snapshot_fast`,
    `texture_snapshot_fast`, `plan_*`): desligadas, ficou PIOR (Market guest máx 19,36 contra 14,9; fps mín 38,5);
  - distância de desenho (multiplier=1, extended=0);
  - perda do perfil PGO: shards 107 e 000 compilados com `-Wmissing-profile -Wcoverage-mismatch`, 0 avisos.
- Das 2972 funções que mudaram desde a p7e, 2938 diferem só pelo span VFP (15 span + bloco, 11 blocos novos,
  3 outras, 1 span + hook). É o suspeito que sobrou.
- Build `nospan`: gerador com `--no-vfp-word-span` (f613f88e, teste
  `test_no_vfp_word_span_restores_the_word_by_word_lowering`); spans VFP no C++ 22.533 → 0, spans LDM/STM iguais;
  auditoria ok (12439 funções, 0 resíduo); 247 arquivos diferentes do C++ do John. Leva também a trava do warp
  durante a morte (441d64d5).

## Resultados dos tours de 13/11 (guest médio por área, ms; `area_campos.py`, campo `guest`)
| área | p7e | r1 (PRs 1-5) | r2 | nospan sem grama | nospan |
|---|---|---|---|---|---|
| Market | 10,70 | 10,63 | 11,03 | 11,95 | 13,51 |
| Kakariko | 10,32 | 9,83 | 10,43 | 11,04 | 11,87 |
| Hyrule Field | 8,13 | 7,88 | 8,55 | 9,33 | 9,81 |
| Water Temple | 10,76 | 7,55 | 8,11 | 12,09 | 12,05 |
| Kokiri | 7,75 | 7,66 | 8,01 | 9,13 | 9,23 |
| Bottom of the Well | 9,63 | 7,80 | 8,17 | 11,00 | 11,06 |

- **Span VFP: descartado.** `nospan` = build atual (Market 13,51 × 13,55).
- **Grama: parte da queda.** Sem `config/grass` o Market cai 1,56 ms; o render volta ao normal no Market
  (`rt_ms_step` 13,9 → 6,5; `plan_lock_waits_render` ~600 → 0; `plan_late_draws` ~250 → 0): o render fica lento
  e o guest espera por ele. Fora do Market a grama custa ≤ 0,8 ms. A busca de chão de grama ("sources") em área
  sem grama custa só 0,1-0,3 ms/quadro no render (`deko3d.txt`).
- **`plan_resolve_locks` (447 → ~12.000) é pista falsa:** sem grama fica alto mas sem espera; subiu com a contagem
  nova do plano paralelo do John (3c819d5e).
- **Sobra +1,0-1,4 ms em toda área** (até +3-4 ms em Water Temple/Bottom of the Well) entre r1/r2 e a `nospan`
  sem grama: 276 commits (227 do John). Suspeito testado em seguida: iluminação por pixel por padrão (3c9d3058;
  os tours lentos rodaram com `lighting: mode perpixel`, a r1 não tinha a opção).

- **Iluminação nativa: descartada** (`config/lighting = native`, tour `nospan-nativa`: Market 13,30 × 13,51, o resto ±0,3).
- **O campo `guest` não é comparável entre r1 e a build atual.** Na r1 o guest passava 8,7 ms "dentro do PICA" no
  Water Temple (`guest_in_pica`), na atual 0,4 ms; somando guest e espera (`guest_wait_frames_ms`) dá o mesmo
  (7,5 + 7,9 ≈ 12,1 + 3,1). A captura por desenho do John (5ee45f23, campos `cap_*`/`list_*`) mudou o que entra
  no guest. A "sobra de +1 ms" é, ao menos em parte, mudança de medida.
- **O que se vê na tela (FPS médio por área):** sem grama a build atual segura 60 em todo lugar (Market 60,0,
  Kakariko 60,0); com grama: Market 52,9/54,0, Kakariko 57,7/58,8, Warps 58,6, Castle Town Entrance 58,7.
  **A queda visível é a grama**, mesmo onde ela não desenha nada (Market): o render dobra (`rt_ms_step` 6,5 → 13,9)
  e aparecem espera do render no plano (`plan_lock_waits_render` 0 → ~580) e desenhos atrasados (0 → ~250).
  A grama do John já fazia isso (john-r1: Market guest 12,82, `rt_ms_step` 11,95); as máscaras de 12/11 somaram
  pouco (13,55).

## Decisão
Medir antes de mexer: tour normal com a `nospan`. Se o Market voltar perto de 10,7 ms, o span VFP é a causa e a
decisão é entre tirar o span ou só para os tamanhos/caminhos que pioram. Se não voltar, o span sai da lista.

## Alternativas rejeitadas
- Testar o C++ da p7e direto: a auditoria do configure exige os 33 blocos novos do manifesto (02006a24).
- Voltar tudo para a p7e: perde as correções do english; o Gustavo preferiu melhorar (12/11).

## Em aberto
- O tour da `nospan` (FTP do Switch não apareceu em 2 h na madrugada de 13/11).
- sys-clk: `clk_ipc_us` de 17-50 ms em algumas janelas.
- Dois travamentos (20 s e 11 s) com a thread do jogo parada em frame_end na sessão do crash.
