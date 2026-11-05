# Varredura de shaders e cache no pacote (Mesa/GL)

Data: 2026-10-22 · Título: OoT3D · Commits: 2a32e108 (canal lab), afd7206a (`lab/script.jsonl`),
f6726e9c (guia, lições 27–28) · Fonte: memória antiga (`triaevum-varredura-shaders.md`).

## Pergunta
Dá para eliminar a travada da primeira compilação de shader no Switch entregando o cache pronto?

## Fontes
- `~/oot3d-work/bench/sweep_lab.sh HH:MM <primeira_cena>`: abre o Eden e faz warp por cada cena
  "Better" (índices 0..48 de `oot3d_dev_warp_catalog.inc`; a 49 é a sala Debug e congela), 30 s cada.
- `tools/oot3d/lab_tour_report.py`, `~/oot3d-work/bench/tour_report.py`.

## Conclusão
- A chave do cache do Mesa é cache_id do build + chipset + TGSI, sem nada do aparelho; o Eden emula
  o mesmo chip (dedução: o cache do Eden vale no console).
- Varredura concluída: 178 `.src` no Eden; cache final baixado do Switch: 269 `.src` + `mesacache`.
  `.src` somam aos do SD; `mesacache` substitui. Conferir no console com `shadercache/status.txt`
  (`warmup_seconds` baixo = cache valeu).
- Bench 2 (cache quente, 29 áreas abaixo de 60): gargalo na render thread, 19,9 ms por passo
  (clear/dequeue 6,1 ms, GPU 12,3 ms). Áreas piores: Water Temple 44, Market 46, Castle Town 51 fps.
- Melhor média (tour17, pgo5l, 4d512833): 59,44 fps, 26/29 áreas ≥ 59. Melhor fluidez (tour20,
  pgo5o, 8f67f53a, `config/dynamic_resolution`): hold 39 → 4,6 por janela.

## Decisão
Cache de shaders vai no pacote; comandos no console durante o jogo pelo `lab/script.jsonl`.

## Alternativas rejeitadas
- `frames_in_flight=2`: heap corrompido; TSan na Ryzen limpo, corrida não achada. Caça abortada.
- `skip_bottom_screen` (49fcb1f5): crash ao salvar ("native PICA composition sequence has no draws").

## Em aberto
- Superado pelo deko3d, que usa o próprio `deko3d.pack` (`docs/DEKO3D_PROGRESSO.md`).
