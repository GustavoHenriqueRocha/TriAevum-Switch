# Frame time como meta e o A/B do early_frame_slot

Data: 2026-10-23/24 · Título: OoT3D · Fonte: memória antiga (`triaevum-foco-frame-time.md`).

## Pergunta
Qual é a meta de desempenho e onde está o gargalo depois do PGO 5?

## Fontes
- tour20 (591 picos), tour26 (`early_frame_slot=1`) × tour27 (`=0`), NRO earlyslot md5 28459058,
  sem Frame Tap, `gpu_timing` desligado (aí `spikes.txt` não grava: use `hitches.txt`).

## Conclusão
- Meta do Gustavo (23–24/10): ≤ 16,7 ms em qualquer área de jogo; FPS médio abaixo de 60 de vez em
  quando é aceitável; queda em carregamento também (ignore `ctx=load`). Sem VRR, quadro de 17 ms
  vira 33,3 ms na tela.
- tour20: 591 picos, 82% com stage=guest, ~320 em load.
- tour27 × tour26: pior quadro 19,14 → 17,82 ms; áreas > 16,9 ms 19 → 14 de 29; médias iguais.
  Travadas de lógica 201 → 115, de render 174 → 255 (total 413 ≈ 409): o gargalo migra para a
  thread de render (Mesa/GL).

## Decisão
`early_frame_slot` ligado no SD; próximo alvo, o render, o que levou ao backend deko3d.

## Alternativas rejeitadas
- Medir por FPS médio: esconde os picos.

## Em aberto
- Nada desta rodada; o acompanhamento segue em `docs/DEKO3D_PROGRESSO.md`.
