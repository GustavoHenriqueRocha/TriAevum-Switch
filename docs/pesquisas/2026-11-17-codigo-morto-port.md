# Código morto no código do port (native_game_runtime), 17/11

## Pergunta
Quais funções escritas à mão em `tools/oot3d/native_game_runtime/` o NRO do Switch nunca usa
(pedido do Gustavo: "levantamento de todas as funções que não são usadas de fato no game").

## Fontes / como reproduzir
- Religação do `oot3d_native_game.elf` 1.0.1 (integra-john-2 431d070d) no Ryzen com
  `-Wl,--print-gc-sections` (o devkitPro já compila com `-ffunction-sections`, e o `switch.specs`
  liga com `--gc-sections`): o comando de ligação sai de `ninja -t commands oot3d_native_game.elf`.
- Total descartado: 42 321 seções; 14 086 de arquivos do port; 169 funções (sem instâncias de
  template de biblioteca) em 50 arquivos de `native_game_runtime`.
- Cada uma conferida com `git grep -w <nome>` no repo inteiro (PC, ferramentas e testes incluídos).

## Conclusão
"Descartada pelo linker" não é "morta": com -O2 a função é inlinada nos chamadores e a cópia
externa sobra. Das 169:
- **42** são destrutores implícitos e cópias inlinadas (nada a apagar).
- **100** são chamadas por outro arquivo (PC, host, ferramentas, painéis), cópias `unlikely.` do
  nlohmann ou inlinadas: vivas.
- **11** são chamadas só dentro do próprio arquivo (inlinadas): vivas.
- **12** só os testes usam, de propósito (ganchos de teste, ex. `NativeOriginalHashes`): ficam.
- **`__wrap__malloc_usable_size_r`**: ligado por `--wrap` (game.cmake:859); o jogo não chama
  `malloc_usable_size` hoje, mas o wrap protege o guard alloc se alguém chamar: fica.
- **3 sem nenhuma referência no repo** (morto confirmado, ~15 linhas):
  - `PublishNativeActorInteractions` (oot3d_native_actor_interactions.cpp:106, snapshot público
    v0.6.0); só citado em `docs/TRIAEVUM_PARALLEL_PRESENTATION.md:57`.
  - `NativeRenderCommandStream::ExecutedCommands` (render_command_stream.cpp:85, coccofresco);
    o campo `mExecuted` continua usado pelas estatísticas.
  - `RomImport::SupportedRevisions` (rom_import.cpp:656, John); `SupportedRevisionsText` é a usada.

## Decisão
Remover só as 3: aprovado pelo Gustavo e feito em a2d91309 (backup
`~/Documentos/triaevum-codigo-morto-20261117-1344.tar.gz`, testes de host e NRO antes e depois).

## Em aberto
- `three_ds_recomp_runtime/` (9 414 seções descartadas) ainda não foi classificado.
- Funções vivas mas nunca executadas no console: **não dá para medir com os dados que existem**.
  O PGO só instrumenta `oot3d_native_whole_aot` (os 256 shards gerados + `oot3d_native_guest_overrides`;
  `CMake/oot3d/native_runtime.cmake:531`, `~/oot3d-work/pgo5`), nenhum arquivo escrito à mão além
  desse. Medir exigiria um build com cobertura do runtime e um tour no console; não feito (custo alto,
  e "não rodou no tour" não prova que é morto: menus e erros raros também não rodam).
