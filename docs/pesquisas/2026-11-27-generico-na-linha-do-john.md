# O genérico do MM3D na linha do John (integra-john-2)

Data: 2026-11-27 · Título(s): OoT3D, MM3D, genérico · Branch: `generico-john` (base `06331814`, integra-john-2)

## Pergunta

Os 28 commits de `deko3d..generico-mm3d` e o `f106f9ed` (mm3d-tour) foram feitos sobre a linha deko3d.
O que deles entra na linha do John sem mudar o OoT3D por acidente? O que o John já tinha por outro
caminho, e o que fica de fora?

## Fontes

- `git log --reverse deko3d..generico-mm3d` (28 commits) e `f106f9ed`.
- Cada commit novo tem a linha "Linha do John (integra-john-2): ..." com o conflito e o que ficou de fora,
  e o `(cherry picked from commit ...)` da origem.
- Testes de host no notebook (`~/oot3d-work/build-host-generico-john`, Debug, `-j 2`) a cada etapa;
  `python3 tests/switch/test_bug_guards.py` a cada commit.

## Conclusão

Entraram 27 commits (26 do intervalo + `f106f9ed`); 2 ficaram de fora porque o John já tem o mesmo:

| Fora | Motivo (prova) |
|---|---|
| `14d2b054` (saída de uma thread fechava o jogo) | é o upstream `034405bd`; o John tem `faf59da1` com a mesma correção (`ResolveStoppedThreadRunKind`, `TestThreadExitWhileOthersWait`), ancestral de `06331814` |
| `485e975d` (percurso rápido da lista) | é a ideia do `1ff04ef2` do John (`config/list_walk_fast`), ancestral de `06331814` |

Feito pelo John por outro caminho, e como ficou:

- **Cópia crua de textura** (`CopyReferences`, `e1198bac`/`48eab28c`): tem precedência sobre o preto do
  `0afcc669` e sobre o feedback do `1e6607c0`; a guarda confere essa ordem.
- **Ordem dos alvos**: o `PicaTargetKeyLess` do snapshot do John dá a mesma ordem que o
  `PicaTargetKeyOrder` do `ed9d6c26`. Ficam os dois (duplicação a refatorar depois).
- **Pack de shaders**: o `828d6550` foi refeito sobre o `ShaderStore` do John (`config/deko3d_pack`,
  flags do tevskip no índice).
- **Relatório de falha**: o John tem o `FaultThread` com `WorldStop` e `GuestReport`. Do `b369d02a`
  entra só o anel dos 32 últimos despachos, dentro do `FaultThread`.
- **Perfil de runtime**: o John já tem o `ProfileRuntime` (opção `ProfileA32Runtime`). O `svc_profile`
  entra ao lado, desligado sem `config/svc_profile`.
- **Guarda da saída de thread** (`ecca52ba`): o John já tem uma mais forte (com `forbid`), então essa
  guarda não entrou.

Erros que só apareceram na linha do John:

- **`cd165df1`** (fila de uniform `==` → `>=`): o bug existe no John. O `TestUniformModeSwitch` novo
  falhou com "the queue did not empty, count 4" e passa com a correção.
- **`b7b5246c`**: o teste fixava os sha256 do gerador do deko3d. O gerador do John dá outros
  (stable `a3f3fa17…`, affinity `de5bfd2f…`), iguais com e sem o commit. A troca dos pinos está
  explicada no commit.
- **`f106f9ed`**: o `TestMemoryFillInPlace` exigia `RangeWriteGeneration == WriteGeneration`, que é
  semântica do deko3d. No John são dois contratos, e o teste passou a conferir os dois. Ele continua
  falhando sem o `NoteWrite` e com a cópia errada, e o caminho antigo passa nele.
- **`e413f7b1`**: o `DispatchGuest` do John (com o segundo núcleo) passa o `mManualCompiledFunctions`
  nas duas chamadas.
- **`c339ccd8`**: os ids de applet passam a ser os do Azahar. O 0x401 era o seletor de Mii no John e
  vira o teclado.

Commits que mudam o que o OoT3D executa (precisam de tour):

- `0afcc669`: preto em endereço escrito pela GPU, só sem cópia crua.
- `ed9d6c26`: ordem dos fills e limpezas.
- `828d6550`: DKSH lido do SD no primeiro uso.
- `b369d02a`: anel por despacho.
- `e413f7b1`: VBlank sem fila de relay deixa de ser erro, e o handle da fila é lido antes do `CreateHandle`.
- `de3a47d9`: o evento volta ao limite de recursos.
- `898293cc`: `AdvanceIrToCurrentTick` a cada quadro.
- `f106f9ed`: o MemoryFill escreve no lugar.
- `2f10e0c6`: o header do whole-AOT recompila os shards, mas o OoT3D executa igual.

## Decisão

Cada commit entra com `cherry-pick -n`, com a conferência do que o John já tem e uma guarda (vista
falhando por mutação) quando há algo novo. O que já existe no John fica na versão do John.

## Alternativas rejeitadas

- **Trazer o `14d2b054`/`485e975d` mesmo assim**: duplicaria o que o John já tem.
- **Manter os sha256 do deko3d no `test_whole_aot_cpp.py`**: o teste falharia sem que nada estivesse
  errado, porque o gerador é outro.
- **Manter o `RangeWriteGeneration == WriteGeneration` no teste do fill**: na linha do John isso é falso
  até para o caminho antigo, então não testa o fill.

## Em aberto

- Não foi conferido se o OoT3D pede o applet 0x401 ou desmapeia um bloco mapeado. Se fizer isso, o
  `c339ccd8` muda o que ele recebe.
- Os commits da lista acima ainda não passaram por um tour no Switch.
- `PicaTargetKeyOrder` e `PicaTargetKeyLess` estão duplicados.
