# MK7: o "carregamento" era espera por um botão

Data: 2026-11-10 · Título: Mario Kart 7 · Branch: deko3d (695e2dae, d7bbce88)

## Pergunta
Por que o MK7 continua no InitialDrawer, a tela de carregamento desenhada pela CPU, mesmo
depois de ~141 s de tempo de jogo na janela? Esta é a frente 2 de
`2026-11-10-mk7-janela-preta.md`.

## Fontes
- Probe de 600 vblanks (com `--complete-gpu-work`) e disassembly do `code.bin` com o capstone.
- A janela no notebook, rodando `~/oot3d-work/mk7/rodar-input.sh <s> <timeline> <pasta de prints>`
  com `--input-timeline timeline_a.json`: aperta A e toca no centro da tela de baixo a cada
  150 quadros, do quadro 300 ao 6000. O despejo de estado vem de
  `TRIAEVUM_GUEST_STATE_DUMP=<n>`, descrito em `docs/GUIA_PORT_3DS_SWITCH.md` §8.4.

## Conclusão
1. **Nada estava travado.**
   - A thread 0 está em `nngxWaitVSync(BOTH)`: o laço em `0x58c5fc` compara os contadores de
     VSync das duas telas, e o modo 0x402 é `NN_GX_DISPLAY_BOTH`.
   - As threads 4–12 são workers ociosos, todos voltando por `0x45aa54`.
   - Depois do boot não há nenhuma leitura de arquivo.
   - O InitialDrawer é a tarefa "BootRender" (vtable `0x6440c0`, criada em `0x102a4c`).
2. **Com A**, o jogo cria o save e chama `fs:USER CreateDirectory` (`0x08090182`). O host
   não atendia (`TRIAEVUM_FS_REQUEST_UNSERVED ... reason=unsupported-fs-command`), e o jogo
   chamava o err:f na thread 5.
   - Corrigido em 695e2dae, com a semântica do Azahar (`SaveDataArchive::CreateDirectory`).
   - O save passou a ter `replay/`, `replay_/`, `system1.dat` e `system2.dat`.
3. **Com o save criado**, o jogo desenha muito mais. Uns 30 s depois do início, a janela caía
   com `vkAllocateDescriptorSets(native PICA)` = `VK_ERROR_OUT_OF_POOL_MEMORY`: o pool do
   quadro tinha 4096 sets fixos.
   - Corrigido em d7bbce88, com uma cadeia de pools que cresce sob demanda, como o caminho
     NRI já fazia (29b8bdbb).
4. **Números da janela com A:**

| | antes | depois |
|---|---|---|
| tela de cima | `0x1780cae0` (InitialDrawer, FCRAM) | `0x1f092000` (VRAM) |
| vblanks / draws | 813, err:f; depois crash aos 30 s | 3730 / 512784 em 150 s, sem falha |

## Decisão
O pedido do Gustavo foi "sair do carregamento", e isso está feito: a tela de cima saiu do
InitialDrawer. A imagem continua preta, mas agora por causa da apresentação (frente 1).

## Apresentação (commit 2f08e375)
- **Pergunta:** por que a janela ficava preta com o jogo já desenhando na VRAM?
- **Causa:** a tela de cima aponta para `0x1f092000`, 0x2000 bytes depois do início da
  display transfer em `0x1f090000` (416 linhas de 0x200 bytes, RGB565, flags 0x2200). Como
  0x2000 / 0x200 = 16, a tela começa 16 linhas de memória dentro da cópia, ou seja 16
  colunas, porque a tela é girada. A janela procurava só o endereço exato
  (`displayTransfersByOutput.find`) e por isso nunca escolhia o topo.
- **Referência no Azahar:** `RasterizerOpenGL::AccelerateDisplay` chama
  `res_cache.GetSurfaceSubRect(src_params, ScaleMatch::Ignore, true)` e lê o sub-rect da
  superfície que contém o endereço (`display_texcoords` vem do `src_rect`).
- **Correção:** `SelectPresentedDisplayTransfer` (`oot3d_native_display_transfer_selection.h`).
  - O endereço exato tem prioridade, olho esquerdo antes do direito, então o OoT3D não muda.
  - Senão vale a cópia que contém o endereço, desde que:
    - o deslocamento seja um número inteiro de linhas;
    - a linha da cópia tenha o mesmo tamanho do stride da tela;
    - a cópia não seja uma TextureCopy crua.
  - Topo, trace e tela de baixo usam a mesma função.
  - Teste sem ROM `oot3d_native_display_transfer_selection_tests`: com a lógica antiga
    falham 6 checagens; com a nova passa.
- **Janela no notebook, 150 s com A e toque (`rodar-input.sh 150 timeline_a.json shots-input5`):**

