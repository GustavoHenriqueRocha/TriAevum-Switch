# Importação do dump do jogador no console (OoT3D)

Data: 2026-11-08 · Título(s): OoT3D · Commits: os desta série (pr/dump-import), portados do fork
JohnUzoka/TriAevum-Switch a8695b96, 1b2af4ad, 19a8ea64

## Pergunta

Como trazer a importação do `.3ds`/`.cci` decriptado no próprio console (pedida na issue
JohnUzoka/TriAevum-Switch#1) para esta árvore, que agora também roda outros títulos de 3DS
(`ctr_title_profile.h`, `tools/ctr/ctr_convert.py`, pastas `sdmc:/switch/triaevum/<id>/`), sem
mudar o OoT3D por acidente?

## Fontes

- Fork: `tools/oot3d/native_game_runtime/oot3d_rom_import.{h,cpp}`, `oot3d_switch_rom_import.cpp`,
  `oot3d_switch_console_screen.h` (a8695b96, 1b2af4ad, 19a8ea64).
- `tools/oot3d/native_a32_runtime/build_process_manifest.py`: grava `process.title_id` desde 5893b105;
  `oot3d_native_a32_process_image.cpp:128-135` lê o campo (opcional, inteiro sem sinal).
- `ctr_title_profile.h`: `IsOot3dTitleId` (0x0004000000033400/33500/33600), `SelectCtrTitleProfile`.
- `oot3d_native_game.cpp`: o argv padrão do NRO (argc <= 1) é o do OoT3D em `sdmc:/switch/oot3dre`.
- libnx `nx/source/runtime/devices/console.c` (`con_write` lê `char` a `char` e chama
  `consolePrintChar`; fonte padrão de 256 glifos): sem decodificação de UTF-8.
- Paridade do manifesto: entradas sintéticas (ExHeader com program id 0x0004000000033500, code de
  3 páginas, RomFS IVFC e visão Level-3) passadas ao `build_manifest` do Python e ao
  `BuildProcessManifest` do C++; script em `~/oot3d-work/pr-dump-import/parity/`.

## Conclusão

- O núcleo do fork gerava o manifesto sem `process.title_id`. Nesta árvore isso ainda cai no perfil
  OoT3D (manifesto sem o campo = OoT3D antigo), mas o arquivo deixava de ser igual ao do PC. Com o
  campo, o JSON do C++ sai byte a byte igual ao do Python nos dois casos (IVFC, `romfs_service_offset`
  4096; Level-3, 0). Sem o campo, o diff era só a linha do `title_id`.
- O importador só conhece receitas do OoT3D; o `game/` de `switch/oot3dre` divide a pasta com o save
  do Zelda. Outro título não tem o que fazer ali.
- O console do libnx não desenha UTF-8: um "ç" viraria dois glifos errados.
- Importação real no PC desta árvore (08/11, `oot3d_rom_import_tests --import` com o cartucho USA
  Rev 1 do John, build Debug do host): receita `oot3d-usa-input-canonical-eur-ef210566`, code
  16a6b0aa…, RomFS dae62037… (473522176 bytes, 1944 arquivos, 2510 registros QM), 53 s em Debug.
  O `build_process_manifest.py` rodado sobre os arquivos gerados dá o mesmo manifesto, byte a byte
  (só os três caminhos trocados pelo nome do arquivo), com `title_id` 0x0004000000033500.

## Decisão

- O manifesto do console grava `process.title_id` (program id do ExHeader, 0x200), como o Python.
- Dump de outro título é recusado pelo program id (`IsOot3dTitleId`) antes de qualquer hash, com a
  indicação do `tools/ctr/ctr_convert.py`.
- A importação só roda no lançamento padrão do NRO (argc <= 1); lançamento com argumentos próprios
  não passa por ela. `config/no_rom_import` desliga tudo.
- Textos da tela e mensagens de erro em pt-BR; na tela passam por `RomImport::ConsoleAscii` (sem
  acento, '?' fora do Latin-1).
- Uma tela só, destruída ao sair de `RunSwitchRomImport()`. A tela compartilhada do fork
  (`StartupScreen`, 9694b6e9) e a correção do stdout da segunda tela (fe7ccbcf) existiam por causa
  da oferta de texturas do HUD, que não está nesta árvore.

## Alternativas rejeitadas

- Importar outros títulos para `sdmc:/switch/triaevum/<id>/game`: não há receita nem verificação por
  hash para eles; o caminho deles é o conversor do PC, e o NRO não abre outro título sem argumentos.
- Fonte com acentos no console (`consoleSetFont`): uma fonte nova só para uma tela de um minuto.
- Linkar o teste com `oot3d_native_a32_process_image` para ler o manifesto com o leitor do runtime:
  puxa o `oot3d_native_a32_process` inteiro. O teste lê com `nlohmann_json` e confere o tipo sem
  sinal que o leitor exige.

## Em aberto

- Tempo no console desta árvore não foi medido: o número (cerca de um minuto num Switch Lite) é do
  fork. No PC, o fork mediu cerca de 9 s para o USA Rev 1, byte a byte igual ao pipeline do PC.
- A tela no console (libnx) não foi rodada nesta árvore: compila com as flags do Switch
  (`swc.py`), mas o teste no aparelho é do fork.
