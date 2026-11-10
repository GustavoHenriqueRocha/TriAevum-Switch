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

## Em aberto
- **Apresentação.** A tela de cima aponta para `0x1f092000`, 0x2000 bytes depois do início
  da display transfer em `0x1f090000`. Só que a janela procura o endereço exato
  (`displayTransfersByOutput.find`), então `selected_top_transfers` fica em 0 e a tela de
  baixo não ganha snapshot (1858 acertos de transfer, 1858 snapshots perdidos).
  - A correção é escolher a transfer cuja saída contém o endereço e aplicar o deslocamento.
- **SIGSEGV no desligamento depois de um erro fatal do renderizador.**
  - Caminho: `Shutdown` → `DestroyNativePicaShaderResources` → `EndNativePicaRenderPass`
    grava o End num command buffer que ficou no meio da gravação. Diagnóstico de outra
    sessão, não confirmado aqui.
  - Com o pool corrigido, esse caminho não aparece mais no MK7. O bug ainda merece teste e
    commit próprios.
- **O teste do pool não roda no build do MK7.** O `descriptor_pool_chain_tests` está
  registrado em `runtime/three_ds_recomp/tests`, mas o build do MK7 não compila essa
  pasta. Hoje ele roda com
  `g++ -std=c++20 -I include tests/descriptor_pool_chain_tests.cpp`, pela trava.
- Os stubs frd:u, boss:U, nim:aoc e ndm:u não bloquearam nada até agora.
