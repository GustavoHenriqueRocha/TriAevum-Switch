# Pesquisa: ports de Xbox 360/PC para Switch homebrew (2026-10-17)

Referência principal: **StevensND/nfsmw-nx** (NFS Most Wanted 2005 X360, recompilação estática PPC→C++ via ReXGlue, renderer Vulkan sobre NVK/mesa-switch Mesa 26; pasta docs/ com medições). Outros: Kaays/nfscarbon-nx, UnleashedRecomp-NX, MarathonRecomp-NX, autorun (Wine+FEX/Box64).

## Lições aplicáveis
- **Clock oficial**: homebrew no portátil começa com GPU 307,2 MHz; `apmSetPerformanceConfiguration(Normal, 0x92220008)` → GPU 460,8 / RAM 1331 (config de jogos comerciais; conferir por ~1,5 s; 0x92220007 = RAM 1600). TriAevum não chama apm (usuário atual usa sys-clk).
- **Threads no Horizon**: só a prioridade 0x3B reveza tempo; em outras, thread ocupada bloqueia as de prioridade igual/menor no mesmo núcleo. Fixar a render thread em máscara exclusiva piorou o pior caso deles. A57 sem LSE: atômicos por draw custam caro (83 mil/quadro = 1,1 ms); cronometrar por draw ≈ ¼ da render thread → amostrar 1/128. Eliminar espera ativa (Sleep(0)).
- **CPU por draw** (~16 µs no console): estado num marcador por draw, constantes só alteradas, uploads sem duplicata em thread de cópia, verificação de hash de textura rareando (66→4,2 µs/draw), logs fora da render thread, structs-chave sem padding, variáveis locais em laços quentes.
- **GPU** (L2 256 KB, não-tiler): ZCULL (~2 ms), céu por último (~3 ms), resolve sem cópia, shaders sem branches; FP16 inútil.
- **Driver**: criar memória de GPU é lento; reaproveitar objetos de textura do mesmo tamanho/formato no LRU; limite de handles NvMap (~4000 com transfermem 8 MB, erro 0x235C).
- **Pacing**: 60 instável pior que 30 travado se p90 do quadro de passo > ~15,5 ms; conferir limitador que dorme período inteiro ao atrasar.
- **Build**: aliases fracos impedem inlining; registradores guest como locais (save/restore ≈ 14% da thread principal); PGO no Horizon: -fno-profile-values e -fprofile-correction.
- **Áudio**: bomba em ritmo fixo (187,5 Hz), fila ~53 ms, prioridade 0x2B.

## Top 5 para nós
1. apm 0x92220008 no portátil (para quem não usa sys-clk).
2. Auditoria de threads/prioridades/espera ativa/atômicos na render thread.
3. Corte de custo por draw no estilo nfsmw.
4. GPU barata: ordem de desenho, ZCULL, resolve sem cópia.
5. Testar GL do mesa-switch (Mesa 26) no lugar do switch-mesa 20.1; Vulkan/NVK a longo prazo.
