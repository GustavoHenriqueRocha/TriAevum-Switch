# SM3DL parado no fs:USER OpenDirectory

Data: 2026-11-05 · Título(s): SM3DL (0004000000053F00), genérico · Continuação de
`2026-11-05-sm3dl-seletor-mii.md` ("Próxima trava").

## Pergunta

Depois do seletor de Mii, a thread 4 do SM3DL pedia `fs:USER` 0x080B0102 (OpenDirectory). O host
respondia "comando sem suporte", o jogo chamava `ConnectToPort("err:f")` e o processo parava no
VBlank ~1080. O que o FS precisa fazer, sem código por título?

Repro: `tools/ctr/build_title_window.sh --work ~/oot3d-work/conv/sm3dl --run --seconds 180 --
--input-timeline ~/oot3d-work/conv/sm3dl/a_repetido.json` com `TRIAEVUM_GUEST_STATE_DUMP=120`.

## Fontes

- Azahar (`~/oot3d-work/ref/azahar`, commit 7654972):
  - `src/core/hle/service/fs/fs_user.cpp` (OpenDirectory: resposta (1,2) com o handle movido,
    handle nulo no erro) e `directory.cpp` (Read 0x0801: count + buffer mapeado → (2,2); Close 0x0802);
  - `src/core/file_sys/savedata_archive.cpp` (`SaveDataArchive::OpenDirectory`: caminho inválido →
    InvalidPath; não existe → PathNotFound 0xC8804471; arquivo → UnexpectedFileOrDirectory 0xE0C04702);
  - `src/core/file_sys/path_parser.cpp:13` (só Char/Wchar; vazio ou sem '/' inicial é inválido);
  - `src/core/file_sys/disk_archive.cpp:62` (`DiskDirectory::Read`) e
    `src/common/file_util.cpp:1111` (`SplitFilename83`): entrada de 0x228 bytes.
- Nosso diário de saves: `oot3d_native_a32_ctr_save_safety.{h,cpp}` (`ActivePath`, sufixos
  `.triaevum-tmp/-new/-old`, `RecoverOnce` no OpenArchive).

## Conclusão

1. **OpenDirectory não existia no host (confirmado).** `TRIAEVUM_FS_REQUEST_UNSERVED header=0x80b0102
   reason=unsupported-fs-command` no teste novo, igual ao log da janela.
2. **A listagem do save tem que esconder o diário (confirmado no teste).** No meio da execução a
   pasta do save tem `X.triaevum-tmp` (escrita sem flush) e pode ter `X.triaevum-old`; o jogo não
   pode ver esses nomes, e o tamanho de `X` tem que ser o do arquivo ativo (o mesmo do GetSize):
   9 bytes no teste, contra 5 no disco.
3. **`CtrSaveJournal::Covers()` não inclui a própria raiz do save (confirmado).** Na listagem de `/`
   o diário era ignorado e `b.bin` saía com 5 bytes; a sessão de diretório compara com
   `Directory()` além de `Covers()`.

**Rodada de 180 s na janela (confirmado):** o jogo chama `OpenDirectory("/GameData.bin")` (wchar)
nos VBlanks 360 e 1080 — testa se o save é pasta —, depois `OpenFile` do mesmo caminho (modo 0x1 e
0x6, cria o save). Sem `err:f`: o processo vai até o VBlank 3480 (fim dos 180 s),
`TRIAEVUM_FS_REQUEST_UNSERVED` = 0, e a captura do quadro 1560 mostra a cena de abertura (Mario, os
Toads e a silhueta da árvore). Antes parava no VBlank ~1080.

## Decisão

- Arquivo novo `oot3d_native_a32_ctr_host_directory.cpp`: `OpenFsDirectory` (chamado do
  `HandleFsRequest`) e `HandleDirectoryRequest` (sessão `client_session:directory`).
- Só backend de pasta (save e extdata). Com `NativeA32CtrFilesystem` (Switch com módulo de arquivos)
  o pedido é falha nomeada `open-directory-archive-without-disk-backend`.
- `DecodeArchiveRelativePath(..., allowRoot)`: "/" vira a raiz do archive; arquivo continua exigindo
  nome. Sem seguir symlink no meio (`CtrArchivePathContained`) nem no fim (`symlink_status`); entradas
  que são symlink não aparecem.
- A pasta é relida a cada Read, em ordem de nome; `FileOffset` é o cursor. O savestate guarda caminho
  (portátil, relativo a savedata/extdata) e cursor; o restore valida o caminho como o de arquivo.
- Entradas: nome UTF-16 (até 0x105 unidades), 8.3 do `SplitFilename83`, `is_hidden` = começa com
  '.', `is_archive` = não é pasta (SD com bit de arquivo nunca limpo, como o Azahar).
- Teste `TestFsOpenDirectory` (visto falhando antes; o esconder do diário visto falhando por
  mutação) e 2 guardas (vistas falhando por mutação, 170/172).

## Alternativas rejeitadas

- Instantâneo da pasta no OpenDirectory (como o `ScanDirectoryTree` do Azahar): o savestate teria de
  guardar a lista inteira; com cursor + releitura, basta o caminho.
- Ordem do `readdir` (a do Azahar no Linux): muda entre máquinas e entre PC e Switch; ordem de nome é
  estável.
- Mostrar `X.triaevum-new` órfão como `X`: a recuperação do OpenArchive já instala o `.new`; o que
  sobra para o jogo é o caso "pendente", que o `ActivePath` cobre pelo tamanho.

## Em aberto

- Diretório no backend de módulo (`NativeA32CtrFilesystem` não tem API de pasta).
- Nome curto repetido (dois "LONGFI~1"): o Azahar também não trata.
