# O que do MM3D entrou no lado genérico (branch generico-mm3d sobre deko3d) (27/11)

## Pergunta
Levar para o `deko3d` (produto OoT3D + `tools/ctr`) os commits do port do MM3D (`mm3d-tour`) que servem a
qualquer título de 3DS, na lista de `docs/pesquisas/2026-11-27-mm3d-o-que-vai-para-o-ctr.md` (está na
`mm3d-tour`), em cinco levas: correções, desempenho, medição, conversor e serviços. Pedido do Gustavo, 27/11.
Regra: o comportamento do OoT3D não muda por acidente.

## Fontes
- `git log --reverse deko3d..generico-mm3d`: 28 commits. Cada um diz de onde veio na linha
  `(cherry picked from commit ...)` e o que mudou na linha `Linha genérica (deko3d): ...`.
- Base: `deko3d` 2edc4290. A base comum com a `mm3d-tour` é 4a7e4a24 (23/11), e o `deko3d` só tem 12
  commits depois dela; quase todos os conflitos vieram de código que o MM3D trouxe antes, não do `deko3d`.
- Testes: build de host Debug no notebook (`~/oot3d-work/build-host-generico`, `-j 2`, ccache) e a suíte de
  host do `run.sh` no WSL do Ryzen (`/root/agentes/conv/generico/build`, pela `trava.sh`, `-j 4`).
  Sintaxe do Switch: `aarch64-none-elf-g++ -fsyntax-only` com a linha de compilação de
  `~/oot3d-work/build-switch` (configurado em 05/11), um arquivo por vez.

## O que entrou, por leva

**Leva 1, correções.** 37559d9c (a saída de uma thread não encerra o jogo), 93627fa4 (textura num endereço
escrito pela GPU lê preto transparente), 174c9a76 (alvos na ordem da chave do GL), 43ebee55 + b2aa70c6 (textura
escrita pela GPU lê a imagem da GPU, atrás do perfil `GpuFeedbackTextures`, desligado no OoT3D).
- O 6b5ef9c9 foi para a leva 2: o teste dele (`RunUniformModeSwitch`) mora no
  `oot3d_native_pica_list_walk_tests.cpp`, que nasce no e630327b. A dependência é a contrária da que a lista
  supunha: o 6b5ef9c9 depende do e630327b, não o inverso.
- Conflitos só em listas: o `main` dos testes de processo (o `TestYieldRotatesEqualPriority` do MM3D não existe
  aqui), as guardas (o contexto trazia guardas da tela única e do warp do MM3D) e os includes do
  `gfx_deko3d_pica_draw.cpp`. O `Mm3dTitleProfile` não existe aqui: o teste e a guarda do perfil perderam só a
  linha do MM3D (OoT3D desligado e genérico ligado continuam conferidos).

**Leva 2, desempenho.** b91bd21d (pack de shaders lido do SD sob demanda), e630327b (percurso rápido da lista
PICA) e o 6b5ef9c9 (fila de uniform com `>=`).
- O `TRIAEVUM_SWITCH_SD_ROOT` é do produto MM3D (`fast/switch_sd_root.h`, 63f7cd22) e não existe aqui. Nos
  caminhos que vieram (list_walk_fast, deko3d_census, deko3d_nofog, svc_profile) ficou o literal
  `sdmc:/switch/oot3dre`, como o resto de cada arquivo. O erro só aparece compilando para o Switch: o
  `-fsyntax-only` do devkitA64 pegou no `oot3d_native_offload.h`.

**Leva 3, medição.** e8669e8a (linha fatal com registradores e o rastro do despacho), deaa047c e 0d31b0b6
(censo do deko3d e diagnóstico da névoa), 675b5969 (faixa de texto do perfilador pelo manifesto), b1fdaefd,
a672ec36 e 63098937 (`config/svc_profile`). Tudo fica desligado sem o arquivo de config. Conflitos: includes e
o `main` dos testes do host CTR, onde só entrou o `TestSvcTimeProfile`.

**Leva 4, conversor (parcial).** 81954ead e 2ce3ebdc (boot probe), 28340382 (shards address-append no
ctr_convert), 8694ef81 e 7933ad71 (opções do gerador no ctr_convert, desligadas por padrão) e a parte genérica
do 423531b0 (`apply_title_sites.py` + teste e o `Oot3dAotTitleSite` no `oot3d_native_whole_aot_runtime.h`).
- **O discover ficou de fora** (95a8d737, fbeb35bb, 6dd980bf, 9d5bd4da/16c9df4c (diffs idênticos), 25cea7c9,
  bbed0722, dc50b5ac). Eles aplicam sem conflito, mas 13 dos 15 testes de `test_discover_functions.py` falham:
  a descoberta chama `whole_aot_program.load_inline_argument_calls` (G2 do upstream, 98255aa4) e
  `whole_aot_thumb.walk` (G3, 6ddacf83). Os dois mudam o gerador compartilhado (`whole_aot_program.py`,
  `whole_aot_cpp.py`), o mesmo que gera o C++ do OoT3D. Fora da lista e arriscado: parei aqui. Trazer o
  `whole_aot_thumb.py` sozinho (autocontido) não bastou, por causa do G2.
