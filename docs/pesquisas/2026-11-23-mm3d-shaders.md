# MM3D: censo dos CMB e pack deko3d previsto (técnica do pack v8/v9)

Data: 2026-11-23 · Título(s): Majora's Mask 3D EUR Rev 1 (CTR-P-AJRP, 0004000000125600), OoT3D ·
Commits de referência: af088449 (romfs + censo), a508928b (CMB → registradores → GLSL),
a7424d3b (pack v8 previsto), 6884c113 (pack v9 = v8 + 8 bases paramétricas)

## Pergunta

Quais fragment shaders o MM3D vai pedir ao backend deko3d, descobertos offline com a mesma técnica
do pack v8/v9 do OoT3D (censo do TEV dos CMB → registradores PICA → GLSL do gerador real → partes
de cena × partes de material)? Quanto o MM3D tem em comum com o OoT3D, qual o tamanho do pack
previsto e o que falta para ele ir ao console. Pedido extra: comparar com o que o upstream
(origin/main, coccofresco/TriAevum) já tem em `titles/mm3d/shaders/`.

## Fontes

- RomFS: `~/oot3d-work/conv/mm3d/extract/romfs.bin` (664 428 544 bytes, gerado pela conversão; igual
  byte a byte ao de `conv/mm3d-ptbr`), extraído com `tools/oot3d/romfs_extract.py` em
  `~/oot3d-work/conv/mm3d-shaders/romfs` (2139 arquivos, 635 MB, 3 min 15 s).
- OoT3D: romfs já extraído em `~/oot3d-work/romfs` (spike de 05/11).
- Shaders conhecidos do OoT3D (fonte das partes de cena): 271 `.src` (PGS1) de
  `~/Documentos/backup-switch-2026-11-17/switch/oot3dre/shadercache` + `logs/deko3d_shaders*` do mesmo
  backup = 343 fragment únicos, copiados para `conv/mm3d-shaders/oot3d-ref/srcglsl`. A pasta do spike
  de 05/11 (`gen/`, `srcglsl/`, `v8glsl/`) e `deko3d-pack-v7/glsl` não existem mais.
- Gerador: `oot3d_deko3d_register_glsl_dump` compilado do worktree deko3d (1c22eabd + estas mudanças),
  build manual com -j1 em scratchpad a partir dos comandos do `~/oot3d-work/build-host-john`.
- Upstream: `git show origin/main:titles/mm3d/shaders/<arquivo>`, `titles/mm3d/catalogs/README.md`,
  `docs/TRIAEVUM_TITLE_SHADER_PACKS.md` (sem checkout).

Reproduzir (de `~/projetos/TriAevum/.claude/worktrees/deko3d`, M=`~/oot3d-work/conv/mm3d-shaders`):

```sh
python3 -I tools/oot3d/romfs_extract.py ~/oot3d-work/conv/mm3d/extract/romfs.bin $M/romfs
python3 -I tools/oot3d/cmb_tev_census.py $M/romfs
python3 -I tools/oot3d/cmb_tev_registers.py $M/romfs $M/regs.txt
oot3d_deko3d_register_glsl_dump $M/regs.txt $M/gen > $M/gen.idx
python3 -I tools/oot3d/deko3d_shader_predict.py $M --conhecidos $M/oot3d-ref/srcglsl \
    --conhecidos ~/oot3d-work/bench/mq-p6n/shaders --conhecidos ~/oot3d-work/bench/mq-p6p-adulto/shaders \
    --conhecidos ~/oot3d-work/deko3d-shaders-faltantes-20261028 \
    --conhecidos ~/oot3d-work/deko3d-shaders-faltantes-20261028b --saida mm3dglsl
python3 -I tools/oot3d/deko3d_shader_pack.py --pack $M/bases-v9.pack --glsl $M/mm3dglsl \
    -o $M/deko3d.mm3d-previsto.pack -j 1
```

## Conclusão

### Formato: o censo do OoT3D não via o MM3D (confirmado)

