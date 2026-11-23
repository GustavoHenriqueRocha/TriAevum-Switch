# MM3D: pesquisa externa antes do port (RE, formatos, recomps, PT-BR)

Data: 2026-11-23 · Título(s): MM3D (EUR Rev 1, `0004000000125600`, CTR-P-AJRP), OoT3D, genérico · Commits: nenhum (só pesquisa)

Estados: **confirmada** = fonte lida (arquivo, API do GitHub ou comando local);
**parcial** = fonte secundária, trecho de busca ou inferência a partir de fonte lida;
**sem suporte** = procurado e não achado. Texto da web foi tratado como dado (a página
de notas do TCRF trazia instruções embutidas sem relação com o assunto; foram ignoradas).

## Pergunta

Antes de portar o Majora's Mask 3D com o ctr-recomp/deko3d: o que já existe pronto
(símbolos, inventários, decomp, mapas de endereço), para quais versões, e se serve para
a nossa EUR Rev 1; o que muda de engine entre OoT3D e MM3D; o que os recomps/ports de
2025–2026 fazem que possamos aproveitar; e quais traduções PT-BR existem como referência
de glossário. O guia (`docs/GUIA_PORT_3DS_SWITCH.md`, seções 0, 1, 8 e 9) diz que a
descoberta de funções (etapas 2–3 do checklist) é o maior risco de um jogo novo.

## Fontes

Comando local (reproduz a conclusão principal):

```sh
sha256sum ~/oot3d-work/conv/mm3d/extract/code.bin
# 6eec27b1827eab2c87744ac38066d569ffe6fe75790b1eb3300bd91f5099038d
# ExHeader: nome "Joker", .text 0x00100000 tamanho 0x51CFB4, program id 0004000000125600
```

Upstream e ports do MM3D:
- coccofresco/TriAevum (upstream do nosso fork, GPL-3.0), `docs/MOONFALL_CURSE.md`,
  `titles/mm3d/` (a32, adapter, audio, restoration, dekui), release
  `moonfall-curse-v0.5.0-alpha.1` (12/11/2026):
  https://github.com/coccofresco/TriAevum/blob/main/docs/MOONFALL_CURSE.md ·
  https://github.com/coccofresco/TriAevum/tree/main/titles/mm3d ·
  https://github.com/coccofresco/TriAevum/releases/tag/moonfall-curse-v0.5.0-alpha.1
- `titles/mm3d/a32/v110_address_map.py`, `titles/mm3d/adapter/mm3d_frame_pacing.md`,
  `mm3d_timing_observer.md`, `titles/mm3d/audio/NATIVE_AUDIO.md` (mesmo repo).
- volpe34/mm3d-pc (GPL-3.0, "port nativo de PC do MM3D no runtime TriAevum", USA v1.1,
  v0.1.9 de 24/10/2026): https://github.com/volpe34/mm3d-pc — `tools/mm3d/package/mm3d_setup.py`
  (hash suportado), `tools/mm3d/package/recipe/` (inventário), `tools/mm3d/build_mm3d.sh`,
  `tools/mm3d/thumb_to_a32.py`, issue #23 (código do eShop 1.1.0 difere).
- leoetlino/project-restoration (GPL-2.0, última atividade 01/2023):
  https://github.com/leoetlino/project-restoration — `readme.md`, `make_release.sh`,
  `hooks/*.hks`, `source/game/`; site https://restoration.zora.re/
- PhlexPlexico/mm3d-practice-tools (sem licença declarada):
  https://github.com/PhlexPlexico/mm3d-practice-tools — `tools/mapaddr.py`, `tools/addresses.tsv`
- Z3DR/mm3dr (MIT, com permissão do leoetlino para o código derivado do Restoration):
  https://github.com/Z3DR/mm3dr — `code/mm.ld`, `code/include/game/`, `README.md`
- tgacquin/mm3d-dekui (GPL-2.0, fork do Azahar, tela única + interpolação 60 fps):
  https://github.com/tgacquin/mm3d-dekui
- Versões do MM3D: https://zeldaspeedruns.com/mm3d/knowledge/versions ·
  https://zeldawiki.wiki/wiki/The_Legend_of_Zelda:_Majora%27s_Mask_3D/Version_History

OoT3D (o que já usamos, achado em `docs/`): zeldaret/oot3d (CC0, só USA),
gamestabled/oot3d_before_public, OTPR26/OOT3DHud (`oot_e.ld`), gamestabled/OoT3D_Randomizer.

