# Diário do port para Switch (16/10 a 24/10), compilado

Data: 2026-10-16 a 2026-10-24 · Título: OoT3D · Fonte: memória antiga do Claude
(`triaevum-oot3d-switch.md`, 34,8 KB), compilada na fase 2 do ai-memory. O passo a passo está no
`git log`; aqui ficam só as decisões, os números e o que não refazer.

## Pergunta
O que o port GL/Mesa aprendeu entre o primeiro boot no console e o início do deko3d, para não
refazer experimento já rejeitado.

## Fontes
- `SWITCH_PORT.md` (notas do port), `docs/SWITCH_PESQUISA_2026-10-17.md` (outros ports),
  `docs/GUIA_PORT_3DS_SWITCH.md`, `tools/oot3d/perf_report.py perf.txt --session -1`.
- Commits: 53d32a8 (HUD), 4282aae (melhor de 17/10), a3d7837 (final 3), 4bf15bd0 (Game Over),
  a312fc02 (polígono da mira), 3e2bdcd6 (registradores), 8325c0b6 (pacing), 9f1fa87e (congelamento),
  d5f43859 (rodada completa), a886ca69 (ETC2).

## Conclusão (marcos com número)
- 16/10: renderizou no console. Base: ~26,8 fps, lógica 13,3 ms/passo a 1785 MHz.
- 17/10 (a3d7837, PGO coleta 3 + aliasing TBAA + 23 substituições nativas): fps60 funcionou,
  48,6 fps médios, velocidade 99,7%, escala 2. 720p deixava o jogo a 83%: resolução padrão 2×.
- integ2 → integ3: 47,7 → 52,0 fps com fps60 e OC. Gargalo seguinte: lógica no núcleo 0.
- 18/10 final 4 (PGO coleta 4, 0 coverage-mismatch): base da release pessoal.
- 22/10: suíte `tests/switch` (guardas, testes de host, DSP no qemu, `eden_smoke.sh`, `perf_gate.py`).
- 23/10: PGO 5 (perfil do tour de 49 áreas): 59,6 fps médios no tour de 29 áreas.

## Decisão
- Comparações de desempenho só com o mesmo clock (o `perf.txt` registra `clocks:` desde 1e6524bc).
- Fluidez (frame time) acima de FPS médio desde 23/10 (ver `2026-10-24-frame-time-ab.md`).
- Render em deko3d a partir de 24/10 (`docs/DEKO3D_PROGRESSO.md`).
- Repo público ok; NRO, ROM e C++ gerado nunca sobem. Release pessoal no repo privado `oot3dre-pessoal`.

## Alternativas rejeitadas
- Registradores como locais (3e2bdcd6, opt-in): +36–47% de instruções e ~2× tráfego de pilha;
  a TBAA já mantém o estado em registradores.
- Pacing `adaptive` (integ4): travado em 30 em 84% do tempo (38,5 contra 53,7 fps). Padrão `legacy`.
- Mesa 26: com shader novo caía a 8,7 fps e o heap crescia 6–8 MB por programa; voltou ao Mesa 20.1.
- Overclock acima do oficial (Switch-OC-Suite): fora por decisão do Gustavo (17/10).
- 5 substituições com VFP (0x372070, 0x2CDE90, 0x3325F0, 0x3727F0, 0x36E168): `disabled` no
  `guest_overrides.json`, custam mais que a tradução.
- `mallinfo` em diagnóstico (`heap_check_windows`): entrava em laço com o lock do malloc.
- Recusas das substituições "medidas" pelo `guest_profile`: artefato do perfil, não real.

## Lições
- Mudança que roda o tempo todo precisa de teste longo antes do console (worker PICA vazava heap).
- Polígono gigante na mira: `GL_DEPTH_CLAMP` do renderer legado valia no caminho PICA (só no Switch).
- Prioridades iguais não revezam no Horizon; não fixar a render thread num núcleo exclusivo.

## Em aberto
- Origem da corrupção de heap vista uma vez (tour16, pgo5o/Mesa): nunca reproduzida.