- 8694ef81: o argparse e o `test_ctr_convert.py` nasceram com os commits do discover. Entraram só as três opções
  do gerador e os dois testes delas; o teste do `--supplemental-entries` fica com o discover.
- Ficaram de fora f1b06387, 82196ac5, 69decb3e e a cola do 423531b0 no `oot3d_native_a32_window_setup.cpp`:
  são a tela única do MM3D (`TRIAEVUM_MM3D_SINGLE_SCREEN`, `mm3dSingleScreen`, `titles/mm3d/`). Aqui nenhum
  título registra o callback dos sítios: o `apply_title_sites.py` e o prólogo são infraestrutura sem
  consumidor na linha genérica até um título usar.

**Leva 5, serviços do upstream.** 59643b3c (kernel/APT), 9d0c19d1 (OpenLinkFile, prioridade), cb944faf
(ir:USER, liberação de evento), d1e0470a (Circle Pad Pro, ptm:u), 179f8239 (teclado de software), 06b5acd7
(idioma pelo CFG fora do OoT3D) e, fora da lista, as guardas do ed8613dc, que protegem dois desses commits
(37559d9c e 179f8239).
- Conflitos: o `docs/MM3D_PORT.md` não existe aqui (ficou de fora em três commits), o `main` dos testes do host
  CTR (sai a chamada ao `TestSystemTimeMicroseconds`, que é de outro commit do upstream) e o
  `mAptScreenCapPostPermission`, que veio no contexto do 179f8239 sem uso aqui e saiu.

## O que muda no OoT3D (tour no console)
Mudam o que o OoT3D executa: 37559d9c, 93627fa4, 174c9a76, b91bd21d, e630327b (ligado por padrão no Switch),
e8669e8a (uma escrita num anel por despacho), 59643b3c, cb944faf (só o contador de eventos), d1e0470a (a
projeção ExtraHid e o `AdvanceIrToCurrentTick` a cada quadro) e 179f8239 (0x401 passa de seletor de Mii a
teclado). Ficam iguais no OoT3D (perfil desligado ou ferramenta): 43ebee55, b2aa70c6, 6b5ef9c9, a leva 3 sem
os arquivos de config, a leva 4 e o 06b5acd7.

## Testes
- `tests/switch/test_bug_guards.py`: 242 → 255 guardas, todas verdes no fim. A da saída de thread falha com o
  `process.cpp` de antes do 37559d9c (254/255).
- Vistos falhar sem a correção: `oot3d_native_a32_process_tests` ("one thread's exit must not end a process
  whose threads wait") e `oot3d_native_pica_list_walk_tests` ("a fila não zerou ..., count 4").
- Host no notebook, verdes no 200ea0bc: process, ctr_host, ctr_fs, process_image, deko3d_backend,
  pica_list_walk (2 446 320 palavras, 16 690 draws), pica_frontend, input.
- Python (venv do projeto): test_whole_aot_cpp 56, test_apply_title_sites 7, test_ctr_convert 2,
  test_build_title_boot_probe 2, test_discover_functions 3 (o arquivo de antes, sem os testes do discover).
- Os 41 testes de unidade do `run.sh` (lista `UNIT_TESTS`), compilados no Ryzen (331 etapas, trava rc=0)
  sobre a árvore do 200ea0bc: 40 ok e o `oot3d_native_audio_tests` pulado (77, pede o romfs extraído).
  `tests/switch/run.sh --quick`: 6/6. A mixagem DSP em AArch64 (qemu) não rodou.

## Em aberto
- Nada foi compilado para o Switch inteiro nem rodado no console; a sintaxe foi conferida com flags de 05/11.
- O comentário da guarda dos ids de applet (ed8613dc) diz que o OoT3D usa o 0x401; não conferido. Se usar, o
  179f8239 muda o que ele recebe ao pedir esse applet.
- O discover (sete commits) depende do G2/G3 do gerador do upstream: trazer exige regerar o C++ do OoT3D e
  comparar os shards antes.
- O `boot_probe.cpp` (2ce3ebdc) só compila com o AOT gerado; a API que ele chama é a mesma da janela
  (`AdvanceHidToCurrentTick`), mas o arquivo não foi compilado.
