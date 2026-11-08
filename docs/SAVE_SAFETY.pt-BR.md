# Saves à prova de perda (backend de pasta: PC e NRO do Switch)

Data: 2026-11-05 · Título(s): OoT3D (compatibilidade), genérico · Commits: nenhum (branch `saves-seguros`, worktree `.claude/worktrees/saves-seguros`, sobre `531bf3c0`)

## Pergunta

O usuário pediu para melhorar o sistema de saves "pra não perder nada". O save do Zelda no Switch
fica em `sdmc:/switch/oot3dre/savedata`: `save00.bin`, `save03.bin` e `save04.bin` de 5340 bytes e
`system.dat` de 34 bytes, sem `.format`. Que defeitos do runtime de FS podem perder ou corromper
esse save, e como fechá-los sem mudar nada do caminho que o OoT3D usa hoje?

## Fontes

- `docs/pesquisas/2026-11-05-fs-switch-generico.md` (investigação verificada da manhã). O NRO usa o
  mesmo backend de pasta do PC (`hostConfig.Filesystem == nullptr`, `SaveDataDirectory =
  sdmc:/switch/oot3dre/savedata`), e o rename do Horizon por cima de arquivo existente dá EEXIST.
- `tools/oot3d/native_game_runtime/oot3d_native_a32_ctr_host_fs.cpp` (antes desta mudança):
  - o Write de save abria um `std::fstream` direto no arquivo e escrevia no lugar (write-through);
  - o Flush não fazia nada;
  - o FormatSaveData fazia `remove_all(SaveDataDirectory)`;
  - o GetFormatInfo respondia NotFormatted sem `<save>.format`;
  - o `.format` era `SaveDataDirectory.string() + ".format"` (com '/' final ia para dentro do save);
  - o DeleteFile usava `weakly_canonical`.
- `oot3d_native_a32_ctr_host_state.cpp`: o `file_path` e o `root` do archive iam para o savestate
  como caminho absoluto do host, e o `validFilePath` só aceitava caminhos dentro do save.
- Guardas existentes em `tests/switch/test_bug_guards.py` ("rename por cima dá EEXIST" no
  controle, nas preferências e nos savestates).
- `docs/OOT3D_NATIVE_APPLICATION_16_9_60FPS_STRATEGY.md` ("Creazione File"): ao salvar, o OoT3D faz
  CreateFile, write, resize, flush e `ControlArchive`.
- `oot3d_native_a32_ctr_host_tests.cpp`, bloco "SaveData File:Write": o teste fixava o contrato
  write-through (bytes no disco logo depois do Write).
- Backup do save real (só leitura, conferido com `sha256sum -c`): `~/oot3d-work/backup-savedata-20261105-1555`.
  Tem também `save00.antes-soh.bin`. Os testes usam dados sintéticos com o mesmo layout.

Reprodução (PC, build de teste em `~/oot3d-work/build-saves-seguros`, cópia do
`build-ctr-title-smoke` reconfigurada para o worktree):

```
nice ninja -j2 oot3d_native_a32_ctr_fs_tests oot3d_native_a32_ctr_host_tests
./oot3d_native_a32_ctr_fs_tests [NomeDoTeste ...]   # sem argumento roda todos
./oot3d_native_a32_ctr_host_tests
python3 tests/switch/test_bug_guards.py
```

## Conclusão

Encontrei seis defeitos que perdem dado ou impedem recuperar. Todos foram confirmados por teste
que falhou no código antigo; o 5 foi confirmado por análise e pela guarda.

1. **Escrita no lugar.** O jogo escreve o save em pedaços (o teste usa 1 KiB). No código antigo
   cada pedaço já chegava ao `save00.bin`, então uma queda no meio deixava o arquivo metade novo,
   metade velho. Falha antiga: `a partial write reached save00.bin before Flush/Close (not atomic)`.
2. **Nenhum backup.** Uma escrita ruim (bug do runtime, queda ou save corrompido pelo jogo)
   substituía a única cópia.
3. **Formatação apaga.** Save legado sem `.format` respondia NotFormatted. Um jogo que pergunta e
   formata (o SM3DL faz isso) recebia `remove_all` na pasta. Falha antiga:
   `legacy save answered NotFormatted (the game would format it)`.
