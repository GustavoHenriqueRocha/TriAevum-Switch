# MM3D: há método melhor para colocar o jogo de pé do que "uma lacuna por rodada"?

Data: 2026-11-23 · Título(s): MM3D EUR Rev 1 (`0004000000125600`), genérico CTR · Commits: nenhum de código
(só pesquisa; leitura do repo, do upstream `origin/main` e da web).

Estados: **confirmada** = fonte lida (arquivo:linha, SHA, página) ou medida por comando;
**parcial** = fonte secundária ou inferência a partir de fonte lida; **sem suporte** = procurado e não achado.
Texto da web foi tratado como dado.

## Pergunta

Hoje cada lacuna do boot do MM3D (SVC/serviço não implementado ou `MissingBlock`) custa uma rodada de
~25 min: `ctr_convert.py` (discover → generate de 256 shards → build -O0 no Ryzen) → boot probe headless, que
para na primeira lacuna. Existe técnica pronta (no upstream, em outros recomps ou em emuladores) que tire várias
lacunas por rodada ou encurte a rodada?

## Fontes

Locais (todas confirmadas por leitura ou comando):
- Nosso fluxo: `.claude/worktrees/mm3d/docs/pesquisas/2026-11-23-mm3d-integracao-leva1-2.md` (tempos das levas 1–5),
  `tools/ctr/ctr_convert.py:88-101` (generate sem `--shard-strategy`), `tools/ctr/build_title_boot_probe.sh:16-20,92-123`
  (Debug -O0, ccache se existir, whole-AOT sem registro A32).
- Gerador: `tools/oot3d/native_a32_runtime/generate_whole_aot_cpp.py:48-60` (padrão `stable`; existe
  `affinity-incremental`), `whole_aot_cpp.py:172-175` (`_write_if_different`), `:3553-3600` (`_stable_shard_index`
  e as estratégias), `:4193-4199` (o incremental lê o manifesto anterior).
- Runtime: `tools/oot3d/native_a32_runtime/upstream/recomp/a32_runtime.cpp:929-962` (bloco ausente → observador
  + `fallback(MissingBlock)`), `a32_core.h:9-16` e `a32_core.cpp:75-89` (`ExecuteCore(raw, pc, state, memory)`
  executa a palavra crua: sistema, ALU, memória), `a32_runtime.h:168-172` (`PackedOp` = palavra crua + metadado),
  `tools/oot3d/native_game_runtime/oot3d_native_a32_process.cpp:495-511` (o host pode retomar um MissingBlock se
  der `ResumePc`), `oot3d_native_a32_window_setup.cpp:119-131,593` (`TRIAEVUM_A32_COVERAGE`, só na janela).
- Medição local dos shards (comando que reproduz):
  `cd ~/oot3d-work/conv/mm3d/cpp && stat -c '%y' *.cpp *.h | cut -c1-16 | sort | uniq -c`
  → 116 arquivos de 10:54 (leva 1), 19 de 12:38 (leva 4, +28 funções), 124 de 13:06–13:07 (leva 5, +158
  funções); o despachante `oot3d_whole_aot_generated.cpp` tem 21 MB e o maior shard 4,4 MB; 599 MB no total.
- Upstream (coccofresco/TriAevum, `origin/main`): `docs/MOONFALL_CURSE.md:122-170` (pipeline do MM3D),
  `:213-219` (bring-up em build de desenvolvimento), `:441-446` (`TRIAEVUM_A32_CALL_TRACE`),
  `:612-618` (o runtime não interpreta fora do registro), `:777-784` (próximos passos deles);
  `docs/TRIAEVUM_FORGE_WHOLE_AOT_V2.md:15-36` (cache de objetos por conteúdo); `docs/OOT3D_FAST_BUILD_ISOLATION.md`;
  `tools/oot3d/decomp_support/docs/azahar_runtime_trace.md` (Azahar instrumentado); `titles/mm3d/a32/whole_aot_product_manifest.json`;
  `tools/oot3d/native_a32_runtime/prove_whole_aot_coverage.py:1-40`; commits citados na tabela abaixo.
