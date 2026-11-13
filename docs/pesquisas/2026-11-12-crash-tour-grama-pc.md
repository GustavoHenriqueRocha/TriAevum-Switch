# Crash no tour da build grama-pc (A32 MemoryFault em 0x2C09D4)

Data: 2026-11-12 · Título: OoT3D · Branch: `integra-john-2` (e65d9a30, NRO md5 37142b27)

## Pergunta
O tour da build grama-pc (máscara da grama, lâmina do PC) caiu depois de ~2,6 min de jogo. Foi a
grama? Foi a mescla do John? Já tinha acontecido?

## Fontes
- `~/oot3d-work/bench/grama-pc-crash/boot-status.txt`: `native A32 process faulted: native A32
  dispatch exited unexpectedly: kind=4 pc=0x002C09D4 ... r0=0871EBA4 r1=08722840 r3=00000000`.
- `kind=4` = `ExitKind::MemoryFault` (`tools/oot3d/native_a32_runtime/upstream/recomp/a32_runtime.h:83`).
- C++ gerado: `~/oot3d-work/gen/cpp-john/oot3d_whole_aot_shard_107.cpp`, função
  `Execute_FUN_002c0998_002C0998`: `r1 += 0x4000; r3 = ldr [r1 + 0xFF8]; r3 = ldrb [r3]`.
- `hang.txt` da mesma sessão (mesmo `nro_text` 0x589287000): travamentos de 20 s (uptime 243 s) e
  11 s (585 s), thread do jogo parada em `frame_end` com a mesma pilha; a sessão anterior teve um de 4,4 s.
- `grep -rl 002C09D4` em todos os `~/oot3d-work/bench/*`: só este tour.

## Conclusão
- Confirmado: o jogo leu um ponteiro nulo. A palavra em 0x08723838 (r1 + 0xFF8, r1 = 0x08722840
  depois do +0x4000) valia 0 e a instrução seguinte leu um byte no endereço 0.
- Confirmado: o bloco que falhou é traduzido igual no C++ da p7e (`gen/cpp-lean`) e no do John
  (`gen/cpp-john`). Na função toda, as diferenças são só 4 STM/VSTM que passaram a usar
  `WriteWordsFast` (com volta ao caminho antigo se falhar), em outros blocos.
- Sem suporte: que a grama tenha causado isso. Ela roda no desenho do deko3d e no fim do quadro,
  não na thread do jogo.
- Primeira vez em todos os tours salvos.

## Decisão
Nenhuma ainda: falta saber se repete e quem zerou aquela palavra.

## Em aberto
- Repetir o tour (mesmo save) para ver se o crash e os travamentos se repetem.
- Simbolizar a pilha dos travamentos: `nm` no ELF do Ryzen (`build/oot3d_native_game.elf`, 17:43)
  deu mapeamento sem sentido (thread de render "parada" em `_mesa_MapBufferRange`); falta a base certa.
- Se repetir: conferir os STM por span (`WriteWordsFast`) com `tools/oot3d/native_a32_runtime/memory_lean_diff`
  e, se preciso, um build com o C++ da p7e para separar gerador × runtime.