- **CMB versão 10.** No OoT3D o CMB é versão 6 (u32 em +0x08): ponteiros `mats`/`tex` em 0x28/0x2C
  e material de 0x15C bytes. No MM3D é versão 10: um bloco a mais (`qtrs`) antes de `mats` empurra os
  ponteiros para 0x2C/0x30, e o material tem **0x16C** bytes. Medido: em `kakusiana_0_info.zsi` a
  tabela de combiners começa em mats+0x13F4 = 0xC + 14 × 0x16C e tem 32 combiners válidos até `tex`.
  O contador e os índices do TEV continuam em 0x120/0x124 (único offset válido em 923 materiais do
  MM3D e 837 do OoT3D), e os campos 0x12C–0x15B têm a mesma distribuição de valores nos dois jogos
  (alpha test em 0x130, função em 0x132): os 0x10 a mais ficam no fim. O README de catálogos do
  upstream diz o mesmo ("CMB v10 adds a chunk pointer before materials (tex at header +0x30)").
- **LzS.** O OoT3D não tem nenhum `.lzs` nem LzS (0 arquivos), então o censo de 05/11 não
  descomprimia nada e não precisava. No MM3D, 182 `.zsi`, 108 `.gar.lzs` e 110 `.ctxb` são LzS
  (`LzS\x01`, u32, tamanho descomprimido, tamanho comprimido; LZSS de Okumura, janela de 4 KiB em
  0xFEE). Outros 355 `.gar.lzs` são GAR sem compressão apesar do nome. O filtro antigo
  (`.zar/.zsi/.gar/.cmb`) também pulava todo `.gar.lzs`.
- Dos 1788 CMB do MM3D, 859 estão dentro de arquivos LzS e 929 fora. Sem as mudanças, o censo
  antigo daria 0 materiais válidos no MM3D (todo CMB v10 cai no `mats` errado).

### Censo do MM3D

| | OoT3D | MM3D |
|---|---|---|
| modelos (CMB) | 1998 | 1788 |
| materiais | 11 173 | 8278 |
| programas TEV únicos (só combiners) | 534 | **1190** |
| TEV + luz de fragmento + nº texturas + alpha test | 579 | 1263 |
| materiais únicos em registradores (`cmb_tev_registers`) | 599 (24 com luz) | **1266 (1113 com luz)** |
| partes de material únicas no GLSL (bloco TEV + alpha) | 542 | 1152 |
| estágios por programa | 1:80 2:233 3:165 4:49 5:6 6:1 | 1:94 2:310 3:459 4:253 5:63 6:11 |

Falhas de leitura: 0 nos dois; enum GL desconhecido: 0; GLSL com erro: 0 (1266 e 599 gerados).

### Sobreposição com o OoT3D

- Programas TEV (mesma identidade do censo): **80 em comum** de 1190 do MM3D (6,7 %); 1110 só no
  MM3D; união 1644. Com luz/texturas/alpha test: 70 em comum de 1263.
- Materiais em registradores: 86 linhas idênticas entre os 599 do OoT3D e os 1266 do MM3D.
- O MM3D usa luz de fragmento em 1113 de 1266 materiais (88 %); o OoT3D em 24 de 599 (4 %). No
  GLSL, 777 de 1152 partes de material do MM3D leem a cor da luz no TEV; no OoT3D, 27 de 542.
- Dos 5588 shaders previstos para o MM3D, 337 já estão no pack v9/v10 do OoT3D.

### Conferência do gerador atual com o OoT3D

Rodar o fluxo do OoT3D com o gerador deste worktree (modo padrão, `oot3d-ref/`) deu 8 partes de
cena × 542 partes de material → **2188** fragment (o v8 de 05/11: 2181 fragment, 2187 shaders) e
reproduziu 342 dos 355 conhecidos. O gerador e os `.src` de 10/2026 ainda falam o mesmo texto,
então as partes de cena do OoT3D casam com os blocos de material gerados hoje.

### Pack previsto do MM3D

