# Ocarina of Time 3D no Nintendo Switch — notas do port

Este repositório é um **fork** do [TriAevum / Echoes of Time](https://github.com/coccofresco/TriAevum),
de **coccofresco**: a recompilação AOT nativa de *The Legend of Zelda: Ocarina of Time 3D*. Todo o
trabalho de recompilação, runtime, renderizador e ferramentas é do projeto original e dos projetos
que ele credita (veja [README.md](README.md) e [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).
Aqui estão as mudanças feitas para o **homebrew de Switch** (`.nro`) rodar e ficar jogável num
console real, no branch `switch-port`.

> **Guia reutilizável:** a receita para portar outros jogos de 3DS com as mesmas técnicas (pipeline,
> substituições nativas, otimizações do gerador, runtime, medição e checklist) está em
> [docs/GUIA_PORT_3DS_SWITCH.md](docs/GUIA_PORT_3DS_SWITCH.md).

> **Nenhuma ROM, código do jogo, C++ gerado a partir do jogo ou asset é distribuído aqui.**
> Você precisa do seu próprio dump decriptado do cartucho. Licença: GPL-3.0, como o original.

## Estado (2026-10-16)

- Roda no console (Erista, Atmosphère, aberto pelo Sphaira em modo título), testado por horas.
- Velocidade de jogo correta; 30 FPS em cenas leves; cenas pesadas ainda limitadas pela CPU
  (a lógica traduzida custa ~25–30 ms por passo a 1785 MHz). Trabalho de CPU em andamento.
- Recomendado no sys-clk: CPU 1785 MHz e memória 1600 MHz.

## O que foi corrigido / adicionado

Bugs que impediam o Switch de rodar (só apareciam no console; o Eden escondia vários):

| Problema | Causa | Correção |
|---|---|---|
| Build sem Vulkan quebrado | módulos DLSS/NGX e iMMERSE/MXAO incluíam `vulkan.h` | excluídos sem `THREE_DS_RECOMP_ENABLE_VULKAN` |
| Crash ao iniciar | `-ffast-math` na ligação põe `crtfastmath.o` depois de `crtend.o` e o ld descarta o terminador do `.eh_frame`; toda exceção C++ abortava | ligação com `-fno-fast-math` |
| Crash ao iniciar | `pthread_detach` do newlib do devkitA64 retorna `ENOSYS` | sem `detach()` no Switch |
| Tela preta / shader falhando | apresentação paralela rodava GL numa thread sem contexto | contexto GL movido para a thread de render (`Submit`+`Drain`) |
| Jogo em câmera lenta (½ velocidade) | 60 refreshes/s esperados, mas 1 por apresentação a 30 Hz | até 4 refreshes por apresentação |
| Configurações não salvavam | FS do Horizon não renomeia por cima de arquivo | remove e renomeia |
| Crash com warm-up em thread | contextos nvc0 compartilham push buffer sem lock | warm-up na thread de render |

Desempenho e usabilidade:

- **Cache de shaders em disco**: Mesa do devkitPro reconstruído com os patches de cache dos ports
  de Wind Waker do centollOS (`MESA_SHADER_CACHE_DIR=sdmc:/switch/oot3dre/mesacache`); shaders já
  vistos carregam em ~12 ms em vez de 1–3 s. Veja `docs/OOT3D_SWITCH_WHOLE_AOT_PORT.md`.
- **Threads**: jogo sozinho no core 0; render thread, worker de planos e áudio com papel próprio;
  demais threads como serviço (0x3B, núcleos 1|2), pilha de 4 MiB (`oot3d_switch_threads.cpp`,
  `--wrap=pthread_create`; política em `oot3d_switch_thread_layout.h`, A/B em `config/thread_layout`).
- **Render thread** (opcional): arquivo `config/parallel_presentation` no SD.
- **Resolução interna** (opcional): arquivo `config/resolution_scale` no SD com `1`, `2` ou `3`
  (ausente = 2 no Switch, 1 no PC). Alvos PICA, depth/stencil, sombras e imagens de display
  transfer ficam N× maiores; viewport/scissor/blits escalados; custo só de GPU. 3 = 1200×720
  (encaixe 1:1 no portátil). No desktop: `TRIAEVUM_GL_RESOLUTION_SCALE=2`.
- **Pós-processamento GL** (opcional, tudo desligado sem os arquivos; lidos uma vez, só GPU), em
  `config/` no SD (desktop: variável `TRIAEVUM_GL_<NOME>`):
  `antialiasing` = `fxaa` ou `smaa` (mesmos shaders/preset do caminho NRI do PC);
  `upscaler` = `fsr` (AMD FSR 1: EASU amplia a imagem N× até a janela, RCAS dá nitidez);
  `sharpen` = `0.0`–`1.0` (força linear do RCAS; com `fsr` o padrão é `0.2`, `0` desliga o RCAS);
  `anisotropy` = `2`/`4`/`8`/`16` (só texturas PICA com filtro linear e mipmaps).
  Ordem: render N× → AA → EASU → RCAS → janela. Só na tela de cima (apresentação opaca).
- **Widescreen 16:9** (padrão ligado no console): `config/widescreen` = `0` desliga (volta ao 5:3
  com faixas), `1` liga. Só o alvo da tela de cima é alargado (400→427 lógico; 853 px na escala 2,
  1280 na escala 3) e o FOV horizontal cresce na mesma razão pelo hook de projeção que o PC já usa;
  HUD, menus e textos continuam num quadro 5:3 centralizado, como no Vulkan do PC. Desktop GL:
  `TRIAEVUM_GL_WIDESCREEN=1`.
  - **Menus no widescreen** (`config/widescreen_menus`, padrão ligado; `0` volta ao comportamento
    anterior; desktop `TRIAEVUM_GL_WIDESCREEN_MENUS`): antes só o domínio `Ui` (HUD, texto, pausa)
    ia para o quadro 5:3; tela de título, seleção de arquivo, criação de nome e escolha normal/Master
    Quest (domínio `Unknown`/`Scene`) esticavam ~6,7%. Agora também vão para o quadro, pelo estado
    PICA: viewport que não cobre a largura apresentada (o menu de baixo que o TopScreen realoca, Y
    40..360) e projeção ortográfica de c0-c3 com pixels quadrados cuja geometria não cobre o viewport
    (painéis, textos, logotipo). Fades/lavagens de tela inteira, redesenhos de cópia do quadro e a
    camada de atmosfera continuam esticados com a cena. Modelos 3D em perspectiva postos no quadro
    cujo frustum o hook alargou (anisotropia medida = 1/fator) têm o viewport alargado pelo mesmo
    fator em torno do centro, recortado ao quadro (não ficam espremidos). Política e testes:
    `gl_widescreen_menu_policy.h`, `tests/gl_widescreen_menu_policy_tests.cpp`.
- **Tela de toque**: tocar funciona onde o mouse funciona no PC (tela de título/seleção de arquivo e
  as páginas de pausa Itens/Mapa/Equipamento do TopScreen); no jogo em movimento o toque é ignorado,
  como o mouse no PC. Toques curtos ficam retidos até o jogo ler.
- **Gerador AOT**: helpers quentes com caminho rápido inline e lento frio; LDM/STM por bloco;
  literais PC-relativos dobrados; shards sem `-ffast-math` (exatidão do VFP).
- **Orçamento de blocos** 2048 (era 128) no Switch.
- **Perfil no console**: `logs/perf.txt` a cada 150 quadros (ms por fase + contadores do Mesa).
- **TopScreen 3.0**: a 2.1.1 saiu do GameBanana; o importador agora aceita o pacote 3.0.
- **ROM USA** aceita no build (adaptador USA→EUR canônico do próprio projeto).
- **Caminhos de entrada** configuráveis por `OOT3D_OPERATIONAL_<variável>` (antes fixos no Windows do autor).
- Ícone original para o `.nro` (`res/switch/icon.jpg`).

## Como compilar (Linux)

1. devkitPro (`switch-dev switch-sdl2 switch-mesa switch-glad switch-zlib`), `cmake`, Python 3 com `capstone`.
2. Extraia a ROM com `tools/triaevum_release/ctr_rom.py` e, se for USA, aplique o adaptador
   (`input_adapters.adapt_extracted_inputs` com a receita `oot3d-usa-input-canonical-eur-ef210566`).
3. Gere o programa e o C++:
   ```sh
   export OOT3D_OPERATIONAL_oot3dExtractRoot=/caminho/extract   # exefs/code.bin, ncch0/exheader.bin
   cd tools/oot3d/native_a32_runtime
   python generate_whole_aot.py --output $W/gen/aot_program.json --inventory-output $W/gen/inventory.csv
   python generate_whole_aot_cpp.py --program $W/gen/aot_program.json --selection whole_aot_functions.json \
     --code $OOT3D_OPERATIONAL_oot3dExtractRoot/exefs/code.bin --output $W/gen/cpp --shards 256 --shard-strategy affinity
   ```
4. Configure e compile (~1h30 em 4 núcleos):
   ```sh
   cmake -S . -B build-switch -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake \
     -DCMAKE_BUILD_TYPE=Release -DOOT3D_ENABLE_VULKAN_RENDERER=OFF \
     -DOOT3D_WHOLE_AOT_PRODUCT_MODE=ON -DOOT3D_REQUIRE_WHOLE_AOT=ON -DOOT3D_REBUILD_WHOLE_AOT=ON \
     -DOOT3D_REGENERATE_WHOLE_AOT=OFF -DOOT3D_WHOLE_AOT_GENERATED_DIR=$W/gen/cpp \
     -DOOT3D_WHOLE_AOT_PROGRAM=$W/gen/aot_program.json \
     -DOOT3D_WHOLE_AOT_CODE_BIN=... -DOOT3D_WHOLE_AOT_EXHEADER=...
   cmake --build build-switch --target oot3d_native_game_nro
   ```
   (Para o Mesa com cache, veja a seção correspondente em `docs/OOT3D_SWITCH_WHOLE_AOT_PORT.md`.)
5. Monte o SD com `scripts/switch/stage-product.sh` e copie `switch/oot3dre/` para o cartão.
   Abra pelo Homebrew Menu **em modo título** (segurando R num jogo), não pelo álbum.

## Diagnóstico no console

- `sdmc:/switch/oot3dre/boot-status.txt` — etapa da inicialização e erro legível.
- `logs/perf.txt` — FPS e ms por fase; `logs/oot3d_native_game.log`; `logs/glthread.txt`.
- Crashes: `atmosphere/crash_reports/` (o `.nro` é carregado pelo hbloader; a base pode ser
  achada alinhando os endereços de retorno com instruções `bl/blr` do ELF).
- Travamentos (imagem parada, sem crash report): `logs/hang.txt`, do watchdog de travamento
  (abaixo). Desligar com `config/no_hang_watchdog`.

### Congelamento na escolha Normal/Master Quest e watchdog de travamento (2026-10-17)
Sintoma (integ5, `fps60` legacy, `parallel_presentation`, `config/heap_check_windows=1`): ~5 s
depois de abrir, a imagem parou nos cartões em animação; sem crash report nem erro no log.
`switch-logs/crash1/perf.txt`, sessão 68: janelas 150 e 300 e nada mais.

**Onde a thread do jogo parou (provável, por evidência do arquivo).** A última linha (janela
300) termina no meio, em `... present_4p=0 hold_`, sem `\n`. A linha é montada por um
`std::ofstream` aberto e fechado a cada janela; o buffer do libstdc++ no devkitA64 é `BUFSIZ` =
1024 bytes e, quando uma string não cabe, o `xsputn` escreve buffer + string de uma vez: o arquivo
só recebe a linha em pedaços de ~1 KiB e, se a janela termina, o destrutor fecha e grava o resto.
Os 2060 bytes gravados são dois desses pedaços; o terceiro sairia por volta do byte ~3083, que numa
linha completa fica dentro de `mesa:`. Logo a thread do jogo entrou no bloco da janela 300 e nunca
chegou ao `close`: parou entre `hold_` e o fim de `mesa:`. Nesse trecho não há espera da render
thread, do pacer nem do stream (o `GetWaitStats` e o mutex de `interpolationPerf` vêm antes de
`hold_`); o sys-clk já tinha respondido (`clocks: ... clk_ipc_us=16` está na linha); as estatísticas
do backend e do Mesa (`mesa_switch_get_stats`, `cache_status`) só leem atômicos/um vetor estático.
Sobram: o **`mallinfo()`** do `config/heap_check_windows=1` (todas as janelas) e a escrita do
pedaço no SD. O `mallinfo()` do newlib (`__malloc_update_mallinfo`, conferido no desassembly do
`libc.a`) segura o lock global do malloc e percorre cada bin por `bk` até voltar ao cabeçalho, sem
limite nem checagem: com uma lista livre corrompida (um double free deixa `p->bk == p`; um
use-after-free escreve no `fd/bk`) o laço não termina, o lock fica preso e qualquer thread que
aloque para — o processo inteiro congela sem crash. Já houve corrupção de heap achada pelo
`mallinfo` (crash na 21ª janela, `interp_scale=1.5`, seção de interpolação), e a config estava
ligada justamente para caçar a próxima. Causa provável: **corrupção de heap (origem ainda
desconhecida) + `mallinfo` em laço infinito segurando o lock do malloc**; não comprovada (falta o
PC da thread), e a escrita no SD não está descartada. O watchdog abaixo responde isso na próxima vez.
Outras três sessões do mesmo arquivo também acabam no meio de uma linha (3150, 13650, 34950;
a 34950 também em `hold_`): podem ser saídas pelo HOME durante o bloco, nada se conclui delas.

**Hipóteses revisadas e descartadas para este caso:**
- *Lost wakeup no `NativeRenderCommandStream`* (sinal só com alguém esperando, 9b45d8e6): todo o
  estado dos predicados e os contadores de esperas (`mWorkerWaiting`, `mProgressWaiters`) mudam
  sob `mMutex`; quem espera incrementa o contador sob o lock antes do `wait`, que solta o lock de
  forma atômica; quem sinaliza testa o contador sob o mesmo lock depois de mudar o estado. Ou o
  sinalizador vê o contador > 0 e sinaliza, ou mudou o estado antes e o predicado (testado sob o
  lock antes de dormir) já o vê. O `BeginShutdown` muda `mShutdown` sob o lock e sinaliza as duas
  condvars sempre. Não há sinal perdido. Prova de estresse:
  `oot3d_native_render_command_stream_stress_tests` (frames em voo 1 e 2, Run/Drain misturados,
  comandos vazios/yield/sleep, leitor concorrente com `try_lock` como o watchdog, parada pedida de
  dentro de um comando; um vigia aborta se nada andar por 20 s): limpo com TSan (100 mil e 300
  mil comandos por configuração) e, sem TSan, 4 milhões de comandos (2 M com 1 e 2 M com 2 frames
  em voo: 952 mil esperas do submissor, 436 mil do worker, 2,19 M sinais pulados) mais 1050
  paradas de dentro de um comando, sem nenhum travamento. O TSan achou só uma corrida nova do próprio leitor (o `StartWorker`
  escrevia `mStop` fora do lock): corrigida.
- *Resolves do plano PICA com o mutex de build a cada até 32 draws*: um único mutex de build,
  nunca segurado junto com o da fila; o pool de lotes (`BatchPool::Mutex`) só é tomado sozinho
  ou dentro do de build (nunca o contrário): grafo de locks sem ciclo. A parada atômica
  (`mStopRequested`) é só um atalho; a espera do worker usa `mStop` sob o mutex da fila.
- *sys-clk*: a chamada da janela 300 respondeu (16 us). A sessão é aberta uma vez, na janela 1.
- *Pacer/cadência e menus em pedaços/fade 16:9*: rodam na render thread ou fora do bloco do
  `perf.txt`; não explicam a thread do jogo parada dentro dele.

**Correção: varredura limitada no lugar do `mallinfo()`** (`oot3d_heap_bin_check.h`). Mesma soma
(livres = bins + top; `heap_used_mb` = arena − livres, o `uordblks`), com o lock do malloc, mas
cada elo é conferido (ponteiro dentro de `[fake_heap_start, fake_heap_end)` e alinhado, tamanho
plausível, `p->bk->fd == p` e `p->fd->bk == p`), ciclo detectado (Brent) e teto de passos. Uma
lista corrompida vira `heap_corrupt=<motivo> bin=… chunk=… size=… fd=… bk=…` no `perf.txt` e um
relatório "HEAP CORROMPIDO" no `hang.txt` com o estado de todas as threads; a varredura então para
nesta sessão. Campo novo `heap_free_chunks`. Testes: `oot3d_hang_watchdog_tests` (heap sintético
no layout do newlib: soma igual à do `mallinfo`, double free, auto-laço, ciclo, ponteiros fora do
heap/desalinhados, tamanhos absurdos e 3000 heaps com elos aleatórios — sempre termina e nunca
lê fora do heap, ASan/UBSan). A corrupção em si continua: o relatório traz o chunk e as threads.

**Watchdog de travamento** (`oot3d_hang_watchdog.h/.cpp`, ligado por padrão no Switch,
`config/no_hang_watchdog` desliga):
- Marcadores baratos: cada thread com papel (jogo, render, planos, áudio do host, áudio do SDL)
  ganha um slot no `PlaceSwitchThread`. Etapa = um store relaxed (`frame_begin`, `guest_refresh`,
  `present_visual`, `ui_submit`, `present_submit`, `frame_end`, `perf_window`, `perf_clocks`,
  `perf_heap_walk`, `perf_file_close`, `wait_frames_in_flight`, `wait_drain`, `sync_command`,
  `render_idle`, `render_command`, `render_pacer`, `render_swap`, `render_resolve_plans`);
  progresso = load + store da dona (um por quadro de host no jogo, um por comando na render thread).
  Nada por draw; as esperas do stream só marcam quando de fato bloqueiam.
- Uma thread de serviço (0x3B, núcleos 1|2) acorda a cada ~1 s. Se o jogo ou a render thread
  (não ociosa) ficar > 3 s sem progresso com o app em foco, grava **uma vez**
  `logs/hang.txt` (append, `FsWriteOption_Flush`), pela API de FS da libnx direto — sem `fopen`,
  sem `printf` de float, sem nenhuma alocação: o travamento pode ser o próprio lock do malloc.
  Quando tudo volta a andar, escreve `--- recuperou após N ms` e se rearma (no máximo 8
  relatórios por execução). Fora de foco (HOME) e saltos de > 2,5 s do próprio watchdog (sleep do
  console) zeram a contagem.
- Conteúdo: etapa, progresso (= `host_frames` do jogo / comandos da render thread), tempo parado
  e, para cada thread registrada (inclusive as de serviço: Mesa, spdlog…), **PC, LR, SP, FP, x0–x2,
  x19, a cadeia de frame pointers (16) e uma varredura da pilha** atrás de endereços de código. A
  thread é pausada por um instante com `svcSetThreadActivity` (a pausa espera ela sair do núcleo),
  lida com `svcGetThreadContext3` (só funciona com a thread pausada; da mesma forma que um
  depurador) e retomada; só se o NPDM permitir (`envIsSyscallHinted(0x32/0x33)`), senão o
  relatório diz isso e fica com os marcadores. Endereços dentro do NRO saem com o offset
  `(+0x…)` para `aarch64-none-elf-addr2line -e oot3d_native_game.elf`. Também: a última chamada de
  serviço (`sm:AtmosphereHasService(sys:clk)`, `sm:GetService(sys:clk)`, `sys:clk GetApiVersion`,
  `sys:clk GetCurrentContext`; `EM_ANDAMENTO` se não voltou), o estado do stream lido com
  `try_lock` (`queued`, `frames_queued`, `busy`, `worker_waiting`, `progress_waiters`,
  `submitted/executed`, esperas e `signals_skipped`; `mutex=OCUPADO` se preso) e o pacer.
- `perf.txt` (`threads:`) ganhou `hang_wd=1 hang_reports=N`.
- Como ler: jogo em `perf_heap_walk` com PC no malloc/`__libc_lock_acquire_recursive` = lock do
  malloc preso; procure nas outras threads quem está dentro de `malloc/free`. Jogo em
  `wait_frames_in_flight` + render em `render_swap`/`render_pacer` = render thread presa no
  swap/pacer. `ultima_chamada_servico=… EM_ANDAMENTO` = IPC sem resposta.

### Atualização (2026-10-16, noite)
- Perfil real no console (`config/guest_profile` → `logs/guest_profile.txt`): as funções mais caras
  eram efeitos de áudio (reverb/eco) e `Runtime_Memcpy`; reescritas em C nativo como substituições
  exatas (milhões de casos, zero diferenças) — ganho estimado ~4% da lógica.
- VFP nativo de ARM64 ligado (`--native-vfp-aarch64`), validado com 193 milhões de checagens no qemu.
- Literais de texto reescritos pelo TopScreen excluídos da dobra de constantes.
- Builds feitos numa máquina Ryzen 5 5600G (WSL2) com ccache: completo ~30–60 min, incremental ~1 min.
- Controles no Switch: botões pelo rótulo, como no 3DS (A/B/X/Y nos lugares certos; o SDL2 do
  devkitPro mapeava por posição, estilo Xbox). ZL/ZR fazem o L/R do jogo (mirar/escudo) e o L/R
  físico faz o ZL/ZR. Mapeamento em `ConnectedPhysicalDeviceManager.cpp`.
- Padrões no Switch (sem arquivos no SD): render thread ligada, resolução 2× (3× = 720p deixou a GPU como gargalo: 23,4 contra 28,5 FPS medidos), FSR 1 e anisotrópico 4×.
  Desligar com `config/serial_presentation`, `config/resolution_scale`=1, `config/upscaler`=linear, `config/anisotropy`=1.
- HUD corrigido: o VAO do PICA vazava para a UI desenhada pelo host (corações, rupias, ícones), que
  herdava divisores de instância e amostrava um único texel. Áudio: teto da fila de áudio maior,
  reamostragem de alta qualidade e render thread abaixo da prioridade do áudio (chiado residual em estudo).

### Substituições nativas e o perfil guest (2026-10-17)
- As "recusas" do rodapé do perfil eram artefato: nos quadros medidos o perfil instala o callback
  em todos os blocos e `Hooked()` recusava toda substituição (as de bloco no topo de laço, a cada
  volta). O tempo medido dessas funções era o do código traduzido. Agora o perfil se registra
  (`Oot3dGuestOverrideProfileHook`), as substituições só recusam se o perfil repassaria um PC do
  intervalo, e o tempo nativo fica no bloco de entrada.
- Para ler o perfil: ~180 ns por entrada de bloco são do próprio perfil; o custo real de um bloco
  é `ns/entrada - 180`. Blocos curtos muito executados (laços de áudio, mutex) quase não custam.
- Maior custo real na floresta Kokiri: `Mtx3x4_Multiply` (0x36C174, ~440 chamadas/quadro, chamada
  externa manual). Núcleo exato novo (`oot3d_native_mtx3x4_kernel.h`, VFP nativo com o FPCR do
  guest): ~4,7x menos instruções; o caminho antigo em double errava IXC/UFC e o sinal de zeros
  descartados (~10% das matrizes do teste divergiam do software).
- Prólogos novos usam `"inline": true` em `guest_overrides.json`: não deslocam linhas do shard, e
  o PGO das outras funções do shard continua valendo (só a função com o prólogo perde o perfil).
- Gargalo sistêmico do VFP gerado: `Oot3dVfpNative*` volta ao software em todo resultado zero,
  inclusive zeros exatos (x*0, a-a), comuns em matrizes de rotação/escala. Resolvido na rodada
  do cabeçalho (abaixo, `--vfp-zero-aware`).

### Rodada do cabeçalho e do gerador (2026-10-17)
Mudanças que recompilam os 256 shards e pedem nova coleta de PGO, juntas numa rodada só. Cada uma
tem chave para A/B; sem as flags do gerador o C++ gerado é byte a byte o de antes.

**Comando de geração do C++ final** (de `tools/oot3d/native_a32_runtime`; o `--guest-overrides`
já aplica as substituições, então o `apply_guest_overrides.py` depois é idempotente e não muda
nada):
```sh
python generate_whole_aot_cpp.py --program ~/oot3d-work/gen/aot_program.json \
  --selection whole_aot_functions.json --code ~/oot3d-work/extract/exefs/code.bin \
  --output ~/oot3d-work/gen/cpp-lean --shards 256 --shard-strategy affinity \
  --native-vfp-aarch64 --vfp-zero-aware --lean-stack --guest-overrides \
  --giant-function-instructions 1000
python apply_guest_overrides.py --generated ~/oot3d-work/gen/cpp-lean
```
(Sem `--vfp-zero-aware --lean-stack` sai exatamente o `cpp-lean` atual; conferido arquivo a arquivo.)

1. **VFP com zero exato no nativo** (`--vfp-zero-aware`, exige `--native-vfp-aarch64`). Todas as
   operações aritméticas (VADD/VSUB/VMUL/VNMUL/VDIV/VSQRT/VMLA/VMLS/VNMLA/VNMLS) usam
   `Oot3dVfpZeroAware*` (`oot3d_native_a32_vfp_aarch64.h`). Caminho quente idêntico ao do
   `Oot3dVfpNative*` (uma instrução e um teste); resultado zero/NaN chama a metade fria
   `ZeroAwareSlow<Op>`, que mantém o zero nativo quando ele é exato nos dois modelos (operandos
   finitos e não subnormais; cancelamento exato `x == -y`, dois zeros, ou fator zero; no VMLA e
   família o produto nativo também tem de ser o do software) e senão roda o software como antes.
   Caso FZ: a soma/produto de normais que dá um subnormal descartado NÃO é exato (o ARMv8 mantém o
   sinal, o software dá +0, e há UFC/IXC): vai ao software. O núcleo Mtx3x4 usa os mesmos
   predicados. Prova: `run_vfp_aarch64_tests_qemu.sh` (devkitA64 -O3 do Switch sob qemu, 16 modos
   RMode×FZ×DN, valor e flags) com 299 milhões de checagens: 3,6 milhões de zeros exatos ficaram no
   nativo e 1,5 milhão de zeros inexatos foram recalculados, zero divergências; Mtx3x4 (230
   milhões de operações) e vfp_fast passam. `run_vfp_zero_aware_mutations.sh`: 9 mutações do
   critério (aceitar subnormal na soma ou no produto, aceitar produto descartado, aceitar qualquer
   zero em cada operação) são todas detectadas.
2. **Tráfego de pilha.**
   - `Oot3dWholeAotFlow` de 16 bytes (`OOT3D_WHOLE_AOT_FLOW_IN_REGISTERS`, padrão 1; 0 volta ao
     layout de 12 bytes, sempre no programa inteiro). Com 12 bytes (um byte, padding e duas
     palavras) o GCC montava o fluxo numa temporária de pilha em todo retorno e em toda chamada
     (store, reload, store de novo antes do epílogo comum), inclusive no caminho quente de cada
     chamada guest. Com um quarto campo `Reserved` (sempre 0) vai direto em x0/x1.
   - `--lean-stack`: o destino de uma leitura guest não é mais inicializado com 0 (o `ReadFast`
     escreve sempre que retorna true; com false o valor é morto) e o LDM com caminho de uma página
     não passa o endereço de `spanWords` ao caminho palavra a palavra (que lê em temporárias
     próprias): o array fica em registradores e as palavras vão do load direto para o estado. Os
     `str wzr, [sp]` antes de cada load e a ida e volta das palavras do LDM pela pilha somem do
     caminho quente; o caminho frio (página cruzada/MMIO) fica um pouco maior.
   - Derrames em volta de chamadas frias: ficam só nos caminhos frios (o GCC já divide as faixas
     vivas); os que sobram no caminho quente são os ponteiros `frame`/`context`/`state` em volta
     de cada chamada guest, que custam o mesmo em callee-saved. Não mexido.
3. **Contador de geração `mRangeGeneration`: avaliado, sem mudança.** A escrita rápida faz load,
   add, store do contador e store do carimbo da página. As gerações são observáveis (monitor
   exclusivo LDREX/STREX pelo `WriteGeneration()`, carimbos de página dos caches da GPU,
   `CaptureState`), então não dá para eliminar. Reordenar (ler antes do store guest, gravar o
   contador por último) não mudou nenhuma instrução dos shards medidos: depois de cada escrita o
   caminho rápido e o lento (`WriteFastSlow`, que altera o contador) se juntam, e na junção o GCC
   recarrega. Fazer em lote exigiria um contador local por função, gravado antes de todo caminho
   lento, chamada, gancho e saída (o mesmo problema da promoção de registradores); não feito.
4. **Promoção de registradores** (`--promote-registers`, commit da avaliação integrado; desligada,
   só acrescenta `Oot3dAotRunBlockEntryHook` no fim do cabeçalho): futuras rodadas não precisam de
   build completo só por ela.

Medição estática (devkitA64 GCC 16.1 com as flags reais dos shards, sem PGO; instruções / loads /
stores / loads de pilha / stores de pilha; "tudo" = cabeçalho novo + `--vfp-zero-aware
--lean-stack`):

| shard | antes | Flow 16 B | tudo |
|---|---|---|---|
| 000 | 157339 / 30343 / 27669 / 4505 / 13287 | 157204 / 29464 / 18464 / 3622 / 4080 | 160288 / 28969 / 17239 / 2950 / 2277 |
| 091 | 84854 / 16718 / 14211 / 1864 / 6479 | 87987 / 16569 / 9986 / 1722 / 2258 | 87840 / 16690 / 9334 / 1826 / 1586 |
| 150 | 116001 / 28857 / 22997 / 10189 / 14011 | 116861 / 28229 / 18800 / 9527 / 9809 | 123776 / 30679 / 19965 / 11559 / 10481 |
| 243 | 114658 / 28978 / 20779 / 10090 / 11973 | 115653 / 27833 / 18886 / 8957 / 10102 | 124378 / 30696 / 19982 / 11171 / 10490 |

A contagem estática mistura caminho quente e frio: as duas flags do gerador aumentam o código frio
(chamada a `ZeroAwareSlow`; um `ReadFast` por palavra no LDM palavra a palavra) e diminuem o
quente. A contagem dinâmica no qemu está na tabela seguinte.

Contagem dinâmica (qemu-aarch64 com uma instrução por TB, PCs executados × objdump; harness do
diferencial com o mesmo código dos dois lados, 32 casos × 40 tentativas, mesmas entradas; só o
código traduzido, em milhões; instruções / loads / stores / loads de pilha / stores de pilha):

| variante | instr | ld | st | ld pilha | st pilha |
|---|---|---|---|---|---|
| antes | 1459,85 | 362,05 | 197,71 | 32,13 | 40,53 |
| Flow 16 B | 1459,81 | 362,00 | 197,70 | 32,08 | 40,52 |
| `--lean-stack` | 1451,42 (−0,58%) | 362,05 | 189,28 (−4,3%) | 32,13 | 32,10 (−20,8%) |
| tudo | 1451,38 (−0,58%) | 362,00 | 189,27 (−4,3%) | 32,08 | 32,09 (−20,8%) |

As funções do harness (substituições e o fecho delas: reverb, memcpy, curvas, matrizes) são
laços com poucas chamadas: o ganho do Flow quase não aparece aqui; ele está nos sítios de chamada
e retorno (estático acima), proporcional ao número de chamadas guest do jogo. As entradas são
aleatórias e quase não exercitam o VFP com FZ=1 (54 mil instruções de software VFP em 1,46
bilhão), então o efeito do `--vfp-zero-aware` só aparece no jogo; o núcleo Mtx3x4, que usa o
mesmo critério, mediu ~4,7x menos instruções com os zeros exatos no nativo.

Provas da rodada (scripts no repositório; rodadas na Ryzen e no notebook):
- `run_register_promotion_equivalence.py --baseline <cpp-lean atual> --promoted <gerado com
  --vfp-zero-aware --lean-stack>`: as mesmas 314 funções reais (substituições, funções com
  substituição de bloco e o fecho de chamadas) dos dois geradores em dois namespaces, estado,
  memória, gerações, orçamento, ganchos e rastreio iguais: host 32 casos × 100 mil, qemu-aarch64
  (-O3 do Switch, VFP nativo) 32 casos × 20 mil, zero falhas.
- `run_guest_override_equivalence.py --qemu-aarch64` no gerado novo: 20 de 21 casos OK. A falha,
  `Math_SmoothStepToF` (0x36E168, tentativa 140844: FPSCR com OFC a mais na substituição nativa,
  FZ=0/RN), **já existia**: repete igual com o `cpp-lean` atual. É da substituição, não desta
  rodada (o lado traduzido usa o software nesse modo); a corrigir na própria substituição.
  **Corrigido (2026-10-18):** a substituição usava `Oot3dVfpAdd/Subtract/Multiply`
  (`oot3d_native_a32_vfp_fast.h`), cujo caminho rápido faz contas de float do host. No console e
  no qemu elas rodam dentro do `Oot3dGuestFpScope` (FPCR do guest, FPSR dobrado no FPSCR guest na
  saída), então as flags das contas auxiliares vazam para o guest: no TwoSum de `s0 - s4`
  (0x36E1C0) com `s4 = FLT_MAX` e `s0 = 0x7EC70697`, `sum - a` estoura (OFC, e IOC de inf - inf)
  embora a subtração em si não estoure. Agora a substituição usa as mesmas `Oot3dVfpNative*` do
  código gerado (instrução do guest com FZ=1, software inteiro nos outros casos; a primeira
  comparação, antes da última recusa, segue no software). O teste diferencial ganhou bordas para
  essa função (perto de FLT_MAX, inf, NaN, subnormais, todos os RMode×FZ×DN) e 16 casos de
  regressão fixos (a tentativa 140844 nos 16 modos; a versão antiga falha já no primeiro). Prova:
  host 20 milhões e qemu-aarch64 4 milhões de tentativas, zero falhas.
  **As outras (2026-10-18):** 0x00372070, 0x002CDE90, 0x003325F0 e 0x003727F0 também usavam o
  caminho rápido dentro do escopo guest. Dois vazamentos reais, achados pelas bordas novas no qemu:
  - `oot3d_translate_mtx3x4_local` (tentativa 9019, FZ=0/RN): `m[7] += 2^-24 * -0x0C7FFFFF`. O
    produto exato fica logo abaixo de 2^-126 e arredonda para -2^-126; a conversão double -> float
    do caminho rápido detecta tininess antes do arredondamento (ARMv8) e levanta UFC, que o
    software não levanta;
  - `Math3D_CylOutsideCylDist` (tentativa 7766, FZ=1): o TwoSum de `dx = x0 - x1` com `x1` perto de
    FLT_MAX estoura (OFC/IOC a mais).
  `FUN_002cde90` (só `s0 * 0.1f`) e `Math_SinF` (entradas limitadas por |x| < 12867) não vazaram em
  4 milhões de tentativas com bordas, mas tinham o mesmo mecanismo.
  Correção: todas as substituições fazem a aritmética VFP pelo namespace `Vfp` de
  `oot3d_native_guest_overrides.cpp`. No AArch64 ele chama as `Oot3dVfpZeroAware*`/`Native*` do C++
  gerado (`--native-vfp-aarch64 --vfp-zero-aware`): uma instrução com FZ=1, software inteiro nos
  outros casos. Nos outros hosts (sem escopo FP guest) usa o caminho rápido de float do host.
  O teste diferencial ganhou `EdgeFloat`/`NearMinNormalFactor`/`EdgeFpscr` (perto de ±FLT_MAX,
  expoentes altos, inf, NaN silencioso e sinalizador, subnormais, normais mínimos, produtos logo
  abaixo de 2^-126, os 16 modos RMode×FZ×DN com flags acumuladas) nas funções com VFP (as 5 acima,
  `FUN_003094cc`, `Math3D_CirSquareVsTriSquare`, as duas curvas e `Mtx3x3_Multiply`). Também ganhou
  casos fixos: 0x00372070 com os dois casos nos 16 modos (32) e 0x003325F0 com 16. As versões
  antigas falham já no primeiro; sem os casos fixos, as bordas aleatórias acham as duas falhas.
  Prova (Ryzen): qemu-aarch64 com 4 milhões de tentativas por função VFP (10 funções; duas sementes
  nas 5 principais) e 200 mil em todas; host com 20 milhões por função VFP e 1 milhão em todas.
  Zero falhas. Novos knobs de medição no teste: `OOT3D_QEMU_FPSCR`, `OOT3D_QEMU_CLEAN` e
  `OOT3D_NO_REGRESSION`.
  Núcleo Mtx3x4 (`Mtx3x4MultiplyFast`): o caminho `HostFloat` (FZ=0) roda numa chamada externa,
  sob `Oot3dGuestFpHostCallScope`, com FPCR do host e FPSR descartado na volta, então não vaza. O
  caminho nativo salva e restaura o FPSR. `Mtx3x3_Multiply` já usava `Oot3dVfpZeroAware*`.
  **Custo e decisão:** instruções executadas no qemu (`-one-insn-per-tb`), FPSCR do jogo
  (0x03000000), sem ganchos nem orçamento curto, 2000 tentativas, por chamada e com as recusas
  incluídas:

  | função | tradução | nova | antiga |
  |---|---|---|---|
  | 0x00372070 mtx3x4_local | 1410 | 1606 | 2470 |
  | 0x002CDE90 | 569 | 664 | 684 |
  | 0x003325F0 CylOutsideCylDist | 917 | 1521 | 1794 |
  | 0x003727F0 Math_SinF | 37197 | 37270 | 37649 |
  | 0x0036E168 SmoothStepToF | 807 | 1032 | 981 |

  A versão nova é mais barata que a antiga em 4 das 5, mas nenhuma ganha da tradução atual (que já
  tem `--vfp-zero-aware --lean-stack`). Por isso as 5 ficaram com `"disabled"` em
  `guest_overrides.json`: não são ligadas no C++ gerado (`load_manifest` as pula) e continuam no
  teste diferencial (`include_disabled=True`). Isso vale na próxima geração do C++. A contagem de
  instruções não é tempo no A57; para religar alguma, medir no console.
- `test_whole_aot_cpp.py` (44 testes, inclusive os das flags novas: opcionais, saem do manifesto
  quando desligadas, C++ idêntico sem elas).

### Áudio fora da thread do jogo (2026-10-17)
- O DSP HLE (mixagem) continua na thread do jogo: cada quadro de áudio lê as regiões compartilhadas
  do DSP, escreve status/aux/final_samples na memória do guest e sinaliza o guest, que roda logo em
  seguida e escreve a configuração do próximo quadro (inclusive os buffers aux de reverb/eco). É uma
  cadeia estritamente sequencial com o guest; levar para outra thread não paralelizaria nada.
  Só as cópias de 2 × 64 KiB zeradas a cada quadro foram eliminadas (buffers persistentes).
- O lado host (stretch, reamostragem 32728→48000, fila do SDL) roda numa thread própria
  (núcleo 1/2, prioridade 0x2B), acordada a cada envio do jogo e a cada 4 ms.
- Em jogo normal o stream não edita nada: só o ajuste fino de taxa (±0,5%, inaudível) segura a fila
  no alvo (100 ms). Períodos de pitch inteiros são inseridos só com a fila quase vazia (abaixo de
  ~37 ms) ou numa lentidão sustentada medida (jogo abaixo de 93% da velocidade: o tempo acompanha a
  velocidade, até 1,5×, sem mudar o tom); removidos só perto do teto de descarte do SDL (2,6× o alvo).
  Se o jogo atrasa e a fila chega ao nível crítico, o último período é repetido (40 ms cheio, depois
  fade de 80 ms) e a volta é contínua, sem a pausa de re-prime. Um período (~12 ms) fica sempre
  pendente para isso.
- 1ª versão (6c0012f) soou granulada: o crédito do stretch tinha sinal e uma inserção que passava
  do pedido virava remoção logo em seguida (e vice-versa), editando o tempo todo (26 min no console:
  153 s inseridos e 346 s removidos). A fila alta (min_queued=10396, alvo 4800) indica que o
  dispositivo drenava abaixo de 48 kHz: o backend audren do SDL só tem 2 wave buffers e espera, em
  quadros de 5 ms, o buffer recém-enfileirado começar a tocar; com 512 quadros (10,7 ms) sobrava
  ~1 ms para enfileirar o próximo e a voz secava entre buffers (grão). A simulação com dreno 5% mais
  lento reproduz os números do console. Buffer do dispositivo agora 1024 quadros
  (`config/audio_device_frames`, 256–4096; `512` volta ao anterior).
- `logs/perf.txt`, linha `audio:` (por janela de 150 quadros): `min_queued avg_queued device_hz`
  (dreno real do dispositivo; deve dar ~48000) `input_hz` (entrega do jogo; ~32728 a 100%)
  `trim_pct game_speed`, e acumulados `inserted_ms removed_ms concealed_ms conceal_events starved
  pump_ms_*` (em ms de áudio do DSP).
- A/B: `config/audio_thread` = `0` (mixagem host volta para a thread do jogo, stretch continua) e
  `config/audio_stretch` = `0`; ambos `0` = caminho antigo idêntico. Desktop: `TRIAEVUM_AUDIO_THREAD=1`,
  `TRIAEVUM_AUDIO_STRETCH=1`.

### Interpolação por fonte no DSP (2026-10-17)
- Onde fica: `third_party/azahar_audio/audio_core/interpolate.cpp`, chamada por
  `hle/source.cpp` (`Source::GenerateFrame`), que reamostra cada voz (rate do jogo) para 32728 Hz.
  O original segue o Citra/Azahar: None = retenção, Linear e "Polyphase" = linear.
- `config/audio_interpolation` (desktop: `TRIAEVUM_AUDIO_INTERPOLATION`): ausente/`original` =
  código antigo, bit a bit; `cubic` = Catmull-Rom de 4 pontos (sem atraso); `sinc` = sinc-Kaiser
  (β 8) polifásico Q15, 256 fases com mistura linear entre fases, 16 taps até rate 1 e alargado
  até 32 taps com corte em 1/rate para vozes aceleradas (anti-aliasing). Atraso fixo de 14
  amostras de entrada da voz (0,43 ms a 32728 Hz). None continua retenção nos dois modos.
- O passo (posições, amostras consumidas, xn1/xn2, fposition, status, loops) é o mesmo código do
  original; só o valor da amostra muda. O fluxo é contínuo entre buffers encadeados e loops
  (`State::history`, 32 amostras, serializado no savestate só quando não é zero).
- Testes: `oot3d_native_audio_interpolation_tests` (`--bench` mede 24 vozes). SNR sinc 79–94 dB
  contra 13–45 dB do linear; aliasing −81/−84 dB contra −4/0 dB. Custo estimado no Switch
  (llvm-mca A57 do laço NEON): ~+0,3 ms por passo com 24 vozes sempre ativas, metade aceleradas.

### Mixagem do DSP mais barata, bit a bit igual (2026-10-17)
- A mixagem (`dsp_mix` no `perf.txt`) continua na thread do jogo; ficou mais barata sem mudar nenhum
  byte do que o guest vê nem do PCM (modo de interpolação `original`). Onde ia o tempo (cena
  OoT3D-like: 18 vozes, música sequenciada em loops ADPCM, stream PCM16, efeitos, reverb no aux 0):
  ~1/3 decodificação ADPCM (o buffer inteiro a cada volta de loop, com alocação + zeragem), ~1/4
  interpolação linear, ~1/4 `MixInto` (3 mixes por voz, mesmo com ganho zero), o resto cópias das
  regiões compartilhadas (4 × 32 KiB por quadro), mixers e saída.
- O que mudou:
  1. `DecodedSampleQueue` reaproveita o armazenamento (sem `new`/zeragem/`free` por buffer) e
     lembra qual decodificação ADPCM guarda: um loop que volta com o mesmo endereço, tamanho,
     coeficientes, estado inicial do preditor e bytes codificados idênticos (`memcmp`) reusa as
     amostras e o estado final, que são exatamente o que a decodificação daria. Só host, não vai
     no savestate; buffers > 64 KiB codificados não são lembrados.
  2. ADPCM: quadro de 14 amostras sem o clamp no caminho realimentado (multiply-add + shift, ~5
     ciclos/amostra no A57 contra ~9); se algum valor sai de s16 o quadro é refeito pelo código
     original. Uma escrita de 32 bits por amostra (os dois canais).
  3. Interpolação None/Linear (`original`): o número de saídas cujo índice lê direto do buffer é
     calculado antes; o laço não testa índice virtual por saída. No AArch64 o Linear usa NEON nos
     dois canais (subtração saturada = o clamp, produto exato de 64 bits, shift 24): ~6,6 contra
     ~14,6 ciclos por saída (llvm-mca A57). Passos absurdos (rate > 65536) usam o laço original.
  4. Mix com os 4 ganhos zero é pulado em `MixInto` e nos mixers (somar zero não muda nada).
  5. Saída copiada por `memcpy` (final_samples e PCM do host); só a cauda não gerada do quadro da
     fonte é zerada.
- Medido no host (i3-8130U, -O3 -ffast-math): 0,0506 → 0,0288 ms por quadro DSP (−43%).
  Estimado no A57 a 1785 MHz (llvm-mca por laço × contagens da cena): ~0,6 → ~0,33 ms por passo.
  As cópias de 4 × 32 KiB por quadro ficam (`Read`/`Write` precisam dos mesmos intervalos para a
  contabilidade de gerações de escrita) e passam a ser ~1/4 do custo.
- Prova: `oot3d_native_audio_dsp_mix_tests` roda 4 títulos aleatórios × 3000 quadros pelo
  `ProcessFrame` inteiro (24 fontes, todos os formatos, loops, resets, filtros, ganhos extremos e
  denormais, aux com efeito do guest, formatos de saída, sample data reescrita durante o loop,
  rates absurdos, savestate a cada ~150 quadros com msgpack no digest) e compara o digest das duas
  regiões após cada quadro + PCM + savestates com o do código antigo: idêntico no x86-64 e no
  AArch64 com as flags do Switch sob qemu (`run_audio_dsp_mix_tests_qemu.sh`, devkitA64 16.1).
  `oot3d_native_audio_interpolation_tests` (digest original) e os testes de armazenamento passam.
  Observação: o padding de `DspStatus` é copiado de uma variável não inicializada desde o
  original (lixo de pilha); o teste ignora esses 28 bytes e o código não foi mudado aí.

### Planejamento PICA fora da thread do jogo (2026-10-17)
- O plano de cada draw (shaders, uniforms, bindings, hashes de textura; `pica_plan`, ~2,5 ms por
  passo) era montado na thread do jogo ao drenar a fila. Agora a thread do jogo põe no acumulador só
  um *placeholder* (id, estado decodificado, produtores de cópia) e o plano é montado a partir do
  mesmo snapshot imutável da submissão (bytes de vértice/índice/textura já copiados no momento da
  submissão) por um worker de planos (prioridade 0x2E, núcleos 1/2) ou, no mais tardar, pelo comando
  de render que executa o quadro / pelo flush de dependência (`Run`, guest parado). Fills e display
  transfers continuam capturados na hora; a ordem de execução não muda.
- A/B no SD: `config/serial_pica` volta ao caminho antigo; `config/pica_plan_render_thread` monta os
  planos na render thread sem o worker. Só vale com a render thread ligada (sem
  `config/serial_presentation`). Trace semântico e diagnóstico estendido usam o caminho serial.
- `perf.txt` ganhou `pica_plan_deferred`, `plan_worker`, `plan_worker_ms`, `plan_late_ms`
  (montados pelo consumidor porque o worker ainda não tinha chegado), `plan_late_draws` e
  `plan_serial_ms` (ms por quadro de host).
- Desktop: `TRIAEVUM_DEFERRED_PICA_PLAN=1` (worker) ou `=render`. Teste: `oot3d_native_pica_deferred_plan_tests`.
- Correção (primeira versão degradava com o tempo de jogo no console: worker 3,9 → 89 ms por quadro
  em 3 min, 30 → 9 FPS): depois de montar cada plano o worker fazia `submissão = {}`, e um pacote
  PICA padrão aloca e zera quatro áreas de shader de 16 KiB (64 KiB por draw); além disso cada
  passada alocava vetores de submissões e de planos de vários MB. Eram ~118 KiB de heap a mais por
  draw (152 KiB contra 34 KiB do caminho serial), em blocos grandes liberados por outras threads:
  banda de memória e fragmentação do heap do newlib, que pesavam em todas as threads. Agora os
  lotes são reciclados (vetores mantêm a capacidade, o vetor drenado volta por troca) e só os
  recursos da submissão são liberados: o tráfego de heap é igual ao serial. `perf.txt` mostra
  `plan_batches_live/queued/allocated` (devem ficar pequenos e constantes). Teste de longa duração:
  `oot3d_native_pica_deferred_plan_soak_tests` (2400 quadros × 160 draws, nos três modos).
- Núcleos fixos (`PlaceSwitchThread`, `oot3d_switch_threads.h`; vale só se a máscara do processo
  permitir): render thread no núcleo 2; worker de planos PICA e thread de áudio no núcleo 1. Antes a
  ordem de criação punha a render thread e o worker no mesmo núcleo, com o outro quase ocioso.

### Escalonamento das threads (2026-10-17)
Regras do Horizon (confirmadas pelo StevensND/nfsmw-nx): só a prioridade 0x3B reveza tempo; em
qualquer outra, uma thread ocupada bloqueia as de prioridade igual/menor no mesmo núcleo, mesmo com
outros núcleos ociosos; máscara de um núcleo nunca migra.

| Thread | Antes (núcleo, prio) | Agora `fixed` (padrão) | `float` | Espera |
|---|---|---|---|---|
| Jogo (main: CPU guest, DSP HLE, submissão) | 0, 0x2C | 0, 0x2C | 0, 0x2C | condvar (frames em voo, Drain) |
| Render thread (`NativeRenderCommandStream`) | 2, 0x2D | 2, 0x2D | 1\|2 (ideal 2), 0x2D | condvar; pacer: dormir + spin 500 us → só dormir |
| Worker de planos PICA | 1, 0x2E | 1, 0x2E | 1\|2 (ideal 1), 0x2E | condvar; mutex de build |
| Áudio do host (`HostAudioStream`) | 1, 0x2B | 1, 0x2B | 1, 0x2B | condvar com timeout 4 ms |
| Áudio do SDL (audren, TIME_CRITICAL) | núcleo da criação (1 ou 2), 0x2B | 1, 0x2B | 1, 0x2B | audren (bloqueante) |
| spdlog (escritor assíncrono) | 1 ou 2, **0x2C** | 1\|2, 0x3B; fila cheia descarta o mais antigo | idem | condvar |
| Mesa: fila do cache de shader em disco (util_queue) | 1 ou 2, **0x2C** | 1\|2, 0x3B | idem | condvar |
| ResourceManager (BS::thread_pool), sombra de salas, texture pack, prebuild de sol interno | 1 ou 2, **0x2C** | 1\|2, 0x3B | idem | condvar |
| SDL joystick/eventos | — (sem thread no Switch: polling na thread do jogo) | — | — | — |

Sem glthread, o Mesa compila os shaders na render thread (nenhuma thread de compilação própria).
Antes, toda pthread de serviço nascia na prioridade do jogo (0x2C) e preemptava a render thread
(0x2D) e o worker (0x2E) sempre que tinha trabalho (escrita de log, compactação do cache do Mesa,
carregamentos).

- **`config/thread_layout`** (primeira palavra; ausente = `fixed`): `fixed`, `float` (render thread
  e worker com máscara 1|2 — o nfsmw-nx viu o pior caso piorar com a render thread fixa) ou
  `legacy` (comportamento anterior exato, incluindo o spin de 500 us). Opção `pacer_spin_us=N`
  (0–4000) depois do modo, p. ex. `float pacer_spin_us=200`.
- **Espera ativa**: o pacer não gira mais antes do prazo (`svcSleepThread` acorda no prazo); a
  `NativeRenderCommandStream` só sinaliza condvar quando alguém espera (cada sinal é uma chamada ao
  kernel); o worker de planos verifica o pedido de parada por um atômico, sem lock por draw.
- **Atômicos por draw** (A57 sem LSE): contador de placeholders por lote (somado no `Publish`),
  contadores do worker por lote; os resolves na render thread pegam o mutex de build uma vez a cada
  sequência de até 32 draws do mesmo lote (a sequência termina logo depois de um draw construído
  ali, para o worker não esperar).
- **`perf.txt`**: `plan_lock_wait_render_ms/_worker_ms` (contenção do mutex de build),
  `plan_resolve_locks`, `plan_worker_idle_ms`; `sync: guest_wait_frames_ms` (jogo esperando a render
  thread), `guest_wait_drain_ms`, `render_idle_ms` (render thread esperando o jogo),
  `signals_skipped`, `pacer_spin_ms`, `pacer_oversleep_ms/_max_ms`; `threads: layout=… <papel>_cpu=%
  @c0:c1:c2` (CPU de cada papel por `svcGetInfo(ThreadTickCount)` e onde a render thread/worker
  foram vistos rodando).
- **Como medir (A/B)**: mesma cena e mesmo save, 2–3 min por modo, gravando `config/thread_layout`
  com `fixed`, `float` e `legacy`; comparar `fps`, pior quadro, `guest_wait_frames_ms` (cai = render
  thread menos atrasada), `pacer_oversleep_max_ms` (se passar de ~0,5 ms, testar `pacer_spin_us=200`)
  e, em `float`, quanto `render@…` aparece no núcleo 1.

### fps60 de verdade e economia de GPU no backend GL (2026-10-17)
- **fps60 não apresentava a 60 Hz**: o pacer e a cadência de amostras vêm do modo `FrameRate` das
  configurações gráficas (padrão `Original30`, escolhido no menu F1 do PC); `--presentation-rate 60`
  só entrava no contrato e nunca chegava ao pacer. Com `config/fps60` o console seguia apresentando
  30 quadros/s com dois refreshes de 60 Hz cada e nunca desenhava a amostra interpolada. Agora, no
  Switch, `ResolveLaunchFrameRateMode` (`oot3d_native_launch_frame_rate.h`) converte timing+taxa do
  lançamento no modo de pacing (`native30_interpolated`+60 → `Interpolated2x`;
  `native30_no_interpolation` → `Original30`, igual ao padrão de antes). Teste:
  `oot3d_native_frame_rate_tests.cpp` (`triaevum_presentation_clock_tests`).
  Atenção: a amostra interpolada redesenha o quadro inteiro (o dobro de GPU); em 720p não cabe.
- **`config/gl_lean`** (ligado por padrão; `0` desliga para A/B), economias exatas do backend GL:
  1. a cópia do display transfer que a apresentação reenvia (a amostra já executou a mesma cópia)
     é pulada quando o alvo de origem não mudou (versão por alvo); em 720p evita ler+escrever
     ~3,7 MB por quadro apresentado;
  2. a limpeza da janela antes do scanout é pulada quando o scanout cobre a janela inteira
     (widescreen 16:9);
  3. a imagem Shadow2D (R32UI do tamanho do alvo) só é limpa quando um produtor/consumidor de
     sombra a usa — antes cada fill de cor limpava ~3,7 MB (topo) + ~2,8 MB (tela de baixo) por
     quadro em 720p mesmo sem sombra;
  4. blocos de uniform iguais ao anterior não são reenviados (o nvc0 empurra o upload de constant
     buffer pela FIFO, palavra a palavra) e a imagem da unidade 5 só é religada quando muda.
- **Memória de GPU limitada** (crash após ~8 min com fps60: o nouveau não conseguiu mapear um
  buffer e o Mesa copiou os uniforms de um draw para NULL dentro do `glBufferData`):
  1. o cache de texturas PICA (por conteúdo) nunca descartava — cada textura animada criava uma
     textura GL nova a cada quadro. Agora sai o que ficou 1800 quadros sem uso e, acima do
     orçamento (`config/gl_texture_budget_mb`, padrão 96, 16–2048), o menos usado recentemente
     (nunca o que o quadro atual/anterior usou). As LUTs de iluminação seguem a mesma regra de
     ociosidade;
  2. uniforms, vértices e índices por draw vão para anéis de 8/4/1 MB (map sem sincronização,
     faixas que nunca se sobrepõem numa geração), com orphan só quando o anel dá a volta — antes
     era um `glBufferData` (realocação) por draw;
  3. falha de alocação/mapeamento (`GL_OUT_OF_MEMORY`, map nulo) não é mais fatal: descarta todas
     as texturas ociosas e pula o draw (contado em `gl_oom`/`gl_draws_skipped` no perf.txt;
     o stderr com `TRIAEVUM_GL_OUT_OF_MEMORY` não chega ao log do console);
  4. `logs/perf.txt` mostra `gl_textures_live`, `gl_tex_mb`, `gl_tex_created/evicted`,
     `gl_luts_live`, `gl_programs`, `gl_targets`, `gl_buffers_live`, `gl_buffer_mb`,
     `gl_ring_orphans`, `gl_oom`, `gl_draws_skipped`, `heap_used_mb` e `proc_used_mb`/`proc_total_mb`:
     todos devem ficar estáveis numa sessão longa. Teste:
     `runtime/three_ds_recomp/tests/gfx_opengl_pica_residency_tests.cpp` (30 min simulados).
- **Escala fracionária**: `config/resolution_scale` aceita `2.5` (ou `2,5`; múltiplos de 0,25). Os
  alvos ficam 1000×600 (1067×600 com widescreen) e o FSR EASU+RCAS sobe para 1280×720: ~69% dos
  pixels de 3×. Bordas de viewport/scissor/cópias = round(borda nativa × escala), então retângulos
  vizinhos continuam encaixando; com escala inteira as contas são as mesmas de antes. Testes:
  `runtime/three_ds_recomp/tests/gfx_opengl_pica_scale_tests.cpp`.
- Em 3× o EASU já era pulado (saída 1:1); só o RCAS roda (`config/sharpen` `0` troca por cópia
  simples). `config/anisotropy` `2` reduz o custo de textura em superfícies oblíquas.

### Cache de estado GL no caminho de draw PICA (2026-10-17)
- Cada draw reenviava todo o estado fixo (~27 chamadas: framebuffer + draw buffer, VAO, programa,
  4 unidades de textura com `glActiveTexture`, color mask, blend/logic op, depth, stencil, cull,
  viewport, scissor) e depois religava o VAO do caminho legado. O Mesa filtra quase todas as
  repetições, mas cada uma ainda paga despacho + contexto + validação de enum na render thread; e o
  vai-e-volta de VAO não era repetição: fazia o state tracker reconstruir os vertex arrays de todo
  draw (no quadro interpolado, o dobro).
- `PicaGlStateCache` (`gfx_opengl_pica_state_cache.h`, sem GL, testado em
  `gfx_opengl_pica_state_cache_tests`) guarda o último valor de cada estado e só emite a chamada que
  muda algo. O VAO legado passa a ser religado sob demanda (`RestoreLegacyVertexArray`, nas funções
  legadas que usam vertex arrays). Ordem dos draws, valores e chamadas de draw não mudam.
- O cache só vale entre draws PICA consecutivos: é esquecido no início de cada lista de execução
  (`PublishPicaCompositionSequence`), em qualquer entrada PICA que não seja draw (transfer, fill,
  clear, apresentação/pós), em qualquer chamada do caminho legado (`MarkPicaGlStateForeign`) e
  quando os caches do legado mostram que ele desenhou. Dentro do draw, criar programa/alvo/textura,
  fills e limpezas de Shadow2D esquecem só o estado que tocam.
- Efeito colateral corrigido: o LUT de iluminação novo era criado (ligado) na unidade 3 depois de
  ligar as texturas, trocando a textura do slot 2 por ele naquele draw; agora é buscado antes.
- A/B: `config/gl_state_cache` com `0` volta ao comportamento antigo (todas as chamadas + religar o
  VAO legado). `config/gl_stats` (arquivo existe, mesmo vazio) liga em `perf.txt`, por quadro de
  host: `gl_draws`, `gl_presents`, `gl_state_calls` (emitidas), `gl_state_skipped`,
  `gl_state_calls_per_draw`, `gl_program_binds`, `gl_texture_binds`, `gl_vao_binds`, `gl_fb_binds`,
  `gl_ubo_uploads`, `gl_draw_ms` (tempo da render thread dentro de `SubmitPicaDraw`) e
  `gl_us_per_draw`. Os contadores rodam nos dois modos, então o mesmo build mede antes e depois.
- Quadro sintético de 400 draws (teste): 27,5 → 2,7 chamadas de estado por draw.

### Perfil de GPU/CPU por etapa: `config/gpu_timing` (2026-10-17)
- Arquivo `sdmc:/switch/oot3dre/config/gpu_timing` (conteúdo opcional: N, amostrar a GPU 1 quadro a
  cada N; padrão 30). Desktop: `TRIAEVUM_GL_GPU_TIMING=30`. Sem o arquivo nada é instalado: cada
  ponto de marcação custa uma carga atômica e um desvio.
- Cada janela de 150 quadros do `perf.txt` ganha uma linha `glperf host_frames=... ` com médias por
  tipo de quadro de host: `step` (executa um quadro novo do jogo), `interp` (só a amostra repetida /
  interpolada do fps60) e `hold` (reapresentação). Chaves `<tipo>_gpu_<etapa>` (ms por quadro
  amostrado), `<tipo>_gpu_ms` (início do quadro → depois do swap), `<tipo>_cpu_<etapa>` (ms da
  render thread por quadro) e `<tipo>_n_<contador>` (por quadro; `*_kb` em KiB).
- Etapas: `begin`, `clear` (janela, alvos PICA, fills), `pica_top`/`pica_bottom`/`pica_off` (draws
  PICA da amostra exibida por alvo: tela de cima, de baixo, outros como sombras/RTT), `pica_canon`
  (execução canônica, não exibida, de um quadro novo), `transfer` (display transfers), `post_aa`,
  `post_easu`, `scanout` (último passe na janela: RCAS, EASU/FXAA direto ou cópia), `ui` (HUD/menus
  TopScreen), `present` (EndFrame + swap); só CPU: `other`, `pacing`, `idle`.
- GPU: `glQueryCounter(GL_TIMESTAMP)` a cada troca de etapa no quadro amostrado (pool de 4×128
  queries); lidos 2+ quadros depois com `GL_QUERY_RESULT_AVAILABLE`, nunca bloqueia (sem resultado,
  a amostra é descartada: `gpu_dropped`). Diferença entre timestamps = execução da GPU **mais** a
  espera por comandos da CPU dentro do quadro: se a render thread é o gargalo, `gpu_*` inclui
  ociosidade.
- Contadores (ganchos nos ponteiros do glad, instalados só com o arquivo): draws, clears,
  `fbo_binds/fbo_changes`, `tex_binds`, `prog_binds/prog_changes`, VAO/UBO binds, uniforms soltos,
  bytes enviados (`buf_upload_kb`, `map_kb`, `tex_upload_kb`), `buf_reallocs` (glBufferData),
  blits, `barriers` (glMemoryBarrier), `syncs`, `finish`, `flush`, `get_error`, `gets`
  (glGet*/glIsEnabled/glCheckFramebufferStatus), `readbacks`, `gen_delete`, `enables`.
- Custo ligado: uma chamada indireta + incremento por chamada GL contada, uma leitura de relógio por
  troca de etapa e ~20–60 timestamps no quadro amostrado.
- Memória: `mallinfo()` (trava o malloc do newlib, compartilhado com o Mesa, enquanto percorre as
  listas livres) agora só a cada 20 janelas (`heap_used_mb` aparece só nelas); `heap_arena_mb` e
  `heap_arena_peak_mb` vêm dos totais que o próprio malloc mantém (sem lock) em toda janela.
- Resumo: `python3 tools/oot3d/perf_report.py perf.txt [--session -1] [--skip 2] [--windows]` —
  fps por sessão, clocks, fases, memória e, com `glperf`, top etapas de GPU, CPU da render thread e
  contadores por tipo de quadro.

### Clocks em cada janela do `perf.txt` (2026-10-17)
- Cada linha `host_frames=` ganha `clocks: cpu_mhz= gpu_mhz= mem_mhz= clk_src= op_mode=
  perf_mode= sysclk_profile= sysclk_enabled= sysclk_api= clk_ipc_us=`
  (`oot3d_switch_clocks.cpp`). Sempre comparar medições com o mesmo clock; agora o `perf.txt`
  registra em que clock cada janela foi medida.
- Fonte dos MHz: IPC público do sys-clk (`sys:clk`, comando `GetCurrentContext`, o mesmo que o
  overlay usa), `realFreqs` (medido) quando a API ≥ 2, senão o alvo (`clk_src=sysclk`). A sessão é
  aberta na primeira janela e mantida; antes, o sm do Atmosphère é consultado
  (`AtmosphereHasService`, 65100) porque `smGetService` de serviço não registrado fica esperando.
  Sem sys-clk: MHz -1, `clk_src=none`, nova tentativa a cada 60 janelas. Custo: uma IPC por janela
  de 150 quadros (`clk_ipc_us`), feita depois de fechar o tempo da janela.
- Somente leitura e sem clkrst/pcv no app (SwitchWakerHD issue #33: clkrst no app travou o console
  ao acordar do sleep); clock continua sendo papel do sys-clk.
- `op_mode` (portátil/dock) e `perf_mode` vêm de `appletGetOperationMode`/`appletGetPerformanceMode`
  (valores guardados pela libnx, sem IPC) e funcionam mesmo sem sys-clk.
- `perf_report.py` mostra o clock de cada sessão; se mudar no meio, avisa (janela, antes → depois) e
  separa fps/`total_ms` por clock; `--windows` lista `cpu/gpu/mem modo` por janela.

### Heap estável e menos trabalho fantasma por quadro (2026-10-17)
Medido (`switch-logs/interp1`, fps60, escala 2, 8,5 min): `heap_arena_mb` 631 → 1659 MB com
`heap_used_mb` ~700 MB (fragmentação do malloc do newlib, que o Mesa/nouveau também usa: o
`libdrm_nouveau` do devkitPro aloca cada BO com `memalign`).
- **Vetor de planos por quadro visual**: um plano de draw tem ~16 KiB; cada quadro do jogo montava
  um vetor novo de ~200 planos (3–4 MB) para o quadro visual e outro para os draws pendentes (que
  ainda crescia por dobra), e liberava os de quadros antigos na render thread: blocos grandes de
  tamanhos variados a 30/s. Agora `Oot3dPicaVisualFrame::Draws` é um `Oot3dPicaPlanVector`, cuja
  memória volta limpa a um pool (até 6) quando o quadro morre; o acumulador pega dali e o vetor
  pendente mantém a capacidade.
- **Paletas da interpolação** (`ShareVisualPaletteMotions`, ~1,7 KiB por draw) eram alocadas a cada
  amostra: agora um rascunho da transição preparada, reaproveitado.
- **Views da amostra**: os bindings de vértice não são mais limpos entre quadros (sem
  `HasVertexBindingOverrides` são ignorados), então a cópia interpolada reaproveita os bytes.
- Teste de longa duração no host: `oot3d_native_pica_visual_frame_soak_tests` (1800 quadros,
  180–220 draws, matching + 2 amostras por quadro, `operator new` contado): antes 12 MB e 4,8
  alocações ≥ 256 KiB por quadro; agora ~0,24 MB e 0 grandes, constante no tempo.
- **Preparação do quadro visual (fps60) sem mapas nem hash por draw**: o matching entre o quadro
  anterior e o novo usa listas ordenadas (identidade, índice) com junção por intercalação no
  lugar dos `unordered_map`/`std::map` (chave nativa compacta de 13 × u64, sem padding), e todo o
  rascunho (marcas, chaves, tabela da DP monotônica, identidades) fica na transição preparada.
  `ShareVisualPaletteMotions` usa grupos e ossos em vetores planos reaproveitados e roda uma vez
  por transição (não depende de alpha). `CmbPaletteSlots` valida o atributo de ossos uma vez e
  lembra o resultado da varredura de vértices por draw, chaveado pelos bytes compartilhados
  (imutáveis, segurados na memo) e pelo layout: no regime estável a varredura não roda.
  `NormalizedVertexUniformDelta` e a interpolação de uniforms pulam linhas iguais (exato,
  inclusive -0,0). `perf.txt` ganhou `rt_prepare_ms` (matching + análise), `rt_match_ms` e
  `rt_sample_ms` por quadro de host. Prova de equivalência:
  `oot3d_native_pica_visual_frame_equivalence_tests` compara com uma cópia congelada do código
  anterior (`..._reference.inc`) pares casados, estatísticas e amostras bit a bit em milhares de
  quadros aleatórios (também com `-O3 -ffast-math`, ASan/UBSan e TSan `--threads`); `--bench`
  mede o custo por quadro de jogo (Prepare + amostras em 0 e 1/2).
- **Anéis de upload PICA ≤ 2 MiB** (eram 8/4 MiB): o nouveau subaloca buffers até 2 MiB de slabs
  que ele mantém (`nouveau_mm`); acima disso cada orphan do anel era um `memalign` novo de 8/4 MiB
  (1610 orphans na sessão).
- **ImGui sem contexto GL**: com a render thread, `ImGui_ImplOpenGL3_NewFrame/RenderDrawData`
  rodavam na thread do jogo, onde o contexto não está atual: tudo ia para o despacho nulo do Mesa,
  e o backend refazia a cada quadro shaders, buffers e o upload do atlas de fontes (~1 MB). Era o
  `tex_upload_kb` ≈ 1 MB e parte de `gen_delete`/`gets`/`buf_reallocs` até em quadros hold (os
  ganchos do glperf contam chamadas de qualquer thread). Agora sem contexto atual só o atlas é
  mantido construído na CPU; nada do ImGui aparecia na tela antes e continua assim.
- **UI legada (HUD TopScreen)**: os triângulos vão para um anel de 1 MiB por map não
  sincronizado (orphan só ao dar a volta) em vez de um `glBufferData` por draw (~29 por quadro);
  o renderer da UI não aloca mais por quadro (ordenação por inserção em vez de `stable_sort`, quad
  em `std::array`, chave de textura e vetores de rascunho mantidos).

### Heap por programa GL no Mesa 26 (2026-10-17)
- Com o Mesa 26 o heap cresce ~6–8 MB por programa PICA novo (20.1: ~1,5 MB). O Mesa 26 converte o
  GLSL para NIR já no `glCompileShader` e guarda esse NIR no objeto de shader; como os shaders ficavam
  anexados ao programa, o `glDeleteShader` só os marcava e o NIR vivia junto com o programa.
  `LinkPicaGlProgram` e o programa de scanout agora fazem `glDetachShader` antes de apagar.
  Medido no host (Mesa 26.2.2, iris e llvmpipe, VS PICA real de 30 KB): −2,0 MB por programa
  (iris 8,6 → 6,6 MB).
- O resto é do driver: o nvc0 do Mesa 26 guarda o NIR inteiro (`nvc0_program::nir`, só usado para
  retraduzir quando muda o número de planos de clip) além do NIR serializado do st/mesa; o 20.1
  guardava só tokens TGSI. Reduzir isso exige patch no Mesa (serializar `prog->nir` depois de
  `nvc0_program_translate` e desserializar só na retradução).

### GPU: ZCULL/early-Z no nvc0 e economias exatas `config/gl_gpu_lean` (2026-10-17)
**O que o Mesa 20.1 (nvc0) faz** (fonte em `mesa-cache/work/mesa-20.1.0-rc3`):
- **ZCULL nunca liga.** `nvc0_screen.c` grava `ZCULL_REGION = 0x3f` ("deactivate ZCULL") na criação
  da tela e `nvc0_validate_zcull` está sob `#if 0`; nenhum caminho reativa. O `libdrm_nouveau` do
  devkitPro até faz `nvGpuChannelZcullBind`, mas sem região configurada o hardware não usa. Formato
  de depth, clears ou estado não mudam isso: ZCULL (os ~2 ms do nfsmw-nx) só com patch no Mesa
  (portar a configuração de região/armazenamento/invalidação do deko3d, que liga ZCULL por padrão)
  ou com deko3d/NVK.
- **Early-Z por pixel é automático** no Maxwell, salvo se o cabeçalho do fragment program disser
  que ele mata pixels (`discard` em qualquer lugar do GLSL → `usesDiscard` → `hdr[0] |= 0x8000`),
  escreve depth (`gl_FragDepth` → `hdr[19] |= 0x2`, e o Mesa ainda põe `ZCULL_TEST_MASK = 0x11`) ou
  grava memória (imagem/atomics → `hdr[0]` bit 26). Função de depth, blend ou logic op não desligam
  early-Z.
- Compressão de depth: o libdrm do devkitPro informa versão `0x01000202` (≥ `0x01000101`), então o
  nvc0 escolhe tipos de memória comprimidos para Z24S8; limpar depth **e** stencil do buffer
  inteiro é o caso que o hardware pode resolver só marcando os tiles.
- Os programas PICA já estavam certos para early-Z: no modo especializado (o único usado no GL; o
  paramétrico/uber é só do Vulkan) `discard` só existe com o alpha test do PICA ligado (função ≠
  ALWAYS) e `gl_FragDepth` só fora da forma "raster depth" (`Oot3dPicaFragmentDepthIsRasterDepth`:
  escala -1, offset 0, z-buffering). Por isso os shaders **não** foram mexidos (mudar a fonte
  invalida o cache de shaders do Mesa sem ganho provado). Os casos que restam são contados (abaixo).

**`config/gl_gpu_lean`** (padrão ligado; `0` desliga para A/B; desktop `TRIAEVUM_GL_GPU_LEAN=0`),
`gfx_opengl_pica_gpu_lean.h` (sem GL, testado em `gfx_opengl_pica_gpu_lean_tests`):
1. **Logic op COPY com blend desligado → logic op desligado.** Todo draw opaco do PICA chegava ao
   hardware com `LOGIC_OP_ENABLE = 1` (o Mesa não filtra COPY), uma operação de ROP que pode ler o
   destino. COPY = s pela especificação.
2. **Blend ADD com fonte ONE e destino ZERO (cor e alfa) → blend desligado** (s·1 + d·0 = s).
3. **Draw sem efeito não é emitido**: máscara de cor vazia, ou blend que mantém o destino
   (ZERO/ONE/ADD), ou logic op NOOP; e sem escrita de depth, sem stencil que mude, sem imagem
   (produtor Shadow2D nunca é pulado). Estado e uploads anteriores continuam iguais.
4. **Fill de depth em alvo sem stencil no PICA (D16/D24)** limpa também o stencil do renderbuffer
   D24S8: o frontend só liga o teste de stencil com formato 3 (D24S8), então esse plano nunca é lido
   nem escrito; a limpeza vira depth+stencil inteira em vez de mascarada.
- Prova de imagem igual: o teste de unidade compara o modelo de operações por fragmento do GL
  (todas as 256 cores de destino × varredura densa de fonte, inclusive fora de [0,1]); e uma checagem
  de pixels em GL real (EGL sem janela, Intel UHD 620 e llvmpipe, 20 cenas aleatórias de 300
  triângulos por caso, 257×193): 0 bytes de cor e 0 valores de depth diferentes nos quatro casos (a
  mesma checagem acusa milhões de diferenças com XOR no lugar de COPY ou sem a limpeza de depth).
- **No console**: mesma cena parada (mercado, câmera fixa, quadro pausado pelo menu), uma captura
  de tela do álbum com `gl_gpu_lean` ausente e outra com `0`; comparar pixel a pixel no PC (a
  captura do álbum é JPEG: comparar com tolerância ou usar as capturas PNG do homebrew, se houver).
  Para medir: `config/gpu_timing` com e sem `gl_gpu_lean`, mesma cena, `step_gpu_pica_top`.
- Ganho esperado: incerto sem medir. (1) atinge quase todos os draws opacos da cena — se o ROP do
  Maxwell lê o destino com logic op ligado, são ~4 B por pixel opaco desenhado (≈ 3,3 MB por camada
  de overdraw a 960×853), algo como 0,2–0,6 ms no quadro cheio; se não lê, ~0. (2)/(3) dependem de
  quantos draws o jogo manda assim (contados). (4) ≤ 0,1–0,2 ms por limpeza.

**Contadores novos no glperf** (`config/gpu_timing`, por quadro de host e tipo): `n_fs_discard`,
`n_fs_depth`, `n_fs_image` (draws cujo programa desliga early-Z), `n_blended`, `n_logic_copy_off`,
`n_blend_identity_off`, `n_noeffect_skipped`, `n_full_depth_clears`. Com eles dá para decidir se vale
especializar mais (ex.: trocar `gl_FragDepth` por `glDepthRange` — não feito: arredondamento de depth
em hardware × shader pode diferir no último bit e mudar testes LEQUAL/EQUAL).

**Amostragem do glperf corrigida**: com fps60 os quadros de passo e intermediário se alternam, e
`FrameSerial % 30 == 0` sempre caía no mesmo tipo — nenhum `perf.txt` tinha `step_gpu_*`. Agora a
amostra anda um quadro a cada bloco de N (um por bloco, alternando a paridade).

**Avaliados e não feitos (sem prova de imagem igual ou sem ganho):**
- Reordenar draws / "céu por último": sem ZCULL, ordenar só ajuda o early-Z por pixel quando o
  opaco vem de frente para trás; o PICA exige a ordem do jogo para blend, depth EQUAL e stencil, e o
  backend não tem marcador seguro no estado PICA que diga "este draw é o céu" (só heurística por
  textura/estado, que pode errar e mudar a imagem). Risco alto, ganho incerto.
- "Resolve sem cópia" do display transfer: a cópia (0,2–0,8 ms medidos) teria de ser adiada até o
  alvo ser reescrito, e ele é reescrito todo quadro (a imagem de display é reapresentada em quadros
  hold/EASU reaproveitado): a cópia só mudaria de lugar.
- Passes de tela cheia: a limpeza da janela e a cópia repetida já são puladas (`gl_lean`); o RCAS
  (~1,9 ms) e a UI (~1,5 ms) mudam a imagem se tirados.
- Shaders: o especializado já tem os ramos resolvidos na geração; sem medição por programa não
  compensa invalidar o cache de shaders.

### Game Over sem o prompt "Continuar?/Salvar?" (2026-10-17)
- Sintoma: ao morrer, o "GAME OVER" aparece (com o fade correndo rápido demais) e as escolhas nunca
  aparecem (no máximo um quadro); a camada do controlador do Game Over some. Não dependia do Mesa.
- Causa: o Game Over do OoT3D é desenhado pelo controlador da tela de baixo; o TopScreen migra as
  escolhas para a tela de cima depois de 60 desenhos de atraso + 24 passos de fade (2.1.1
  `005CF75C`/`005CFB38`) e as compõe no quadro (viewport 0,40,480,320, `PauseUi_Draw` auxiliar, depois
  o desenho nativo `0x00427A3C`; `docs/TRIAEVUM_ISSUES_45_46.md`). No produto isso é feito pelo gancho
  na entrada `0x00427A3C`. Desde o commit do upstream que ligou o source port inteiro nos builds de
  produto (80ef0f6), a chamada `0x00300544` também rodava a rota do mod com o MESMO estado: atraso e
  fade andavam duas vezes por quadro e, terminado o fade, a chamada suprimia o desenho nativo, então o
  gancho de `0x00427A3C` (que desenha as escolhas) nunca mais rodava.
- Correção: com o gancho de `0x00427A3C` ativo (produto), a chamada `0x00300544` só desvia para o
  desenho nativo na cena 3 (Game Over); a cena 7 e os builds de desenvolvimento seguem a rota do mod.
  Política em `TopScreenRendererVisibilityCallDefersToGameOverDraw`
  (`oot3d_top_screen_mod_profile_pause.cpp`); teste em `oot3d_top_screen_mod_profile_tests`
  (simula os quadros: antes ≤ 1 quadro composto, agora todo quadro a partir do 84º).
- **Teste no console** (`config/debug_force_game_over`, desligado sem o arquivo): zera a vida uma
  vez depois de N segundos de jogo normal (texto do arquivo = N; vazio = 10; `0`/`off` desliga),
  pela memória do SaveContext (`0x0058799C`); a morte, o Game Over e as escolhas são os nativos.
  Desktop: `TRIAEVUM_DEBUG_FORCE_GAME_OVER=10`. Carregue um save, espere, e confira: "GAME OVER",
  depois "Salvar?" Sim/Não centralizado, depois "Continuar?"; Não/Sim devem voltar ao jogo com 3
  corações ou ir para o título. Apague o arquivo depois (dispara uma vez por sessão).

### Polígono gigante na mira em 1ª pessoa: recorte no plano near (2026-10-17)
- Sintoma (desde a 1ª versão no Switch): na mira do estilingue, um polígono marrom enorme saía do
  canto superior esquerdo e cobria meia tela por um instante ("as texturas dão uma pulada e voltam").
  Continuava com `config/no_guest_overrides`, com `fps60_mode=legacy` e não dependia da interpolação.
- Causa: `GfxRenderingAPIOGL::Init` liga `GL_DEPTH_CLAMP` (para o renderizador legado) e nunca
  desliga; o caminho PICA desenhava com ele. Com o clamp o GL não recorta nos planos near/far: um
  triângulo com vértice entre o olho e o plano near (braço, manga ou estilingue colados na câmera em
  1ª pessoa; z do PICA > 0 com w > 0) era rasterizado inteiro, com profundidade presa no near, em vez
  de recortado como no PICA (e no Vulkan do PC, `depthClampEnable = false`; o dialeto GL
  `z_gl = -2z - w` já supõe o recorte).
- Correção: os draws PICA rodam com `GL_DEPTH_CLAMP` desligado (`PicaGlStateCache::DepthClamp`, uma
  troca por passagem PICA → legado, não por draw); o legado o recebe de volta em
  `RestoreLegacyVertexArray` (antes de todo draw legado) e no `ShutdownNativePica`.
- Testes: `gfx_opengl_pica_near_clip_tests` (contexto GL real via EGL surfaceless, pulado sem ele):
  shader pelo dialeto GL de verdade, triângulo da "manga" com um vértice a 0,2 do olho (near = 1);
  com o clamp ligado cobre 9295 px contra 4334 px do recorte do PICA (referência em CPU), e um
  triângulo inteiro dentro do near cobre 17280 px em vez de 0; com a sequência do backend, 4325 px e 0.
  `gfx_opengl_pica_state_cache_tests` (`PicaGlDepthClamp`): todo draw PICA sem clamp, todo legado com.

### Seleção de arquivo no widescreen: aba BACK, fade da transição (2026-10-17)
- Sintomas (prints do console, widescreen e `widescreen_menus` ligados): a aba "B BACK" sai do
  canto do menu (x=128 em vez de 160, de 1280); o cartão que entra deslizando aparece até x=1152; o
  fade preto ao escolher o save (e o escurecimento da escolha Normal/Master Quest) cobre só o
  retângulo 4:3 do menu e o céu fica nas laterais.
- Evidência: build Linux do produto com OpenGL no host (Xvfb/llvmpipe, 1280x720, TopScreen, escala
  2, `TRIAEVUM_GL_WIDESCREEN=1`) reproduz o print do console; o log de diagnóstico abaixo mostrou a
  classificação de cada draw.
- Causa 1 (BACK/cartões): os draws do menu de baixo realocado (viewport 0,40,480,320, ortográfica
  320x240, pixels quadrados) vêm atribuídos à camada de atmosfera (domínio Scene, camada 3: o menu
  roda dentro da fila de ambiente do `Gameplay_Draw` da tela de título), e a política de 111c86e
  deixava atmosfera esticada com a cena. O menu inteiro esticava 6,7% (viewport 40..360 -> x
  128..1152): os cartões só pareciam estar no quadro; a aba BACK (encostada no canto da tela de
  baixo) e o cartão deslizando, perto das bordas, denunciavam. Correção: no viewport realocado a
  camada de atmosfera não conta (nenhum efeito de ambiente usa Y 40..360); na tela inteira continua
  presa à cena.
- Causa 2 (fade): o preto é um quad do domínio Ui, sem textura, com a ortográfica 400x240 da tela
  de cima desenhada no viewport do menu; ia para o quadro e cobria só o 4:3 (x 160..1120), com a cena
  (o céu, que a composição do frontend mantém atrás do menu, `TRIAEVUM_TOPSCREEN_FRONTEND_SKY.md`)
  nos 16:9. Correção: draw no viewport realocado, sem textura, sem recorte do jogo e cuja geometria
  comprovadamente cobre o viewport nativo (prova de cobertura agora calculada também para ele; com
  frustum alargado a borda nativa fica em 1/fator, `PicaDrawGeometryNdcBounds`) cobre o alvo
  alargado inteiro. Texturas, botões, barras e os painéis do Game Over (domínio Ui, mesmo viewport)
  seguem no quadro.
- Draws em perspectiva compensados (viewport alargado em torno do centro) agora são recortados no
  seu viewport nativo: o viewport alargado só devolve as proporções, não mostra o que no 3DS fica
  fora da tela.
- **Cantos de baixo do menu nos cantos da tela** (pedido do usuário: o BACK no canto da tela 16:9,
  não no canto do 4:3 do menu): a aba BACK vem no MESMO draw que cartões, cabeçalho e textos (1104
  vértices), então o draw do menu realocado é emitido por recortes: o meio no lugar e cada canto de
  baixo (64 px x 40 linhas do menu nativo) deslocado pela margem até a borda do alvo alargado
  (`ResolveGlWidescreenCornerPasses`). O toque acompanha: um toque num canto de baixo da janela fora do
  menu vai para o canto equivalente do menu (`RemapGlWidescreenCornerPointer`, só com o frontend do
  TopScreen ativo no GL widescreen; o lugar antigo da aba continua tocando o BACK). Desliga com
  `config/widescreen_menu_corners` = `0` (desktop `TRIAEVUM_GL_WIDESCREEN_MENU_CORNERS=0`). Domínio Ui
  (Game Over), fades de tela inteira e modelos compensados não são divididos.
- O eixo apresentado (viewport/recorte: quadro, esticado, fade, compensação) saiu do backend para
  `ResolveGlWidescreenPresentedAxis` (`gl_widescreen_menu_policy.h`), testado no host
  (`gl_widescreen_menu_policy_tests`, 21/21, também com ASan/UBSan).
- **Diagnóstico no console** (`config/widescreen_menus_log`, desligado sem o arquivo; desktop
  `TRIAEVUM_GL_WIDESCREEN_MENUS_LOG=<arquivo>`): `logs/widescreen_menus.txt` com uma linha por draw
  distinto no alvo alargado: decisão (`QUADRO`, `QUADRO+COMP`, `ESTICA`, `TELA`), viewport, recorte,
  domínio/camada, projeção c0-c3, anisotropia, cobertura, textura. Apague o arquivo depois.
- O Link 3D, a caixa "...adventure through time itself!" e o logo atrás dos cartões e as linhas
  finas do cabeçalho: ver a seção seguinte.

### Menus realocados sobre o próprio fundo; emendas do atlas (2026-10-17)
- Sintomas (prints do console, MQ liberado): na seleção de arquivo o Link 3D da tela de cima e a
  caixa de texto dela aparecem atrás e através dos cartões; na escolha Normal/Master Quest o logo do
  título e o "© 1998-2011 Nintendo / Codeveloped..." aparecem entre os cartões e através do cartão do
  Master Quest (semitransparente); linhas finas claras verticais no cabeçalho "Please select a file."
  (e no da criação de nome), a ~28 px de cada borda.
- Reprodução no host: build Linux do produto na Ryzen (`/root/agentes/menus-visual`, Xvfb/llvmpipe,
  1280x720, TopScreen, escala 2, widescreen) com `TRIAEVUM_UNLOCK_MASTER_QUEST=1`: o caminho pela escolha
  de versões mostra exatamente os prints (sem ela o Link e a caixa não aparecem na seleção).
- Causa 1 (cena atrás do menu): a composição do frontend do port (`TRIAEVUM_TOPSCREEN_FRONTEND_SKY.md`)
  removia o fundo do próprio menu (CommonBackground00 da seleção/criação de nome, common_bg01 da escolha
  de versões) para mostrar a cena de cima atrás. No 3DS o menu e esse fundo (pedra marrom com ornamentos)
  formam a tela de baixo, que o mod leva inteira para a tela de cima (troca dos alvos 0x400/0x401); a
  cena de cima (logo, copyright, Link 3D e a caixa da seleção) não fica atrás do menu. Correção: o fundo
  do menu volta a ser desenhado; só o clear opaco do canvas do menu continua removido, então o céu da
  cena fica nas laterais do 4:3 no widescreen (como nas laterais do menu de 320 na tela de 400).
  `OmitTopScreenFrontendMenuDraw` (`oot3d_top_screen_frontend_composition.h`); `config/topscreen_menu_backdrop`
  = `0` volta à composição antiga (desktop `TRIAEVUM_TOPSCREEN_MENU_BACKDROP=0`).
- Causa 2 (linhas do cabeçalho): emenda das peças do atlas do menu com resolução interna N×. O menu é
  montado com peças desenhadas texel por pixel nativo; no 3DS cada pixel cai no centro de um texel e o
  linear devolve o texel exato, mas com 2× ou 3× os fragmentos da borda de uma peça ficam a 1/(2N) de
  texel do limite e misturam o texel vizinho do atlas. Prova: com escala 1 a linha some, com 2 e 3
  aparece; com nearest some. Correção: as peças da tela de baixo realocada (viewport que não cobre a
  largura apresentada, ortográfica de pixels quadrados, com textura; nos dois domínios, o que inclui a
  criação de nome e os painéis do Game Over) usam um sampler GL_NEAREST na ampliação quando a escala é
  inteira ≥ 2: reproduz o resultado nativo pixel a pixel e não lê fora da peça. Minificação continua
  com o filtro da textura; escala fracionária (1,5×) fica linear. HUD, Items, Map e System (largura
  toda) não mudam. `GlRelocatedMenuNearestSampling` (`gl_widescreen_menu_policy.h`), sampler em
  `gfx_opengl_pica.cpp`; `config/menu_nearest` = `0` desliga (desktop `TRIAEVUM_GL_MENU_NEAREST=0`).
- Resíduo conhecido: depois da escolha de versões, a caixa de texto 3D da cena de cima é mais larga que
  o 4:3 e uma tira dela (~25 px) aparece nas laterais, junto do céu, na parte de baixo da seleção.
- Testes: `gl_widescreen_menu_policy_tests` (22/22, ASan/UBSan) e
  `oot3d_top_screen_frontend_composition_tests` (compilar junto com `oot3d_native_pica_visual_frame.cpp`).
  Capturas no host conferidas: escolha de versões, seleção, arquivo 1, criação de nome, cópia, fade até
  entrar no jogo, aba BACK no canto.

## Próximos passos

- Ideias futuras (pedido do usuário, 2026-10-17):
  - "Grama leve" para o Switch: versão OpenGL simplificada da grama procedural do renderizador NRI do PC
    (menos folhas, só perto da câmera, sem interação cara), opcional via `config/grass`, pensada para o modo 30 FPS.
  - Outros efeitos do PC (toon shading, contornos, FOV maior) adaptados ao GL, se couberem na GPU.
  - Mods: testar pacotes de texturas HD da comunidade (formato Citra/Azahar) no sistema de substituição
    de texturas (`.o3tu`/PICA), depois da correção de memória de vídeo; substituição de arquivos de dados
    (modelos/textos) pelo runtime; mods de código exigem regenerar o AOT por mod.

### Frame generation: opções avaliadas (2026-10-22, só anotado, nada implementado)
1. **Eden no PC com LSFG.** O código já existe no Eden (`src/video_core/frame_gen` e
   `renderer_vulkan/present/lsfg_*`), mas só compila no Android (`ENABLE_LSFG` via
   `cmake_dependent_option "ANDROID"`), e a UI Qt esconde as opções (`IsAndroidOnly` em
   `src/yuzu/configuration/shared_widget.cpp`). Há um patch local, ainda não compilado nem testado
   (`~/projetos/eden-framegen-pc/framegen-pc.patch`, base em `BASE_COMMIT`). Ele liga o `ENABLE_LSFG` em
   todas as plataformas, mostra na UI frame_gen, multiplier, target_rate e flow_scale e adiciona as
   traduções em `src/qt_common/config/shared_translation.cpp`. Precisa do `Lossless.dll` do próprio
   usuário em `%APPDATA%\eden\lossless\`; o Eden só lê o arquivo, sem login.
   - Pendente na Ryzen: atualizar o Eden para o Nightly PGO v3, guardar o `Lossless.dll` em
     `D:\Ferramentas`, compilar com o patch e testar. A GPU é a Vega integrada, então é preciso medir o
     custo.
2. **Frame gen no port (Switch): não recomendado por agora.** O LSFG do Eden é Vulkan (shaders
   DXBC→SPIR-V), e o port é OpenGL/Mesa. A GPU do Switch já é o gargalo: a escala 3× travou e voltamos a
   2×. O caminho preferido é a interpolação nativa de 60 FPS que já está no roteiro
   (`native30_interpolated`). Uma versão simplificada só deve ser reavaliada se o custo medido no Eden
   do PC (Vega) for baixo. O repositório é público e o código do Eden é GPL-3: antes, checar a
   compatibilidade de licença e exigir que o usuário traga o próprio `Lossless.dll`.
3. **Frame gen direto no Switch (sysmodule): não existe nada conhecido.** A alternativa é o Switch Frame
   Tap, um homebrew que transmite por USB para o PC em 1080p60 (https://gbatemp.net/posts/10917720/),
   combinado com o Lossless Scaling no PC. Funciona com qualquer jogo, inclusive o port, mas acrescenta
   latência. A compatibilidade com AMS 1.11.1 / FW 22.1.0 ainda não foi verificada.

### Interpolação a 60 FPS mais barata e com pacing (2026-10-17)
- **Replay nativo pulado no GL** (`config/native_replay` = `1` volta ao antigo). Cada quadro de passo
  era desenhado duas vezes: no namespace nativo (1), que existe para devolver pixels à memória do guest
  (read-backs, savestate) e no visual (2), que é o apresentado. O backend GL nunca lê o namespace 1
  (texturas sempre vêm dos bytes do guest; sem read-back nem captura de savestate), então a cena
  inteira era desenhada à toa: metade da GPU a 30 FPS, um terço com `config/fps60`. O backend diz isso
  por `PicaNativeNamespaceObservable()` (Vulkan continua `true`). Única coisa que cruzava namespaces: o
  consumidor de sombra (Shadow2D) pegava a imagem do namespace 1 (o primeiro do mapa); agora prefere a
  do próprio namespace. A 30 FPS a imagem é a mesma; com fps60 a sombra deixa de adiantar um passo em
  relação à geometria (correção). `perf.txt`: `native_replay_skipped`.
- **Pacing "manter a velocidade do jogo"** (`config/interp_pacing`, ligado; `0` desenha todo
  intermediário como antes). Um governador na render thread
  (`oot3d_native_interpolation_pacing.h`) aprende o custo recente de cada tipo de quadro (máximo entre
  o tempo de GPU e o da render thread) e, para cada intermediário, escolhe o melhor nível cujo par
  (passo + intermediário) cabe em dois refreshes (33,3 ms): cheio, reduzido ou **descartado**
  (reapresenta a imagem do passo: só cópia já feita + RCAS, sem replay da cena). Começa no cheio e só
  desce com medição real: o nível atual precisa ter sido medido e o par passar de 100% do orçamento em
  3 decisões seguidas; sobe quando o par do nível melhor cabe em 97%. Sondas a cada 60 decisões abaixo
  do preferido. (1ª versão, bug no console: 95%/90%, sondas a cada 150 e, antes da calibração do relógio
  da GPU, custo = render thread + swap — o swap inclui a espera do vsync —; os primeiros segundos
  descartavam por custos inflados e um par de ~16+16 ms, que cabe, nunca voltava abaixo de 90%: preso
  em `dropped`, ~30 quadros reais apresentados de forma irregular. Sem relógio de GPU o custo agora é só
  o da render thread.) O custo de um nível não desenhado é estimado
  pelo custo do passo × a razão medida por último (os dois são um replay da mesma cena), então ele
  volta sozinho quando a cena fica leve; sondas (uma por vez, nível nunca medido ou a cada 150
  intermediários abaixo do preferido) refrescam a razão. Picos contam no máximo 2× a média
  (SwitchWakerHD `interp_pacing.h`). O quadro de passo nunca muda; a lógica nunca depende dos
  intermediários (o agendador continua juntando refreshes).
- **Resolução reduzida no intermediário** (`config/interp_scale`, ex.: `1.5` com escala 2; múltiplos
  de 0,25 abaixo da escala; ausente = desligado). O intermediário que não cabe cheio é desenhado num
  namespace próprio (3) com alvos na escala menor (`SetPicaNamespaceResolutionScale`; viewport,
  scissor, cópias e apresentação usam a escala do alvo) e o FSR EASU amplia como sempre.
  `config/interp_scale_mode` = `always` reduz todo intermediário. Só vale se a amostra limpa (memory
  fill) cada alvo antes do primeiro draw (cor; profundidade/stencil se usados), porque o namespace 3
  só vê alguns quadros; senão vai cheio (`reduced_refused`). Recusado com `antialiasing` ligado (as
  imagens do AA teriam de ser realocadas a cada quadro).
- Crash no console com `interp_scale=1.5` (2026-10-17, mallinfo achou o heap corrompido na 21ª janela):
  no perf.txt da sessão o nível reduzido nunca desenhou (`interp_reduced=0` em todas as janelas,
  `gl_targets=2`: nenhum alvo do namespace 3); só na última janela houve 11 decisões "reduzido", todas
  recusadas (`reduced_refused=11`) pela checagem de CPU. Essa checagem tinha vetores `static` de função;
  agora usa scratch da sessão (`oot3d_native_reduced_replay.h`) e
  `oot3d_native_reduced_replay_stress_tests` (worker gerando quadros reais, render thread com o
  governador alternando cheio/reduzido/descartado ~200 vezes em 6000 quadros, thread do guest lendo os
  contadores) passa limpo com ASan e TSan. `SetPicaNamespaceResolutionScale` não apaga mais imagens vivas
  (recusa se o namespace já tem alvos). `config/heap_check_windows` (1–1000, padrão 20) controla a
  frequência do mallinfo: `1` acha uma corrupção ~5 s depois de ela acontecer.
- **Tempo de GPU sem stall** (`oot3d_native_gpu_frame_timer.h`): `glQueryCounter(GL_TIMESTAMP)` no
  início e no fim de cada quadro, lidos só quando disponíveis (anel de 8). O relógio da GPU do
  switch-mesa não está em ns (SwitchWakerHD mediu 1,63 ns reais por "ns"): o fator é calibrado contra o
  relógio da CPU numa base de ≥ 3 s e nada é usado antes disso (`gpu_timer=0`; o governador usa o tempo
  da render thread + swap).
- **EASU reaproveitado** (parte do `gl_lean`): reapresentar a mesma imagem (intermediário descartado,
  apresentação sem quadro novo) pula o EASU e só roda o RCAS a partir da imagem ampliada guardada.
- Tela de baixo/HUD: no perfil TopScreen a tela de baixo já é recomposta na de cima e o HUD (N64 UI) é
  barato e pode mudar a cada refresh; nada a reaproveitar. Sombras: dependem das matrizes dos atores
  (interpoladas), não dá para reaproveitar no intermediário.
- `logs/perf.txt`, bloco `interp:` (por janela de 150 quadros): `pacing interp_scale level
  step_frames interp_frames interp_reduced interp_dropped other_frames reduced_refused repeat_fallbacks
  native_replay_skipped probes level_changes gpu_timer gpu_ns_per_tick`, `gpu_ms_<tipo>` e `rt_ms_<tipo>`
  (ms médios de GPU e da render thread por tipo: step/full/reduced/dropped/other) e as estimativas
  `est_step_ms est_full_ms est_reduced_ms pair_budget_ms`.
- Teste: `oot3d_native_interpolation_pacing_tests` (governador, latência de medição, histerese, picos,
  simulação de fila de GPU: com descarte os passos ficam no horário, sem descarte >100 atrasados).
- A/B no console: `native_replay` = `1`; `interp_pacing` = `0`; `interp_scale` = `1.5` (e
  `interp_scale_mode` = `always` para ver o custo/qualidade do reduzido em todo intermediário).

### Cadência do fps60: regularidade antes da média, `config/fps60_mode` (2026-10-17)
- **Diagnóstico** (`switch-logs/integ2b`, CPU 1785, 4,5 min a 60, 52 FPS de média): nos trechos
  pesados (host_frames 9450–14250) a thread do jogo gasta 27–31 ms de cada par de passo de 33,3 ms
  (`game_cpu` 80–95%) e a GPU ~16,5 ms por replay (`est_step`≈`est_full`≈16,5: par 33,0–33,2 de 33,3).
  Quadro a quadro, a janela 12150 (fps 54,6, 2,75 s) teve 165 refreshes em 150 quadros de host:
  ~15 quadros de host com **dois refreshes** (o jogo atrasou: o passo sai sem quadro intermediário) e,
  nos outros, passo + intermediário descartado (81 passos, 67 descartes, 1 intermediário). Isto é, ~30
  imagens novas/s com o "fps" contando as repetições, e ~5 passos por segundo segurando a imagem 3
  refreshes em vez de 2. Na janela 10650 (fps 45, 3,33 s): 200 refreshes em 150 quadros → 50 quadros
  de 2 refreshes (passo sozinho) + 50 pares (26 com intermediário, 24 descartados): imagens de 1 e de 2
  refreshes misturadas várias vezes por segundo. Mecanismo (reproduzido no simulador): um atraso põe
  dois refreshes num quadro de host — um passo inteiro (~23 ms) de uma vez — o que atrasa o seguinte;
  o laço cai num ritmo irregular de quadros de 1 e 2 refreshes e fica lá; ao mesmo tempo o governador
  por quadro alterna desenhar/descartar com o par de GPU no limite (33,2 > 97% de 33,3).
- **Política nova** (`oot3d_native_cadence_governor.h`, padrão `adaptive`):
  1. a 60, um refresh por quadro de host: um quadro atrasado carrega **um refresh de dívida** em vez de
     rodar dois (a imagem fica um refresh atrás; uma segunda dívida é descartada; travamentos de 2,5+
     refreshes — carregamento — recuperam como antes); o intermediário é desenhado (cheio/reduzido),
     nunca descartado um aqui outro ali;
  2. 3 apresentações de 2+ refreshes entre as últimas 60 imagens → **trava em 30**: o jogo roda os dois
     refreshes do passo num quadro de host (some o quadro intermediário e o custo dele na thread do
     jogo), o pacer espera 2 períodos e um passo atrasado reancora o relógio (hold 3,2,2… e não 3,1);
  3. fica em 30 pelo menos 2 s (dobra até 16 s se 60 falhar de novo em menos de 10 s) e só volta com
     folga comprovada: par da thread do jogo p90 ≤ 90% de 33,3 ms, par de GPU/render estimado ≤ 90% e
     nenhum passo atrasado nos últimos 2 s.
- **A/B** (`config/fps60_mode`, desktop `TRIAEVUM_FPS60_MODE`): `legacy` (padrão, ver abaixo), `adaptive`, `strict60` (todo
  intermediário desenhado, laço de antes; = `interp_pacing=0`, que sem o arquivo novo vira `strict60`),
  `legacy` (exatamente o comportamento anterior). Opções depois do modo: `adaptive misses=3 window=60
  hold=2 max_hold=16 relapse=10 game=0.9 gpu=0.9`.
- **perf.txt**, bloco novo `cadence:` (no fim da linha antes de `gl_textures_live`, sem mexer nos
  campos existentes): `fps60_mode lock present_1/2/3/4p` (apresentações N refreshes depois da anterior)
  `hold_1/2/3/4p` (refreshes que cada imagem nova ficou na tela) `repeats cadence_switches` (trocas de
  hold entre imagens seguidas) `late_steps superseded_frames` (passos nunca mostrados) `lock_changes
  lock30_ms hold_s game_pair_ms game_pair_p90_ms`. `perf_report.py` resume a cadência e mostra
  passo/interp/desc, hold e trava por janela em `--windows`.
- **Simulação no host** (`oot3d_native_cadence_sim.h`; `oot3d_native_cadence_governor_tests --report`):
  pipeline do console (jogo → render thread → GPU → pacer → swap no vblank) com os governadores de
  produção e custos do perf.txt. Swap bloqueante (como o console mede) / com fila de 1 quadro:
  | cenário | legacy: hold 1/2/3, trocas/s, judder | adaptive: hold 1/2/3, trocas/s, judder |
  |---|---|---|
  | pesado (jogo 21±3 ms, GPU 16,5) | 332/1624/6, 11,2/s, 8,6 ms | 1/1799/0, 0,03/s, 0,7 ms (30 cravado) |
  | m. pesado (24 ms, GPU 17) | 75/1661/66, 4,6/s, 4,9 ms | 0/1799/0, 0/s, 0,6 ms |
  | normal (18 ms, GPU 14) | 3408/95/0, 3,2/s, 2,9 ms | 3584/7/0, 0,23/s, 0,7 ms (60) |
  | sessão 220 s (como integ2b) | 5,2 trocas/s, judder 4,1 ms, 95 holds de 3 | 0,22/s, 0,9 ms, 0 holds de 3 |
  | sessão, swap com fila | 0,36/s, desvio 5,7 ms | 0,07/s, desvio 3,4 ms |
  (judder = RMS de intervalo mostrado − intervalo de conteúdo entre imagens seguidas; jogo a 60
  refreshes/s em todos). A média cai pouco ou nada; o que some são as trocas de cadência.
- Testes: `oot3d_native_cadence_governor_tests` (parser, planos de refresh, histograma de hold,
  trava/volta/recaída, estimativa do par do jogo, governador por quadro sem descarte, simulação; limpo
  com ASan/UBSan) e `triaevum_presentation_clock_tests` (`AdvancePlanned`, empréstimo fora do estado
  salvo, pacer de 2 períodos com reancoragem).
- O que olhar no console: `cadence_switches` e `hold_3` por janela (devem cair a ~0 nos trechos
  pesados), `lock`/`lock30_ms`, `game_pair_p90_ms` (perto de 30 = o jogo é o limite; abaixo de 28 com
  `lock=30` = a GPU é o limite) e `guest_fps` ~60 (velocidade do jogo).

- **Resultado no console (integ4, CPU 1785) e correção**: o 1º `adaptive` ficou travado em 30 83,7%
  do tempo (18 trocas), FPS 53,7 → 38,5, e mesmo travado não era regular (hold 1/2/3 =
  28/63/8,7%, 983 passos atrasados). A lógica não piorou (12,25 contra 11,9 ms/passo); o par do jogo
  (30,6 ms) ficava acima do limiar de volta (30) e nunca destravava, e o laço de 30 (dois refreshes por
  quadro de host) serializava a thread do jogo e a GPU. Simulando com os números da integ4, o 30 travado
  com o laço de 60 também não fica mais regular que o legacy, e a dívida a 60 (`PlanSixtyRefreshes`)
  atrasava o jogo (52 de 60 refreshes/s). Agora: **padrão `legacy`**; `adaptive` não usa mais o laço de
  30 nem a dívida (o agendador planeja como legacy em todo modo), não descarta intermediários um a um e
  só trava em 30 com sobrecarga real (par do jogo médio ou par de GPU > 1,15 × 33,3 ms, opção `lock=`);
  volta com par médio ≤ 92%. Na prática, hoje `adaptive` ≈ `strict60`. Os contadores `cadence:` ficam.
  Defeito "texturas pulam e voltam": não reproduzido; candidatos eram as 18 trocas de trava e os quadros
  de host com dois refreshes ou com zero (repetição) do laço de 30, que deixaram de existir.

### Núcleo 0 mais leve: `config/offload_*` (2026-10-17)
Diagnóstico (`switch-logs/integ5`, sessão 67, CPU 1785, fps60): a thread do jogo é o gargalo
(`game_cpu` 68/81/85% nos trechos a 57–60 / 45–57 / <45 fps; render 39%, planos 20%). Fora do
guest ela gastava por quadro de host `pica_submit` 1,62 ms (que **inclui** o guest retomado dentro
da drenagem: `guest_in_pica` ≈ 0,6 ms/quadro na sessão), `visual_present` 0,91, `dsp_mix` 0,50,
`pica_plan` 0,28 (dentro de `pica_submit`) e `frame_start` 0,25.

Onde ia o tempo: um plano PICA tem **15,7 KiB** (`sizeof(Oot3dPicaVulkanDrawPlan)`: uniforms do
fragment shader e do uber com a fog LUT, 6 KiB cada) e um pacote de draw 11 KiB. Com o plano adiado,
o *placeholder* de cada draw ainda era construído zerado duas vezes (a variável `plan` do laço e o
retorno de `MakeDeferredPlan`), copiado por atribuição e movido três vezes (`Capture` →
`Append` → `push_back`); fechar o quadro (`FinishFrames`, em `visual_present`) movia cada plano mais
uma vez. Mover um `std::array` é copiá-lo: ~6 cópias de 15,7 KiB por draw na thread do jogo. O
pacote era montado no frontend e copiado de novo na fila (2 × 11 KiB por draw, dentro do guest).

O que mudou (cada parte desligável, `0` no arquivo volta ao caminho antigo; sem arquivo = ligado
no Switch; desktop: `TRIAEVUM_OFFLOAD=1` ou `TRIAEVUM_OFFLOAD_<PARTE>=0/1`, desligado por padrão):
- **`config/offload_plan`**: o placeholder é escrito no lugar onde o acumulador o guarda
  (`CaptureInPlace` + `FillDeferredPlan`): uma construção por draw, nenhuma cópia. `FinishFrames`
  entrega o vetor pendente inteiro ao quadro quando um só quadro do jogo fecha (o caso comum) em vez
  de mover plano a plano; com vários quadros (host atrasado) segue o caminho antigo. Mesmos quadros,
  mesma ordem, mesmos planos resolvidos.
- **`config/offload_capture`** (dentro do guest, nos comandos GX): o pacote é construído direto na
  submissão da fila a partir do estado do frontend (`SubmitDrawPacketSource`, uma cópia de 11 KiB a
  menos); hashes de textura numa passada só (FNV-1a é sequencial: o hash do mip base é o estado depois
  dos seus bytes, e sem transformação o hash da origem é o do conteúdo — eram até 3 passadas); hash de
  geometria só quando um savestate pede (`CaptureState` calcula; o savestate é o mesmo documento);
  faixa mín/máx de índices lembrada com os bytes de índice em cache (mesmos bytes, mesma faixa).
- **`config/offload_perf`**: a linha do `perf.txt` é montada em memória e escrita por uma thread de
  serviço (`AsyncLineWriter`, prioridade 0x3B, núcleos 1|2): abrir/escrever no SD sai da thread do
  jogo. Fila de 32 linhas; cheia, descarta (nunca espera) e conta em `perf_dropped`.
- **`config/offload_actors`**: a varredura dos atores para os colisores da grama
  (`CollectNativeActorInteractions`, até 255 atores por categoria lidos da memória do guest + um
  `unordered_set` por quadro) só é feita quando o backend pode usá-la: o GL nunca lê a ponte da grama
  (só o Vulkan/NRI). No Switch a imagem não muda.
- Sem chave: o vetor de amostras do DSP de cada refresh é reaproveitado (sem `malloc` por refresh).

O que **não** saiu do núcleo 0, e por quê:
- `dsp_mix`: cada quadro do DSP lê a configuração que o guest escreveu na resposta ao quadro
  anterior e escreve status, buffers aux e `final_samples` na memória do guest antes do sinal; o
  guest fica parado esperando esse sinal. Não há o que sobrepor sem especular (ou mudar a ordem
  observável); a reamostragem/stretch já está na thread de áudio. Só o buffer foi reaproveitado.
- Decodificação do estado e cópias de vértice/índice/textura: precisam ser feitas no momento da
  submissão (o guest reescreve a memória logo depois); o cache por geração de escrita já evita as
  cópias repetidas.
- Entrada (SDL polling): mudaria em que quadro o jogo vê o botão.

`perf.txt`, bloco novo `offload:` (antes de `mesa:`, sem mexer nos campos existentes):
`offload_capture/plan/perf/actors` (modos), `pica_capture_ms` (tempo da thread do jogo dentro das
submissões de draw: parte do `guest`), `pica_capture_draws`, `capture_copy_kb`, `capture_hash_kb`,
`index_range_reuses` (por quadro de host), `pica_submit_host_ms` (`pica_submit` sem o guest
retomado dentro dele), `perf_line_ms` (custo da linha anterior na thread do jogo),
`perf_writer_ms`/`perf_dropped` (thread de serviço). Nada novo espera por lock: a fila do perf só
segura o mutex para empilhar/desempilhar. `perf_report.py` resume o bloco.

Prova de equivalência (`oot3d_native_offload_tests`, limpo também com ASan/UBSan e TSan): captura
antiga × enxuta (+ pacote da fonte) em milhares de draws aleatórios sobre memória sintética com
escritas do guest (com e sem gerações de escrita, reescritas com o mesmo valor, com transformação de
payload): submissões, snapshots, hashes, faixas de índice e o savestate (`CaptureState`, também
depois de restaurar) idênticos; frontend copiando × construindo pela fonte em listas de comandos
aleatórias (com spans de composição e light binding): pacotes idênticos; acumulador movendo ×
entregando o vetor com draws, fills, transfers, cópias raw, quadros superados, draws do quadro
seguinte e telas de continuação: quadros e pendências idênticos; placeholders movidos × no lugar,
com o worker de planos ligado e flushes de dependência: quadros idênticos antes e depois de
resolver; escritor do perf: todas as linhas, em ordem. Rodado na Ryzen (g++ 15.2, -O2, ASan/UBSan,
TSan): offload, plano adiado (unitário e soak), visual frame (soak e equivalência, também
`--threads`), vulkan plan, submission, frontend, command stream (inclusive o stress) e shutdown
passam. O soak do plano adiado no modo worker mede tráfego de heap/lotes vivos e falhou uma vez com a
máquina carregada por outro build; sozinho passou 4/4 com `TRIAEVUM_SOAK_MOVE_FREE=1` e 4/4 com `=0`
(caminho antigo), base e ASan. Os 8 TUs alterados compilam com as flags do Switch (devkitA64 16.1).

Ganho estimado na thread do jogo. `oot3d_native_offload_tests --bench` (Ryzen 5 5600G, -O2, 200
draws por quadro, mediana de 3): caminho antigo 2,08 µs/draw no placeholder (dos quais 0,68 µs no
trecho que o `pica_plan` mede) + 0,78 µs/draw no `FinishFrames` = 2,86 µs/draw; caminho novo
1,20 + 0,006 = 1,21 µs/draw. Escalando pelo `pica_plan` medido no console (0,28 ms/quadro ↔ 0,68
µs/draw): antes ~0,86 ms em `pica_submit` (cabe nos ~1,0 ms de `pica_submit` sem guest) + ~0,32 ms em
`visual_present`; depois ~0,49 ms. **Ganho estimado ≈ 0,7 ms por quadro de host** (~4% do orçamento
de 16,7 ms), mais a captura (uma cópia de 11 KiB por draw a menos e ~75% menos bytes com hash nos
testes; o custo real no console aparece em `pica_capture_ms`, que não era medido) e a escrita do
perf.txt (uma abertura+escrita no SD a cada 150 quadros, que podia travar a thread do jogo: ver o
congelamento analisado no watchdog). O `bench capture` não serve de estimativa: a memória sintética
com endereços aleatórios enche o cache de geometria (4096 entradas) e o custo fica dominado pela
expulsão LRU por varredura linear (`std::min_element`), que já existia e não muda aqui.

A/B no console: criar `config/offload_plan`, `config/offload_capture`, `config/offload_perf` ou
`config/offload_actors` com `0` (um de cada vez) e comparar `pica_submit`, `pica_plan`,
`visual_present`, `pica_submit_host_ms`, `pica_capture_ms` e `game_cpu` na mesma cena e clock.
Com `offload_perf` ligado, a linha do perf.txt é escrita inteira de uma vez (não fica mais cortada no
meio); para achar onde a thread do jogo parou, use o `logs/hang.txt` do watchdog.

### Estado em 2026-10-17 (melhor versão até agora)
- PGO (2ª coleta) + modo enxuto + planos PICA no núcleo 1 + render thread no núcleo 2 + áudio em thread própria.
- Medido a 30 FPS, escala 2×: 29,9 FPS médios, 93% do tempo em 30, lógica ~11 ms por passo.
- Áudio sem falhas audíveis (stretch só em queda real; buffer do audren 1024). 720p + 60 FPS (`config/fps60`) chega a ~96% de GPU.
- Pendentes: aliasing no código gerado e funções quentes nativas (agentes em andamento, exigem nova coleta PGO);
  outras formas de aliviar a GPU para 720p/60 (escala 2,5×, upscaler linear em 3×, anisotrópico menor);
  `config/audio_interpolation` (`sinc`/`cubic`) ainda não testado no console; liberar Master Quest.

- Master Quest sem zerar o jogo: arquivo `config/unlock_master_quest` (conteúdo `0` desliga; desktop:
  `TRIAEVUM_UNLOCK_MASTER_QUEST`). O desbloqueio é o byte 0 = `0xBE` do `savedata/system.dat`
  (34 bytes, CRC-16/ARC em +0x20); o host devolve esse byte ao jogo na leitura e nunca o grava no
  disco (`oot3d_native_master_quest_unlock.h`). Saves e system.dat ficam intactos; sem a opção tudo
  volta como era (os saves MQ, slots 3–5, continuam no cartão). Ainda não testado no console.

Medição de referência (2026-10-16, console, CPU 1785 MHz, render thread ligada): a lógica do jogo
custa ~17,5 ms por refresh (~35 ms por passo de jogo); o jogo roda a ~72% da velocidade nas cenas
pesadas. Meta: ≤ 33 ms por passo (velocidade real a 30 FPS), depois 60 FPS por interpolação.

Desempenho (prioridade):
1. Validar no console a recompilação com hot/cold split, LDM/STM por span e literais dobrados.
2. VFP nativo de ARM64 (opção do gerador, exato contra o oráculo de ~86 mi casos) — em andamento.
3. PGO com GCC (build instrumentado → jogar → .gcda por FTP → build final) — em preparação.
4. Decompilação parcial: profiler leve no Switch + funções quentes reescritas em C nativo com
   teste de equivalência (overrides) — em andamento.
5. "Modo enxuto": contar gerações de escrita só nas páginas relevantes à GPU/savestate, tirar
   trace/estatísticas do caminho quente.
6. Registradores do guest em locais: **avaliado e descartado** (2026-10-17). O `--local-state`
   antigo regrediu porque grava/recarrega os 15 GPRs em toda chamada e em cada uma das milhares
   de saídas (código 2× maior, módulo de 318 MB) e passa lambdas que capturam os locais para
   funções não inline (os locais escapam e moram na pilha). O novo `--promote-registers`
   (conjunto sujo exato por bloco, rótulos de saída compartilhados, orçamento local, nada escapa;
   exato pelo diferencial `run_register_promotion_equivalence.py`) também piora no A57:
   shard 243 com +36% de instruções, loads +77% e stores +89% (derrames na pilha 2,4×), porque
   com referências o próprio `state` é a casa de cada registrador e as chamadas frias dos
   caminhos lentos de memória obrigam os locais a callee-saved/pilha. Fica desligado.
7. glthread do Mesa (compilado; ligar atrás de config e medir).
8. Fastmem (reserva do espaço de endereços do 3DS, acesso sem checagem) — grande.
9. Longo prazo: deko3d no lugar de OpenGL/Mesa.

Imagem (só GPU, sem custo de CPU; cada item atrás de um arquivo em `config/`):
- Resolução interna 1×/2×/3× — feito (`config/resolution_scale`).
- FSR 1 (EASU+RCAS) como upscaler, FXAA/SMAA, filtro anisotrópico, nitidez — em andamento.
- Interpolação para 60 FPS (`native30_interpolated`) quando a lógica couber em 33 ms.
- Oclusão ambiente leve só se sobrar GPU no portátil.

Ferramentas e distribuição:
- ccache nos builds (instalado); usar um PC mais forte (Ryzen 5 5600G) como máquina de build.
- "Forge para Switch": script único que, a partir da ROM do usuário, extrai, adapta, gera o C++,
  compila com devkitPro e monta `switch/oot3dre/` (o `.nro` nunca é distribuído).
- A longo prazo: recompilador genérico 3DS→Switch juntando a descoberta de funções do
  fearkov/3dsrecomp com o runtime de Switch do TriAevum.

## Referências e créditos

- [coccofresco/TriAevum](https://github.com/coccofresco/TriAevum) — projeto original (GPL-3.0).
- TopScreen / Single Screen Experience — [GameBanana 695893](https://gamebanana.com/mods/695893).
- [centollOS/SwitchWaker](https://github.com/centollOS/SwitchWaker) e
  [SwitchWakerHD](https://github.com/centollOS/SwitchWakerHD) — patches de cache do Mesa,
  placement de threads e o estudo de desempenho no Switch que guiou várias mudanças.
- [fearkov/3dsrecomp](https://github.com/fearkov/3dsrecomp) — referência de técnicas de recompilação de 3DS.
- Zelda3drecomp, Azahar, Ship of Harkinian / libultraship, NVIDIA NRI, devkitPro/libnx — via o projeto original.
