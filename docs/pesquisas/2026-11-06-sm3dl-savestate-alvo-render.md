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
  flag +0x2C = 4 e "vtable" 0x29042104 **também ao vivo**, em 3100 e em 3150.
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

## Hipótese principal (sem prova ainda)

A memória do guest nesses alvos não é o que a GPU real deixaria lá. A suspeita é que o host grave
o alvo 64×64 inteiro de volta na memória, inclusive áreas que a GPU real não toca (fora do
viewport/scissor). Ou então que capture/materialize de forma diferente do que o caminho ao vivo
mostra ao guest. Isso estragaria um objeto do heap colocado logo depois da área desenhada. Na
mesma linha: o contêiner do layout aponta para 0x23503E00, que o jogo não usaria como alvo se a
área desenhada fosse 64×64 inteira.

Como provar: trace semântico (`--pica-semantic-trace`) dos draws e clears que miram 0x23502500
(viewport, scissor, memory fill), e os escritores de memória do host (materialização de cópias
adiadas, writeback de alvo) com endereço e tamanho.

## Decisão

- Corrigido só o `--savestate-action FRAME:save:PATH` (guarda nova, vista falhando 175/176), que
  tornou possível salvar e carregar com caminho fixo na mesma execução.
- O bug do savestate continua aberto, agora com a região exata.

## Alternativas rejeitadas

- Comparar estados entre execuções diferentes: o mesmo quadro de execução cai em vblanks diferentes
  (6181 e 6187), então os estados não ficam alinhados.
- Estado de thread velho ou SleepThread extra: foi um alinhamento errado do trace; refeito com o
  despertar e o vblank no log, o caminho restaurado repete o vivo.
