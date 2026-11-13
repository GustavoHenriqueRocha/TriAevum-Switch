# Grama desligada no Switch (decisão do Gustavo, 13/11) e como retomar

Data: 2026-11-13 · Título: OoT3D · Branch: `integra-john-2` (último commit da grama: 7826c82d)

## Pergunta
Depois das builds grama-pc2 e grama-metade, o Gustavo decidiu: "só desliga a grama e deixa anotado; se
alguém quiser usar em algum momento, segue do que eu fiz; não vou gastar tempo nisso".

## Estado deixado
- No SD do Switch: `config/grass` = `off` (antes `draw`). Ausente ou `off`/`0`/`false` = sem grama
  (`ParseGrassSwitchMode`, `grass_switch_profile.cpp:61`); com Off nenhum gancho roda (guardas
  "[decision] grass off without config/grass"). Para religar: gravar `draw` em `config/grass`.
- O código fica como está no branch (nada revertido). Padrão do código: grama desligada.

## Onde parou (o que já foi feito, em ordem)
- Máscara de sobreposição do PC (c66c97b0, a78e1862); curva das folhas (f0ce6d09, 0d83b373); tamanho
  da lâmina do PC e `config/grass_density` (e65d9a30).
- Custo do teste de fonte no Market resolvido (filtro de texturas que não são chão, 5132a006;
  `docs/pesquisas/2026-11-13-queda-guest-span-vfp.md`).
- Folha a folha até 730 u, cor do preset (influência 0,22), vento e dobra do PC, densidade 100 (740353e4).
- Metade da altura do PC (7826c82d), a pedido do Gustavo.
- Viabilidade de portar o renderizador do PC: `docs/pesquisas/2026-11-13-portar-grama-pc-deko3d.md`
  (shaders compilam no uam com 4 trocas; preset do PC inviável na GPU; plano em 3 etapas).

## Medido (uma rodada cada, tour27; GPU só da grama, média / pico por quadro)
| build | GPU grama | observação |
|---|---|---|
| grama-filtro (5132a006) | 0,62 / 4,9 ms | Market 59,4 fps méd |
| grama-pc2 (740353e4) | 0,98 / 7,3 ms | Kakariko 55 mín, Lon Lon 37 mín |
| grama-metade (7826c82d) | 1,29 / 7,6 ms | Hyrule Castle 42 mín, Hyrule Field 46 mín (60 na pc2: ruído?) |
Logs em `~/oot3d-work/bench/grama-{filtro,pc2,metade}/` (linhas `grass: ... gpu ... ms/frame`).

## Opinião do Gustavo (visual)
- Grama do PC "bem mais bonita e original"; a do Switch "muito artificial".
- Em Kakariko e dentro do castelo "não combina" (não é outra camada de textura: o caminho faz parte da
  mesma textura do chão); pediu menor (metade); depois desistiu da grama.

## Em aberto, para quem retomar
- Repetir o tour (só uma rodada por build) e cruzar janela a janela queda × GPU da grama.
- Shader faltando no pack visto no tour grama-metade: `frag_594c87e2a8e1e325.glsl` (refazer o pack).
- Se for portar o visual do PC: seguir as etapas da pesquisa de viabilidade (shader do PC sobre a
  seleção do Switch → memória da dobra → toon), com go/no-go por tour A/B.