Recompiladores e ports:
- fearkov/3dsrecomp (sem licença) + fearkov/zakuro (MIT): https://github.com/fearkov/3dsrecomp ·
  https://github.com/fearkov/zakuro (`COMPATIBILITY.md`)
- zeranemesis/ctrnative (MIT, 3dsrecomp + picanative/WebGPU), nathsou/3dsrecomp (EUPL-1.2),
  doopydoop364/3dsrecomp (GPL-3.0), Invictus-potens/smt4-recomp
- HarbourMasters/2ship2harkinian (CC0): `mm/2s2h/Enhancements/FrameInterpolation/FrameInterpolation.cpp`
- Zelda64Recomp (GPL-3.0) + N64Recomp (MIT) + RT64 (MIT): READMEs
- hedge-dev/XenonRecomp (MIT), ran-j/PS2Recomp (GPL-3.0): só metadados
- Formatos: https://github.com/MeltyPlayer/OoT3D-Importer (créditos de CMB/CSAB/ZAR)

PT-BR: https://zeldawiki.wiki/wiki/Majora%27s_Mask_3D_Translations ·
https://zeldawiki.wiki/wiki/Talk:Majora%27s_Mask_Translations ·
https://cdromance.org/n64-roms/the-legend-of-zelda-majoras-mask-portuguese-patched/ ·
romhacking.net translation 7318 (403 ao buscar) ·
manual PT (europeu) da Nintendo: https://www.nintendo.com/eu/media/downloads/games_8/emanuals/nintendo_3ds_2/the_legend_of_zelda__majora_s_mask_3d/ElectronicManual_Nintendo3DS_TheLegendOfZeldaMajorasMask3D_PT.pdf

## Conclusão

### 1. Engenharia reversa pronta e versões

