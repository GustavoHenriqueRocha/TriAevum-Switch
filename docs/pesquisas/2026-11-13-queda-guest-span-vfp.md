# Queda de 1-2 ms no guest depois do merge do english: o span VFP do John?

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