4. **DeleteFile sem cópia.** O Erase file do OoT3D apagava o slot sem cópia. Falha antiga:
   `deleted save file was not kept in the backups`.
5. **DeleteFile no Switch.** Com raiz `sdmc:/`, o `weakly_canonical` concatena o cwd no devkitA64,
   e o resultado provável é CommandNotAllowed (análise da pesquisa da manhã). No PC não se
   reproduz; o que falhou antes foi a guarda que proíbe `canonical/weakly_canonical/absolute` no
   `host_fs.cpp`.
6. **Savestate preso à pasta.**
   - O save aberto era guardado com caminho absoluto, e o savestate do PC não restaurava no
     Switch. Falha antiga: `save file path was serialized as an absolute host path`.
   - Com um arquivo de extdata aberto (Play Coins), o restore falhava. Falha antiga:
     `kernel object file path is incompatible`.

Depois da mudança passam:
- `oot3d_native_a32_ctr_fs_tests`: 21 testes, 12 novos;
- `oot3d_native_a32_ctr_host_tests`;
- `test_bug_guards.py`: 155/155, 6 guardas novas, 5 delas falhavam antes.

O teste de compatibilidade `TestLegacyZeldaSaveOpensUnchanged` passa antes e depois. Ele monta o
layout sintético do Zelda, abre o save, lê os quatro arquivos byte a byte, abre um slot em modo 3
e confere que nada foi criado dentro da pasta nem ao lado dela.

Custo: a primeira escrita de cada arquivo numa transação copia 5 KB, e o commit faz até 2 renames
e 1 remove. A primeira escrita de cada arquivo na execução copia mais 5 KB para o backup e faz uma
rotação (lista no máximo umas 20 pastas). Isso só acontece ao salvar e não foi medido no Switch.

## Decisão

Código em `oot3d_native_a32_ctr_save_safety.{h,cpp}`. O ponto de uso fica em `host_fs.cpp`, só no
backend de pasta: `Filesystem == nullptr`, arquivo de SaveData dentro de `SaveDataDirectory`. O
extdata e o módulo Forge não mudam.

1. **Escrita atômica com diário por arquivo.**
   - A primeira escrita ou SetSize de uma transação copia `<nome>` para `<nome>.triaevum-tmp`.
     Write, Read, GetSize e SetSize usam a cópia, e outro handle do mesmo arquivo também a vê.
   - **Flush, Close e ControlArchive** confirmam em três passos:
     1. fsync do tmp (Linux/macOS; no Switch o `fsFileClose` de cada operação já descarrega no SD;
        no Windows basta o fechamento do stream);
     2. `tmp -> <nome>.triaevum-new` (o destino não existe, o que vale no Horizon);
     3. `new -> <nome>`. Quando o destino existe, o rename dá EEXIST (Horizon, simulado no PC por
        `SetCtrSaveRenameFailsOnExistingForTesting`). Então o destino vira `<nome>.triaevum-old`,
        o novo entra e só depois o `.old` sai; se o rename do novo falha, o `.old` volta para o
        lugar (rodada 2; antes era remove + rename, e o save sumia se o segundo rename falhasse).
   - **Regra de commit (rodada 2):** a transação de um arquivo é confirmada quando o **último
     handle que escreveu nela** dá Flush ou fecha (FSFILE_Close, ou svcCloseHandle sem
     FSFILE_Close), ou no **ControlArchive** (commit do archive inteiro). Handle só de leitura
     nunca confirma. Flush de um escritor com outro escritor ainda pendente não confirma (os
     dados ficam no tmp até o último fechar ou até o ControlArchive). Write ou SetSize que falha
     marca a transação como falha: Flush/Close/ControlArchive respondem falha, o tmp vai para
     `incompleto-*` e a transação seguinte parte do save antigo.
   - **Recuperação** no primeiro acesso ao save da execução (o diário é criado no primeiro
     OpenArchive, OpenFile, E/S, GetFormatInfo ou restore de savestate, e a recuperação roda na
     criação; antes era só no OpenArchive):
     - um `.triaevum-new` que sobrou está completo por construção e é instalado (se não der, a
       E/S lê dele e a próxima escrita tenta instalar antes de começar);
     - um `.triaevum-old` volta para o lugar se o arquivo sumiu, ou sai se o arquivo está lá;
     - um `.triaevum-tmp` que sobrou não foi confirmado: o save continua o anterior, e o tmp vai
       para `<save>-backups/incompleto-<data>/` (não é apagado).
   - Sufixos distintos (`.triaevum-tmp`/`.triaevum-new`, em vez de `.tmp`) evitam colisão com
     arquivo do jogo e são ignorados na detecção de save legado.
