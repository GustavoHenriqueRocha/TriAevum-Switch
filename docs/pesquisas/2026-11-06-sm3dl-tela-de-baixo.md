# SM3DL: tela de baixo no PC (picture-in-picture)

## Pergunta

Por que a tela de baixo do Super Mario 3D Land nunca aparecia na janela, e como mostrar ela sem mudar
o OoT3D?

## Fontes

- Rodada com `--extended-diagnostics` (06/11, `~/oot3d-work/conv/sm3dl-o1`, timeline `fase_pula.json`):
  os transfers da tela de baixo são `TriggerCmdReqQueue:id=3,p0=0x1f012c80` (físico 0x18012c80) para os
  framebuffers de baixo 0x14890850 / 0x148b6060 / 0x148db870, `p2=0x019000f0` (entrada 240x400),
  `p3=0x014000f0` (saída 240x320), `p4=0x00002004` (bit 2 = crop_input_lines).
- Alvo de render de cima: 0x18000080, 240x400 RGBA8. 0x18012c80 − 0x18000080 = 0x12c00 = 76800 bytes =
  80 linhas (240 × 4 bytes por linha) = 10 linhas de tiles de 8.
- `oot3d_native_pica_submission.cpp` (`SubmitDisplayTransfer`): só ia para a GPU com
  `mKnownColorRenderTargets.contains(*inputPhysical)` (início exato do alvo). Os transfers de baixo iam
  pela CPU e copiavam VRAM velha: framebuffer de baixo azul sólido (0x001F).
- `pica_framebuffer_encode.h`: a linha y da memória é a linha `altura − 1 − y` do attachment.
- Citra: `SurfaceParams::GetSubRect` acha a superfície que contém o endereço e copia o sub-retângulo.
- `oot3d_native_pica_presentation_scheduler.cpp` (`HasSnapshot`): exige o `CompletionId` do último
  transfer capturado; com interpolação visual o sample executado está um quadro atrás.

## Conclusão

1. O SM3DL desenha a tela de baixo nas linhas de baixo do mesmo alvo da tela de cima e copia de 80 linhas
   para dentro dele. Nas linhas do attachment isso é [0, 320): `PicaDisplayTransferSourceRowsFor(400, 400, 80)`
   = primeira 0, 320 linhas.
2. Com o sub-retângulo indo para a GPU, o transfer de baixo chegou na janela (0 → 1565 de 1568 quadros em
   90 s), mas o PiP falhava em 1564 deles: o `CompletionId` do sample executado (atrasado pela interpolação)
   nunca era o do último transfer capturado.
3. Apresentando o último snapshot executado da saída: PiP em 3008 de 3014 quadros (170 s até o mapa),
   2 sem snapshot (início). No mapa W1 aparece a tela de baixo certa (abas W1..W8, "Return to HOME Menu"):
   `~/oot3d-work/conv/sm3dl/progresso/2026-11-06_0940_e2329a28_pip-mapa/quadro_002220.png`.

## Decisão

- Perfil: `BottomScreenPictureInPicture` e `SubRectDisplayTransfers` só no título genérico; o OoT3D
  continua com o frontend nativo dele e transfer de dentro de alvo pela CPU.
- Fila: origem no começo de uma linha de tiles (8 linhas) de um alvo conhecido com a mesma largura vira
  `InputPhysicalAddress` = base do alvo + `InputLineOffset`. Fora disso (meio de linha de tile, depois do
  fim, outra largura) segue pela CPU.
- Vulkan: o plano usa só as linhas lidas; o blit começa na linha de origem (escalada para a resolução do
  alvo); a cópia NRI, que lê da linha 0, só é usada com origem 0. Sem deslocamento, nada muda.
- PiP: 1/3 da altura da janela, canto inferior direito, margem de 2% (`PlaceUiPictureInPicture`).
  Nunca vira a "última apresentação" do savestate.

## Alternativas rejeitadas

- Copiar pela CPU depois de ler o alvo da GPU (readback): caro por quadro e o caminho de CPU já mostrava
  VRAM velha.
- Aceitar qualquer endereço dentro do alvo: no meio de uma linha de tile não existe linha inteira; fica na
  CPU.
- Esperar rodadas longas para ver se a tela aparecia: a causa estava no log de GX em segundos (pedido do
  Gustavo, 06/11).

## Em aberto

- O deko3d (Switch) não lê `InputLineOffset`; hoje nenhum título no Switch liga `SubRectDisplayTransfers`.
- Toque: clicar no PiP ainda não vira toque do 3DS (passo 2).
- Savestate de 02:47 (anterior a e2329a28) falha ao carregar (fault em pc 0x3361CC); não investigado.
