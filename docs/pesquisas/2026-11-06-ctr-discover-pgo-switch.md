# OoT3D no Switch com o AOT descoberto: build instrumentada para o PGO

## Pergunta

O AOT descoberto (`discover_functions.py --external-from whole_aot_functions.json`,
ver `2026-11-06-ctr-discover-no-build.md`) dá o mesmo boot que o inventário revisado no
probe de host, mas o frame time no Switch nunca foi medido. Para comparar de igual para
igual com a melhor rodada (p7e, PGO `pgo5`), o Gustavo escolheu refazer o PGO da build
descoberta: build instrumentada, tour de coleta dele, build final com o perfil novo.
Este registro cobre a primeira etapa (build instrumentada enviada ao Switch).

## Fontes

- Procedimento: `docs/GUIA_PORT_3DS_SWITCH.md` §3.5 (PGO) e §3.6 (sharding/ccache).
- Configuração de referência: `~/oot3d-work/build-switch/CMakeCache.txt` (p7e) e a build da
  coleta do `pgo5` no Ryzen (`/home/gustavo/oot3d-work/build-pgo-gen/CMakeCache.txt` no WSL).
- Comando de geração do C++ do revisado: `SWITCH_PORT.md`, "Rodada do cabeçalho e do gerador".
- Entradas descobertas: `~/oot3d-work/conv/oot3d-discover/{aot_program.json,functions-nativos.json}`.
- Scripts fora do repo: `~/oot3d-work/discover-nro/derivar.py` e `build.sh`
  (cópia em `/root/agentes/discover-nro/` no Ryzen, irmão do `agentes/deko3d/build.sh`).

## O que foi feito

1. **Contrato de produto para o fecho descoberto** (`derivar.py`). O
   `whole_aot_product_manifest.json` fixa o SHA da seleção revisada e as contagens do
   inventário, então a build de produto recusaria o descoberto. O contrato derivado troca só
   a identidade (SHA da seleção, `inventory_sha256`, `boundary_audit_sha256`) e as contagens;
   todas as checagens estruturais continuam (CFG fechado, 0 blocos sem dono, 0 entradas A32
   residuais, os 3 nativos do host como únicas fronteiras). Auditoria: ok, 14579 funções
   compiladas, 3 fronteiras, 162942 blocos, 162933 entradas de despacho, 924535 instruções
   únicas, 1135616 emitidas, 0 residuais.
   - Duas diferenças de forma, sem efeito no código guest:
     - `executable_size` do programa vai de 3971496 (tamanho exato do text, como a descoberta lê)
       para 3973120 (páginas do ExHeader, como o `generate_whole_aot.py` do revisado e o audit
       exigem). Só muda o intervalo de dobra de literais: 1624 bytes de preenchimento no fim
       da página do text, somente leitura.
     - Os 3 externos ganham nome e motivo da seleção revisada (`validated host override`);
       as entradas são as mesmas.
2. **C++ gerado** com as mesmas opções do `cpp-lean`: `--shards 256 --shard-strategy affinity
   --native-vfp-aarch64 --vfp-zero-aware --lean-stack --guest-overrides
   --giant-function-instructions 1000` + `apply_guest_overrides.py`. Saída em
   `~/oot3d-work/gen/cpp-descoberto-lean` (montado), 260 arquivos, 411 MB, 19 substituições nativas.
   - Conferência do gerador: o mesmo comando com a seleção revisada reproduz o `cpp-lean` do
     p7e em 254 de 259 arquivos. Os 5 shards diferentes (008, 015, 050, 217, 242) diferem só
     pelo prólogo das 5 substituições desligadas em 2e28914e (o `cpp-lean` é de antes e tem 24).
     A build descoberta segue a decisão atual (19).
3. **Build instrumentada no Ryzen** (`build.sh config generate` + `build.sh build`): fonte
   `bab13da0` (deko3d), Release `-g -O2 -DNDEBUG` (shards `-O3`, como sempre), produto ON,
   `LEAN_STATS=ON`, sem biblioteca pré-compilada, `-fprofile-generate=sdmc:/switch/oot3dre/pgo
   -fprofile-prefix-path=/home/gustavo/oot3d-work/build-switch-descoberto`, 12 jobs,
   `FETCHCONTENT_FULLY_DISCONNECTED` com os `_deps` copiados do build do deko3d (o clone do
   imgui travou 15 min na primeira tentativa).

## Resultado