- Web: links em cada linha das seções 2 e 3 (pesquisa feita por um agente; as frases do Zakuro, o gancho do
  dynarmic e os números do gbarecomp eu reli nas páginas).

## Conclusão

### 1. Como o upstream colocou o MM3D de pé (e o que não trouxemos)

O upstream **não iterou uma lacuna por rodada do jeito que estamos fazendo**. Três diferenças, todas confirmadas:

1. **Bring-up sem o whole-AOT.** `MOONFALL_CURSE.md:213-219`: "Bring-up therefore uses a development build (no
   product mode, no title module): the MM3D A32 registry, the A32 core and the Thumb interpreter." O registro A32
   (`generate_aot.py` → `oot3d_pack/a32_cpp_aot.py:1174-1239`) é **tabela `constexpr` de `PackedOp`** (palavra
   crua + classe), interpretada por `ExecuteBlock`; não é C++ de função. Compilar tabela é muito mais barato que
   compilar os 599 MB de C++ do whole-AOT. O whole-AOT (G2/G3, `407a4b43`, `757f90ae`) só veio depois, quando o
   inventário já estava fechado.
2. **Inventário fechado antes de rodar, com prova estática.** Ghidra headless + sementes (`ghidra_inventory.py`,
   `init_array_entries.py`, `data_pointer_entries.py`, 80,9% → 92,5% do texto) e, no fim,
   `prove_whole_aot_coverage.py`: cada byte do executável tem de estar numa instrução traduzida ou ser dado
   classificado (literal, tabela de switch, ponteiro, preenchimento, string, fato de título); o resto é
   **resíduo** e reprova (exit 1). O contrato do produto (`whole_aot_product_manifest.json`) registra
   `residual_a32_entries: 0`, `unclaimed_blocks: 0`, 18 751 funções e 241 681 blocos, **todos** entrada do
   despachante. Ou seja: eles acham as lacunas de código de uma vez, offline, e não pelo boot.
3. **Ordem das lacunas de serviço já conhecida.** `MOONFALL_CURSE.md:320-335` lista as travas em ordem
   (agendador GX esperando 2 conclusões, `OpenLinkFile`/`SetPriority`, vtables sem inventário) e os commits
   `ctr-host` resolvem cada uma.

O que existe no upstream e ainda **não** está nas nossas branches `mm3d*` (conferido com
`git log <branch> --format=%b | grep 'cherry picked from'`):

| SHA | O que é | O que resolve para nós | Estado |
|---|---|---|---|
| `82bf5a2f` | o host nunca deixa thread do guest parada num pedido que ele não serve (3 caminhos de FS + 35 fallthroughs de serviço/SVC devolviam `Wait` sem log) | evita rodadas perdidas com probe "travado sem erro" (0 vblanks, nenhuma falha no log) | confirmada (mensagem) |
| `109bc32f` | ir:USER como Circle Pad Pro do New 3DS, ptm:u | o MM3D consulta o ir:USER até o acessório responder; sem isso a tela de cima fica preta depois de escolher o arquivo | confirmada |
| `2734bbac` | teclado de software (applet 0x401) | criar arquivo novo; o APT bloqueia no 1º pedido sem ele | confirmada |
| `034405bd` | saída de uma thread não encerra o jogo | fim de vídeo/thread auxiliar não vira "Terminated" | confirmada |
| `e63f6fcb` | RTC na página compartilhada (é port do nosso Switch) | já temos: `51074da6` no deko3d | confirmada |
| `c2324b12` | linha `TRIAEVUM_FATAL` com registradores e as 32 últimas transições do despachante | diagnóstico de cada parada sem rodar de novo | confirmada |
| `0bc54250` (parte) | `TRIAEVUM_A32_CALL_TRACE=PATH,FIRST,LAST[,WORD[,ENTRIES]]`: um registro por entrada de bloco (pc, lr, sp, r0–r3, thread) | rastro barato de onde o guest passou antes da parada | confirmada (`MOONFALL_CURSE.md:441-444`) |
| `1b507be8`, `cc4688a9` | `init_array_entries.py`, `match_functions.py` | já resolvemos os construtores no `9d5bd4da`; o casador OoT3D↔MM3D deu "poucos pares" | confirmada; baixo valor |
| `titles/mm3d/a32/*` (já no tree desde `b77f12a7`/`757f90ae`) | seleção de 18 751 entradas, `thumb_ranges.csv`, `audited_dispatch_entries.csv`, `guest_patch_supplemental.csv`, `inline_argument_calls.json`, `whole_aot_data_facts.csv` | sementes prontas, validadas no jogo inteiro (eles visitaram ~100 cenas) | confirmada; **não usamos** |
| `docs/TRIAEVUM_FORGE_WHOLE_AOT_V2.md` | "compile missing shards with the content-addressed ThinLTO object cache", cada etapa com chave de hash | é o que o ccache já faz para nós, se os shards não mudarem | confirmada |
| Azahar instrumentado (`azahar_runtime_trace.md`) | gancho no interpretador DynCom do Azahar (CPU JIT desligado), JSONL por PC de uma lista; commits fora do repo (`4314ccc`, `72dc346`) | precedente de rastro por emulador; o gancho deles é por lista de PCs, não cobertura total | confirmada; código do gancho não publicado |
| Catálogo/Forge-rs (`57413d54` e afins) | distribuição: título pré-compilado + importação da ROM | não ajuda o bring-up | confirmada |

