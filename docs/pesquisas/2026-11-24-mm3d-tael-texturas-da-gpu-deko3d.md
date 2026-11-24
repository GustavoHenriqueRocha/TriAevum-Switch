# Tael escura no MM3D: texturas que leem a imagem da GPU no deko3d (24/11)

## Pergunta

Na abertura do MM3D a Tael aparece como uma bola roxa escura, sem rastro nem brilho. O upstream
corrigiu no Vulkan (1757f77c, complemento b43f76a2). O que o deko3d já fazia, e como portar a ideia?

## Fontes

- `git show 1757f77c` (upstream coccofresco/TriAevum): a abertura reduz a cena com DisplayTransfer
  tiled->tiled 2x2 (flags `0x2000020`) para memória de textura, faz bloom em ping-pong entre alvos
  pequenos e mistura de volta o quadro anterior (alfa 200/255). Sem produtor da GPU, a leitura pega
  memória do guest que a GPU nunca escreveu. `docs/TRIAEVUM_GPU_COPY_PATH_PLAN.md` §9 do upstream.
- `git show b43f76a2`: só o Vulkan (cópias guardadas num reset de alvo). Não se aplica aqui: o
  deko3d não tem livro-razão de cópias, e `ResetPicaState` espera a GPU parar antes de destruir.
- Censo do deko3d, Floresta Perdida (`~/oot3d-work/bench/mm3d-fix2/census.jsonl`, ns 2):
  - `18237fc0` 512x400 -> `18381180` 256x200 (`02000020`), lida como textura 256x256 RGBA8 por um
    draw no alvo `183c1200` 64x128;
  - `183c1200` 64x128 -> `183c9280` 32x64 (`02000020`);
  - alvos 32x64 `183c9280`/`183d3c00` e 16x32 `183cb300`/`183d5c80` amostram um ao outro (276 draws
    cada) e a cena `18237fc0` soma `183c9280` e `183cb300` (ONE+ONE);
  - `18237fc0` -> `18300000` 256x200 (`02000020`), lida como 256x256 pela cena (o quadro anterior).
  Todas RGBA8, sem mips.
- Orientação: o decodificador (`pica_texture_decode.cpp`, `Detile`) põe a primeira linha da
  memória na linha 0 da imagem; o shader gerado lê `vec2(s, 1.0 - t)`
  (`pica_shader_instrumentation.cpp:81`); o Citra inverte t na textura e y no framebuffer
  ("laid out from bottom to top"). O viewport do deko3d põe NDC -1 (fundo do quadro, última linha
  da memória) na linha 0 do alvo (`ApplyDkViewport`, `config/deko3d_flip_y`).

## O que já existia

O deko3d copia cada DisplayTransfer (inclusive tiled->tiled) com o 2D engine para uma "imagem de
exibição" própria, chaveada por namespace e endereço de saída, que só a varredura da tela lê. Uma
TextureCopy crua não tem produtor (`TextureHandle` decodifica só `NativeBytes`). Desde 93627fa4,
textura cujo endereço é o de cor de um alvo ou a saída de uma transferência conhecida
(`PicaTargets::GpuWritten`) lê a imagem reservada 1x1 preta e transparente: o brilho azul parado da
Floresta Perdida sumiu, mas sem brilho, rastro nem bloom (a Tael escura).

## Conclusão e decisão

- `gfx_deko3d_pica_feedback.h` (`PlanPicaFeedbackTexture`): entre o alvo e a imagem de exibição no
  endereço da textura, vale o que a GPU escreveu por último (série global: draw com máscara de cor,
  clear, fill, transferência). Só serve se a textura e o produtor forem RGBA8, a textura for 2D sem
  mips, a largura for a mesma (linha de tiles) e o produtor cobrir linhas de tiles inteiras do
  começo da textura; transferência só com saída tiled->tiled sem inversão (critério do
  `PlanPicaTiledTransferResult` do upstream). Fora disso, o preto de 93627fa4.
- No console: o produtor é copiado (`dkCmdBufBlitImage` com `DkBlitFlag_FlipY`) para uma imagem
  derivada no tamanho da textura na escala de alocação do produtor; linhas além do produtor
  (256x256 sobre 256x200) ficam pretas e transparentes. Copia de novo só quando a série muda.
- Perfil de título `GpuFeedbackTextures`: desligado no OoT3D (fica exatamente o preto de hoje),
  ligado no genérico e no MM3D.

Rejeitado:
- Livro-razão de cópias do upstream (posse por byte, escritas do guest observadas, cópias raw): o
  deko3d não tem a infraestrutura; a decisão por endereço exato cobre o que o censo mostra.
- Amostrar a imagem do alvo direto, sem cópia: a orientação é a oposta à das texturas e o shader
  não sabe inverter só essas.
- Manter os bytes do guest no resto de uma textura mais alta (o upstream mantém): exigiria
  decodificar e compor; o resto no censo é memória que o jogo não usa como imagem.

## Em aberto (só o console prova)

- Se a Tael, o rastro e o brilho aparecem como no Azahar; se a orientação (FlipY) está certa.
- Custo: duas barreiras completas por cópia nova (no censo, ~12 texturas por quadro).
- Escrita do guest num endereço que a GPU escreveu (o upstream cita um asset IA8 carregado na
  memória do alvo do bloom ao trocar de cena): IA8 cai no preto; um RGBA8 carregado ali leria a
  imagem velha da GPU. Antes lia preto (também errado).
- Alvo alargado (16:9) como fonte da redução: o conteúdo alargado volta comprimido para a extensão
  nativa; deve casar com o desenho de tela cheia, não conferido.
