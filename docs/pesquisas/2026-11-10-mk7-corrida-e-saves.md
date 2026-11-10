# MK7: queda ao entrar na corrida, desligamento e saves (MK7 e SM3DL)

## Pergunta
1. Por que o MK7 caía ao entrar na corrida, com
   "native PICA display transfer mode is unsupported" seguido de rc=139?
2. Sabemos salvar e recarregar o save do MK7 e do SM3DL? E o rodízio `system<N>.dat` do MK7 é
   do jogo ou é efeito do nosso FS?

## Fontes
- **Reprodução:** `~/oot3d-work/mk7/rodar-input.sh 240 timeline_so_a.json` (só A). Caiu por
  volta do vblank 1200, logo depois do `y2r:u`, com rc=139.
- **Flags:** `~/oot3d-work/mk7/rodar-gdb.sh`, com um breakpoint condicional na entrada de
  `GfxRenderingAPIVulkan::SubmitPicaDisplayTransfer`. O `PicaDisplayTransferView` foi lido por
  offset: Flags em +32, TextureCopyBytes em +40.
  - O `--pica-semantic-trace` deixa o jogo lento demais: em 300 s não passou da escolha do
    kart, com 1 GB de trace.
- **Azahar** (`src/video_core/pica/regs_external.h`, `renderer_software/sw_blitter.cpp`,
  `gpu.cpp`):
  - `BitField<16, 1, u32> block_32; // TODO(yuriks): unimplemented`
  - O blitter de software e o `AccelerateDisplayTransfer` executam a transfer como uma comum.
  - O scaling 3 é "Unimplemented display transfer scaling mode".
- **Desligamento:** backtrace do gdb `Shutdown -> DestroyNativePicaShaderResources ->
  EndNativePicaRenderPass -> libvulkan_intel`, e `gfx_vulkan_lifecycle.cpp` (ordem do
  Shutdown: linha 676 antes da 683).
- **Saves:** IPC `fs:USER` no `window/stderr.log`; `window/savedata-backups/` (cópia de
  segurança por sessão, `oot3d_native_a32_ctr_save_safety.h`); md5 das gerações.

## Conclusão
### Corrida (commit 541857b5)
- A transfer recusada tem flags `0x12200`: RGB565 → RGB565 com `block_32`. É a cópia da tela de
  cima, 256×416, de `0x180f8000` para `0x18090000`. Não é texture copy nem scaling 3.
- O plano, o Vulkan, o GL e o deko3d recusavam o bit. Agora os quatro seguem o Azahar por uma
  regra só (`PicaDisplayTransferModeSupported`).
  - Nos caminhos de GPU a origem é uma imagem do host, sem tiling de memória para
    reinterpretar.
  - O caminho de CPU (`ExecuteOot3dPicaDisplayTransfer`) continua recusando o bit; não foi
    tocado.
- Teste sem ROM `pica_display_transfer_mode_tests`: falha 3 checagens com a regra antiga.
- **Na janela:** com o mesmo input, 240 s com rc=0 e nenhum "unsupported". Capturas na pista:
  - `~/oot3d-work/mk7/shots-corrida2/quadro_001500.png`: corrida, 8º lugar, volta 1/3, mapa na
    tela de baixo;
  - `~/oot3d-work/mk7/shots-corrida4/quadro_003000.png`: timeline versionado, acelerando, com
    estrela no item.

### Desligamento (commit c099240b)
- O `Shutdown` zera o `PicaDynamicRenderingScope` e só depois destrói os recursos do PICA. Com o
  passe ainda aberto (erro fatal no meio do quadro), o `EndNativePicaRenderPass` caía no
  `vkCmdEndRenderPass` e nos timestamps, gravando num command buffer abandonado.
- **Correção:** no desligamento o passe é esquecido sem gravar nada
  (`ResolveNativePicaPassEnd` → Abandon).
