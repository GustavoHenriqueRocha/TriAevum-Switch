# Áudio adiado (pgo5q/pgo5r)

Data: 2026-10-23 · Título: OoT3D · Commits: bf81c030 (pgo5r), cd28c9f2 (testes de perf) ·
Fonte: memória antiga (`triaevum-estado-audio.md`).

## Pergunta
Tirar o som do caminho da lógica do jogo reduz os picos acima de 16,7 ms?

## Fontes
- tour23 (pgo5p, contador por thread), tour24 (pgo5q), tour25 (pgo5r) em `~/oot3d-work/bench/`.

## Conclusão
- Thread de som ~1,2 ms/quadro + mix DSP 0,44 ms, antes da lógica; as secundárias somam ~2 ms por
  quadro de tela. Todas as threads guest são cooperativas num núcleo.
- pgo5q (áudio depois da apresentação): guest 8,46 → 7,83 ms, mas intermediários 75 → 55 e o
  Gustavo sentiu +2 ms.
- pgo5r (áudio na espera do render quando FrameSlotsFull): neutro em 28 janelas.

## Decisão
`config/audio_deferred` com `0` desliga. O Gustavo ficou com a pgo5o no Switch (23/10); depois a
base passou ao deko3d.

## Alternativas rejeitadas
- Áudio adiado como solução de frame time: sem ganho medível.

## Em aberto
- Thread de som em outro núcleo exigiria StoreExclusive como CAS real e `++mRangeGeneration` por thread.
