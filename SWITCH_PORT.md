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
