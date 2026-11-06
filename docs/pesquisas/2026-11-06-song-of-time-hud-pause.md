# Song of Time: HUD some, B em cima do A e pause não abre (NRO saves-ff4fc3b8)

## Pergunta

Relato do Gustavo (06/11, Switch, NRO de produto saves-ff4fc3b8):
- na aula da Song of Time ele errou as notas e mesmo assim o Link tocou a canção;
- no fim da cutscene o HUD sumiu inteiro e o ícone B ficou desenhado em cima do A;
- o inventário/pause não abria;
- o Link andava e o jogo seguia rodando.
Ele não salvou depois do bug.

## Fontes

- Logs do Switch: `~/oot3d-work/bug-cancao-tempo/20261106_132426/`. O `oot3d_native_game.log` só traz o
  aviso de mapeamento de controle; o `perf.txt` não marca cena. Não explicam o bug.
- Saves: `~/oot3d-work/saves-backup/20261106-131938/` (backup antes de qualquer envio).
- Diagnóstico de mesa (agente, sem rodar o jogo). Nenhum arquivo de `tools/oot3d/ui_topscreen` mudou
  entre ff4fc3b8 e 014e0dee, então o HEAD vale para a NRO.
- `docs/TRIAEVUM_POST_1C_REGRESSION_AUDIT.md:148` e
  `docs/TRIAEVUM_TOPSCREEN_PRACTICE_SAVE_FIXTURES.md:89-135`: o único aprendizado de canção já
  qualificado foi a Song of Storms (ator EnFu, notas certas, desktop). Nunca se testou uma aula
  guiada por cutscene nem o caminho de erro.
- Web: nenhum relato do bug com o mod.

## Mecanismos que explicam cada sintoma (código confirmado, gatilho não)

| sintoma | onde | condição |
|---|---|---|
| HUD some | `oot3d_top_screen_mod_profile_hud.cpp:180-241` (`ReadTopScreenHudCompositorGate`) | gate falso se um dono concorrente (`0x0050446C+0x18`, `0x005066F8+0x34`, `0x00506CB0+0x38`, `0x0050A508+0x28`), `play+0x318C`, `0x00588958+0x4E4`, `play+0x7F40` ou `player+0x224E` for ≠0, ou se `play+0x2E30` for 0 |
| pause não abre | `oot3d_top_screen_pause_transaction.cpp:96-128` | START exige `0x0050AF68==2` e os predicados `0x002F43E8()`/`0x00374BE8(play,3)` em 0; senão o pedido é descartado em silêncio e expira em 8 refreshes |
| B sobre o A | `oot3d_top_screen_mod_profile_quest.cpp:56-63` | quad com centro em x 330–398 e y>150 é recentrado em (288,24), a posição do A |

Leitura unificada (hipótese, sem suporte): um estado nativo de mensagem, ocarina ou cutscene ficou
vivo depois da aula, e os três leitores reagem a ele. O "errou e tocou mesmo assim" vem antes de
tudo isso. O nosso código não escreve no caminho das notas, então as candidatas são:
- notas diferentes chegando ao guest (mapeamento ou máscara do D-pad, `oot3d_native_a32_window_frame.cpp:2175-2179`);
- o runtime de texto da ocarina chamando o gerador de mensagens nativo no meio da aula (`oot3d_top_screen_ocarina.cpp:128`, `:287`);
- erro de AOT ou de gancho no caminho de cutscene.

## Conferido

- Seis correções do 3.0 feitas no `origin/main` em 17/10 contra o oráculo Azahar não estão na
  deko3d (`git merge-base --is-ancestor`): fc5ddfa6, 5da8aa7c (grupo A), d8ffc8f1 (ícone B),
  6f478bcc (HUD de cutscene), ac63eb21, 94b293ea. Nenhuma trata aprendizado de canção. São
  diferenças de desenho que podem mudar o sintoma "B sobre o A".
- Leitura do backup pelo agente (layout de `tools/oot3d/soh_save_to_oot3d.py`, sem suporte até eu
  conferir): nenhum arquivo é "antes do evento" (criança, 3 pedras, sem a Song of Time). O save00
  é criança só com a Kokiri Emerald; o save03 e o save04 são MQ, adultos, com a canção.

## Em aberto

- Qual arquivo/slot ele jogava: o backup não tem o estado "antes".
- Reprodução, da mais barata à mais cara:
  1. savestate no Switch antes do fosso e outro com o HUD travado (este dá para ler offline);
  2. Eden no notebook com a mesma NRO e injeção de notas erradas (`lab/script.jsonl`);
  3. oráculo Azahar com o 3.0 original, para saber se o bug é do mod;
  4. build desktop com timeline.
- Trazer para a deko3d as correções do 3.0 do main: decisão do Gustavo.
