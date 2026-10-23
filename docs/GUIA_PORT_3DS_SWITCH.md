# Guia de port 3DS → Switch (receita reutilizável)

Receita e checklist para portar o **próximo** jogo de 3DS para o Nintendo Switch homebrew
reaproveitando o que foi feito no TriAevum (Zelda: Ocarina of Time 3D). O OoT3D é uma
recompilação estática ARM11/A32 → C++ (12.439 funções geradas em 256 shards, runtime próprio,
nenhum interpretador no produto), portada para o Switch com libnx, SDL2 e OpenGL/Mesa, compilada
com GCC 16 `-O3` e PGO.

O objetivo de longo prazo é o **Forge para Switch**: um recompilador genérico ROM → `.nro` que junte
a descoberta de funções do [fearkov/3dsrecomp](https://github.com/fearkov/3dsrecomp) com o
gerador e o runtime de Switch deste repositório. (O projeto original já usa o nome "Forge" para o
módulo que compila o título no Windows; aqui "Forge para Switch" é o plano equivalente para o
console.)

Fontes deste guia: [SWITCH_PORT.md](../SWITCH_PORT.md) (fonte principal),
[OOT3D_SWITCH_WHOLE_AOT_PORT.md](OOT3D_SWITCH_WHOLE_AOT_PORT.md),
[SWITCH_PESQUISA_2026-10-17.md](SWITCH_PESQUISA_2026-10-17.md),
[SWITCH_PESQUISA_XBOX360_2026-10-17.md](SWITCH_PESQUISA_XBOX360_2026-10-17.md),
[TRIAEVUM_AOT_LEVERS_RESULTS.md](TRIAEVUM_AOT_LEVERS_RESULTS.md),
[TRIAEVUM_NATIVE_CPU_COSTS.md](TRIAEVUM_NATIVE_CPU_COSTS.md), o código e as mensagens de commit
do branch `switch-port` (a partir de `eba0b4b7`, 2026-10-16). Números sem fonte no repositório estão
marcados como **a medir**; números das notas de sessão (testes no console que não viraram
documento) estão marcados como *(notas de sessão)*.

> Nada de ROM, `code.bin`, RomFS, C++ gerado, `.nro` ou derivados no Git nem em release. O `.nro`
> contém o código do jogo traduzido: ele é construído na máquina de quem tem o próprio dump.

## Índice

0. [O que é genérico e o que é do OoT3D](#0-o-que-é-genérico-e-o-que-é-do-oot3d)
1. [Pipeline](#1-pipeline)
2. [Substituições nativas (guest overrides)](#2-substituições-nativas-guest-overrides)
3. [Otimizações do gerador](#3-otimizações-do-gerador)
4. [Runtime Switch](#4-runtime-switch)
5. [Medição e diagnóstico](#5-medição-e-diagnóstico)
6. [Infraestrutura](#6-infraestrutura)
7. [Armadilhas e lições](#7-armadilhas-e-lições)
8. [Checklist do próximo jogo](#8-checklist-do-próximo-jogo)
9. [Lacunas conhecidas deste guia](#9-lacunas-conhecidas-deste-guia)

---

## 0. O que é genérico e o que é do OoT3D

| Peça | Genérico (vale para qualquer jogo 3DS) | Específico do OoT3D |
|---|---|---|
| Extração da ROM | `tools/triaevum_release/ctr_rom.py` (NCCH, ExeFS, RomFS, ExHeader) | adaptador USA→EUR canônico (`oot3d-usa-input-canonical-eur-ef210566`) |
| Descoberta de funções | front-end A32 em `tools/oot3d/native_a32_runtime/upstream/` (CFG, blocos, saídas dinâmicas) | o inventário `upstream/analysis/codebin_function_inventory.csv`, `runtime_discovered_entries.csv` e a seleção `whole_aot_functions.json` foram revisados à mão para o OoT3D |
| Gerador de C++ | `generate_whole_aot_cpp.py` / `whole_aot_cpp.py` e todas as opções da seção 3 | `HOST_WRITTEN_TEXT_RANGES` (0x003CC430, Obj_Mure) na dobra de literais; ganchos do TopScreen |
| Runtime de CPU/memória | `oot3d_native_a32_memory.*`, `oot3d_native_whole_aot_runtime.h`, VFP (`oot3d_native_a32_vfp_*`) — o nome tem "oot3d", o conteúdo não | 3 fronteiras de host auditadas do produto |
| Serviços CTR (fs, gsp, dsp, hid, ptm) | `oot3d_native_a32_ctr_*`, DSP HLE do Azahar (`third_party/azahar_audio`) | Master Quest (`oot3d_native_master_quest_unlock.h`), relógio lido por `nn::fnd::DateTime::GetNow` |
| GPU (PICA → GL) | `runtime/three_ds_recomp/src/fast/backends/gfx_opengl_pica*` (cache de estado, residência, economias exatas, perfil) | widescreen de menus (`gl_widescreen_menu_policy.h`), hook de projeção, TopScreen |
| Threads, áudio, pacing, perf | `oot3d_switch_threads.*`, `oot3d_switch_thread_layout.h`, `oot3d_host_audio_stream.*`, `oot3d_native_interpolation_pacing.h`, `perf.txt` | — |
| Substituições nativas | mecanismo inteiro (prólogo, recusa, harness, contadores) | as 23 funções da lista |
| Infra | build na Ryzen/WSL, ccache, PGO, deploy FTP/MTP | caminho `sdmc:/switch/oot3dre/` (vira `sdmc:/switch/<jogo>/`) |

Regra prática: **tudo que é "como" é genérico; tudo que é "quais endereços" é do jogo.**

---

## 1. Pipeline

```
ROM (.3ds/.cci/.cia decriptada)
 └─ ctr_rom.py ──────────────► extract/ (exefs/code.bin, ncch0/exheader.bin, romfs)
     └─ (adaptador de região, se houver)
 └─ generate_whole_aot.py ───► aot_program.json (funções, blocos, chamadas, saídas dinâmicas) + inventory.csv
 └─ generate_whole_aot_cpp.py ► gen/cpp/oot3d_whole_aot_shard_000..255.cpp + whole_aot_cpp_manifest.json
     └─ apply_guest_overrides.py (ou --guest-overrides) insere os prólogos nativos
 └─ CMake + devkitA64 (GCC 16, -O3, PGO) ► build-switch/oot3d_native_game.nro
 └─ stage-product.sh ────────► sd/switch/oot3dre/ (nro + game/ + resources.o2r + config/)
 └─ FTP (Sphaira, porta 5000) ou MTP ► cartão SD → abrir pelo Homebrew Menu em modo título
```

### 1.1 Comandos

Pré-requisitos: devkitPro (`switch-dev switch-sdl2 switch-mesa switch-glad switch-zlib`), `cmake`,
`ninja`, `ccache`, Python 3 com `capstone`. Para o cache de shaders em disco, o Mesa reconstruído
(seção 4.9).

```sh
# 1. Extração (e, para a ROM USA do OoT3D, o adaptador
#    input_adapters.adapt_extracted_inputs com a receita oot3d-usa-input-canonical-eur-ef210566)
python tools/triaevum_release/ctr_rom.py ...        # gera exefs/code.bin, ncch0/exheader.bin, romfs
export OOT3D_OPERATIONAL_oot3dExtractRoot=/caminho/extract

# 2. Programa estrutural e C++
cd tools/oot3d/native_a32_runtime
python generate_whole_aot.py --output $W/gen/aot_program.json --inventory-output $W/gen/inventory.csv
python generate_whole_aot_cpp.py --program $W/gen/aot_program.json --selection whole_aot_functions.json \
  --code $OOT3D_OPERATIONAL_oot3dExtractRoot/exefs/code.bin --output $W/gen/cpp \
  --shards 256 --shard-strategy affinity \
  --native-vfp-aarch64 --guest-overrides --giant-function-instructions 1000

# 3. Configurar e compilar o NRO
cmake -S . -B build-switch -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake \
  -DCMAKE_BUILD_TYPE=Release -DOOT3D_ENABLE_VULKAN_RENDERER=OFF \
  -DOOT3D_WHOLE_AOT_PRODUCT_MODE=ON -DOOT3D_REQUIRE_WHOLE_AOT=ON -DOOT3D_REBUILD_WHOLE_AOT=ON \
  -DOOT3D_REGENERATE_WHOLE_AOT=OFF -DOOT3D_WHOLE_AOT_GENERATED_DIR=$W/gen/cpp \
  -DOOT3D_WHOLE_AOT_PROGRAM=$W/gen/aot_program.json \
  -DOOT3D_WHOLE_AOT_CODE_BIN=... -DOOT3D_WHOLE_AOT_EXHEADER=... \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build build-switch --target oot3d_native_game_nro

# 4. Pasta do SD
scripts/switch/stage-product.sh build-switch/oot3d_native_game.nro /caminho/extract /caminho/sd
```

Alternativas prontas em `scripts/switch/`: `build-whole-aot.sh` (só o arquivo `.a` ARM64 dos
shards), `build-whole-aot-probe.sh` (NRO mínimo que executa um bloco e grava um recibo — bom
**primeiro portão** para um jogo novo), `build-product.sh` (produto inteiro), `run-product-eden.sh`
e `run-whole-aot-probe-eden.sh` (smoke no emulador Eden).

Tempos: build completo ~1h30 em 4 núcleos (notebook) e ~30–60 min na Ryzen 5 5600G com 12 jobs;
incremental com ccache ~1 min; build `use` do PGO 18 min *(notas de sessão)*.

### 1.2 O que muda num jogo novo

- `ctr_rom.py` é genérico. Adaptadores de região só existem para o OoT3D.
- **Descoberta de funções é o trabalho grande.** O OoT3D herdou um inventário revisado
  (`upstream/analysis/codebin_function_inventory.csv`) e uma seleção baseada em perfis
  (`whole_aot_functions.json`). Para outro jogo, a ideia do Forge é gerar esse inventário com a
  descoberta do 3dsrecomp (e usar o `verify` dele como teste), depois alimentar
  `whole_aot_program.py`. Entradas achadas só em execução vão para `runtime_discovered_entries.csv`.
- Os caminhos `sdmc:/switch/oot3dre/...` estão espalhados pelo runtime (configs, logs, PGO, cache de
  shaders). Para outro jogo, trocar o nome da pasta (lacuna: ainda não é uma única constante).
- Validar cedo com o probe (`build-whole-aot-probe.sh`): executa o primeiro bloco do entrypoint e
  confere a continuação por limite de blocos.

---

## 2. Substituições nativas (guest overrides)

### 2.1 O que são

Funções (ou laços dentro de funções) do jogo reescritas à mão em C++ nativo, com o **mesmo efeito
observável** da tradução: registradores, flags preguiçosas, VFP/FPSCR, memória, gerações de escrita,
orçamento de blocos, ganchos e rastreio. É uma "decompilação parcial" só onde o perfil manda.

- Implementação: `tools/oot3d/native_game_runtime/oot3d_native_guest_overrides.{h,cpp}`.
- Lista: `tools/oot3d/native_a32_runtime/guest_overrides.json` (formato `oot3d_guest_overrides_v1`).
- Ligação no C++ gerado: `apply_guest_overrides.py` (idempotente, marcadores `guest-override`,
  `--remove` desfaz) ou `generate_whole_aot_cpp.py --guest-overrides`. Só os shards das funções
  listadas mudam.
- Liga/desliga sem recompilar: `config/no_guest_overrides` no SD (desktop:
  `TRIAEVUM_GUEST_OVERRIDES=0`).

Dois tipos:

1. **Função inteira** (`{"entry": "0x..."}`): o prólogo vai no início de
   `Execute_<nome>_<ENTRADA>` e só age quando a função é chamada pela própria entrada.
2. **Bloco/laço** (`{"entry": bloco, "function": função, "resume": [PCs]}`): o prólogo vai logo
   depois do rótulo `block_<BLOCO>:` da função traduzida; a versão nativa roda o laço quente e devolve
   `resumePc`, para onde a tradução continua com `goto block_<PC>`, ou `resumePc = 0` com um fluxo de
   saída (limite de orçamento etc.). Antes da chamada o prólogo faz `commitState()` e depois
   `reloadState()`.

### 2.2 Escolher candidatas pelo perfil real do console

1. Gerar o mapa de nomes e copiar para o SD:
   `python summarize_guest_profile.py --program aot_program.json --export-function-map guest_functions.txt`
   → `sdmc:/switch/<jogo>/config/guest_functions.txt`.
2. Criar `config/guest_profile` (chaves opcionais: `sample_every=4`, `dump_frames=900`,
   `top_functions`, `top_blocks`, `window=1`, `max_gap_us=200`). Jogar a cena pesada alguns minutos.
   Sai `logs/guest_profile.txt`: tempo exclusivo por bloco e por função (contador de 19,2 MHz),
   mais o apêndice `# substituicoes nativas` com `nativa=N recusou=M` por substituição.
3. Agregar por função:
   `python summarize_guest_profile.py --program aot_program.json --switch-profile guest_profile.txt --top 40 --markdown`.
   O script junta dados estáticos (instruções, blocos, VFP, acessos à memória, chamadas, folha) e
   marca o que já tem substituição.
4. **Descontar a sobrecarga do perfil.** Cada entrada de bloco custa ~190 ns ao próprio perfil
   (no `guest_profile.txt` da floresta Kokiri os blocos curtos mostram 184–216 ns por entrada). Isso
   infla funções feitas de muitos blocos curtos (os laços dos efeitos de áudio apareciam com ~14%).
   O `summarize_guest_profile.py` estima a sobrecarga (2º percentil de ns/entrada) e subtrai; force
   com `--overhead-ns`.
5. Preferir: folhas ou quase folhas, laços de cópia/zeragem/busca, matemática VFP sem chamadas,
   funções com muitas entradas e pouco trabalho por entrada. Confirmar com uma segunda fonte (a
   contagem do PGO: `whole_aot_hot_from_gcda.py`). O lote de 2026-10-17 foi escolhido assim (as seis
   funções de maior contagem fora o áudio na coleta PGO 2).
6. Remover `config/guest_profile` do SD ao terminar: ele deixa o jogo mais lento.

> **Atenção (lido no código, 2026-10-17):** nos quadros medidos o perfil instala o callback de
> entrada de bloco em modo "todos os blocos" (`oot3d_native_a32_window_frame.cpp`, lista de PCs
> vazia). Com isso `Hooked()` dá verdadeiro para qualquer faixa e **toda** substituição recusa
> naqueles quadros. Consequências: (a) com `sample_every=N`, até ~1/N das chamadas aparecem como
> recusadas por causa do próprio perfil — na Kokiri (N=4) a maioria das substituições ficou entre
> 14% e 25% de recusa; (b) o tempo medido é sempre o da tradução, nunca o da versão nativa (bom
> para escolher candidatas, inútil para medir o ganho delas). Recusa real é o que passa muito disso
> (ver 2.6). Os contadores só são gravados com o perfil ligado (lacuna 9).

### 2.3 Escrever uma substituição EXATA

Contrato (cabeçalho de `oot3d_native_guest_overrides.h`):

- **`false` = recusa ANTES de qualquer efeito colateral.** A tradução roda inteira, intacta. Recusar
  quando: orçamento de blocos insuficiente (`Budget(context, n)`), algum gancho de bloco cobre a faixa
  da função (`Hooked(context, first, last)`), FPSCR fora do modo padrão (`VfpDefault`: LEN/STRIDE ou
  traps), acesso que não cairia no caminho rápido da memória (`Readable`, `Writable`,
  `SinglePageReadable`), ponteiros desalinhados, sobre a pilha ou sobre o próprio objeto, contagens
  fora do esperado (ex.: canais > 4 no efeito de áudio).
- **`true` = deixou tudo exatamente como a tradução deixaria**: GPRs, CPSR (com as mesmas máscaras
  de liveness do código gerado), VFP, FPSCR, bytes da memória, geração de escrita global e por
  página (cada palavra escrita conta +1, a página é carimbada com a geração da última palavra),
  `BlocksRemaining/BlocksConsumed` (consumir bloco a bloco; se o orçamento acabar no meio, sair com
  `BlockLimit` exatamente onde a tradução sairia) e o `Oot3dWholeAotFlow`.
- **Mesma ordem de acessos** à memória e as mesmas chamadas quando a tradução chama outra função
  (cabeçalho/cauda do `Runtime_Memcpy` usam as mesmas chamadas da tradução).
- Aritmética do guest: inteiros em `uint32_t` (wrap de MUL/MLA, ASR aritmético, arredondamento
  simétrico por sinal como o original); VFP pelas mesmas operações `Oot3dVfpNative*` que a tradução
  usa (não usar `float` do host direto).
- Contadores: `Decline(index)` incrementa `Declined`, `Accept(index)` incrementa `Calls`; a tabela
  `gOverrides` no `.cpp` precisa ter a entrada e o nome.

Passo a passo para uma nova:

1. Ler a função traduzida em `gen/cpp/oot3d_whole_aot_shard_NNN.cpp` (`Execute_*_<ENTRADA>`) e a
   desmontagem; anotar a faixa de PCs, quantos blocos cada caminho consome e o que é lido/escrito.
2. Declarar com `OOT3D_GUEST_OVERRIDE_DECLARE(<ENTRADA>)` ou
   `OOT3D_GUEST_BLOCK_OVERRIDE_DECLARE(<BLOCO>)` no `.h`, implementar no `.cpp`, acrescentar à tabela.
3. Acrescentar o item em `guest_overrides.json` (com `switch_share` do perfil).
4. Provar equivalência (2.4). Só então ligar no C++ gerado e recompilar os shards afetados.
5. Medir no console com o perfil **desligado** e comparar `perf.txt` (lógica por passo) com
   `config/no_guest_overrides`, mesmo save, mesma cena, **mesmo clock**.

### 2.4 Provar equivalência

- Harness: `tools/oot3d/native_a32_runtime/run_guest_override_equivalence.py` +
  `guest_override_equivalence.cpp`. O script extrai do C++ gerado (sem prólogos) cada função listada
  e o fecho das que ela chama, renomeia para `Translated_<ENTRADA>` (bloco: a função inteira e uma
  cópia com o prólogo, `Patched_<BLOCO>`, as duas começando no bloco) e compila com as mesmas flags
  de ponto flutuante do build (`-fno-fast-math -ffp-contract=off`).
- Cada caso parte de cópias idênticas (estado, memória, orçamento, ganchos, modo de rastreio) e exige
  tudo igual no fim: GPRs, CPSR, FPSCR, VFP, fluxo, orçamento, estatísticas, sequência de ganchos,
  bytes, gerações global e por página, impressão digital de escrita.
- Casos aleatórios + bordas: ponteiros em dados, atravessando página, desalinhados, somente leitura,
  não mapeados, sobre o objeto, aliasing, objeto encostado no fim da página, sobreposição, pilha,
  tamanhos enormes, orçamento que acaba no meio.

```sh
python run_guest_override_equivalence.py --generated $W/gen/cpp --code $W/code-canonico.bin \
  --nlohmann $W/build-switch/_deps/nlohmann_json-src/include --output /tmp/eq --trials 2000000
python run_guest_override_equivalence.py ... --only 0x004A0338            # uma só
python run_guest_override_equivalence.py ... --extra-flags "-O3"          # como o build
python run_guest_override_equivalence.py ... --codegen-cxx aarch64-none-elf-g++   # conta instruções ARM64
```

Volume usado até agora: 3M+1M casos por função de áudio e 3M no memcpy (ba8cce93); 300 mil por
função (500 mil na CirSquare) no lote de blocos (e26eb2d1); 1M por função em `-O3` depois do TBAA
(ad972f24). Zero diferenças em todos.

ASan/UBSan e qemu-aarch64: o harness não tem modo pronto (lacuna 9: `--extra-flags` não vai para o
link). O padrão de qemu do repo é `run_vfp_aarch64_tests_qemu.sh` (devkitA64 + flags dos shards +
`qemu_aarch64_test_runtime.cpp`, binário estático `-nostartfiles`), também usado por
`run_audio_dsp_mix_tests_qemu.sh` e `memory_lean_diff/run_store_sequence_diff.sh` (`ALVO=switch`).

### 2.5 As 23 substituições atuais (OoT3D)

`switch_share` = fração do tempo guest no perfil do console (coleta PGO 1); `kokiri_share` = perfil
de blocos de desktop na floresta Kokiri. São medidas do custo **traduzido**; o ganho real de cada
uma não foi medido isoladamente.

| Entrada | Nome | Tipo | O que faz | Peso no perfil |
|---|---|---|---|---|
| 0x004A0338 | AudioEffect_ProcessStereoReverb | função | reverberação estéreo, inteiros u32, orçamento bloco a bloco | switch 5,35% |
| 0x004A022C | AudioEffect_ProcessFourChannelDelay | função | atraso de 4 canais | switch 2,02% |
| 0x00371738 | Runtime_Memcpy | função | laço de 32 bytes vira cópia por trecho de página (+1 geração por palavra) | switch 1,38% |
| 0x003727F0 | Math_SinF | função | seno VFP | kokiri 1,00% |
| 0x00307C94 | PicaCommandWriter_UploadVertexFloatUniforms | função | upload de uniforms float para o pacote PICA | kokiri 1,03% |
| 0x003325F0 | Math3D_CylOutsideCylDist | função | distância cilindro–cilindro | kokiri 0,64% |
| 0x0036E168 | Math_SmoothStepToF | função | aproximação suave | kokiri 0,61% |
| 0x002F9430 | PauseRenderBuffer_SetVec2Range | função | preenche faixa de vec2 | kokiri 0,54% |
| 0x00372070 | oot3d_translate_mtx3x4_local | função | translação local de matriz 3×4 | kokiri 0,44% |
| 0x003094CC | FUN_003094cc | função | (sem nome) | kokiri 0,41% |
| 0x002CDE90 | FUN_002cde90 | função | (sem nome) | kokiri 0,36% |
| 0x00466E68 | MeshCommandPacket_Submit:copia | bloco | cópia do pacote para a lista | switch 3,02% |
| 0x003247B0 | BgCheck_LineTestImpl:zera_marcas | bloco | zera o mapa de polígonos | switch 2,53% |
| 0x0047AF24 | FUN_0047af24 | função | busca em 50 entradas | switch 1,31% |
| 0x002FA674 | CalculateGyroscopeAxisStatus:media | bloco | média sobre o anel | switch 1,04% |
| 0x00307B50 | PicaTev_ConvertSource | função | conversão de fonte do TEV | switch 0,84% |
| 0x00307DC8 | PicaCompare_ConvertFunction | função | conversão de função de comparação | switch 0,53% |
| 0x00308920 | NativeCurve_SampleFloat:busca | bloco | busca do quadro-chave | switch 0,53% |
| 0x00308604 | CsabCurve_SampleRotation:busca | bloco | busca do quadro-chave (rotação) | switch 0,34% |
| 0x002BBB14 | Math3D_CirSquareVsTriSquare | função | teste círculo×triângulo, só VFP | switch 0,70% |
| 0x00303C78 | FUN_00303b24:copia | bloco | cópia curta de 64 bytes | switch 0,23% |
| 0x0046700C | MeshCommandPacket_PatchBoneUniforms:copia | bloco | cópia curta em laço | switch 0,21% |
| 0x003563E0 | FUN_003562c0:percurso | bloco | percurso da lista de polígonos da colisão | switch 0,55% |

Ganho documentado: o primeiro lote (áudio + memcpy) foi estimado em ~4% da lógica (SWITCH_PORT.md).
O lote total (23) foi para o console junto com PGO 3, TBAA e fps60 (versão "final 3"), sem A/B
isolado: **a medir** com `config/no_guest_overrides`.

Referência de Windows (outra máquina, outro compilador): baixar os mesmos núcleos de áudio por
`--memory-region` não deu ganho (0,996, TRIAEVUM_AOT_LEVERS_RESULTS.md). No Switch a versão escrita à
mão apareceu como ganho porque o GCC não inlinava os helpers e a sobrecarga por bloco é maior —
medir antes de assumir que vale num terceiro alvo.

### 2.6 Lição: recusa alta mata o ganho

Contadores da sessão Kokiri (fps60 + overclock, `sample_every=4`, 2251 quadros medidos; arquivo
`switch-logs/profile-kokiri/guest_profile.txt`, fora do repo):

| Substituição | Recusou | Leitura |
|---|---:|---|
| BgCheck_LineTestImpl:zera_marcas | 99,4% | quase nunca roda nativa: pré-condição errada para o caso comum |
| CalculateGyroscopeAxisStatus:media | 96,4% | idem |
| FUN_003562c0:percurso | 76,0% | idem |
| NativeCurve_SampleFloat:busca | 65,5% | idem |
| CsabCurve_SampleRotation:busca | 59,1% | idem |
| as outras 18 | 14–25% | compatível com o artefato do perfil (1 de 4 quadros com todos os blocos ligados) |

Uma recusa custa o teste de pré-condições **mais** a tradução inteira, e nos blocos-laço ainda o
`commitState()/reloadState()`. Com 60–99% de recusa a substituição é pior que nada. Regras:

- Antes de ligar, rodar no console e olhar `recusou` contra `nativa` (descontando o ~1/N do perfil).
- Recusar só o que a tradução realmente faria diferente; tratar no caminho nativo o caso comum
  (ex.: varrer a faixa com várias páginas rápidas em vez de exigir uma só).
- Colocar no relatório o **motivo** da recusa (contador por motivo) é o próximo passo natural.

### 2.7 Atualizações pendentes

> Outro agente (worktree `.claude/worktrees/nativas2`) está ampliando as pré-condições das
> substituições com recusa alta e acrescentando novas (ex.: MeshCommandPacket_Submit). Completar
> aqui quando integrar: quais recusas caíram, novos itens da tabela 2.5, equivalência rodada e ganho
> medido.
>
> - [ ] recusas depois da correção (por substituição)
> - [ ] novas substituições
> - [ ] A/B no console com `config/no_guest_overrides`, mesmo clock

---

## 3. Otimizações do gerador

Tudo em `tools/oot3d/native_a32_runtime/generate_whole_aot_cpp.py` (emissão em `whole_aot_cpp.py`) e
no runtime de cabeçalho (`tools/oot3d/native_game_runtime/oot3d_native_whole_aot_runtime.h`,
`oot3d_native_a32_memory.h`). Genérico: vale para qualquer código ARM11.

| Técnica | Onde / opção | Validação | Rendeu |
|---|---|---|---|
| Caminho rápido inline + lento frio nos helpers | `OOT3D_AOT_HOT/COLD` (só GCC); `ReadFast/WriteFast` → `*Slow` | corpos lentos inalterados | a medir (no GCC quase todo helper virava chamada fora de linha) |
| LDM/STM por span | `_emit_block_transfer`; `ReadWordsFast/WriteWordsFast` | `test_whole_aot_transfer_lowering.py` (faltas no meio, escrita parcial) | a medir |
| Literais PC-relativos dobrados | padrão; `--no-literal-folding` desliga | testes de dobra/exclusão | a medir |
| VFP nativo AArch64 | `--native-vfp-aarch64` / `OOT3D_WHOLE_AOT_NATIVE_VFP_AARCH64` | 193,5 milhões de checagens no qemu, 16 modos | Switch: a medir; análogo x86 no Windows: 0,924 do tempo pré-NRI |
| Aliasing por TBAA | padrão no GCC; `-DOOT3D_GUEST_ACCESS_MEMCPY` volta ao memcpy | `memory_lean_diff/run_store_sequence_diff.sh` | −4,0% instruções, −6,7% loads, −7,2% stores, −19,5% recargas de pilha |
| Funções gigantes | `--giant-function-instructions N` + `--hot-functions` | símbolos externos iguais, `.text` +0,02% | Fishing_UpdateFish compila em 88 s (antes 192 s) |
| GCSE limitado | `OOT3D_WHOLE_AOT_GCSE_MEMORY_KB=32768` | shards quentes 091/243 byte a byte iguais | shard 100: PRE 85 s → 1 s |
| Sharding estável | `--shard-strategy affinity-incremental` | mesma distribuição na 1ª geração | trocar uma função: de 5–117 shards alterados para 0–1 |
| Modo enxuto | `0126356c`, `fbb68c15`; `OOT3D_WHOLE_AOT_LEAN_STATS` | `69a49b4d` (diferencial da memória contra a base) | a medir *(notas de sessão: ~10% de instruções)* |
| Orçamento de blocos | `WholeAotBlockBudget` 2048 no Switch | quadro ≤ 240 B, < 0,5 MiB da pilha de 1 MiB | Linux: 64 → 128 deu +3,93%; 2048: a medir |
| PGO (GCC) | `OOT3D_WHOLE_AOT_PROFILE_MODE=generate/use` | coleta 3 com 0 coverage-mismatch | Switch: a medir isolado; Windows: 0,864 do tempo pré-NRI |
| VFP com zero exato no nativo | `--vfp-zero-aware` (com `--native-vfp-aarch64`) | 299 milhões de checagens no qemu, 16 modos; 9 mutações detectadas; diferencial de 314 funções reais | ver SWITCH_PORT.md, "Rodada do cabeçalho" |
| Fluxo de 16 bytes | `OOT3D_WHOLE_AOT_FLOW_IN_REGISTERS` (padrão 1) | só layout; diferenciais com o layout novo | stores −18% a −33%, stores de pilha −28% a −69% (estático) |
| Pilha enxuta nas leituras/LDM | `--lean-stack` | diferencial de 314 funções reais (host e qemu) | ver SWITCH_PORT.md |
| Registradores em locais | `--local-state`; `--promote-registers` | `run_register_promotion_equivalence.py` | piora no A57 (3.8); desligado |

### 3.1 VFP nativo (`--native-vfp-aarch64`)

- Emite VADD/VSUB/VMUL/VNMUL/VDIV/VSQRT, VMLA/VMLS/VNMLA/VNMLS (como FMUL + FADD/FSUB **separados**),
  VCVT e VCMP/VCMPE como instruções AArch64 em `asm volatile`, com o modo do FPSCR do guest no FPCR e
  as flags acumuladas no FPSR (modelo do Dynarmic). Arquivo: `oot3d_native_a32_vfp_aarch64.h`.
- `Oot3dGuestFpScope` na entrada de `ExecuteOot3dWholeAotFunction`: carrega o FPCR do guest, na saída
  (inclusive unwind) junta o FPSR ao fpscr e restaura o do host. Chamadas externas, callbacks e
  caminhos lentos de memória (MMIO) rodam sob o FPCR do host.
- Onde o ARMv8 difere do modelo de software (sinal do zero com FZ, NaN em VSUB com DN=0, UFC com
  FZ=0, VCMP denormal com FZ=1) cai no software; aritmética com FZ=0 fica toda no software. O software
  continua sendo o oráculo.
- Validar: `run_vfp_aarch64_tests_qemu.sh` (≈299M checagens, valor e flags, 16 modos RMode×FZ×DN; as
  mutações conhecidas precisam ser detectadas).
- `--vfp-zero-aware`: em vez de mandar todo resultado zero/NaN ao software, uma metade fria
  (`ZeroAwareSlow<Op>`) mantém o zero nativo quando ele é exato nos dois modelos (operandos finitos
  e não subnormais com cancelamento exato, dois zeros ou fator zero). Cuidado com FZ: soma de
  normais que vira subnormal descartado NÃO é cancelamento exato. Mutações do critério:
  `run_vfp_zero_aware_mutations.sh`.

### 3.2 Aliasing (TBAA)

- Problema: `std::memcpy` em `uint8_t*` é acesso "qualquer tipo"; cada escrita guest obrigava o GCC a
  recarregar registradores guest, flags, orçamento, ponteiros da tabela de páginas e o contador de
  geração, e cada leitura obrigava a gravar o estado pendente.
- Solução (ad972f24): `ReadFast/WriteFast/ReadWordsFast/WriteWordsFast` acessam os bytes pela união
  empacotada `GuestCell` de `enum class` de 8/16/32/64 bits. Todos os tamanhos compartilham o conjunto
  de alias da união (acessos sobrepostos de tamanhos diferentes continuam ordenados); enum class não
  alia tudo, então o conjunto é disjunto do estado do host. Caminhos lentos, spans e regiões seguem
  com memcpy.
- **Contrato**: numa função que inlina esses caminhos, bytes guest só por esses auxiliares, memcpy,
  `char` ou `std::byte`. Quebrar isso é UB silencioso.
- Validar: diferencial de sequências inlinadas (100 mil no host e 100 mil no devkitA64/qemu, zero
  divergências). A mutação "um conjunto de alias por tamanho" precisa divergir (diverge em 100%).
- Rendeu (devkitA64 -O3, shards 000/050/091/100/180/243): instruções −4,0%, loads −6,7%, stores
  −7,2%, recargas de pilha −19,5%; funções do fixture no qemu −17,6% instruções.

### 3.3 Funções gigantes e tempo de compilação

- Medido (GCC 16.1 -O3, Ryzen 5600G): parsing de headers 1,3–2,4 s por shard (2–6%); o resto é
  otimização, concentrada nas funções gigantes: `sched1` 50–65% e PRE/gcse-after-reload 15–45% nos
  shards mais lentos.
- `--giant-function-instructions N` marca funções com ≥ N instruções guest com
  `[[gnu::optimize("no-schedule-insns","no-gcse-after-reload")]]` na própria linha da assinatura
  (números de linha não mudam). `--hot-functions lista.txt` exclui as quentes.
- Lista de quentes: `whole_aot_hot_from_gcda.py --gcda-dir <coleta> --generated-dir <cpp> --output hot.txt`
  (casa cada `.gcda` pelo `profile_id` = crc32 do nome mangled; casou 12439/12439, 3983 quentes) ou
  `whole_aot_hot_functions.py` a partir do `guest_profile.txt`.
- O atributo não muda `profile_id` nem checksums de CFG/linha: os `.gcda` valem com ou sem ele. No
  build `generate` pode marcar todas; no `use`, só as frias da coleta (com
  `-fprofile-partial-training` as não executadas continuam otimizadas para velocidade).

### 3.4 Modo enxuto (gerações de escrita e entrada de bloco)

- Memória (0126356c): rastreio de escrita vive na tabela de páginas (com ele ligado as entradas
  tipadas ficam nulas e o caminho rápido não testa nada); `FastPage` de 32 bytes; um contador só
  (`mRangeGeneration`): a escrita rápida faz `++` e carimba a página com o mesmo valor.
- Entrada de bloco: `BlockEntryGate` (filtro superconjunto calculado na inicialização: um load e um
  teste de bit no caminho quente; o lento continua exato). `RefreshBlockEntryGate()` para quem muda os
  ganchos depois.
- `BlocksConsumed()` derivado (fbb68c15): um contador só por entrada de bloco; código gerado com o
  layout antigo deixa de compilar em vez de errar em silêncio.
- `OOT3D_WHOLE_AOT_LEAN_STATS` (padrão = PRODUCT_MODE) compila fora as estatísticas por chamada.
- Validar: `69a49b4d` compara a memória nova com a da revisão base em sequências aleatórias
  (retornos, faltas, gerações global/por página, impressões digitais, `CaptureState`).

### 3.5 PGO com GCC no Switch

Fluxo:

1. Build instrumentado: `-DOOT3D_WHOLE_AOT_PROFILE_MODE=generate` (grava em
   `OOT3D_WHOLE_AOT_GCC_PROFILE_RUNTIME_DIR`, padrão `sdmc:/switch/oot3dre/pgo`). Usa
   `-fprofile-generate=<dir> -fprofile-prefix-path=<build> -fprofile-update=single` só nos shards e liga
   a libgcov. Nomes de `.gcda` planos e iguais entre builds
   (`CMakeFiles#oot3d_native_whole_aot.dir#...#oot3d_whole_aot_shard_NNN.gcda`).
2. Coleta no console: limpar a pasta `pgo/` do SD, jogar ~15 min nas cenas que importam (com as
   mesmas configs que o usuário vai usar, ex.: fps60 e escala 2). O jogo cria a pasta e grava os
   contadores a cada ~3 min (`__gcov_dump` + `__gcov_reset`, `oot3d_whole_aot_gcov.h`) e na saída.
3. Baixar os `.gcda` (FTP) para uma pasta da coleta.
4. Build final: `-DOOT3D_WHOLE_AOT_PROFILE_MODE=use -DOOT3D_WHOLE_AOT_PROFILE_FILE=<pasta>`
   (`-fprofile-use -fprofile-partial-training -Wno-missing-profile -Wno-error=coverage-mismatch`).
   O C++ gerado precisa ser o mesmo da coleta e estar no mesmo `OOT3D_WHOLE_AOT_GENERATED_DIR`.

Armadilhas resolvidas:

- **libgcov e `tpidr_el0`** (2be7235d): o libnx acha o TLS por `__aarch64_read_tp` (`-mtp=soft`), mas
  a libgcc do devkitA64 (profiler de chamadas indiretas, dados `__thread`) lê `tpidr_el0` direto, que
  o Horizon deixa em 0: o build instrumentado caía no primeiro call indireto. Correção em
  `oot3d_switch_threads.cpp`: espelhar o ponteiro de TLS em `tpidr_el0` num construtor de prioridade
  101 e em toda thread nova (`--wrap=threadCreate` e `--wrap=pthread_create`).
- **coverage-mismatch**: uma função cujo CFG mudou desde a coleta (header do runtime editado) só perde
  o perfil, em vez de quebrar o build (7e820da8). Conferir no log quantas perderam; a coleta 3 deu 0
  *(notas de sessão)*.
- A pesquisa do nfsmw-nx sugere `-fno-profile-values` e `-fprofile-correction` no Horizon (não
  adotado aqui; avaliar).

**O que invalida o PGO** (exige nova coleta, ou aceita perder perfil nas funções afetadas):
regenerar o C++ com outra opção que mude corpos (VFP nativo, literais, `--local-state`), mudar a
estratégia de shard (as funções trocam de arquivo), reaplicar/alterar substituições nativas nos shards,
mudar headers inlinados nos shards (`oot3d_native_a32_memory.h`, `whole_aot_runtime.h`, TBAA),
mudar o caminho do diretório gerado ou do build. Não invalida: mudanças só no runtime do host
(GL, áudio, threads), configs do SD, o atributo de funções gigantes.

Quando refazer: depois de qualquer item acima que mexa em funções quentes, ou quando o log do
`use` mostrar muitas `coverage-mismatch`. Coletas feitas: 1 (2026-10-16), 2 (enxuto), 3 (TBAA + 23
substituições + fps60).

### 3.6 ccache e sharding estável

- ccache com `base_dir=$HOME`, `hash_dir=false`, mantendo **o mesmo diretório de C++ gerado** entre
  builds (o caminho entra no hash e nos nomes de `.gcda`).
- `affinity` recalcula do zero: uma função a mais ou a menos remaneja ~30% dos shards.
  `affinity-incremental` preserva o shard de cada função existente (manifesto anterior) e põe as novas
  junto do vizinho mais chamado (teto 1,25× a média). Só troque de estratégia antes de uma coleta PGO.
- Ao copiar uma pasta de build para um segundo build, **não apagar** `.ninja_log`/`.ninja_deps`
  (vira recompilação total). Estimar antes com `ninja -n | wc -l`.

### 3.7 Exatidão de ponto flutuante

- Shards sempre com `-fno-fast-math -ffp-contract=off` (9466d722): com fast-math, 1,97 milhão de
  86,4 milhões de checagens VFP divergiam na flag IXC.
- O arquivo dos shards usa `-g0` (o Release do toolchain põe `-g`; o código gerado com debug ocupa
  centenas de MB).

### 3.8 Promoção de registradores para locais (`--local-state`)

> **Avaliado e desligado** (2026-10-17): `--promote-registers` (conjunto sujo exato por bloco,
> diferencial `run_register_promotion_equivalence.py`) piora no A57: shard 243 +36% de instruções,
> loads +77%, stores +89%. Integrado ao gerador e ao cabeçalho, desligado por padrão.
>
> Contexto já documentado: no Windows, `--local-state` em todas as funções + PGO não ganhou nada
> (1,000, módulo de 318 MB, TRIAEVUM_AOT_LEVERS_RESULTS.md). No Switch o nfsmw-nx mediu o
> salvar/restaurar de registradores guest em ~14% da thread principal
> (SWITCH_PESQUISA_XBOX360_2026-10-17.md), e a lógica do OoT3D é o gargalo na floresta Kokiri com
> fps60.

---

## 4. Runtime Switch

### 4.1 Threads, prioridades e núcleos no Horizon

Regras do Horizon (confirmadas pelo nfsmw-nx):

- Só a prioridade **0x3B** reveza tempo. Em qualquer outra, uma thread ocupada bloqueia as de
  prioridade igual ou menor no mesmo núcleo, mesmo com outros núcleos ociosos.
- Máscara de um núcleo nunca migra. O homebrew só tem os núcleos 0–2 (o 3 é do sistema).
- Sem embrulho, o libnx põe toda thread no núcleo 0 a 0x3B: render thread e compilação só rodavam
  quando o jogo esperava.

Layout `fixed` (padrão, `oot3d_switch_thread_layout.h`, aplicado por `--wrap=pthread_create` em
`oot3d_switch_threads.cpp`, pilha mínima 4 MiB):

| Papel | Núcleo | Prioridade |
|---|---|---|
| Jogo (CPU guest, DSP HLE, submissão) | 0 sozinho | 0x2C |
| Render thread | 2 | 0x2D |
| Worker de planos PICA | 1 | 0x2E |
| Áudio do host e do SDL (audren) | 1 | 0x2B |
| Serviços (spdlog, fila do cache do Mesa, ResourceManager, carregadores) | 1\|2 | 0x3B |

- `config/thread_layout` = `fixed`, `float` (render/worker com máscara 1|2) ou `legacy`; opção
  `pacer_spin_us=N`.
- O SDL do devkitPro traduz `SDL_THREAD_PRIORITY_TIME_CRITICAL` para 0x3B (abaixo do jogo!):
  `__wrap_SDL_SetThreadPriority` vira 0x2B.
- Serviços fora do caminho: toda pthread não classificada nascia na 0x2C e preemptava a render thread;
  agora 0x3B. spdlog descarta a linha mais antiga em vez de bloquear o jogo.
- Sem espera ativa: `svcSleepThread` acorda no prazo (pacer sem spin), condvar só é sinalizada
  quando alguém espera, atômicos por draw evitados (A57 sem LSE: o nfsmw-nx mediu 83 mil atômicos por
  quadro = 1,1 ms).
- Rendeu: o jogo sozinho num núcleo levou o SwitchWakerHD de 20 para 28 fps (referência externa).
  A render thread tirou ~10 ms de GL da thread do jogo (3d342fcd).

### 4.2 pthread, TLS e exceções

- `pthread_detach` do newlib do devkitA64 retorna `ENOSYS`: `std::thread::detach()` lança
  `std::system_error`. Não usar detach no Switch.
- **Ligar com `-fno-fast-math`**: com `-ffast-math` na ligação o `crtfastmath.o` entra depois do
  `crtend.o`, o ld descarta o terminador do `.eh_frame` e **toda** exceção C++ aborta.
- TLS: espelhar em `tpidr_el0` (3.5) se algo da libgcc for usado.
- FS do Horizon não renomeia por cima de arquivo existente: salvar config = remover e renomear.

### 4.3 Áudio: 48 kHz, reamostrador e thread própria

- O SDL2 do devkitPro passa `spec.freq` direto ao audren (não reamostra; a hint não tem efeito), usa
  só 2 wave buffers e espera em quadros de 5 ms o recém-enfileirado começar a tocar. Com buffers
  pequenos a voz passa fome (chiado) e a fila cresce até descartar.
- Solução: saída a 48000 Hz (`config/audio_sample_rate`) com reamostrador sinc-Kaiser polifásico
  próprio (`oot3d_host_audio_resampler`, ganho DC unitário por fase, saturação s16, ajuste fino de
  taxa ±0,5% para segurar a fila); buffer do dispositivo 1024 quadros (`config/audio_device_frames`,
  256–4096).
- Thread do host (`oot3d_host_audio_stream.*`, núcleo 1, 0x2B, acordada a cada envio e a cada 4 ms):
  stretch por períodos de pitch só em fila quase vazia ou lentidão sustentada; ocultação (repete o
  último período, fade) quando o jogo atrasa. O DSP HLE fica na thread do jogo: é sequencial com o
  guest e não paraleliza.
- Lição do stretch: a 1ª versão editava o tempo todo (26 min: 153 s inseridos, 346 s removidos) por um
  crédito com sinal; corrigido, a simulação de 30 min a 100% edita 264 ms (0,015%).
- Mixagem do DSP mais barata, bit a bit igual (8ee3f632): host 0,0506 → 0,0288 ms por quadro DSP
  (−43%); estimativa A57 ~0,6 → ~0,33 ms por passo. Prova: `oot3d_native_audio_dsp_mix_tests` +
  `run_audio_dsp_mix_tests_qemu.sh` (mesmo digest no x86-64 e no AArch64).
- Interpolação por fonte opcional (`config/audio_interpolation` = `cubic`/`sinc`); `original` é o
  código do Azahar bit a bit. No console: a medir.
- Diagnóstico: linha `audio:` do `perf.txt` (`device_hz` ~48000, `input_hz` ~32728 a 100%);
  `config/audio_dump` grava 60 s em `logs/audio_dump.wav`.

### 4.4 Render thread e apresentação paralela

- O contexto GL tem de viver na render thread: `Run()` da fila executa na thread chamadora, então o
  `MakeCurrent` vai por `Submit()+Drain()` (e46fe275). Sem isso, `glCreateShader` devolvia 0.
- Desligar para A/B: `config/serial_presentation`.
- Planos de draw PICA fora da thread do jogo (6471c483): a thread do jogo põe um placeholder e um
  worker monta o plano a partir do snapshot imutável da submissão (~2,5 ms por passo saíram da thread
  do jogo). `config/serial_pica` e `config/pica_plan_render_thread` para A/B.
- Encerramento (a3d7837f): `BeginShutdown`/`StopRendering()` primeiro; nenhum comando novo roda depois
  do pedido de saída (antes um quadro enfileirado caía no `glBufferData` e dava Data Abort).
- Contextos nvc0 do mesmo share group usam o mesmo push buffer **sem lock**: warm-up de shaders em
  outra thread trava; fazer na render thread.
- ImGui sem contexto GL atual na thread do jogo refazia shaders, buffers e atlas por quadro no despacho
  nulo do Mesa (2c0e30e9).

### 4.5 Ritmo, fps60 e interpolação

- O guest avança a 60 refreshes/s e o jogo dá um passo a cada dois. Apresentando a 30 Hz com limite
  de 1 refresh por apresentação o jogo roda a ½ velocidade (o Eden escondia isso). Limite 2 (com a
  render thread, 4 davam a mesma velocidade com metade dos quadros: ~11 contra ~21 fps).
- fps60: `config/fps60`; o modo de pacing vem do lançamento (`oot3d_native_launch_frame_rate.h`).
- Replay nativo pulado no GL (`config/native_replay=1` volta): o namespace nativo só serve para
  read-back/savestate, que o GL não usa; desenhar a cena duas vezes custava metade da GPU a 30 FPS.
- Governador de pacing (`oot3d_native_interpolation_pacing.h`, `config/interp_pacing`): desenha o
  intermediário cheio, reduzido (`config/interp_scale`) ou o descarta quando o par passo+intermediário
  não cabe em 33,3 ms. Começa cheio, só desce com medição real (3 decisões acima de 100%), sobe ao
  caber em 97%, sondas a cada 60.
- Tempo de GPU sem stall (`oot3d_native_gpu_frame_timer.h`): o relógio da GPU do switch-mesa não está
  em ns (SwitchWakerHD mediu 1,63 ns reais por "ns"); calibrar contra a CPU em ≥ 3 s antes de usar.
- **Tempo de GPU sem a espera da janela** (2026-10-22): o `end − begin` do quadro incluía o buraco
  em que a GPU fica ociosa enquanto a render thread espera o `nwindowDequeueBuffer` (lição 26),
  no meio do quadro. No tour2 o governador via 18,5 ms por passo contra 12,3 ms reais (glperf) e
  descartava 74 de 75 intermediários (`interp_dropped`): o "60 FPS" era 30 imagens distintas com
  repetição. Dois timestamps em volta do acquire (`Fast::SetWindowAcquireHook`,
  `NativeGpuFrameBusyTicks`) tiram o buraco; `config/interp_count_acquire_wait` volta a medida
  antiga. Teste: `oot3d_native_gpu_frame_timer_tests`. Junto, um `glFlush` logo antes do acquire
  entrega a cena já gravada à GPU enquanto a CPU espera (`config/gl_acquire_noflush` tira).
- **Dois quadros em voo** (`config/frames_in_flight`): com 1, o fim de quadro do jogo espera a
  render thread terminar o quadro anterior, que espera o display; um passo longo (16–18 ms) seguido
  de um intermediário curto não se compensa e o passo perde o refresh. O modelo
  (`oot3d_native_cadence_governor_tests --report`, cenário "áreas lentas") com o passo de 21 ms:
  55 → 59,7 imagens/s; 24 ms: 40,9 → 59,6. **No console travou** no primeiro warp (tour3b): jogo e
  render thread dentro do `malloc` com o heap corrompido — estado compartilhado da sessão supõe o
  jogo no máximo 1 quadro à frente. O stream suporta (`SetMaxFramesInFlight`, testado), a sessão
  não: a chave é ignorada até auditar o que os comandos capturam por referência (rodar com TSan).
- Resultado: fps60 com a versão "final 3" = 48,6 fps médios, 46% ≥ 55, velocidade 99,7%
  *(notas de sessão)*. Áreas pesadas ficam limitadas pela lógica no núcleo 0.

### 4.6 Heap sem fragmentação

- O malloc do newlib tem lock global, compartilhado com o Mesa; o `libdrm_nouveau` aloca cada BO com
  `memalign`. Blocos grandes de tamanhos variados por quadro fragmentam: `heap_arena_mb` foi de 631 a
  1659 MB com ~700 MB em uso (8,5 min, fps60).
- Regras: nada de `new`/`free` grande por quadro; pools que devolvem a memória limpa
  (`Oot3dPicaPlanVector`), vetores que mantêm a capacidade, lotes reciclados (o worker de planos tinha
  152 KiB de heap por draw contra 34 KiB do serial e degradava de 3,9 para 89 ms por quadro em 3 min).
- Anéis de upload ≤ 2 MiB (até 2 MiB o nouveau subaloca de slabs; acima, cada orphan é um `memalign`).
- `mallinfo()` percorre o heap inteiro segurando o lock: só com `config/heap_stats` ou
  `config/gpu_timing`, a cada `config/heap_check_windows` janelas.
- Teste de longa duração obrigatório: `oot3d_native_pica_visual_frame_soak_tests` (12 MB e 4,8
  alocações ≥ 256 KiB por quadro → 0,24 MB e 0), `oot3d_native_pica_deferred_plan_soak_tests`.

### 4.7 Cache de estado GL e memória de GPU

- `PicaGlStateCache` (`gfx_opengl_pica_state_cache.h`, sem GL, com teste): só emite a chamada que muda
  o valor; esquecido em toda fronteira com o caminho legado. Quadro sintético de 400 draws: 27,5 → 2,7
  chamadas de estado por draw. `config/gl_state_cache=0` para A/B.
- VAO: o caminho PICA não pode vazar o VAO para a UI legada (HUD herdava divisores de instância,
  53d32a84); religar sob demanda.
- Residência (`gfx_opengl_pica_residency.h`): texturas por conteúdo com LRU e orçamento
  (`config/gl_texture_budget_mb`, padrão 96), anéis de upload com map sem sincronização, falta de
  memória não é fatal (pula o draw, conta `gl_oom`). Sem isso o console caía após ~8 min com fps60.
- Uniforms soltos fazem o nvc0 copiar o bloco inteiro pela FIFO (SwitchWakerHD: UBO std140 cortou
  comandos a 1/3); não reenviar blocos iguais.
- **Uniforms enxutos** (`config/gl_uniform_trim`, padrão ligado; `0` desliga;
  `gfx_opengl_pica_uniform_trim.h`, teste `gfx_opengl_pica_uniform_trim_tests`): na criação do
  programa GL o GLSL final é varrido e guarda-se o prefixo `[0, N)` de cada bloco que ele pode ler
  (membro → fim no layout de `pica_uniform_layout.h`; índice literal `f[12]` → até o registrador;
  índice não literal → o membro inteiro; endereçamento relativo a0/aL = `get_offset_register()`
  chamado → `f[]` inteiro, só definido e nunca chamado → não conta; qualquer coisa não entendida →
  bloco inteiro). O draw copia ao anel só esse prefixo, mas **reserva e liga a faixa do bloco
  inteiro como antes** (mesmo `glBindBufferRange`; o resto da faixa fica com lixo que ninguém lê),
  então o estado GL não muda e os bytes lidos são idênticos. O dedup passou a comparar só o
  prefixo (bloco anterior que difere só na cauda não lida é religado). Corpus do console (271
  programas do `shadercache`): fragmento 6112 → **1184 bytes em 257** (sem iluminação: para em
  `texture_lod_bias`) e 2096 nos 14 com iluminação; **vértice fica em 1616 nos 271** (todo programa
  chama `get_offset_register`, indexação relativa, e lê `f[95]`). `draw_uniform_sent_bytes_kb`
  passa a contar os bytes copiados. Lição: o tamanho declarado do UBO é contrato do layout, não do
  que o shader lê; medir no corpus real antes de prometer (o bloco de vértice não encolhe).

### 4.8 Economias de GPU (exatas)

  Console (tour13, 23/10, pgo5h, GPU 614, Link andando): uniforms 551→203 KB/quadro (Water Temple), 777→318 (Market); `draw_submit_ms` 8,3→3,5 (com o fim da assinatura); média das 29 áreas 58,96→59,4, Water Temple 54,7→59,4, Kakariko 55,9→58,8. **Market segue 58,6 com 2 intermediários e 72 `hold` por janela**: passo 19,4 + intermediário 17,5 ms > 33,3, então o governador descarta o intermediário (60 Hz de imagem com 30 quadros distintos). Faltam ~4 ms no par; alvo: `step_cpu_other` 5,9 ms (cópia dos planos de 15,7 KB em `resolve_plans`) e `pica_top` 6,0.
- `config/gl_lean` (padrão): pula cópia de display transfer repetida, limpeza da janela que o scanout
  cobre, limpeza de Shadow2D sem uso, uniforms iguais; reaproveita o EASU ao reapresentar.
- `config/gl_gpu_lean` (padrão, `gfx_opengl_pica_gpu_lean.h`): logic op COPY → desligado, blend
  identidade → desligado, draw sem efeito não é emitido, fill de depth em D16/D24 limpa também o
  stencil. Prova: modelo de fragmento + checagem em GL real (0 bytes diferentes). Ganho: incerto sem
  medir (estimativa 0,2–0,6 ms no quadro cheio para o logic op).
- Nvc0 do Mesa 20.1: ZCULL nunca liga (`ZCULL_REGION=0x3f`, `nvc0_validate_zcull` sob `#if 0`).
  Early-Z por pixel é automático salvo `discard`, `gl_FragDepth` ou escrita em imagem.
- **Tela de baixo no perfil TopScreen** (`config/skip_bottom_screen`, ausente = desenha como antes):
  o framebuffer 240×320 da tela de baixo nunca aparece nesse perfil (HUD e menus vêm das
  primitivas de UI do guest), mas custava ~2,7 ms de GPU e ~0,5 ms de render thread por passo
  (glperf `pica_bottom`). O agendador deixa fora da apresentação os draws, fills e transfers que só
  produzem essa tela (`oot3d_native_bottom_screen_skip.h`), tudo ou nada por amostra: nada sai se
  outro draw amostra ou escreve na memória dela, ou se um transfer a copia para algo usado. Uso em
  amostra posterior não é visto: é o risco da chave. O transfer de topo (submetido à parte) conta
  como transfer da amostra, e um transfer posterior cuja fonte foi pulada é deixado de fora
  (`BottomSkipTracker`; antes o GL lançava "PICA display transfer source is missing" ~5 s após a
  abertura) e esse alvo volta a ser desenhado. No tour4 quase nada era pulado (`pica_bottom` 2,4 ms):
  a regra "tudo ou nada" recusava as amostras; a suspeita é o depth buffer único das duas telas
  (comum no 3DS), agora aceito (só draws da tela de baixo testam aquele depth; o fill que também
  cobre o uso da tela de cima fica). O `perf.txt` ganhou `bottom_skip: draws= missing_sources=` e
  as amostras por motivo (`skipped`, `no_bottom`, `unsafe`, `color_shared`, `depth_shared`,
  `texture_read`, `transfer_into`, `transfer_top`, `transfer_used`). Teste:
  `oot3d_native_bottom_screen_skip_tests`.
- Resolução: `config/resolution_scale` 1/2/2,5/3; FSR 1 (`config/upscaler=fsr`), AA, anisotropia. Em
  3× (720p) a GPU virou gargalo: 23,4 contra 28,5 FPS em 2×. Padrão 2×.
- Junção de draws (técnica do aurora/Dusklight): **medida e descartada, código removido**. A
  assinatura por draw (`step_mergeable=` etc., `gfx_opengl_pica_merge_stats.h`) mostrou no tour11
  (23/10, 15 áreas) 0,7 juntáveis em 74,7 draws por quadro (~1%), maior sequência 1,8: quase todo
  draw troca textura ou uniform (matriz por objeto), ao contrário do GX do Twilight Princess. A
  própria assinatura (FNV byte a byte de uniforms e vértices) custava 3–4 ms por quadro da render
  thread com `gpu_timing` (`draw_perf_sig_ms`, tour12), então foi apagada (fase `perf_sig`, campos
  `*_merge*` e o teste saíram); tours com `gpu_timing` anteriores saem pessimistas nesse tanto.
  Quebra do draw no tour12 (Water Temple, 122 draws, 69 µs/draw com a assinatura): `glDraw*` 1,9 ms,
  vértices 1,1, uniforms 0,8 (**551 KB de uniforms por quadro**, 181 envios), estado 0,5, upload de
  textura 0,5, fora do submit 0,9; `sec_resolve_plans` 2,0 ms; hash de textura no draw = 0.
- `sec_resolve_plans` (~2 ms na render thread, Water Temple) **não é plano montado tarde**: no
  tour12, numa janela da área com `plan_late_draws=0`, `plan_late_ms=0` e `plan_lock_wait_render_ms=0`
  (o worker montou tudo a tempo) a seção ainda deu 1,73 ms; nas outras janelas o atraso real é
  0,08–0,19 ms por quadro. O custo é **pegar** o plano pronto: `Oot3dPicaVulkanDrawPlan` tem
  **15.712 bytes** (dois `Oot3dPicaGeneratedFragmentShader` de 6.272 com o estado de uniforms
  inteiro, o normal e o uber, mais 1.688 do de vértice), e `plan = std::move(batch.Plans[i])`
  copia tudo isso por draw: ~1,9 MB por quadro, frio (o lote do worker e o vetor do quadro somam
  mais que o L2 de 2 MB). Microbench no host (x86, -O2): 0,43 ms por quadro de 122 movimentos; a
  identidade estrita (`ComputeOot3dPicaVisualDrawIdentity`) é ~0. Não há correção simples sem
  estado compartilhado novo: o caminho é encolher o plano (estado de uniforms fora do struct do
  shader, ou o uber só quando existe) ou fazer o quadro guardar o plano por ponteiro em vez de
  movê-lo; os dois mudam o layout usado pelo agendador e pela interpolação. Sem mudança por ora.

### 4.9 Mesa 20.1 contra Mesa 26

- Mesa 20.1 (switch-mesa do devkitPro): reconstruído com os patches de cache em disco do
  SwitchWaker/SwitchWakerHD (+ correção `thrd_success == 4` para newlib),
  `MESA_SHADER_CACHE_DIR=sdmc:/switch/oot3dre/mesacache` antes do SDL criar o contexto; shaders vistos
  carregam em ~12 ms em vez de 1–3 s.
- Mesa 26: converte GLSL para NIR no `glCompileShader` e guarda o NIR no objeto de shader. Com os
  shaders anexados, `glDeleteShader` só marca: fazer **`glDetachShader` antes de apagar**
  (−2,0 MB por programa no host). O resto (~6–8 MB por programa no console contra ~1,5 MB do 20.1) é
  do nvc0 guardando o NIR inteiro: só com patch no Mesa.
- A 30 FPS com cache quente os dois empatam *(notas de sessão)*; com cache frio o 26 trava mais.
- **Cache pronto no pacote** (2026-10-22): a chave do cache do Mesa é o build do Mesa + o chip + a
  fonte do shader, sem nada do aparelho; os `.src` do `shadercache/` (fontes PICA→GLSL que o
  warm-up da abertura compila) somam entre SDs. Como gerar: passar por todas as áreas (warp pelo
  canal lab, 5 s cada, ver 5.3) no console ou no Eden, juntar os `.src` (união) no SD do console,
  abrir o jogo uma vez (warm-up compila e grava o `mesacache/`, inclusive a pasta
  `mesa_shader_cache_sf/<id>/NV12B/*.foz`) e copiar `shadercache/` + `mesacache/` para o pacote.
  Conferir em `shadercache/status.txt`: `warmup_skipped=0` e `warmup_seconds` baixo (269 shaders
  em 2,3 s). No tour, áreas como Water Temple foram de 11,7 a 44 FPS só com o cache quente.

### 4.10 Crash reports do Atmosphère

- Arquivos em `sdmc:/atmosphere/crash_reports/`. O `.nro` é carregado pelo hbloader em endereço
  variável: achar a base alinhando os endereços de retorno da pilha com instruções `bl`/`blr` do ELF
  (para a base certa, `retorno − base − 4` cai num `bl/blr` em quase todos os quadros); depois
  `aarch64-none-elf-addr2line -e oot3d_native_game.elf` com `endereço − base`.
- Guardar o `.elf` exato de cada `.nro` enviado (`build-switch/oot3d_native_game.elf`).
- "perf.txt parou e não há crash report" também é saída normal: perguntar antes de concluir crash.

---

## 5. Medição e diagnóstico

### 5.1 Arquivos no SD (`sdmc:/switch/oot3dre/`)

| Arquivo | Conteúdo |
|---|---|
| `boot-status.txt` | etapa da inicialização (`stage=runtime_running`), contagens do whole-AOT, erro legível |
| `logs/perf.txt` | uma linha `host_frames=...` a cada 150 quadros + blocos `clocks:`, `audio:`, `interp:`, `sync:`, `threads:`, `glperf` |
| `logs/oot3d_native_game.log`, `logs/glthread.txt` | log geral e troca de contexto GL |
| `logs/guest_profile.txt` | perfil por função (com `config/guest_profile`) |
| `logs/failed_pica_*.glsl` | fonte de shader PICA que falhou |
| `logs/hang.txt` | watchdog de travamento: etapa e PC/LR/pilha de cada thread quando o jogo ou a render thread param > 3 s; "HEAP CORROMPIDO" da varredura do heap |

### 5.2 Campos importantes do `perf.txt`

- `glperf` CPU por draw PICA (só com `config/gpu_timing`; `step_`/`interp_`, ms por quadro de host): `draw_<fase>_ms` com fases `validate program target state uniforms vertices tex_hash tex_lookup tex_upload indices draw rest` (soma = `draw_submit_ms`, tempo dentro do `SubmitPicaDraw`; a fase `perf_sig` da medição de junção foi removida), `draw_outside_ms` (etapas PICA menos submit = trabalho do chamador entre draws), `draw_us` (submit por draw), contadores por quadro `draw_draws`, `draw_tex_lookups`, `draw_tex_hashes`, `draw_tex_hash_bytes_kb`, `draw_tex_uploads`, `draw_tex_upload_bytes_kb`, `draw_uniform_sent[_bytes_kb]`, `draw_uniform_skipped[_bytes_kb]`, `draw_vertex_bytes_kb`, `draw_index_bytes_kb`; e `sec_<resolve_plans|pin_copy|scene_env|execute|visual_sample>_ms` (parede de trechos da render thread, para decompor `cpu_other`; `execute` inclui os draws, `visual_sample` inclui `pin_copy` e `execute`). Fonte: `gl_pica_draw_phases.h` (teste `gl_pica_draw_phases_tests`).
- Linha principal (ms por quadro de host): `fps`, `guest` (lógica do jogo), `dsp_mix`, `audio_out`,
  `pica_submit`, `pica_plan`, `pica_backend`, `visual_present`, `visual_replay`, `present`,
  `pacing_wait`; memória `heap_arena_mb`, `heap_arena_peak_mb`, `proc_used_mb`/`proc_total_mb`
  (sempre estáveis numa sessão longa); GL `gl_textures_live`, `gl_tex_mb`, `gl_programs`,
  `gl_buffers_live`, `gl_oom`, `gl_draws_skipped`.
- `clocks:` `cpu_mhz gpu_mhz mem_mhz` (clock real medido pelo sys-clk; `clk_src=sysclk_real`, ou
  `sysclk` = alvo, `none` = sys-clk ausente → MHz -1), `op_mode=handheld|docked`,
  `perf_mode=normal|boost`, `sysclk_profile`, `sysclk_enabled`, `sysclk_api`, `clk_ipc_us`. Só
  leitura: uma chamada `GetCurrentContext` ao `sys:clk` por janela, sessão aberta uma vez; o app
  nunca abre clkrst/pcv nem altera clock (SwitchWakerHD issue #33).
- Planos: `plan_worker_ms`, `plan_late_ms`, `plan_late_draws`, `plan_batches_live/queued/allocated`
  (pequenos e constantes), `plan_lock_wait_*`.
- `sync:` `guest_wait_frames_ms` (jogo esperando a render thread), `render_idle_ms`,
  `pacer_oversleep_max_ms` (> ~0,5 ms → testar `pacer_spin_us=200`).
- `threads:` `layout=… <papel>_cpu=% @c0:c1:c2` (CPU por papel via `svcGetInfo(ThreadTickCount)`):
  `game_cpu` 95–98% no núcleo 0 = gargalo na lógica.
- `audio:` `min_queued avg_queued device_hz input_hz trim_pct game_speed inserted_ms removed_ms
  concealed_ms`.
- `interp:` `level step_frames interp_frames interp_dropped reduced_refused gpu_ms_<tipo>
  rt_ms_<tipo> est_*`.

### 5.3 Ferramentas

- `python3 tools/oot3d/perf_report.py perf.txt [--session -1] [--skip 2] [--windows] [--top 8]`:
  resumo por sessão (uma sessão nova começa quando `host_frames` diminui): fps, clocks, fases,
  memória e, com `glperf`, top etapas de GPU, CPU da render thread e contadores. Se o clock muda no
  meio da sessão, avisa (janela e clock antes/depois) e separa fps/`total_ms` por clock.
  Em sessões a 60 FPS termina com o **veredito 60 FPS**: GPU e render thread *ocupada* (StartFrame→swap
  menos ocioso e pacer) contra 16,7 ms, o par de passos da lógica (`game_pair_ms`) contra 33,3 ms e as
  travadas (`hitch_*`) da sessão; marca o que estoura. Para ter GPU e render thread, `config/gpu_timing`.
- Bench de input (`config/bench_record` / `config/bench_replay`, `oot3d_input_bench.h`): grava o
  controle por quadro do jogo a partir do warp de boot e reproduz o mesmo trajeto em cada build; no
  replay o próprio jogo passa a tela título. `~/oot3d-work/bench/run_bench.sh <nro> <rótulo>` roda no
  Eden e tira a média do `perf.txt`. Serve para regressão de comportamento, não para medir velocidade
  (ver lição 22).
- Canal lab (`config/dev_lab`, `oot3d_dev_lab.h`): comandos JSON por linha em
  `lab/commands.jsonl` (warp, wait, capture, status, graphics, peek...), respostas em
  `lab/acks.jsonl`. No console o FTP do Sphaira **fecha com o jogo aberto**: deixar um roteiro em
  `lab/script.jsonl`, que o jogo lê uma vez na abertura (renomeia para `script.running`), começando
  por `{"op":"gameplay"}` (espera o jogador ter o controle). Tour de desempenho: warp por cada área
  "Better" (0..48; a 49 é a sala Debug e congela) + `wait` de 300 quadros, depois
  `tools/oot3d/lab_tour_report.py` casa as janelas do `perf.txt` com os acks pelo quadro do guest
  (soma de `guest_fps × 150/fps` por janela) e ordena as áreas por FPS. Rodar **duas vezes**: a
  primeira passada mede compilação de shader, a segunda o peso real.
  Tour andando: `{"op":"stick","y":1}` segura o circle pad (x/y em -1..1, ±154 no HID; y=1 anda
  para a frente) até outro `stick`; `{"op":"stick","y":0}` solta. Pôr depois do warp e soltar antes
  do próximo (no Market, y=-1: para a frente sai da área).
  `"buttons":["R"]` no mesmo `stick` segura botões do 3DS (A B X Y L R ZL ZR) até o próximo `stick`;
  apertar = `stick` com o botão, `wait` de ~6 quadros, `stick` sem ele.
- `config/gpu_timing` (N, padrão 30): GPU por etapa com `GL_TIMESTAMP` sem bloquear, CPU da render
  thread por etapa, contadores de chamadas GL (ganchos nos ponteiros do glad), separado por tipo de
  quadro (`step`/`interp`/`hold`). `gpu_*` inclui espera da CPU quando a render thread é o gargalo.
- `config/gl_stats`: draws, chamadas de estado emitidas/puladas, binds, `gl_us_per_draw`.
- `config/heap_stats`, `config/heap_check_windows=1`: acha corrupção de heap ~5 s depois de ela
  acontecer. Usa uma varredura limitada dos bins do newlib (`oot3d_heap_bin_check.h`), não o
  `mallinfo()`: este percorre as listas sem limite segurando o lock do malloc e, com uma lista
  corrompida, congela o processo inteiro sem crash (foi o congelamento de 2026-10-17).
- `config/debug_force_game_over` (exemplo de gatilho de teste específico do jogo): zera a vida após
  N s para testar uma tela rara.
- Fotos do Status Monitor (overlay) e capturas do álbum (`album_sd:` via FTP) para GPU %/núcleos.

### 5.4 Travamentos sem crash report: watchdog

Um congelamento (imagem parada, nada no `crash_reports`) não tem pilha. Receita usada no port:

1. **Marcadores baratos por thread** (`oot3d_hang_watchdog.h`): um slot por papel (jogo, render,
   planos, áudio) atribuído onde a thread já recebe prioridade/núcleo; a etapa atual é um store
   relaxed, o progresso um load + store da dona (por quadro / por comando, nunca por draw).
2. **Thread vigia de prioridade mínima** (0x3B) acordando a cada ~1 s; "travado" = jogo ou render
   thread não ociosa sem progresso por > 3 s com o app em foco (`appletGetFocusState`); ignora
   saltos do próprio relógio (sleep do console).
3. **Gravar sem alocar**: a trava pode ser o lock do malloc. Texto num buffer estático com
   formatação própria (nada de `printf` de float, que aloca no `dtoa` do newlib), arquivo pela API
   de FS da libnx (`fsdevGetDeviceFileSystem("sdmc")`, `fsFsOpenFile` com `Append`, `fsFileWrite`
   com `FsWriteOption_Flush`). Uma vez por travamento; "recuperou após N ms" se voltar.
4. **PC/LR das outras threads**: `svcSetThreadActivity(h, Paused)` (o kernel espera a thread sair
   do núcleo), `svcGetThreadContext3`, ler a cadeia de FP e varrer a pilha com
   `svcQueryMemory` antes de cada leitura, `svcSetThreadActivity(h, Runnable)`. Só funciona com a
   thread pausada e só se o NPDM permitir: testar `envIsSyscallHinted(0x32)`/`(0x33)` antes (SVC
   não permitida derruba o processo). `svcGetDebugThreadContext` exige sessão de debug e não serve
   para o próprio processo. Endereços: offset em relação ao início do segmento de código do NRO
   (`svcQueryMemory` num endereço de função) → `addr2line`.
5. **Última chamada de serviço** (nome literal + ticks de início/fim) em volta de cada IPC que
   pode bloquear (`smGetService` de serviço inexistente espera o registro; porta cheia espera
   sessão).
6. **Ler a última linha do `perf.txt`**: um `ofstream` por janela grava em pedaços de `BUFSIZ`
   (1024 no devkitA64); uma linha cortada sem `\n` diz que a thread parou dentro daquele bloco,
   depois do último pedaço e antes do próximo — dá para localizar o trecho pelo número de bytes.

### 5.5 Protocolo de A/B

1. **Mesmo clock.** Overclock (sys-clk: CPU 1785, memória 1600) e clock padrão dão resultados
   diferentes: com clock padrão a lógica do OoT3D chega a ~17 ms por passo (game_cpu 95–98%, jogo
   entre 71 e 100% da velocidade); com overclock fica 30 FPS cravado com game_cpu ~60%
   *(notas de sessão)*. **Sempre comparar com o mesmo clock; o `perf.txt` agora registra**
   (bloco `clocks:` em cada janela, `perf_report.py` mostra e separa por clock). Sem sys-clk os MHz
   ficam -1: aí perguntar o clock antes de comparar.
2. Mesma cena e mesmo save, 2–3 min por modo, descartar as primeiras janelas (`--skip`).
3. Cache de shaders quente nas duas pontas (a primeira passada compila).
4. Uma variável por vez, por arquivo em `config/` (quase tudo tem chave de A/B) — sem recompilar.
5. Para lógica, olhar `guest` e `game_cpu`; para GPU, `glperf`; para áudio, `device_hz` e ocultações.

---

## 6. Infraestrutura

### 6.1 Build na Ryzen (WSL2)

- Máquina de build: Ryzen 5 5600G, Windows 11, WSL2 Ubuntu (12 GB / 12 threads no `.wslconfig`),
  devkitPro em `/opt/devkitpro`, mesmos caminhos `/home/<usuário>/...` do notebook (para ccache e
  `.gcda` casarem). Acesso por ssh com chave (shell PowerShell). Endereço e usuário ficam fora do repo.
- Scripts (hoje **fora do repo**, em `C:\Users\<usuário>\`): `ryzen-build.sh`/`ryzen-status.sh` (build
  completo e estado), `rt-build.sh` (recompila só o runtime a partir de `repo.tar.zst` e recusa se os
  shards mudarem; devolve `rt.nro`), `lean-use.sh` (build `use` do PGO), `chk.sh`. Rodar com
  `wsl -d Ubuntu -u root -- bash /mnt/c/Users/<usuário>/<script>`; evitar aspas aninhadas pelo
  PowerShell.
- Copiar fontes: `git ls-files | tar | zstd` local → `scp` para o Windows → extrair no WSL.
- **O WSL desliga quando a última sessão `wsl.exe` fecha.** Um keepalive por ssh morre se o notebook
  suspender. Para build longo sem o notebook acordado, criar uma tarefa agendada no Windows
  (`schtasks`) que rode `wsl -d Ubuntu -u root -- sleep N` (`Start-Process` via ssh não sobrevive ao
  fim da sessão).
- O notebook (4 núcleos) só para edição e comandos leves: conferir `sensors | grep Package` abaixo de
  ~85 °C antes de qualquer coisa pesada (já chegou a 98 °C com agentes compilando).

### 6.2 Deploy

- **FTP do Sphaira**: `ftp://<IP do Switch>:5000/switch/oot3dre/`. O IP muda com a rede (achar
  varrendo a porta 5000). `curl -T arquivo.nro ftp://<ip>:5000/switch/oot3dre/` sobrescreve direto.
  **Conferir o tamanho** depois (ex.: `curl -sI ftp://<ip>:5000/switch/oot3dre/oot3d_native_game.nro`
  e comparar `Content-Length` com o local, ou `md5sum` de uma cópia baixada).
- Baixar logs e `.gcda`: `curl ftp://<ip>:5000/switch/oot3dre/logs/perf.txt -o perf.txt`.
- **MTP** (Sphaira): `gio mount mtp://<dispositivo>/`, pasta `microSD card/switch/oot3dre`. MTP não
  sobrescreve: `gio remove` e depois `gio copy`.
- Abrir pelo Homebrew Menu **em modo título** (segurando R num jogo), não pelo álbum (memória
  limitada).
- Guardar cópia de cada NRO enviado com nome e md5 (`~/oot3d-work/oot3d_native_game.<versão>.nro`) e o
  `.elf` correspondente.

### 6.3 Testes de host

- Regra: **mudança que roda o tempo todo precisa de teste de longa duração antes de ir ao console**
  (o worker de planos vazava heap; o stretch de áudio oscilava; os dois só apareceram em minutos de
  jogo).
- Política sem GL/sem hardware em cabeçalhos puros com teste: `gfx_opengl_pica_state_cache_tests`,
  `gfx_opengl_pica_residency_tests` (30 min simulados), `gfx_opengl_pica_gpu_lean_tests`,
  `gfx_opengl_pica_scale_tests`, `gl_widescreen_menu_policy_tests`, `oot3d_switch_thread_layout_tests`,
  `oot3d_native_interpolation_pacing_tests`, `triaevum_presentation_clock_tests`,
  `oot3d_native_gpu_frame_timer_tests`, `oot3d_native_bottom_screen_skip_tests`,
  `oot3d_native_cadence_governor_tests` (modelo do pipeline; `--report`).
- Concorrência com ASan/UBSan e TSan: `oot3d_native_render_command_stream_tests`,
  `oot3d_native_render_shutdown_tests`, `oot3d_native_reduced_replay_stress_tests`,
  `oot3d_native_pica_deferred_plan_tests`.
- Soak: `oot3d_native_pica_visual_frame_soak_tests`, `oot3d_native_pica_deferred_plan_soak_tests`.
- Exatidão bit a bit: `oot3d_native_audio_dsp_mix_tests`, `oot3d_native_audio_interpolation_tests`,
  `oot3d_native_a32_vfp_aarch64_tests` (qemu), `memory_lean_diff`, `run_guest_override_equivalence.py`.
- O backend GL do Linux é o mesmo do Switch: bom proxy para correção visual (no Linux, remover um
  `glFinish` global por completion levou de 23,3 a 49,7 FPS), mas **não** é evidência de desempenho
  do console. O Eden serve para smoke de boot, não para medir.

---

## 7. Armadilhas e lições

1. Emulador (Eden) esconde bugs do console: meia velocidade, contexto GL, `pthread_detach`.
2. `-ffast-math` na **ligação** quebra todas as exceções C++ (`.eh_frame` sem terminador).
3. `-ffast-math` nos **shards** quebra o VFP do guest (flags IXC).
4. `pthread_detach` → `ENOSYS` no newlib do devkitA64.
5. libgcov lê `tpidr_el0`, que o Horizon deixa em 0: espelhar o TLS do libnx.
6. Prioridade igual não reveza no Horizon (só 0x3B): thread de serviço na prioridade do jogo trava a
   render thread; `TIME_CRITICAL` do SDL vira 0x3B.
7. Contexto GL só na thread que o tornou atual; nvc0 sem lock no push buffer compartilhado.
8. FS do Horizon não renomeia por cima.
9. Heap do newlib: lock global e fragmentação; `mallinfo()` trava tudo (e com heap corrompido
   não termina nunca: congelamento sem crash); nada de alocação grande por quadro.
10. Cache de texturas sem descarte = crash por memória de GPU depois de minutos.
11. Substituição nativa com recusa alta é pior que nada; o perfil guest força recusas nos quadros
    medidos.
12. O perfil por bloco infla blocos curtos (~190 ns por entrada): descontar.
13. Mudou C++ gerado ou header inlinado → o PGO perde funções; planejar coleta nova.
14. `affinity` remaneja ~30% dos shards a cada mudança de seleção: usar `affinity-incremental`.
15. Não apagar `.ninja_log`/`.ninja_deps` ao duplicar build.
16. Comparar desempenho só com o mesmo clock, mesma cena, cache quente.
17. 720p (3×) passou de gargalo de CPU para gargalo de GPU: padrão 2×.
18. O relógio de GPU do switch-mesa não está em ns: calibrar.
19. WSL desliga com a última sessão: `schtasks` para builds longos.
20. Prompt de agente: dizer onde pode compilar (Ryzen) e o limite de temperatura do notebook.
21. Congelou sem crash report: ver `logs/hang.txt` (watchdog) e onde a última linha do
    `perf.txt` foi cortada antes de supor deadlock.
22. Não otimizar guiado pelo Eden (2026-10-22): ele roda o ARM do Switch por JIT no x86, varia 5–10%
    entre rodadas iguais, a 1ª rodada de cada .nro é mais lenta (JIT frio) e, com a janela fora da tela
    (workspace escondida no Hyprland), o FPS cai a ~⅓. O mesmo build fez 3 FPS escondido e 10 visível;
    o console já fazia 30 FPS a 99,5%. Medir no console, com `config/gpu_timing`.
23. Cota de blocos do whole-AOT no Switch é 2048 (pilha de 1 MiB da thread principal). Substituições
    nativas com laço longo (reverb/delay de áudio, ~2–3 mil blocos) saem pela cota no meio do laço e o
    resto roda na tradução, porque a retomada entra pelo `switch(entryPc)` da função traduzida, não pela
    substituição. Subir a cota para 16384 não mudou a lógica no Eden (82 ms nas duas): não é o freio.
24. O perfil guest (`config/guest_profile`) custa ~1,5 µs por entrada de bloco no Eden e dobra o tempo
    do jogo; o perfil sai plano (nenhuma função > ~10%): o custo está espalhado na tradução, e
    substituir função por função não chega a ganhos grandes.
25. Os logs antigos do console já dizem muito: antes de pedir rodada nova, passar o `perf_report.py`
    em `~/oot3d-work/switch-logs/*`. Em `iscale` (sessão 46, ~17/10, 60 FPS interpolado) a GPU usava
    10,4 ms e a render thread 18,3 ms ocupada por quadro interpolado: o limite era CPU de render, com
    ~1 MB de upload de textura por quadro mesmo nos quadros sem cena nova.
26. Etapa `clear` cara na render thread (4-5 ms) é espera, não trabalho: o EGL do Mesa no Switch
    (`egl_switch.c`) só chama `nwindowDequeueBuffer` (bloqueante) no primeiro clear/desenho do
    framebuffer 0. Limpar a janela no começo do quadro trava a thread antes da cena (que vai para
    FBOs próprios). Deixar o clear da janela pendente até o primeiro uso real (scanout, UI, ImGui,
    cópias, fim do quadro): `GfxRenderingAPIOGL::FlushDeferredWindowClear`, `config/gl_eager_clear`
    desliga. Descontada essa espera, os 18,3 ms "ocupados" da lição 25 viram ~13,6 ms. Vale para
    qualquer port com o mesmo Mesa: medir o clear da janela antes de culpar a CPU.
27. FTP de homebrew de menu (Sphaira) só existe com o menu aberto: para automatizar algo durante o
    jogo, o comando tem que estar no SD antes da abertura (roteiro lido no boot) ou vir de um
    sysmodule. O Wi-Fi do Switch também dorme com a tela apagada: o FTP some até acordar.
28. Medir por área antes de otimizar (2026-10-22): no tour com cache quente, 20 de 49 áreas ficaram
    cravadas em 60 e as piores (Water Temple 44, Market 46, Castle Town Buildings 51) têm o guest
    acima de 12 ms por quadro: é lógica do jogo, não GPU. Uma área só (o Mercado) não representa o
    jogo.
29. Tempo de GPU por timestamps de início e fim do quadro mede também a GPU ociosa (2026-10-22):
    se a render thread bloqueia no meio do quadro (acquire da janela, fence, fila), o buraco entra
    na conta. Comparar o span com a soma das etapas do glperf (18,5 contra 12,3 ms no tour2) e
    marcar os bloqueios com timestamps antes e depois. Um governador alimentado com o span inflado
    descartava quase todo quadro intermediário: o contador `fps` (apresentações por segundo) seguia
    perto de 60, mas `interp_dropped` e `hold_2` mostravam ~30 imagens distintas.
30. Pular trabalho de GPU que "ninguém usa" exige seguir a fonte até o fim: o transfer de topo é
    submetido fora da lista de transfers da amostra e um transfer de quadro posterior pode ler o
    alvo pulado. E mais folga na fila entre threads (2 quadros em voo) não é só um número: o heap
    corrompeu em 21 s porque estado compartilhado supunha 1 quadro (2026-10-22).
31. Uma otimização "tudo ou nada" precisa de contador de motivo desde a primeira versão: o skip da
    tela de baixo foi ao console, rodou sem erro e não pulou quase nada (tour4), e sem o motivo no
    `perf.txt` só restava adivinhar (depth compartilhado entre as telas era a suspeita).
32. Um skip nunca pode esvaziar a saída: ao salvar o jogo, todos os draws da amostra eram da tela de
    baixo e a composição ficou sem draws (crash no PGO5). Amostra só com draws de baixo (menu, salvar,
    pausa) nunca é pulada (`kept_empty`) e as 60 amostras seguintes também não (`kept_menu`).
    Console (tour10, 23/10, pgo5b, CPU 1785/GPU 614/mem 1600): 29 áreas lentas sem crash, save
    final ok; média 59,59 e 27 áreas ≥59 (PGO5 sem skip, GPU 768: 59,56 e 25); 28.388 amostras
    puladas, 120 `kept_menu`, 2 `kept_empty`, 0 `unsafe`.
33. **Relógio no Switch: `cntpct_el0` (ou `armGetSystemTick()`), nunca `cntvct_el0`.** O Horizon não
    libera o contador virtual no EL0: `mrs cntvct_el0` dá Data Abort na primeira leitura (pgo5e
    morria na abertura com `config/gpu_timing`, PC exatamente no `mrs`). Guarda em
    `tests/switch/test_bug_guards.py`; conferir no ELF com `objdump -d | grep -c cntvct_el0` = 0.
34. **Uma medição cara sai do caminho quente assim que responde a pergunta.** A assinatura de
    junção custava 3–4 ms por quadro (mais que todos os uniforms) e ficou ligada a cada tour com
    `gpu_timing`, inflando as conclusões; e antes de otimizar uma seção medida, conferir o que ela
    contém: `sec_resolve_plans` parecia "plano atrasado" mas era cópia de structs de 15,7 KB
    (`plan_late_draws=0` e a seção ainda em 1,7 ms).

---

## 8. Checklist do próximo jogo

Estimativas a partir do que o OoT3D levou no Switch (o port de Switch inteiro levou ~2 dias de
trabalho intenso com vários agentes, partindo de uma recompilação de Windows já madura). Para um jogo
sem recompilação prévia, as etapas 2–4 dominam e são **a medir**.

| # | Etapa | Saída / critério | Esforço estimado |
|---|---|---|---|
| 1 | Dump próprio, extrair com `ctr_rom.py`, conferir ExHeader (entrypoint, `.text`) | `code.bin`, `exheader.bin`, RomFS | horas |
| 2 | Descoberta de funções (3dsrecomp + front-end A32) → inventário | `codebin_function_inventory.csv` do jogo | a medir (dias a semanas) |
| 3 | `generate_whole_aot.py` + seleção inicial (todas as funções) | `aot_program.json` sem saídas residuais | a medir |
| 4 | `generate_whole_aot_cpp.py` (sem otimizações extras) e build do arquivo ARM64 | `build-whole-aot.sh` passa | 1 dia |
| 5 | Probe no Eden e no console (`build-whole-aot-probe.sh`) | recibo `whole-aot-probe.txt` | horas |
| 6 | Serviços CTR que o jogo usa (fs, gsp, dsp, hid, ptm, relógio da shared page) | `boot-status.txt` chega a `runtime_running` | dias (depende do jogo) |
| 7 | Produto Switch: SDL2 + GL, `stage-product.sh`, pasta do jogo | imagem na tela no console | 1–2 dias |
| 8 | Correções de console da seção 7 (já resolvidas no runtime: conferir que valem) | sem crash, velocidade certa | horas |
| 9 | Threads/prioridades (`thread_layout`), render thread, áudio 48 kHz | `perf.txt` com `threads:`, `audio:` limpos | horas (reuso) |
| 10 | Opções do gerador: VFP nativo, TBAA, enxuto, gigantes, `affinity-incremental`, ccache | build completo + testes de exatidão | 1 dia |
| 11 | Coleta PGO 1 no console → build `use` | `.gcda` sem coverage-mismatch | meio dia (+ build) |
| 12 | Perfil guest → candidatas → substituições exatas com harness | equivalência 1M+ casos, recusa baixa | 1–3 dias |
| 13 | GPU: cache de estado, residência, economias exatas, escala/FSR | `glperf` estável, memória estável em sessão longa | reuso + horas |
| 14 | Soak de 30+ min no console (heap, GPU, áudio) | campos de memória estáveis | horas |
| 15 | Coleta PGO final com tudo integrado → build final | NRO final + `.elf` guardados | meio dia (+ build) |
| 16 | Documentar no guia o que mudou (genérico vs. específico) | este arquivo atualizado | horas |

---

## 9. Lacunas conhecidas deste guia

- **Contadores de recusa contaminados pelo perfil**: só são gravados no apêndice do
  `guest_profile.txt`, e o perfil força recusa em todos os quadros medidos. Falta gravar
  `nativa/recusou` no `perf.txt` sem o perfil (e um contador por motivo de recusa).
- A mensagem de `DescribeOot3dGuestOverrides` cita `config/guest_overrides_off`, mas o arquivo que
  desliga é `config/no_guest_overrides`.
- `run_guest_override_equivalence.py` não repassa `--extra-flags` ao link (ASan/UBSan não ligam) e não
  tem modo qemu-aarch64.
- A faixa de texto do perfil guest está fixa no código para o OoT3D (`0x00100000`, tamanho
  `0x003CA000` em `oot3d_native_a32_window_setup.cpp`); para outro jogo, ler do ExHeader.
- Caminho `sdmc:/switch/oot3dre/` espalhado pelo runtime; falta uma constante por jogo.
- Scripts da Ryzen (`ryzen-build.sh`, `rt-build.sh`, `lean-use.sh`) fora do repo.
- Sem script para achar a base do NRO nos crash reports (o `resolve_whole_aot_minidump.py` é do
  Windows).
- Ganhos isolados no console (A/B no mesmo clock) ainda **a medir** para: VFP nativo, modo enxuto,
  orçamento 2048, cada coleta PGO, as 23 substituições, `gl_gpu_lean`.
- `OOT3D_SWITCH_WHOLE_AOT_PORT.md` ainda descreve o orçamento de 128 blocos e a apresentação sem
  interpolação; o produto atual usa 2048 e tem `config/fps60`.
- Descoberta de funções para um jogo novo (integração com o 3dsrecomp) ainda não foi feita: as etapas
  2–3 do checklist são o maior risco.