- Build: 1027 etapas, 256 shards compilados, 53 min 50 s no Ryzen (12:06 → 13:00, 12 jobs,
  ccache frio para esses shards). Auditoria no build: `14579 compiled functions, 3 host
  boundaries, 0 residual A32 entries`.
- NRO instrumentada: 279565887 bytes, md5 `bfd7630fdc199fff6f7498d702959380` (igual no
  `build/`, no `out/` do Ryzen e na cópia do notebook). Rotulada `descoberto-pgogen`
  (`rotular_nro.py`, só troca a ASET): 279560205 bytes, md5 `d1b8b2b158e55d3ea0c9b81e7e98408d`.
- Cópias: `~/oot3d-work/nro-descoberto/` (as duas NROs e `nro.md5`).
- Switch (FTP 192.168.3.54:5000, 06/11 13:20): backup dos saves antes
  (`~/oot3d-work/saves-backup/20261106-131938`, 36 arquivos). A pasta `pgo/` tinha 258 `.gcda`
  iguais byte a byte ao `~/oot3d-work/pgo5` (cópia também em
  `~/oot3d-work/pgo-switch-antes-descoberto/`); foram apagados (0 falhas, pasta vazia depois).
  NRO enviada como `/switch/oot3dre/oot3d_native_game.descoberto-pgogen.nro`, 279560205 bytes
  no Switch e local, 69,8 s a ≤5 MB/s. A NRO em uso (`oot3d_native_game.nro`, 149216374 bytes)
  ficou. No Switch os caminhos do jogo são fixos (`sdmc:/switch/oot3dre/...`,
  `oot3d_native_game.cpp:188`), então o nome diferente da NRO não muda config nem saves.

## A NRO não abre (06/11, 13:2x)

- **Observado:** o Gustavo disse que a `descoberto-pgogen` não abre. `boot-status.txt` do SD
  (baixado em `~/oot3d-work/bug-cancao-tempo/20261106_132426/`): `status=failed`,
  `detail=the title module does not start a block at the TopScreen sites 0x002D84C8 …
  0x00478EBC; rebuild it with whole_aot_observed_sites.json`.
- **Esperado:** abrir como a instrumentada do `pgo5` (a coleta de 22/10 gravou os 258 `.gcda`).
- **Diagnóstico (confirmado):** `whole_aot_program.py` começa um bloco em cada um dos 47 pcs de
  `whole_aot_observed_sites.json` (29d03751; ganchos e retomadas do TopScreen) quando recebe
  `observed_sites`. O `generate_whole_aot.py` do revisado passa o catálogo; o
  `discover_functions.py` chama `build_program` sem ele. Conferido nos JSONs: faltam 0 dos 47
  pcs nos blocos do `gen/aot_program.json` (revisado) e faltam os 47 no programa descoberto. A
  checagem que recusa o módulo (`oot3d_native_a32_window_setup.cpp:732-761`) só existe com
  `OOT3D_WHOLE_AOT_PRODUCT_MODE`, por isso o boot probe de host (produto OFF) passou. A causa é
  o programa descoberto, não o `-fprofile-generate`: a instrumentada do revisado abria.
- **Correção (f03290ef):** `discover()` passa
  `observed_sites=load_observed_sites(DEFAULT_OBSERVED_SITES, code)`. O catálogo é amarrado ao
  SHA do code.bin do OoT3D, então os genéricos não mudam. Teste
  `test_observed_sites_of_this_code_image_start_blocks` visto falhar antes (`0x100004 not
  found`), guarda nova vista falhar por mutação (186/187). O `derivar.py` agora recusa o
  programa se algum dos 47 pcs não começa bloco; com o programa antigo ele lista os 47.
- **Rodada 2:** descoberta no Ryzen (2 min 24 s, capstone 5.0.7): 14582 funções, 924535
  instruções, 0 blocos sem dono; blocos 162942 → 162989 (+47), seleção idêntica (mesmo md5).
  Faltam 0 dos 47 pcs nos blocos do programa e no despacho do C++ gerado. Dos blocos do
  revisado, 16 não começam bloco no descoberto (0x00161654 … 0x0048A4A8, as entradas do
  inventário que caem no meio de bloco); nenhum aparece no código do runtime (grep em
  `native_game_runtime`, `runtime` e nos JSONs do `native_a32_runtime`).
- Build instrumentada 2: 28 shards recompilados, 3 min (13:38 → 13:41); auditoria `14579
  compiled functions, 3 host boundaries, 0 residual A32 entries`. NRO 279582271 bytes, md5
  `1e4cb56366cb88b70fc5f5cbaea33025` (build e out iguais); rotulada 279576589 bytes, md5
  `75ba328b7be9497c1a17e5f5015a2a0c`. A rodada 1 ficou em `~/oot3d-work/nro-descoberto/v1-nao-abre/`.