2. **Backup rotativo.**
   - Antes da primeira escrita (ou do DeleteFile) de cada arquivo na execução, a versão anterior
     vai para `<save>-backups/<AAAAMMDD-HHMMSS>/` (no Zelda: `sdmc:/switch/oot3dre/savedata-backups/`).
   - Quando a mesma execução apaga um arquivo depois de escrever nele, a versão atual vai junto,
     como `<nome>.apagado`.
   - Ficam 20 gerações, mas uma geração que tem a única cópia de algum arquivo nunca é apagada,
     nem a recém-criada. `format-*` e `incompleto-*` não entram na rotação.
   - Rodada 2: a ordem das gerações vem de `<save>-backups/.triaevum-geracoes` (uma linha por
     geração, na ordem de criação); as que não estão nele (anteriores) vêm antes, por nome. A
     data no nome não decide nada (o relógio do console pode voltar).
   - Rodada 2: cada cópia é gravada como `<nome>.triaevum-tmp`, relida e conferida contra a
     origem (tamanho + FNV-1a 64) e só então renomeada. Cópia que não confere é apagada (o
     original continua intacto) e não conta; queda no meio deixa só o `.triaevum-tmp`.
   - Se o backup falha (SD cheio), a escrita segue, porque a troca atômica continua protegendo o
     save, e o log mostra `TRIAEVUM_SAVE_BACKUP_FAILED`. Se o backup antes de um DeleteFile
     falha, o DeleteFile é recusado.
3. **Formatação sem perda.**
   - O FormatSaveData confirma o que está pendente e move a pasta para `<save>-backups/format-<data>/`
     (copiando o `.format` anterior para `format-<data>.format`). Sem rename de pasta, copia,
     confere e só depois esvazia. Se não houver cópia segura, responde falha e não formata.
   - O GetFormatInfo de save legado (pasta com arquivos e sem `.format`) responde um formato
     sintetizado (tamanho arredondado a 512, pastas, arquivos, duplicado 0) e loga
     `TRIAEVUM_FS_SAVE_ADOPTED`. Não grava `.format`.
   - Pasta vazia continua NotFormatted, como no Azahar.
   - O `.format` é `CtrSaveFormatPath`: fica ao lado da pasta, sem barra final, nunca dentro dela.
4. **Pasta por título.**
   - `ResolveCtrTitleSaveDataDirectory` em `window_setup.cpp`: um título que não é o OoT3D e cairia
     em `sdmc:/switch/oot3dre/savedata` (ou dentro dela, ou num ancestral dela) vai para
     `sdmc:/switch/triaevum/<title id 16 hex>/savedata`.
   - O OoT3D não muda: o argv padrão de `oot3d_native_game.cpp` continua com o literal, protegido
     por uma guarda de compatibilidade.
   - No PC o padrão já é por título (`<pasta do manifest>/oot3d_native_savedata`) e não mudou.
5. **Contenção lexical** (`CtrArchivePathContained`): `lexically_normal` mais verificação de
   prefixo, sem `absolute/canonical`. No PC continua recusando subdiretório intermediário que seja
   symlink, a correção do verificador da pesquisa da manhã. O teste roda com a raiz literal
   `sdmc:/switch/x/savedata`, tanto pela função quanto ponta a ponta pelo IPC, com o cwd numa
   pasta temporária.