- Teste sem ROM `native_pica_pass_end_tests`: falha 2 checagens com a decisão antiga.

### Timeline que chega à pista (commit 50487f8d)
- `tools/oot3d/native_game_runtime/input_timelines/mk7_title_to_race.json`: A por 40 quadros,
  solto por 20, até o quadro 7200.
- Com o save criado, chega à pista em uns 1500 quadros e fica nela acelerando (rodada de 360 s,
  capturas 2100–3000 em pista).
- A primeira versão, com A segurado a partir do quadro 1500, parou em "Start Race/OK" numa
  rodada mais lenta: segurar não gera toque.

### Saves
- **MK7: o rodízio é do jogo (parcial).**
  - Em cada sessão o jogo lê `system<N>.dat` (mode 0x1), cria `system<N+1>.dat` (mode 0x6) e
    pede `DeleteFile /system<N-1>.dat`. É o guest que pede as três coisas.
  - A numeração é circular, `system0..9`, mantendo os dois últimos: as gerações dos backups vão
    8,9 → 0,8 → 1,9 → 0,2 → ...
  - Nenhuma fonte externa cita esses nomes. A busca só achou o `data.dat` do extdata
    (KartMiner 7).
- **MK7: ida e volta no arquivo (confirmado).**
  - A sessão seguinte lê exatamente o que a anterior gravou: md5 `5f6e70bf…` em todas as
    gerações das sessões 15:02, 15:07 e 15:13.
  - O jogo não pede para criar outro save: nas sessões conferidas (14:42 e 15:13) não há
    `CreateDirectory` nem criação do save, e o boot segue direto para os menus.
- **MK7: estado preservado (sem suporte).**
  - O conteúdo é idêntico em todas as gerações porque nenhuma rodada salvou progresso. O MK7
    salva ao terminar corridas, e o timeline não tem direção para terminar uma.
- **MK7: extdata.** `OpenArchive ExtSaveData 0x0703` volta NotFound e o jogo não chama
  `CreateExtSaveData` nas sessões seguintes. Fantasmas e StreetPass (`data.dat`) não ficam
  guardados.
- **SM3DL: carregar o save (parcial).**
  - Na sessão de 06/11 00:39 o jogo abriu `GameData.bin` só para leitura (mode 0x1, sem
    criar) e usou o save gravado em 05/11 22:40.
  - Isso foi com o binário de 06/11, anterior às correções de FS de hoje.
  - Estado preservado não conferido.

## Decisão e alternativas rejeitadas
- **block_32 ignorado como no Azahar.** Rejeitado: implementar o tiling 32×32. Só faria sentido
  no caminho de CPU, que lê memória tiled, e o Azahar também não implementa.
- **Desligamento esquece o passe.** Rejeitado: reordenar o Shutdown para fechar o passe antes
  de zerar o escopo. Gravaria comandos num command buffer que nunca mais é enviado.
- **Flags pelo gdb, e não pelo trace semântico,** que muda demais o tempo do jogo.
- **Timeline com pulsos contínuos,** e não com quadros fixos: a velocidade da janela varia
  (a GPU estava dividida com a cópia do Gustavo).

## Em aberto
- **Tela de baixo branca na corrida:** numa rodada (`shots-corrida4`) o PiP ficou 100% branco
  do quadro 900 em diante; em outra, com o mesmo binário (`shots-corrida2`), mostrava o mapa.
  Intermitente, não investigado.
- **Correção do SIGSEGV não revista na janela:** depois do 541857b5 não há mais erro fatal
  para disparar o caminho. A causa e o caminho estão confirmados pelo gdb e pelo teste.
- **Estado preservado do MK7:** exige terminar uma corrida, com um timeline que tenha direção.
- **SM3DL com o código atual:** exige recompilar no Ryzen (uns 10–15 min), não feito.
- **Extdata do MK7** (`CreateExtSaveData` nunca chamado depois do NotFound).
- **block_32 no caminho de CPU.**
