# MK7 na janela: tela preta com 100 mil draws

Data: 2026-11-10 · Título: Mario Kart 7 · Branch: deko3d (7b3cb771)

## Pergunta
A janela (`oot3d_native_game`, Vulkan) roda o MK7 sem falhar, mas todas as capturas saem
com os pixels zerados. Isso acontece porque o jogo ainda não desenhou nada ou porque a
apresentação não mostra o que ele desenha?

## Fontes
- Rodada no notebook (Intel UHD 620): o binário e o `extract/` do Ryzen
  (`/root/agentes/conv/mk7`) foram copiados para `~/oot3d-work/mk7`, junto com 4 libs do
  Ubuntu que o Arch não tem (`libs/`, via `LD_LIBRARY_PATH`).
  Comando: `tools/ctr/build_title_window.sh --work ~/oot3d-work/mk7 --no-build --run --seconds 180`.
  O WSLg do Ryzen não abre a janela (7b3cb771).
- Trace: o mesmo comando com `-- --pica-semantic-trace trace.jsonl`, rodando 40 s.
- Log temporário em `ApplyGspFramebufferUpdate`, fora do commit, no probe do Ryzen
  (`--vblanks 120`).
- Disassembly do `code.bin` com o capstone no Ryzen. A base é 0x100000.

## Números
| rodada | vblanks | draws | display transfers | pixels não pretos |
|---|---|---|---|---|
| 60 s | 1038 | 11844 | 2959 | 0 |
| 180 s | 8463 | 100944 | 25234 | 0 |

- No trace, as 408 `presentation_selection` pegaram a tela de cima em
  `0x1780cae0`/`0x1780c360` e nenhuma transfer foi escolhida
  (`selected_top_transfers 0`).
- As transfers vão só para a VRAM:
  - de cima: `0x1f000000`/`0x1f034000`, `0x1f090000`/`0x1f0c4000` e `0x1f328000`/`0x1f35c000`;
  - de baixo: `0x1f068000`, `0x1f300000` e `0x1f390000`.
- Tela de baixo: `bottom_screen_transfer_hits 381` e `bottom_screen_snapshot_misses 381`.

## Conclusão
- **O host lê a GSP certo.** É o próprio jogo que escreve `1780cae0 1780c360 1e0 122`
  em todo quadro, alternando o índice 0/1. Enquanto isso, a tela de baixo alterna entre
  os 3 buffers da VRAM.
- **De onde vem o endereço.** A função `0x104f34` tem a string "InitialDrawer" no pool.
  - Ela aloca 0x30c00 bytes, que são 416×240 em RGB565, por `0x10d580`. A alocação dá certo:
    o buffer começa em `0x1780c360` e o campo +0xe8 guarda 0x18600 (416×240 pixels).
  - O endereço da esquerda é o buffer + 0x780, 4 colunas à frente: é o deslocamento do 3D.
  - A tela de cima é, portanto, uma tela de carregamento desenhada pela CPU em FCRAM.
    Nenhuma transfer da GPU vai para ela.
- **Por que tudo sai preto.** A janela só apresenta a tela de cima quando encontra uma
  display transfer cujo destino é o endereço da tela (`displayTransfersByOutput`). Com
  uma tela desenhada pela CPU, nada é escolhido.
  - Os snapshots que a tela de baixo (picture-in-picture) usa só saem quando a tela de
    cima é escolhida (`FinishFrames`). Por isso a de baixo também fica preta.
  - O Azahar lê a memória do framebuffer direto e não tem essa limitação.
- **O jogo também está parado no carregamento.** Em ~141 s de tempo de jogo, o MK7 não
  trocou a tela de cima do InitialDrawer para os buffers da VRAM. O renderizador
  principal só troca a tela de baixo.

## Decisão
Nada foi corrigido ainda. O log temporário foi revertido. Há duas frentes, em aberto.

## Em aberto
1. **Apresentação:** quando a tela não tem transfer, mostrar o framebuffer lido da memória
   do guest (RGB565/RGBA8, girado como no 3DS), e soltar o snapshot da tela de baixo da
   escolha da de cima. É recurso novo no renderizador da janela e precisa ficar atrás
   do perfil de título.
   - Opção enxuta: o probe grava a tela de cima como PNG a partir da memória. Ele mostra
     a tela da CPU, mas não as feitas pela GPU.
2. **Carregamento:** descobrir o que segura o InitialDrawer. As threads 0, 4, 5, 7, 9, 10
   e 12 esperam em ArbitrateAddress, e ainda há serviços em stub (frd:u, boss:U, nim:aoc,
   ndm:u, cecd:u Open).