6. **Savestate.**
   - Arquivos e archives de save e de extdata vão como `file_path`/`root` relativos, com
     `file_path_base`/`root_base` (`savedata` ou `extdata`), e são religados à pasta configurada
     no restore.
   - Savestates antigos (com caminho absoluto) continuam aceitos pelo caminho antigo.
   - O `validFilePath` aceita a base de extdata.
   - O diário (pendências e backups da execução) sobrevive ao restore.

**Contrato que mudou de propósito:** o backend de pasta deixou de ser write-through. O teste antigo
de `host_tests` fixava "bytes no disco logo após o Write". As mesmas asserções (bytes `{1,9,8,7}`,
tamanho 6) passaram para depois do Flush, e ganharam uma nova: antes do Flush o disco não muda.
Motivo: o save do 3DS é transacional (nada é durável antes do commit), e o OoT3D faz flush e
ControlArchive ao salvar.

Compatibilidade do save atual: os caminhos do OoT3D não mudaram (`sdmc:/switch/oot3dre/savedata`).
Abrir e ler o save não cria nada. A primeira escrita de uma execução cria
`sdmc:/switch/oot3dre/savedata-backups/<data>/` com a versão anterior dos arquivos escritos.
Nenhum arquivo novo fica dentro de `savedata/`.

### Rodada 2: buracos achados na verificação adversarial

O harness (cenários S1..S9, fora do repo) achou formas de perder dado que os testes da rodada 1
não cobriam. Cada uma virou teste em `oot3d_native_a32_ctr_fs_tests` que falhou no código da
rodada 1 antes da correção:

| Cenário | Problema | Teste |
|---|---|---|
| S2 | Write que falha (SD cheio) seguido de Close instalava o tmp meio escrito | `TestFailedWriteIsNotCommitted` |
| S3 | Close/Flush de um handle só de leitura confirmava a escrita pela metade de outro | `TestReadOnlyCloseDoesNotCommit` |
| S4 | relógio para trás: o prune ordenava pelo nome e apagava a geração recém-criada | `TestBackupPruneUsesSequence` |
| S1 | backup truncado (SD cheio) ou estragado ficava com o nome do arquivo e contava como cópia | `TestBackupIsVerified` |
| S8 | troca interrompida + execução que começa por savestate: o Read não achava o save | `TestRecoveryBeforeFirstAccess` |
| rename | Horizon: remove do destino e rename do novo falhando = save sumido | `TestRenameFailureNeverLosesSave` |

**Referência para a regra de commit (S3).** O Azahar não serve de modelo aqui: o `DiskFile::Write`
grava direto (`src/core/file_sys/disk_archive.cpp`) e o `ArchiveBackend::Control` é stub que só
loga e devolve sucesso (`src/core/file_sys/archive_backend.h`, commit e9b14fd1 do repo local).
No 3DS, o 3dbrew descreve o `FSUSER_ControlArchive` ação 0 como commit do save data; isso não foi
conferido em hardware. A regra escolhida é a que não perde dado: o ControlArchive confirma tudo
(como o commit do 3DS) e, sem ele, só o último escritor confirma; o que não for confirmado vai
para `incompleto-*`.

O identificador do escritor fica no objeto do handle (`SaveJournalWriter`) e vai para o savestate
(`save_journal_writer`); no restore, escritores que não existem no estado restaurado são
esquecidos (`RetainWriters`), para não segurarem a confirmação para sempre.

## Alternativas rejeitadas

- **"Não introduzir escrita atômica por rename"** (conclusão da pesquisa da manhã, por causa do
  EEXIST): rejeitada. O EEXIST só impede o rename por cima, e o protocolo em dois passos (tmp ->
  new, e new -> nome com remove + rename) mais a recuperação cobre a janela entre o remove e o
  rename.
- **Commit por caminho (qualquer Close/Flush do arquivo confirma):** era a rodada 1; confirma a
  escrita pela metade de outro handle (S3).
- **Ordenar as gerações por mtime:** o mtime vem do mesmo relógio que o nome; com o relógio para
  trás erra do mesmo jeito. O arquivo de ordem não depende do relógio.
- **Arquivo de sequência dentro de cada geração:** misturaria metadado com as cópias que o usuário
  restaura à mão; o arquivo de ordem fica na raiz de `<save>-backups`.