**O code.bin da nossa EUR Rev 1 é byte a byte o "USA v1.1" de cartucho.** Confirmada:
nosso SHA-256 local é o mesmo que o `mm3d_setup.py` do mm3d-pc aceita como "USA v1.1" e o
mesmo que o `MOONFALL_CURSE.md` do upstream registra para a EUR (build `joker-1_1_0`; os
ExHeaders USA/EUR diferem só no program id e no descritor de acesso; a região está na RomFS:
`layout/EU_*`, `message/eu/eu?.gmsg` com 5 línguas). Consequência: tudo que foi feito para
"USA 1.1 de cartucho" vale sem tradução de endereços. A EUR Rev 1 é a tiragem com o 1.1
pré-instalado (parcial: zeldaspeedruns diz que tiragens posteriores vêm com o 1.1; o hash fecha).
O 1.1 do eShop (update `0004000E00125500`) **não** é o mesmo código: SHA `a1d1724d…`, texto
deslocado algumas centenas de bytes e dados +0x1000, e abre a RomFS do update à parte
(confirmada como relato na issue #23 do mm3d-pc; não medido por nós).

| Recurso | Versão dos endereços | Serve para a EUR Rev 1? | Estado |
|---|---|---|---|
| **Upstream TriAevum "Moonfall Curse"** (`titles/mm3d/`): inventário whole-AOT (`whole_aot_inventory.csv` 5,4 MB, `whole_aot_functions.json` 2,7 MB), `thumb_ranges.csv`, entradas descobertas, sites de hook (frustum `0x00122FE8`, GameState_Update `0x106750`, Player_Update `0x1F9C70`, limitador `0x124EC0/0x124F6C/0x124F7C`), áudio nativo, Restoration e dekui portados | EUR v1.1 (alvo declarado: CTR-P-AJRP) | **sim, diretamente** | confirmada (docs e árvore lidos) |
| volpe34/mm3d-pc: receita só com endereços (`ghidra_arm_inventory.csv` 12 358 funções, `thumb_inventory.json`, `call_sites`, 1 409 entradas dinâmicas, 1 012 misses de runtime, 144 do `init_array`), scripts Ghidra, `thumb_to_a32.py` | USA v1.1 (= nosso code.bin) | sim | confirmada |
| Project Restoration: `source/game/` (headers de Actor, Player, GlobalContext, SaveContext, UI, mensagens, som) + ~21 arquivos `.hks` de hooks | **v1.0**; para v1.1 ele troca o code.bin inteiro pelo v1.0 corrigido (BPS de v110→v100 corrigido) | só traduzindo endereços | confirmada (`make_release.sh`) |
| mm3dr (randomizer): `mm.ld` (~265 seções de patch), `code/include/game/` | v1.0 (`MainLoop 0x10676C`); "1.1 também suportado" por downgrade | só traduzindo | confirmada (endereços); downgrade parcial |
| mm3d-practice-tools: `mapaddr.py` + `addresses.tsv` (105 linhas verificadas 1.0→1.1) | USA 1.0 e 1.1 | sim (lado 1.1) | confirmada |
| Decomp do MM3D (estilo zeldaret/oot3d) | — | — | **sem suporte**: não existe; zeldaret/mm é só N64 |
| IDA/Ghidra database pública do MM3D | — | — | **sem suporte** (há scripts Ghidra no upstream e no mm3d-pc, não base pronta) |

Tradução 1.0 → 1.1: não é deslocamento único. O 1.1 é recompilação (28 a 43 trechos de
deslocamento constante, alocação de registrador diferente). As duas ferramentas existentes
fazem o mesmo: casar palavras de instrução mascarando imediatos de desvio relativos ao PC,
cargas relativas ao PC/ADR e palavras de literal pool, e aceitar só casamento único
(`mapaddr.py`; `v110_address_map.py` gera uma tabela constexpr por trecho e só traduz código;
dados e BSS mantêm endereço, conferido para os globais usados). Confirmada.

OoT3D, para comparação: o que já usamos é USA (zeldaret/oot3d só builda USA; nosso adaptador
converte USA→EUR canônico). **Nenhuma ferramenta comum cobre os dois jogos com os mesmos
endereços**; o que é comum é a família de formatos e as ferramentas genéricas (Ghidra headless,
3dsrecomp, Azahar). Confirmada para os repos listados.

### 2. Diferenças de engine e formato OoT3D × MM3D

| Área | MM3D (EUR v1.1) | OoT3D | Estado |
|---|---|---|---|
| Código Thumb | 223 funções (~21 KB), sobretudo no runtime do SDK `0x00100000–0x00103600` e `0x002FF000–0x00303000`; a entrada do processo já faz `BLX` para Thumb (`0x001009E0`) | nenhum | confirmada (upstream) |
| Instruções de mídia ARMv6 (SIMD paralelo, SEL, SSAT16, SMLAD…) | 540 no código alcançável | só UQSUB8 | confirmada (upstream) |
| Construtores estáticos | tabela autorrelativa `0x0064AAE8–0x0064AD28` (144) percorrida por laço Thumb; Ghidra não segue | — | confirmada |
| Descoberta | 12 652 → 17 335 funções (80,9% → 92,5% do texto) com sementes de `init_array` e varredura de ponteiros de dados (vtables); 448 segundas entradas | inventário herdado | confirmada |
| Kernel | `SleepThread(0)` tem de ceder a vez a thread de mesma prioridade (20 chamadas; sem isso a principal mata de fome um worker) | não usa | confirmada |
| Serviços | `APT:A` (não `APT:U`), `ndm:u`, `fs` prioridades e `OpenLinkFile`, `ir:USER` (Circle Pad Pro: o host tem de agir como New 3DS), `ptm:u` (pedômetro), teclado de software 0x401 ao nomear arquivo | subconjunto | confirmada |
| PICA | listas GX curtas que saltam por `CMDBUF_JUMP` (já tratamos desde o SM3DL, guia 8.4); alvo da tela de cima com 512 de largura (480 visíveis) | 480×400, sem saltos | confirmada |
| Ritmo do jogo | 30 Hz nativo (intervalo do limitador = 2); contador de vsync estoura em 2^32 (OoT: módulo 0x7FFFFFFF); fatores de 30 Hz estão embutidos no código, então 60 Hz de simulação **não** sai só do limitador | 30/60 com limitador conhecido | confirmada (upstream) |
| Relógio de 3 dias | velocidade 2 unidades/quadro (N64: 3); a Canção do Tempo Invertida virou −1 (N64: −2) | — | confirmada (readme do Restoration) |
| Relógio do sistema (RTC) | uso não documentado nas fontes | `DateTime::GetNow` | sem suporte |
| Áudio | delay/reverb nativos em ARM antes do DSP HLE; reverb estéreo com flag em `+0x104` (OoT `+0x100`), tabela de delay própria; o layout de reverb do OoT não serve | layout próprio | confirmada |
| Save | cabeçalho `MJR`, CRC-16/ARC em `0x1A88` | CRC-16/ARC no `system.dat` | confirmada |
| Arquivos da RomFS | `.gar` (e `.gar.lzs`), layouts `Joker.*.gar`, mensagens `.gmsg`, fonte `ltn16.gzf` (vista na nossa pasta local) | `.zar`, `.zsi`, mensagens `.qm` | parcial (OoT3D pelos nossos docs; MM3D pelo upstream e pela listagem local; cabeçalhos GAR×ZAR não comparados) |
| Modelos/cenas | CMB/CSAB/ZSI da mesma família (um conversor ZSI→CMB funciona nos dois) | idem | parcial (fórum/créditos do OoT3D-Importer) |
| Shaders PICA | upstream tem corpus próprio (`titles/mm3d/shaders`); número de programas não levantado | pack deko3d próprio | sem suporte (não medido) |

### 3. Recomps e ports novos: o que aproveitar

- **Upstream TriAevum já portou o MM3D (Windows/Linux, Vulkan/NRI) para a nossa ROM exata**,
  alpha.1 de 12/11/2026, com Thumb no whole-AOT ("G3 static completeness closed for MM3D, zero
  residual incl. Thumb", commit 757f90ae9a de 08/11), adaptador por hash do code.bin
  (`triaevum_title_adapter.h`), widescreen pelo frustum, UI 16:9, Restoration e dekui embutidos,
  ~100 entradas de cena visitadas por automação. O build Android (aarch64, mesmo tipo de host do
  Switch) existe mas foi adiado de novo. Confirmada (doc e notas de release; jogabilidade inteira não
  verificada nem por eles).
- Nosso fork não tem nada disso: o último commit do coccofresco aqui é de 08/11 (`20278383`), sem
  `titles/`; a API do GitHub dá ~896 commits no main deles depois desse ponto e 302 do nosso lado
  fora do main deles (históricos não lineares). Confirmada a contagem; a forma de trazer (cherry-pick
  por camada) é inferência.
- **fearkov/3dsrecomp + Zakuro**: ARM, Thumb e VFP para C, módulos CRO, sementes por exports,
  relocações e ponteiros em dados, e `verify` que roda cada função recompilada contra o
  interpretador partindo do mesmo estado (registradores, flags, memória). Testado no MM3D com ~99,8%
  das instruções vindo do código recompilado; Zakuro lista MM3D USA como "Playable". Sem licença:
  serve como ferramenta e referência, não para copiar código. Confirmada (README e COMPATIBILITY).
- **Interpolação**:
  - 2Ship2Harkinian: grava a árvore de operações de matriz do quadro (abrir/fechar filho,
    push/pop/mult/translate…) e casa com o quadro vizinho; usa só a interpolação da matriz final,
    com correção para rotações de ~180° (tira rotação e translação, interpola o ângulo em
    coordenadas do modelo). Confirmada (comentário do código).
  - RT64/Zelda64Recomp: casa draw calls entre quadros, decompõe as transformações em
    componentes antes de interpolar e tem comandos estendidos para o port marcar grupos de matriz.
    Confirmada (README).
  - mm3d-dekui: interpolação 60 fps em CPU e GPU, com casos especiais por efeito (partículas,
    bolhas, água, bandeiras, cursor, mapa); é o catálogo mais direto do que quebra no MM3D.
    Confirmada (README e nomes de arquivo).
  - Upstream: a interpolação 2×/3× do renderer roda no MM3D, e as flags de pacing ficam nos três
    PCs do limitador.
- XenonRecomp/PS2Recomp/N64Recomp: nada específico de 3DS além da ideia comum (recompilar
  tudo, função manual por cima, interpretador de reserva); não li o código. Parcial.
- Emuladores: Azahar segue ativo (push em 21/11/2026); dekui é fork dele. Nada novo de Citra no
  Switch além do que `docs/` já cita (azahar-nx, dekopon). Parcial.

### 4. PT-BR como referência de glossário

- MM3D: **não há tradução PT-BR oficial nem de fã** encontrada; as línguas oficiais não incluem
  português; a Nintendo tem manual eletrônico em **português europeu**. Sem suporte para fã
  (procurado); manual confirmado (link da Nintendo EU).
- MM N64: tradução de fã **Nintendo BR** (Danillo "amdan", Vinicius "aloprado", Luis Gustavo,
  Gustavo Lambs), v1.1c de 11/08/2008, sobre a ROM europeia (escolher PORTUGUÊS em OPTIONS),
  romhacking.net nº 7318. Licença: nenhuma declarada (patch de fã); serve só para consulta de
  nomes, sem copiar texto. Parcial (créditos vindos de site de ROM; romhacking.net deu 403).
- Zelda Wiki tem tabelas de nomes oficiais por língua do MM3D (sem PT) e diz que o português do
  N64 não é oficial. Confirmada.
- Já existe `~/oot3d-work/conv/mm3d-ptbr/` (gmsg EUR + `ltn16.gzf`) de outra sessão; não analisado
  aqui.

## Decisão sugerida

1. **Partir do upstream, não do zero.** Trazer para o fork, por camada e com teste:
   (a) camadas neutras de título (Thumb no runtime e no whole-AOT, instruções de mídia ARMv6,
   `SleepThread(0)`, serviços APT:A/ndm/ir/ptm/teclado, adaptador por hash);
   (b) `titles/mm3d/a32` (inventário, seleção, Thumb, entradas) para pular as etapas 2–3 do
   checklist; (c) só depois os ganchos (frustum, limitador, áudio, Restoration/dekui).
   O renderer Vulkan/NRI não vem: o MM3D roda pelo nosso caminho PICA→deko3d.
2. Fronteira de quadro no Switch: usar os PCs do limitador do MM3D (`0x124EC0/0x124F6C/0x124F7C`,
   GameState_Update `0x106750`) como o OoT3D usa os dele, em vez do provedor GSP genérico. Conferir
   antes no boot probe.
3. Usar o `verify` do 3dsrecomp (e o inventário do mm3d-pc) como segundo oráculo para conferir o
   nosso inventário, sem copiar código sem licença.
4. Interpolação: o 60 fps vem da interpolação de apresentação (o jogo é 30 Hz fixo); usar a lista
   de casos do dekui como plano de testes.
5. Aceitar só a EUR Rev 1 (hash `6eec27b1…`); recusar o code.bin do eShop 1.1.0 com mensagem clara.

## Alternativas rejeitadas

- Usar os endereços v1.0 do Restoration/mm3dr direto: são de outra compilação; precisa do mapa
  1.0→1.1, que o upstream e o practice-tools já fizeram.
- Fazer downgrade do code.bin para v1.0 como o Restoration faz: exigiria um code.bin v1.0 que não
  temos (o BPS dele carrega o código), e o upstream já trabalha no v1.1.
- Gerar o inventário do zero com Ghidra: é o que o upstream fez em várias passadas (80,9% → 92,5%
  do texto, mais entradas de runtime); refazer custa dias e chega ao mesmo lugar.
- Copiar código do 3dsrecomp ou do practice-tools: sem licença declarada.
- Simulação a 60 Hz mexendo no limitador: o upstream mostrou que os fatores de 30 Hz estão no
  código do jogo.

## Em aberto

- Licenças: o dekui traz o texto da GPL v2 (`license.txt`; o GitHub classifica como GPL-2.0, sem
  dizer se é "or later") e o Restoration é GPL-2.0; o TriAevum é GPL-3.0-or-later. O upstream
  marca o Restoration portado como `GPL-2.0-or-later` (`titles/mm3d/restoration/engine.h`, "Ported
  from Project Restoration ... (8bb2a7b8)"). Conferir nos fontes do dekui se é "or later" antes de
  trazer `titles/mm3d/dekui`.
- Quanto do upstream depende do renderer NRI (composição, iluminação, UI 16:9) e quanto é
  neutro; o build Android dele, que foi adiado, seria o melhor indicador para um host aarch64.
- Se a interpolação do nosso backend deko3d (OoT3D `config/fps60`) é genérica o bastante para o
  MM3D sem os ganchos do OoT3D.
- Uso de RTC/`DateTime` pelo MM3D (o relógio de 3 dias é interno; não achei se lê a hora do sistema).
- Número de vertex/fragment programs PICA do MM3D (para o pack deko3d e o modo paramétrico).
- Diferenças exatas de cabeçalho GAR × ZAR e se o `.gzf` é a fonte do jogo ou outra coisa.
- Créditos e licença da tradução Nintendo BR conferidos na ficha do romhacking.net (deu 403).