Interpretador de reserva para A32 **não existe** no upstream: `MOONFALL_CURSE.md:612-618` diz que o runtime não
interpreta fora do registro, e o produto proíbe o caminho de `PackedOp` (`"packed_op_fallback": "forbidden"`).
Mas as peças estão no nosso tree (ver item 1 da recomendação).

### Diagnóstico do tempo da rodada (dos números das levas)

| Etapa | Onde | Tempo medido | Fonte |
|---|---|---|---|
| discover | notebook, Python | ~488 s | leva 4 |
| generate (256 shards) | notebook, Python | 300–680 s (802 s e 1172 s com o discover) | levas 4 e 5 |
| build -O0 completo | Ryzen, -j3 | 740–814 s | levas 2 e 3 |
| build incremental, 19 arquivos mudados | Ryzen | 163 s | leva 4 |
| build incremental, 124 arquivos mudados | Ryzen | não medido; ~124/256 do completo ≈ 6 min (inferência) | leva 5 |

- **Metade da rodada é Python no notebook** (discover + generate = 13–20 min), não compilação. Confirmada.
- **A divisão em shards não muda** entre rodadas: o `ctr_convert` usa a estratégia `stable` (hash do endereço de
  entrada, `whole_aot_cpp.py:3553-3555`) e o gerador só reescreve o que mudou (`_write_if_different`). O que
  espalha é o hash: 158 funções novas caíram em ~124 shards diferentes. Confirmada pela contagem de horários.

### 2. Outros recompiladores: cobertura e iteração