- **Escrever no `.tmp` só no Flush ou Close, mantendo os pedaços em memória:** perde tudo se o
  processo morrer antes. O tmp em disco acompanha cada Write, e a recuperação o guarda em
  `incompleto-*`.
- **Copiar `<nome>` para `<nome>.bak` a cada commit, sem tmp:** não evita o arquivo metade
  escrito, só dá uma cópia anterior.
- **Diário fora da pasta do save (pasta de metadados):** o rename dentro do mesmo diretório é o
  caso mais simples e seguro no FAT. Os sufixos distintos já evitam colisão e são ignorados na
  detecção de legado.
- **Backup a cada commit:** com 20 gerações, uma sessão longa empurraria para fora a versão de
  antes da sessão. Backup 1x por arquivo por execução, mais a regra "nunca apagar a única cópia",
  garante que a última cópia de cada arquivo sempre fica.
- **Gravar `.format` ao adotar save legado:** o GetFormatInfo é leitura, e gravar fixaria um
  formato chutado. Ele é sintetizado a cada chamada.
- **Responder NotFormatted como o Azahar para save legado:** é exatamente o que leva o jogo a
  formatar o save importado ou antigo. A divergência é proposital e está comentada no código.
- **Fazer o OpenArchive(SaveData) responder NotFormatted quando a pasta está vazia (Azahar):** o
  OoT3D depende do OpenArchive sempre dar sucesso. Ficou para o perfil genérico (em aberto).
- **`copy_file` da libstdc++:** a cópia é feita por stream (5 KB), sem depender de
  sendfile/permissões do devoptab.
- **Pasta por título também no PC, com mudança de caminho:** o padrão do PC já é por manifest, e
  mudar o caminho quebraria os saves existentes do SM3DL no PC.

## Em aberto

- **Nada foi rodado no Switch nem no Eden.** Os testes são de PC. Ficam por confirmar no
  console:
  - o rename de pasta (`fsdev`, para o format) e o `recursive_directory_iterator` sobre `sdmc:/`;
  - o `localtime_r` (nome das gerações);
  - o DeleteFile/Erase, que antes respondia CommandNotAllowed (hipótese da pesquisa da manhã),
    agora com a contenção lexical.

  Repro proposta: NRO no Eden com o Erase file de um slot copiado.
- O formato sintetizado para save legado pode não bater com o que um jogo genérico espera, e o
  jogo pode reformatar. Se isso acontecer, o save vai para `format-*` (sem perda), mas o jogo vê
  um save vazio. Comparar com a ordem e os valores que o SM3DL usa quando chegar ao save.
- Um jogo que escreve e nunca chama Flush, Close nem ControlArchive perde as escritas da sessão
  (elas ficam em `incompleto-*`). Idem para dois handles escrevendo no mesmo arquivo em que um
  nunca fecha: o Flush do outro não confirma. É a semântica do 3DS, mas não foi verificada com um título real
  além da sequência documentada do OoT3D.
- Extdata (Play Coins, banco de Miis) continua write-through e sem backup.
- O OpenArchive(SaveData) continua criando a pasta e respondendo sucesso (legado do OoT3D). A
  semântica do Azahar no perfil genérico ficou de fora.
- `oot3d_native_a32_savestate_tests` não linka nesta configuração de teste (falta
  `Ship::WriteConfigFileAtomically`; o problema já existia antes, como mostra o
  `build-tests2.log` do build de origem). O teste não abre arquivo de save.
- O efeito no frame time do primeiro save de cada execução (cópia de 5 KB + rotação) não foi
  medido no Switch.
- Restaurar um backup é manual (copiar de `savedata-backups/<data>/` para `savedata/`). Não há
  menu.
- Fora do escopo da rodada 2 (escopo fechado em "não perder save"): `--save-data` com a pasta do
  Zelda escrita em outra caixa (`sdmc:/SWITCH/OOT3DRE/savedata`) não é redirecionado para outro
  título (o FAT ignora caixa; S9 do harness). Só acontece se alguém passar esse argumento à mão.
