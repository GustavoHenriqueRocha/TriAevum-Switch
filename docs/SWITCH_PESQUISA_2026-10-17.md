# Pesquisa: técnicas de outros projetos 3D no Switch homebrew (2026-10-17)

Fonte principal: **SwitchWakerHD** (Wind Waker HD recompilado PPC→C no Switch; 48 rodadas de otimização medidas):
https://github.com/centollOS/SwitchWakerHD (docs/switch-port.md, docs/deko3d-plan.md).

## Achados
- Mesa/nouveau ≈ metade do tempo da render thread; switch-mesa tem glthread mas o EGL nunca o inicia (SwitchWakerHD forçou via st_set_background_context).
- glGetError/glGen* sincronizam com glthread; uniforms soltos fazem o nvc0 copiar o bloco inteiro no FIFO (UBO std140 cortou comandos a 1/3); **mallinfo trava o malloc** (usado no perf.txt desde 1e2bea5 a cada 150 quadros — avaliar).
- deko3d: fps igual/pouco melhor que GL, sem a thread GL do Mesa, ~400 MiB a mais de heap, zero travadas de shader (cache DKSH offline). Não melhora a GPU.
- NXVK (https://github.com/PalindromicBreadLoaf/nxvk): NVK no Switch, Vulkan 1.4 + GL 4.5 via Zink; adotado pelo Dekopon (Azahar no Switch, https://github.com/PalindromicBreadLoaf/dekopon). Sem benchmark público vs switch-mesa 20.1.
- Núcleo 3 indisponível (NPDM do hbloader: núcleos 0–2). Jogo sozinho num núcleo: SwitchWakerHD 20→28 fps.
- Ordenar funções quentes contíguas (500 funções = 88% do tempo em 4,5 MB) — complementar ao PGO.
- malloc do newlib com lock global compartilhado com o Mesa: tirar malloc do caminho quente (pools/anéis).
- appletSetCpuBoostMode(FastLoad): CPU 1785 mas GPU 76,8 MHz → só em carregamentos (Dekopon: CpuBoostScope).
- Não usar clkrst no app (problema de sleep, SwitchWakerHD issue #33); deixar com sys-clk. RAM a 1600 no portátil é perfil oficial.
- SoH/2S2H no Switch: SDL3 + GL switch-mesa; interpolação refaz o quadro inteiro (mesmo custo que o nosso).

## Ranking de oportunidades
1. Baratear o quadro interpolado (reaproveitar tela de baixo, resolução dinâmica no intermediário, descartar intermediários que não cabem).
2. Clocks pelo caminho seguro (RAM 1600, GPU 614 via sys-clk, FastLoad só em carregamento).
3. Medir a GPU por passe (GL_TIMESTAMP), contar barriers/clears/zcull.
4. Experimento NXVK (GL via Zink, depois Vulkan).
5. glthread forçado + auditoria de sincronização; deko3d como projeto de longo prazo.
Fora do top 5: tirar malloc do caminho quente; ordenar funções quentes contíguas.
