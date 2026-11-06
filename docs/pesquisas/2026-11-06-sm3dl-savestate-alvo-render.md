# SM3DL: savestate na W1-1 cai num "objeto" que é memória de alvo de render

Data: 2026-11-06 · Título(s): SM3DL (0004000000053F00), genérico (savestate, PICA no PC) ·
Continuação de `2026-11-06-sm3dl-stencil-sombra.md` ("Em aberto": savestate na fase quebra).

## Pergunta

Salvar na W1-1 (quadro de execução 3100) e carregar, até na mesma execução, leva a `MemoryFault` no
pc 0x3361CC (chamada virtual com vtable 0x29042104). Ao vivo, o jogo segue normalmente. O que o load
muda?

Repro (janela -O1, `~/oot3d-work/conv/sm3dl-o1`, roteiro `mapa_e_fase.json`):
`--savestate-action 3100:save:<dir>/S0.oot3dsav --savestate-action 3103:load:<dir>/S0.oot3dsav`.
Caiu em todas as rodadas com load (9 de 9). O `save:PATH` passou a valer neste commit (antes o save ignorava
o caminho, e dois saves no mesmo segundo viravam `-2`).

## Fontes

- Desmontagem de `code.bin` (`llvm-objdump --triple=armv6k`): 0x33619C percorre um contêiner
  (contagem em +0xC, vetor em +0x10) e, para cada objeto com byte +0x2C diferente de zero, chama
  vtable+0xC; vem de uma chamada virtual em 0x2B832C, num escopo de perfil "nw::lyt::Layout::…".
- Estados lidos com `scratchpad/mp.py`: contêiner 0x16C2C58C com 22 objetos; o 16 (0x17503E00) tem
  flag +0x2C = 4 e "vtable" 0x29042104 nos estados salvos em 3100 e em 3150 (que já trazem o overlay
  dos alvos; ver item 5).
- `oot3d_native_a32_savestate_diff S0 S2`, com S2 salvo logo depois do load (sem executar o guest):
  memória e estado do host iguais; só diferem as fases do agendador de apresentação.
- Trace temporário (fora do commit) em `HandleSvc` (thread, SVC, pc, lr, entrada e saída), no
  despertar dos que dormem, no sinal de vblank e com hash da memória por região e por página de 4 KiB.

## Conclusão

1. **O save e o load são fiéis (confirmado):** S0 → load → S2 dá memória e estado do host idênticos.
   Os registradores das threads cobrem todo o `GuestState`; as regiões salvas cobrem tudo o que o
   jogo mapeia.
2. **A sequência de SVCs depois do load é igual à ao vivo (confirmado):** mesma ordem de threads,
   mesmos argumentos e resultados, mesmo despertar e mesmo vblank, até a queda (28 SVCs).
3. **O que diverge é a memória dos alvos de render (confirmado):** já na primeira SVC depois do load,
   as únicas páginas diferentes do caminho ao vivo são as dos alvos do PICA: cor 64×64 RGB565 em
   0x233C3500 (e depth 0x233BF480), as três cores 64×64 em 0x23502500/0x23504580/0x23506600 e o depth
   0x23610880 (físico; virtual = físico − 0x0C000000), além de VRAM e DSP.
4. **O "objeto 16" mora dentro de um desses alvos (confirmado):** 0x23503E00 fica dentro de
   0x23502500–0x23504500 (64×64×2 bytes). 0x29042104 são dois pixels RGB565 (0x2104 e 0x2904).
   Ao vivo, o conteúdo dessas páginas depois do save já não é o do arquivo, e o laço não chama o
   objeto; restaurado, os bytes do S0 (flag 4) fazem o laço chamar a "vtable".
