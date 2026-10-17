# Ocarina of Time 3D no Nintendo Switch — notas do port

Este repositório é um **fork** do [TriAevum / Echoes of Time](https://github.com/coccofresco/TriAevum),
de **coccofresco**: a recompilação AOT nativa de *The Legend of Zelda: Ocarina of Time 3D*. Todo o
trabalho de recompilação, runtime, renderizador e ferramentas é do projeto original e dos projetos
que ele credita (veja [README.md](README.md) e [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).
Aqui estão as mudanças feitas para o **homebrew de Switch** (`.nro`) rodar e ficar jogável num
console real, no branch `switch-port`.

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
- **Threads**: jogo sozinho no core 0; demais threads nos cores 1/2 com a prioridade do jogo e pilha
  de 4 MiB (`oot3d_switch_threads.cpp`, `--wrap=pthread_create`).
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
- Núcleos fixos (`SetSwitchThreadCore`, `oot3d_switch_threads.h`; vale só se a máscara do processo
  permitir): render thread no núcleo 2; worker de planos PICA e thread de áudio no núcleo 1. Antes a
  ordem de criação punha a render thread e o worker no mesmo núcleo, com o outro quase ocioso.

## Próximos passos

- Opção para liberar o Master Quest sem zerar o jogo (flag de desbloqueio no save; `config/unlock_master_quest`, testar numa cópia do save).

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
6. `--local-state` (registradores do guest em locais) depois do inline.
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