- 8 partes de cena do OoT3D (fog 150 usos, sem nada 122, wbuf 39, fog+wbuf 16, quatro com luz+sombra
  15) × 1152 partes de material do MM3D, podado pela luz → 4608; mais os 245 materiais marcados com
  luz no CMB (`regs.txt.luz`) que não leem a luz no TEV, nas 4 cenas com luz → **5588 fragment**
  (4088 com luz), 76 MB de GLSL em `conv/mm3d-shaders/mm3dglsl/`.
- Estimativa pelo tamanho médio dos DKSH do v8 (5695 B com luz, 3002 B sem): ~27 MiB.
  Compilado (abaixo): 27,7 MiB.
- O `ShaderStore` lê o pack inteiro para a RAM de uma vez (a7424d3b) e copia cada DKSH para os
  32 MiB de memória de código só quando o draw pede; um pack de ~27 MiB cabe, mas é 3,5× o v9.

**Pack compilado** (uam, -j1, `cc1plus` = 0 antes de começar): `conv/mm3d-shaders/deko3d.mm3d-previsto.pack`
= **5596 shaders** (5588 previstos + 8 bases paramétricas do v9, tiradas de v9 − v8; 0 vertex),
**0 falhas, 28 372 KiB (27,7 MiB)**, 5 min 53 s. O v9 do OoT3D tem 2195 shaders e 7,8 MiB.

### Upstream (origin/main): o que eles têm e o que serve para nós

- `titles/mm3d/shaders/mm3d_native_fragment.spvpack` tem **16 bytes: cabeçalho SPVK com 0
  entradas**. Não há fragment shader do MM3D derivado da ROM no upstream: o MM3D deles desenha com a
  família uber de luz nativa (`pica_native_lighting_uber.spvpack`, 104 programas + os 36 originais =
  140 no startup, `kMm3dNativeFragmentLighting = true`) e uma lista de módulos uber de startup
  gravada de sessões reais (`TRIAEVUM_RECORD_UBER_MODULES`); o `mm3d_startup_uber_modules.json` não
  está no repo. Não houve censo estático de material: o README de catálogos diz "Not a full material
  census" e "Observed Gzw/Layout coverage is not an all-title corpus".
- Vertex: `mm3d_native_vertex.spvpack` (1,5 MB) vem de tradução **estática** dos 5 SHBIN do romfs
  (`shaders/DebugShader, DummyShader, GzwShader, LayoutShader, LineShader.shbin`; hashes conferem com
  a nossa extração) pelo decompilador do Citra/Azahar: 6 entradas, 14 módulos SPIR-V (9 de passe do
  título); o DVLE de geometria do `LineShader` (entrada 1) ficou sem tradução.
  `mm3d_translated_vertex_programs.h` é só referência (não compila em nenhum alvo).
- **O texto não serve de chave para o nosso pack.** A chave do `deko3d.pack` é FNV-1a 64 do texto
  GL 4.3 completo que o *nosso* backend gera (`TranslatePicaShaderToOpenGl43`). O upstream tem o
  corpo decompilado do programa num invólucro próprio para SPIR-V/NRI; o mesmo decompilador
  (`glsl_shader_decompiler.cpp`) está no nosso frontend, então o corpo serve de conferência, mas o
  DKSH tem de sair do nosso texto. Para o fragment não há nada a reaproveitar: eles não têm
  especializados.
- Para nós, o upstream confirma o formato (CMB v10, LzS, ZSI v9, GAR v2) e dá a lista exata de
  programas de vértice que o MM3D tem (5 SHBIN, 6 entradas de vértice, 1 de geometria).

## Decisão

- `tools/oot3d/grezzo_lzs.py` (novo) descomprime LzS; `tools/oot3d/cmb_layout.py` (novo) dá o leiaute
  por versão do CMB e lê o arquivo já descomprimido. `cmb_tev_census.py` e `cmb_tev_registers.py`
  usam os dois e aceitam `.lzs`. Versão desconhecida usa o leiaute do OoT3D, como antes. No OoT3D a
  saída é idêntica à de antes (censo igual e `regs.txt`/`.luz` iguais byte a byte, `cmp`).
