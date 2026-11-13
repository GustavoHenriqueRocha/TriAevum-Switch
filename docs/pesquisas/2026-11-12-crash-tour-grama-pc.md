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
Travar o warp de desenvolvimento durante a morte e a transição (seção abaixo); continua em
aberto quem zerou aquela palavra.

## Causa provável (relato do Gustavo) e correção
- Relato do Gustavo (observado no console): o Link estava morrendo (animação de morte / game over)
  quando o tour do lab mandou um warp; o jogo ficou num estado estranho e caiu depois em 0x2C09D4.
- Confirmado no código: o warp de desenvolvimento (`DevWarpRuntime::Advance`) só esperava por
  "gameplay" (`gameMode == 0` e sem cutscene de título) e por `PlayState+0x5C32 >= 0`. Durante a
  morte e o game over o `gameMode` continua 0 (mesmo que no N64), então o warp passava. O teste
  sintético novo (`DyingOrTransitionDefers`) viu isso: `requested ... health=0`.
- Correção: o pedido pendente fica em `Waiting` enquanto
  - a vida (save context +0x44, s16 em dezesseis avos de coração) é <= 0. Fonte do campo:
    `oot3d_damage_multiplier.h:6-8` (Health_ChangeBy), `oot3d_top_screen_gameplay_action_consumer.cpp:85`
    (portão `health != 0` do TopScreen) e `tools/oot3d/soh_save_to_oot3d.py:86`;
  - o gatilho de transição `PlayState+0x5C2D` é diferente de 0. Fonte:
    `decomp_support/scripts/build_transition_request_helper_semantics.py:242-247` (os helpers
    gravam o gatilho 0x14 ali; 0x002E2E60 trata enquanto não é zero e zera depois).
  Cada mudança de motivo sai uma vez no log: `TRIAEVUM_DEV_WARP deferred target=... reason=link_dying|
  transition|not_gameplay`. A op `warp` do lab continua com o limite de 3600 quadros: se o Link não
  voltar a ter vida (tela de game over parada), ela responde `timeout` com o motivo e o tour segue.
- Teste: `oot3d_dev_warp_tests` (`DyingOrTransitionDefers`), falhou antes ("a warp started while Link
  was dying (health 0)") e passa depois (38 checagens); guarda em `tests/switch/test_bug_guards.py`.

## Em aberto (depois da correção)
- Hipótese, sem prova: que o warp durante a morte tenha zerado a palavra em 0x08723838. Falta repetir o
  tour com a trava e ver se o crash some.
- Revive por fada: a vida sobe aos poucos enquanto a fada ainda age; o warp pode sair nesse intervalo.
  O bit de morte do Player (N64 `PLAYER_STATE1_DEAD` 0x80, stateFlags1 em +0x1710 no OOT3D) não foi
  conferido no código do OOT3D e não foi usado.
- Fade de entrada da cena nova: não se sabe se o OOT3D mantém +0x5C2D diferente de 0 nele (no N64 é
  `TRANS_TRIGGER_END`); `PlayState+0x7F12` (logado como `transition_state`) ficou sem semântica conferida.
- Um pedido adiado que deu `timeout` no lab continua pendente e sai quando o estado ficar seguro
  (ou é substituído pelo próximo warp do tour).

## Em aberto (antes da correção)
- Repetir o tour (mesmo save) para ver se o crash e os travamentos se repetem.
- Simbolizar a pilha dos travamentos: `nm` no ELF do Ryzen (`build/oot3d_native_game.elf`, 17:43)
  deu mapeamento sem sentido (thread de render "parada" em `_mesa_MapBufferRange`); falta a base certa.
- Se repetir: conferir os STM por span (`WriteWordsFast`) com `tools/oot3d/native_a32_runtime/memory_lean_diff`
  e, se preciso, um build com o C++ da p7e para separar gerador × runtime.