5. **Causa (confirmada no código e nos estados):** o `SaveState`
   (`oot3d_native_a32_window_savestate.cpp`) grava por cima da cópia salva da memória a imagem de
   **todo** alvo que o renderer conhece (`CapturePicaColorTargets`, cor e depth na extensão inteira).
   O alvo 64×64 em 0x23502500 foi abandonado pelo jogo: nenhum draw mirou nele nos vblanks
   6184–6186 (trace semântico) e a memória dele virou heap (contêiner do layout, objeto 16 em
   0x23503E00). O save escreveu os pixels velhos do alvo por cima do objeto vivo; depois do load,
   o laço de animação do layout (0x33619C, chamado de 0x2B832C) leu flag 4 e chamou a "vtable"
   0x29042104. As leituras de "objeto com lixo também ao vivo" vinham de estados salvos, que já
   traziam esse overlay.

## Decisão

- A fila de submissão (`Oot3dNativePicaSubmissionQueue`) carimba, a cada draw, a geração de escrita
  (`RangeWriteGeneration`, por página de 4 KiB) da faixa de cor e de depth do alvo.
  `RenderTargetWrittenSinceDraw(físico)` diz se a CPU escreveu nessa faixa depois do último draw;
  o save não grava overlay desse alvo (cor e depth separados). Sem carimbo ou sem geração certificada
  (ponteiro escapado), segue o comportamento antigo.
- Teste em `oot3d_native_pica_submission_tests` (alvo 8×8: não escrito → escrita da CPU dentro da cor
  → escrito, depth intacto → novo draw → da GPU de novo), visto falhando com o stub (`a CPU write
  inside the color target was not noticed`). Duas guardas, cada uma vista falhando por mutação
  (177/178).
- `PicaColorFormatBytes` foi para `pica_framebuffer_encode.h`, ao lado de `PicaDepthFormatBytes`, para
  o carimbo e o encode usarem a mesma regra de bytes por pixel.
- Antes disso, só o `--savestate-action FRAME:save:PATH` (guarda vista falhando 175/176), que tornou
  possível salvar e carregar com caminho fixo na mesma execução.

## Alternativas rejeitadas

- Comparar estados entre execuções diferentes: o mesmo quadro de execução cai em vblanks diferentes
  (6181 e 6187), então os estados não ficam alinhados.
- Estado de thread velho ou SleepThread extra: foi um alinhamento errado do trace; refeito com o
  despertar e o vblank no log, o caminho restaurado repete o vivo.
- Hipótese anterior "o host grava o alvo inteiro de volta na memória ao vivo, fora do
  viewport": ao vivo nenhum draw mira o alvo nesses vblanks; quem grava é só o save.
- Não salvar alvo nenhum: perderia a imagem de alvos vivos que o jogo lê depois do load (cópia,
  display transfer) antes de redesenhar.
- Gravar só as páginas não tocadas pela CPU: um alvo meio reusado já não é imagem de GPU; pular o
  alvo inteiro é mais simples e o próximo draw recria a posse.

## Resultado com a correção

Mesmo repro (save 3100, load 3103, janela -O1): rodada de 300 s com `save_count` 1, `load_count` 1,
13569 vblanks e nenhum `MemoryFault`; outra sob gdb foi até os 420 s (vblank 18720). Antes, 9 de 9
caíram. Dos 5 alvos capturados, só 384000 bytes viraram overlay (a tela de cima, 400×240 RGBA8): os
quatro 64×64 abandonados ficaram de fora. Folha em
`~/oot3d-work/conv/sm3dl/progresso/2026-11-06_0409_4b2ce887_savestate-fix/folha.png` (aviso "State
loaded" e a W1-1 seguindo, cronômetro 497 → 334).

## Armadilha de medição

Várias rodadas desta noite "saíram cedo" em exatamente 5897 vblanks (~114 s de janela): não era o
jogo. O comando em segundo plano do agente tinha o tempo-limite padrão de 120 s; o SIGTERM vira
`SDL_QUIT` e a janela fecha limpa, com `summary.json` normal. Debaixo do gdb (outro grupo de
processos) a mesma rodada foi até os 420 s. Rodadas longas precisam de tempo-limite explícito.
