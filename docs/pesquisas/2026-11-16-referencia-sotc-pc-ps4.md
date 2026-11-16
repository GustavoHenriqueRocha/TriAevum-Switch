# Referência: Shadow of the Colossus PC, port nativo do PS4 (16/11)

## Pergunta
Guardar a referência do port SotC-PC (estava só como zip no Ryzen) antes de apagar os arquivos pesados de PS4.

## Fontes
- Repo: https://github.com/AjncSystem/ShadowOfTheColossus-PC (GPL-2.0, 1950 estrelas, último push 12/11, release v0.75 de 12/11).
  `gh repo view` / `gh api .../contents/{src,docs,.gitmodules}` em 16/11.
- Base: https://github.com/Supermedo/bloodborne_pc (Bloodborne nativo no Windows, 5472 estrelas).
- Cópia local do binário: Ryzen `D:\Downloads\SotC-PC` (v0.75, README.md e CHANGELOG.md lidos em 16/11).

## Como funciona (README + árvore do repo)
- Não é emulador completo: o código do jogo (eboot do PS4, x86-64) roda direto na CPU do PC; um carregador liga
  o ELF e um runtime em C repõe as funções de sistema (`src/runtime_{kernel,thread,fiber,memory,file,savedata,
  pad,audio,ngs2,ajm,...}.c`, `import_names.inc` com os nomes importados).
- GPU: núcleo Vulkan do fork shadPS4 SotC (`gpu/`, xbyak, imgui), FSR 1/3 (FSR-Vulkan), HDR10.
- Som: desde a 0.75 um mixer NGS2 embutido (LibAtrac9) dispensa os módulos .sprx do console.
- Patches de jogo por versão (60 FPS do GoldHEN, modo PS4 Pro, af16) e reuso de binds de descritores
  (SOTC_FAST_BIND, ~25 → 30 FPS no Santuário segundo o README).
- O repo tem CLAUDE.md e docs/bbport (histórico do port do Bloodborne).

## Conclusão
Mesma família de técnica do ctr-recomp (carregador + HLE das chamadas de sistema + GPU traduzida), mas sem
recompilação: o PS4 já é x86-64, então o código roda nativo. Útil como referência de organização do runtime
por serviço e de cache de binds, não como código para copiar (CPU diferente do 3DS).

## Decisão
Só referência. No Ryzen saíram o Mortal Kombat 11 (3 arquivos, ~72 GB) e D:\PS2-PS4 (16/11, pedido do Gustavo).
O SotC-PC fica, com o .pkg e o dump extraído do SotC (o port precisa do dump, não de ISO).

## Em aberto
- Nada.