| Projeto | Função faltando em runtime | Iteração/build | Fonte | Estado |
|---|---|---|---|---|
| N64Recomp / Zelda64Recomp | sem reserva: `get_function` imprime "Failed to find function" e sai. As funções vêm dos símbolos de um ELF (decomp) e de um toml. O live recompiler (sljit) só serve para mods | um arquivo por função | https://github.com/N64Recomp/N64Recomp · https://raw.githubusercontent.com/N64Recomp/N64ModernRuntime/main/librecomp/src/overlays.cpp | confirmada (agente leu) |
| XenonRecomp / UnleashedRecomp | chamada direta desconhecida vira comentário `// ERROR`; a indireta usa tabela plana e quebra. As funções vêm do `.pdata` e de um toml manual | `i % 256` por índice (instável: inserir desloca os seguintes) | https://github.com/hedge-dev/XenonRecomp | confirmada |
| Port Burnout Revenge (Xenon) | loga `[LOOKUP] Missing target=… LR=…` e só aceita o alvo depois de conferir que é entrada real | — | https://github.com/Luizfe181/Burnout-Revenge-X360-Recomp | parcial (agente) |
| RPCS3 (PPU LLVM) | **reserva**: endereço sem código compilado cai em `ppu_recompiler_fallback`, que interpreta instrução a instrução até um ponto compilado | cache por módulo, em pedaços de ~100 KiB, nome do `.obj` = SHA-1 do conteúdo; compila só o que falta | https://github.com/RPCS3/rpcs3 (`rpcs3/Emu/Cell/PPUThread.cpp`) | parcial (agente leu; eu não reli) |
| PS2Recomp | `lookupFunction()` em runtime; stubs de triagem `ret0`/`ret1`; "function not found" = bloqueio a corrigir | um arquivo por função ou arquivo único | https://github.com/ran-j/PS2Recomp | parcial |
| **fearkov/3dsrecomp + Zakuro** (3DS) | **reserva**: "anything it doesn't cover still goes through the interpreter". O agente leu também que o Zakuro anota onde interpretou e o build seguinte inclui | "takes a while" | https://github.com/fearkov/zakuro · https://github.com/fearkov/3dsrecomp | confirmada a primeira frase (eu li); a segunda é parcial |
| nathsou/3dsrecomp (3DS) | o código que faltou "is recompiled on the fly… and recorded so that tools/regen.sh includes it next time" | recompilar para C leva "a few seconds" | https://github.com/nathsou/3dsrecomp | parcial (agente) |
| doopydoop364/3dsrecomp (3DS, Kirby) | igual ao nosso: a entrada que falta para e imprime o endereço, mas o `run` grava, regera e **reinicia sozinho**; diferencial contra Unicorn | "whole game compiles in about 25 s", cada etapa pulada se a entrada não mudou | https://github.com/doopydoop364/3dsrecomp | parcial (agente) |
| mstan/gbarecomp (GBA) | o código não resolvido cai no interpretador; `GBARECOMP_STRICT_STATIC=1` falha no 1º PC (modo de aceite); os ports gravam os misses num `.toml.frag` | i9-9900K: 1 TU no GCC 10m34s–21m37s; 16 shards no Clang com 8 jobs 55,9 s; shard inalterado com ccache 24,7 s → 0,037 s; ~3000 funções por shard | https://raw.githubusercontent.com/mstan/gbarecomp/main/docs/BUILD_PERFORMANCE.md | confirmada (eu li os números) |

O padrão dos recomps que iteram rápido é **híbrido**: o que falta roda num interpretador e vai para um arquivo
de misses, e a próxima regeração inclui o lote inteiro (RPCS3, Zakuro, nathsou, gbarecomp). Os que param na
primeira falta (N64Recomp, Xenon, doopydoop) ou têm a lista de funções pronta (ELF/decomp, `.pdata`) ou têm build
de segundos. Nós não temos nenhuma das duas coisas. Os números do gbarecomp misturam compilador e divisão em
shards, então não isolam o ganho do Clang sobre o GCC. Uma alegação do agente estava errada para o nosso caso:
ele supôs que dividimos por contagem, como o Xenon. Usamos hash estável por endereço (seção 1).

### 3. Rastro por emulador (Azahar/Citra)

