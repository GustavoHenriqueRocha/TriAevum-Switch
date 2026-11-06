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