- `deko3d_shader_predict.py`: `--conhecidos DIR` (repetível) dá a fonte das partes de cena sem varrer
  `~/oot3d-work`, toma o material só do `gen/` do título e não copia os conhecidos para a saída;
  `--saida NOME` troca `v8glsl`. Modo padrão: igual, mas ignora `/conv/` e `/jogos/` na varredura de
  `~/oot3d-work` (antes destes arquivos não havia `frag_*.glsl` lá, então o OoT3D não muda; sem isso,
  a próxima previsão do OoT3D leria os 5588 do MM3D como conhecidos).
- `CMake/oot3d/pica.cmake`: o alvo `oot3d_deko3d_register_glsl_dump` não ligava
  (`TranslatePicaShaderToOpenGl43` indefinido); entrou `pica_gl_shader_dialect.cpp`, como no
  `oot3d_deko3d_parametric_glsl_dump`.
- Testes de host com bytes sintéticos: `tools/oot3d/test_cmb_tev_registers.py` (LzS com referência
  sobreposta, janela zerada, tamanho errado; CMB v6, v10 e v10 dentro de LzS; censo com as duas
  versões) e `tools/oot3d/test_deko3d_shader_predict.py` (`--conhecidos/--saida`; `/conv/` fora do
  modo padrão). Contra o código de antes: 3 de 8 e 2 de 2 falham pelo motivo certo.

## Alternativas rejeitadas

- **Usar o pack do upstream:** é SPIR-V para o NRI/Vulkan e o de fragment está vazio; o deko3d precisa
  de DKSH com a chave do nosso texto GL 4.3.
- **Detectar a versão pelo filtro de extensão ou por nome de arquivo:** a versão está no próprio CMB
  (+0x08), e `.gar.lzs` pode ou não ser LzS (355 não são).
- **Copiar os 343 conhecidos do OoT3D para o pack do MM3D (como o v8 fazia):** são cena e material do
  OoT3D; os 337 que o MM3D pode usar já saem da previsão pelos materiais em comum.

## Em aberto

- **Partes de cena do MM3D.** As 8 cenas são do OoT3D (luz, fog, w-buffer, sombra configurados como o
  OoT3D configura). Com 88 % dos materiais do MM3D com luz de fragmento, o MM3D quase certamente tem
  configurações de luz (número de luzes, LUTs, bump) que nenhuma das 4 cenas com luz do OoT3D cobre:
  os 4088 previstos com luz são os de menor confiança. Precisa de captura real do MM3D (GL no PC:
  os `.src` do `shadercache` são o mesmo texto do deko3d; ou `logs/deko3d_shaders/` no console) e
  rodar de novo a previsão com essas cenas em `--conhecidos`.
- **Vertex.** O pack previsto não tem vertex do MM3D: o único vertex dos packs do OoT3D é do
  `CmbVShader.shbin`. O MM3D tem 5 SHBIN próprios; o texto GL 4.3 deles também vem da captura (ou de
  uma ferramenta de host que gere o vertex a partir do SHBIN, que não existe hoje). O DVLE de
  geometria do `LineShader` não tem caminho no deko3d (ver `2026-11-10-mk7-geometry-shader.md`).
- Dos 13 conhecidos do OoT3D que a previsão não reproduz (342 de 355), 12 são as bases paramétricas
  de `deko3d-pack-v10/bases` (nenhuma tem o bloco `color_output_0`); o 13º não foi olhado.
- Scratch por warp dos 5596 DKSH não foi conferido offline (o `deko3d_shader_pack.py` deste ramo não
  mede; o `ShaderStore` recusa no console o que passar de `kPerWarpScratchBytes`).
- Holdout (`deko3d_shader_predict_holdout.py`) continua com caminhos do OoT3D (v7); não foi usado.