- **Gancho pronto no dynarmic do Azahar:** `PreCodeTranslationHook(bool is_thumb, VAddr pc, A32::IREmitter&)`,
  "called before the instruction at pc is interpreted", com corpo vazio por padrão, e `MemoryReadCode(VAddr)` (lido
  em https://raw.githubusercontent.com/azahar-emu/dynarmic/master/src/dynarmic/interface/A32/config.h). Confirmada.
  O `arm_dynarmic.cpp` do Azahar implementa `MemoryReadCode`, `CallSVC` e um `InterpreterFallback` UNREACHABLE.
  Parcial (agente).
- **Passo a passo (inferência, não testado):** (1) no `DynarmicUserCallbacks` do Azahar, sobrescrever
  `PreCodeTranslationHook` para guardar `pc | is_thumb` num conjunto. Para ter início de bloco, guardar só o 1º PC
  de cada tradução. (2) Gravar o arquivo ordenado ao fechar. (3) Jogar o MM3D pelo trecho desejado: boot, título,
  arquivo, Cidade do Relógio. (4) Diferença com os blocos do nosso `aot_program.json` vira `seed_roots`. São ~15
  linhas. O custo de runtime é quase zero, porque o gancho roda só quando o JIT traduz um bloco novo. Ressalva:
  ele registra o que foi traduzido, não o que foi executado (inclui instruções depois de saída condicional).
- **Gdbstub** (Azahar, Luma3DS): existe, mas cobrir por breakpoint ou single-step é ordens de grandeza mais lento.
  Rejeitado.
- **Ferramenta pronta de cobertura para Citra/Azahar:** sem suporte (não achada). O upstream fez um gancho no
  interpretador DynCom, com o JIT desligado e lista fixa de PCs (`azahar_runtime_trace.md`). O código está fora do
  repo deles.
- **Custo para nós:** compilar o Azahar. O clone em `~/oot3d-work/ref/azahar` é parcial (só `src/core/hle`,
  `src/common`), sem o backend ARM. Build completo no Ryzen; no notebook não (29 GB livres, 4 núcleos, 3 GB de RAM
  livre medidos com `df`/`free`).

### 4. Encurtar a rodada

- **Delta shards:** com `stable`, as funções novas caem espalhadas: na leva 5, 158 funções novas mudaram 124
  arquivos (medido). Uma estratégia que mantém o manifesto anterior e põe as funções novas em 1–4 shards próprios
  (o `incremental` atual põe no shard mais leve, ou seja, espalha também) mudaria só esses arquivos, o despachante
  de 21 MB e quem chama direto as funções novas. Ganho estimado: build de ~6–13 min para ~2–3 min (a leva 4, com 19
  arquivos, levou 163 s). Custo: ~2 h no `whole_aot_cpp.py`, com teste sintético. O C++ do OoT3D não muda (a
  estratégia é opcional). Parcial (estimativa).
- **Mais jobs no Ryzen:** o boot probe rodou com `-j 3`, e o `CLAUDE.md` do repo registra o WSL com 12 GB e
  `JOBS=12` para o build do deko3d. Falta medir o pico de RAM de um shard de 4,4 MB em -O0 (`/usr/bin/time -v`)
  antes de subir. Ganho: até ~2–3× no build completo. Custo: zero. Risco: OOM no WSL.
- **Python é metade da rodada:** discover + generate levam 13–20 min no notebook, single-thread (nenhum dos dois
  usa `multiprocessing`; conferido por grep). Opções: rodar no Ryzen; perfilar uma vez com `cProfile` antes de
  otimizar; cache por função do texto C++ gerado. Ganho possível de 5–15 min por rodada. Não medido.
- **ccache:** já está ligado se existir (`build_title_boot_probe.sh:93`); não conferi se está instalado no WSL.
  Com shards que não mudam, o ninja nem recompila: o ccache só ajuda depois de limpar o build.
- **Unity build / menos shards:** vai contra o incremental (um shard maior recompila mais por mudança). Rejeitado
  para o bring-up.
- **Registro A32 de `PackedOp` (o caminho do upstream no bring-up):** tabelas `constexpr` compilam bem mais rápido
  que funções. Mas exige um segundo gerador (`generate_aot.py`, com padrões do OoT3D) e o inventário em CSV, e
  continua parando no 1º bloco ausente. Fica atrás do interpretador de reserva.

## Decisão sugerida (top 3, em ordem)

1. **Interpretador de reserva para `MissingBlock`, mais o arquivo de misses no boot probe.** Quando o PC não tem
   bloco, o host interpreta até cair num PC traduzido e devolve `ResumePc`. A costura já existe
   (`a32_runtime.cpp:929-962` chama `fallback(MissingBlock)`; `oot3d_native_a32_process.cpp:502-511` aceita
   retomada com `ResumePc`). As peças também: `ExecuteCore(raw, pc, state, memory)` cobre sistema, ALU e memória a
   partir da palavra crua; `ExecuteBlock` já trata Branch, BranchReg e SVC; há VFP scalar e transport, e o
   interpretador Thumb (`a32_thumb.cpp`). Falta o laço de busca, condição e despacho por classe (~200–400 linhas)
   e levar o `TRIAEVUM_A32_COVERAGE` da janela para o probe. Uma sessão do probe passa a juntar todas as lacunas de
   código que alcança, e uma regeração inclui o lote. É o que fazem RPCS3, Zakuro, nathsou e gbarecomp.
   **Ganho:** k lacunas de código numa rodada só, em vez de k × ~25 min. **Custo:** ~1 dia, com testes sintéticos
   (programa A32 mínimo que salta para fora do registro e volta). **Risco:** instrução fora do subconjunto do núcleo
   para como hoje (não piora); manter um modo estrito para o aceite, como o `STRICT_STATIC` do gbarecomp; o retorno
   para código compilado depende das arestas de `resume`. Precisa de guarda para o modo produto continuar sem
   reserva.
2. **Fechar offline o que já se sabe, antes da próxima rodada.**
   - (a) Sementes do upstream: as entradas de `titles/mm3d/a32/whole_aot_functions.json`,
     `audited_dispatch_entries.csv`, `guest_patch_supplemental.csv` e `inline_argument_calls.json` como
     `seed_roots`, filtradas por "decodifica como A32/Thumb". Elas já estão no tree; do upstream ainda faltam 4
     alvos Thumb, 548 PCs no meio dos nossos blocos (podem ser alvo indireto, e a entrada do despachante só existe
     em início de bloco) e 204 fora de qualquer bloco.
   - (b) Rodar `prove_whole_aot_coverage.py --check completeness` no nosso programa. Ele lista de uma vez todo byte
     de código-forma sem dono, que é o que o upstream zerou.
   - (c) Um lote de serviços que o upstream já mostrou serem os próximos: `82bf5a2f`, `109bc32f`, `2734bbac`,
     `034405bd`, `c2324b12` (o `e63f6fcb` já é nosso: `51074da6`).

   **Ganho:** elimina de antemão as lacunas já mapeadas. Com o `82bf5a2f`, o probe para de "travar sem erro".
   **Custo:** ~3–4 h mais 1 rodada; a prova roda no Ryzen (programa de 87 MB). **Risco:** entradas do upstream que
   são dado (ex.: `0x1082E0`), por isso o filtro; conflitos de cherry-pick como nas levas 1–2.
3. **Encurtar a rodada de ~25 min para ~10 min:** delta shards (só as funções novas mudam de arquivo), `-j` acima
   de 3 no Ryzen depois de medir a RAM de um shard, e discover + generate no Ryzen (ou perfilados). **Ganho:** build
   de ~6–13 min para ~2–3 min e Python de 13–20 min para menos (a medir). **Custo:** ~2–4 h. **Risco:** baixo; o C++
   do OoT3D não muda.

Em 4º: o rastro pelo Azahar (`PreCodeTranslationHook`, ~15 linhas, build do Azahar no Ryzen). Vale quando o probe
passar do título e for preciso cobrir gameplay (cenas, máscaras) sem jogar no nosso runtime. Com o item 1 ele vira
segundo oráculo, não o caminho principal.

## Alternativas rejeitadas

- **Gdbstub para cobertura:** um breakpoint/passo por bloco pela rede; lento demais.
- **Unity build ou menos shards:** piora o incremental, que é o caso comum no bring-up.
- **Bring-up pelo registro de `PackedOp` como o upstream fez:** compila rápido, mas é um segundo gerador com
  padrões do OoT3D e continua parando no 1º bloco; o item 1 resolve o mesmo problema dentro do caminho que já usamos.
- **Catálogo/Forge-rs do upstream:** é distribuição (título pré-compilado + importação da ROM), não ajuda a achar
  lacunas.
- **Ghidra do zero (`ghidra_inventory.py`):** o upstream levou várias passadas para 92,5% do texto; a nossa
  descoberta própria já tem mais entradas (20 996 contra 18 751), e as sementes dele vêm de graça no item 2.

## Em aberto

- Quanto do MM3D o `ExecuteCore` + VFP cobre fora do registro (instruções de mídia ARMv6 e SEL estão na lowering,
  não sei se estão no núcleo interpretado): medir com o item 1 em teste sintético e no probe.
- Pico de RAM por shard em -O0 e se o ccache está instalado no WSL do Ryzen.
- Onde vão os 488 s do discover e os 300–680 s do generate (sem perfil ainda).
- Tempo de compilação do despachante de 21 MB, que muda em toda rodada.
- Como o "on the fly" do nathsou/3dsrecomp funciona por dentro, e o fallback do RPCS3, que não reli eu mesmo
  (só o agente).