| | antes | depois |
|---|---|---|
| selected_top_transfers | 0 | 1259 |
| PiP da tela de baixo (quadros) | 0 | 1258 |
| snapshots perdidos da tela de baixo | 1858 de 1858 | 307 de 1566 |
| captura | preta (máximo 0) | tela de customização do kart, 585692 de 984256 pixels não pretos |

Captura: `~/oot3d-work/mk7/shots-input5/quadro_000600.png`.

## Recorte do LCD (commit c4dfb9de)
- **Pergunta:** a cópia apresentada tinha 416 colunas de 256 pixels, mas o LCD mostra
  400×240 a partir do endereço da tela. Como mostrar só isso?
- **Faixas pretas medidas na captura antes do recorte (`shots-input5`):**
  - 16 colunas à esquerda: a imagem começava em x=129, e não em 84, na janela 1352×728;
  - cerca de 44 px no topo, que são os pixels 240..255 de cada coluna, contados de baixo
    para cima.
- **Caminhos até a tela:** PresentExisting, sample interpolado, snapshot repetido e PiP.
  - Todos terminam em `SubmitPicaDisplayTransfer` com `Present` e passam pelo mesmo scanout
    (`gfx_vulkan_pica_display.cpp`).
  - A imagem é achada pelo endereço de saída, então esse é o ponto mais estreito.
- **Correção:** `PlaceNativePicaScanout` (`gfx_native_pica_layout.h`).
  - Encaixa a parte visível na janela e estende o viewport para a cópia inteira, com o
    scissor na parte visível. O efeito é um retângulo de origem, como o
    `GetSurfaceSubRect`/`display_texcoords` do Azahar, sem mexer no shader.
  - O recorte fica no backend, guardado pelo endereço de saída (`SetPicaScreenCrop`; fora
    do Vulkan não faz nada).
  - A janela registra 400 colunas (cima) e 320 (baixo), de 240 pixels, a partir do
    `LineOffset`. Isso só acontece no perfil genérico (`LcdScreenCrop`); o OoT3D nunca
    chama.
- **Teste sem ROM `native_pica_screen_crop_tests`:** com a colocação antiga falham 9
  checagens; com a nova passa. Sem recorte, ou com recorte da saída inteira, o resultado é
  idêntico ao de antes.
- **Janela, 150 s com A e toque (`shots-input6`):**
  - tela de cima em 1213×728, proporção 1,666 (400/240 = 1,667), começando em x=69, sem
    faixa à esquerda nem no topo;
  - PiP com 322×242;
  - selected_top_transfers 1578; PiP em 1577 quadros.
  - Captura: `~/oot3d-work/mk7/shots-input6/quadro_000600.png`.

## Snapshots perdidos da tela de baixo (diagnóstico, sem correção)
- **Números:**
  - 150 s: 298 perdidos de 1876.
  - Rodada de 12 s com o mesmo input: 168 acertos de transfer de baixo, 167 perdidos e
    `selected_top_transfers` = 0. A tela de cima ainda era o framebuffer de CPU do
    InitialDrawer.
- **Leitura:** nenhum sample é executado enquanto a tela de cima não vem de uma display
  transfer, então o PiP não tem snapshot para mostrar. Os perdidos se concentram no boot,
  antes do jogo sair do InitialDrawer.
- **Grau de certeza:** parcial. Os 167 do começo estão confirmados. Que os outros cerca de
  130 também sejam do boot é inferência; não medi por quadro.
- **Por que não corrigi:** mostrar a tela de baixo nesse período exigiria apresentá-la sem
  depender de um sample do topo, e isso não é simples.

## Em aberto
- **SIGSEGV no desligamento depois de um erro fatal do renderizador.**
  - Caminho: `Shutdown` → `DestroyNativePicaShaderResources` → `EndNativePicaRenderPass`
    grava o End num command buffer que ficou no meio da gravação. Diagnóstico de outra
    sessão, não confirmado aqui.
  - Com o pool corrigido, esse caminho não aparece mais no MK7. O bug ainda merece teste e
    commit próprios.
  - Lendo o código, o diagnóstico é plausível: `EndNativePicaRenderPass` grava em
    `mCommandBuffers[mCurrentFrame]` sem saber se o quadro foi abandonado.
  - Reproduzir exige forçar um fatal do Vulkan com gdb no binário de 758 MB. É caro, então
    ficou de fora.
- **O teste do pool não roda no build do MK7.** O `descriptor_pool_chain_tests` está
  registrado em `runtime/three_ds_recomp/tests`, mas o build do MK7 não compila essa
  pasta. Hoje ele roda com
  `g++ -std=c++20 -I include tests/descriptor_pool_chain_tests.cpp`, pela trava.
- Os stubs frd:u, boss:U, nim:aoc e ndm:u não bloquearam nada até agora.
