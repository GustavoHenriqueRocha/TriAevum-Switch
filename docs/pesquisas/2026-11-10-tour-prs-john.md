# Tour dos PRs do John (integra-john) contra a p7e (10/11)

## Pergunta
Os PRs #1–#5 do JohnUzoka (branch local `integra-john` = deko3d bd84151b + #1 #3 #4 #2 #5,
NRO `integra-john-0eb4d734`, md5 4f459dfe…, C++ igual ao da p7e) pioram a fluidez no Switch?

## Fontes
- Roteiro: `~/oot3d-work/bench/tour27/script.jsonl` (49 warps, ~6 min; o mesmo da p7e).
- Resultados: `~/oot3d-work/bench/p7e`, `bench/integra-john/r1`, `bench/integra-john/r2`.
- Reproduz: `~/oot3d-work/bench/integra-john/tour.sh enviar` (jogo fechado), abrir a NRO,
  `tour.sh baixar integra-john/rN`; tabela por área: `tools/oot3d/lab_tour_report.py`.

## Conclusão
| rodada | CPU real | áreas < 59 fps (de 29) | pior área | áreas com ms máx > 16,7 | perdidas/janela (soma) |
|---|---|---|---|---|---|
| p7e (bench antigo) | 1779–1787 | 7 | 56,6 Goron City | 8 | 12,5 |
| integra-john r1 | **1894–1901** | 3 | 57,3 Castle Town Entrance | 4 | 7,6 |
| integra-john r2 | 1785 | 3 | 56,4 Castle Town Entrance | 6 | 10,6 |

- As duas rodadas chegaram ao fim do roteiro (132/132 acks `ok`), sem crash report e sem
  shader caindo no uber.
- A r1 não vale para comparar: a CPU real estava em ~1897 MHz com o sys-clk dizendo 1785
  (`CPU clock set : 1785.0 MHz` no log dele). Nenhum PR mexe em clock (diff sem sysclk/apm/
  CpuBoost); no Sphaira também aparecia 1899, então é do console. Voltou a 1785 depois de
  desligar e ligar o console. Causa em aberto.
- Na r2 (mesmo clock da p7e) nenhuma área piorou de forma clara: menos áreas abaixo de 59
  (3 contra 7) e menos quadros perdidos (10,6 contra 12,5). A diferença é pequena e a p7e é
  uma rodada só de outro dia: conta como "sem regressão", não como ganho.
- Na r2 o Link morreu durante o tour e a tela apagou depois do fim; as janelas lentas
  (0,6 fps) ficam depois do último comando do roteiro (quadro ~36800 contra 22653), fora da
  tabela.

## Decisão
Os PRs #1–#5 não pioram a fluidez: podem ser mesclados (por cima de f25fdd87, que já tem o MK7).

## Em aberto
- Por que o console ficou com a CPU em ~1900 com o sys-clk em 1785 (sumiu com o reboot).
- Uma rodada da p7e no mesmo dia daria a comparação limpa (regra: duas rodadas).