- Envio 2 (por cima do mesmo nome): o primeiro caiu no meio às 13:4x (FTP fechou; ficaram
  69967792 bytes no SD); o segundo, às 13:47, fechou em 58,6 s: 279576589 bytes no Switch e
  local. `pgo/` vazia. Não deu para confirmar no Switch que a NRO abre: depende do Gustavo.

## Etapa final preparada (não rodada)

- `~/oot3d-work/discover-nro/etapa-final.sh`: baixa os `.gcda` (5 MB/s) para
  `~/oot3d-work/pgo-descoberto-<data>/`, confere tamanhos contra a listagem do SD e os 258 nomes
  esperados (um por `.cpp` do C++ instrumentado + substituições; para se faltar ou se houver
  `.gcda` de outra build), confere por md5 os 260 arquivos do C++ no Ryzen contra
  `cpp-instrumentada.md5`, roda a build `use` (`VAR=use`, build dir próprio
  `build-switch-descoberto-use`, `PROFILE_FILE=/home/gustavo/oot3d-work/pgo-descoberto`
  montado de `agentes/discover-nro/pgo`), conta `coverage-mismatch`, rotula `descoberto-pgo`,
  envia como `oot3d_native_game.descoberto-pgo.nro` conferindo o tamanho e apaga a
  `descoberto-pgogen`. Dry-run com pasta falsa: passa com os 258; para com um `.gcda` do
  `cpp-lean` e com um faltando. Build dir `use` já configurado (258 regras com `-fprofile-use`,
  launcher `ccache-pgo.sh`).
- `~/oot3d-work/discover-nro/tour.sh enviar|baixar`: roteiro do tour27 (o da p7e) em
  `lab/script.jsonl`; depois baixa para `bench/descoberto-pgo` e roda `lab_tour_report.py` na
  p7e e na nova. A lista de `config/` do SD é a mesma do backup de 05/11 15:55 (o tour p7e foi
  às 10:55 de 05/11; não há backup entre os dois).

## Decisão

- PGO refeito, não reaproveitado: o `pgo5` é do C++ revisado; com outro corte em funções os
  shards mudam e o perfil não casa (§3.5, "o que invalida").
- Build de produto (ON), igual à p7e, com contrato derivado. Rejeitado: produto OFF, que
  liga o runtime A32 decodificado e deixaria a comparação desigual.
- Rejeitado editar o `whole_aot_product_manifest.json` do repo: ele continua sendo o
  contrato do revisado, que segue padrão.

## Em aberto

- Coleta (~15 min, Gustavo), build `use` com a pasta da coleta e o tour de comparação com a p7e.
- **O C++ da p7e ainda liga as 5 substituições desligadas em 18/10.** O `gen/cpp-lean` que o
  `agentes/deko3d/build.sh` compila (o mesmo do notebook, manifesto com 24 `guest_overrides`) é
  de antes do 2e28914e e nunca foi regenerado; a descoberta tem 19. Se a comparação ficar
  apertada, refazer a revisada com 19 para separar os efeitos. Não foi conferido no binário
  da p7e se os prólogos estão lá.

## Rodada 3: lab desligado (06/11, 14:06)

- Observado: com a rodada 2 no Switch, o roteiro `lab/script.jsonl` (tour de 49 áreas, o mesmo da
  coleta do pgo5) não rodou; o arquivo ficou intacto e não houve `acks.jsonl`.
- Causa (confirmada no CMakeCache): a build descoberta saiu com `TRIAEVUM_DEVELOPER_TOOLS=OFF`; a
  `build-switch` da p7e tem `ON`, e o canal lab só existe com ela
  (`oot3d_native_a32_window_setup.cpp:1058-1093`). Diff completo das opções contra a p7e: só essa
  difere, além de programa, seleção, manifesto, C++ e perfil.
- Correção: `-DTRIAEVUM_DEVELOPER_TOOLS=ON` no `config` do `build.sh` (generate e use). No Switch a
  opção não toca os shards (CMakeLists.txt:202), então a build levou 73 s e o C++/`.gcda` não mudam.
- NRO: md5 `17b70bb2…` (rotulada `cb092d8a…`, 279797773 bytes, conferido no Switch), mesmo nome
  `oot3d_native_game.descoberto-pgogen.nro`; `pgo/` vazia; roteiro do tour de 49 áreas no lab.
