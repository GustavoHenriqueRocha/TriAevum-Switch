# Backend deko3d — progresso e como retomar

Branch `deko3d` (worktree `~/projetos/TriAevum/.claude/worktrees/deko3d`, remote `gustavo`).
Não integrar em `topscreen-3.0` nem enviar ao Switch antes de validar no console.

## Objetivo

Trocar o OpenGL/Mesa (nouveau) da thread de render por **deko3d** (API nativa da GPU do
Switch), atrás de opção: `sdmc:/switch/oot3dre/config/renderer` com `deko3d` liga; arquivo
ausente ou `gl` mantém o GL de hoje (o fallback continua sempre compilado e funcionando).

Por quê (A/B tour26 × tour27, `config/early_frame_slot`): as travadas de lógica caíram 201→115,
mas as de render subiram 174→255. O gargalo é a thread de render (`pica_submit` 3–4 ms/quadro
no Mesa). Meta: nenhum quadro > 16,7 ms em jogo normal.

## Estado dos pacotes

| # | Pacote | Status |
|---|---|---|
| 1 | Mapa da interface PICA→GL e desenho da abstração | **feito** (este arquivo) |
| 2 | Init deko3d + swapchain + clear/apresentação atrás de `config/renderer` | **código pronto e compilado** (NRO `deko3d-p2`); falta o teste no Eden (Ryzen caiu durante ele) |
| 3 | Shaders: GLSL→DKSH (uam), cache em disco, pré-compilação offline | **feito e compilado** (NRO `deko3d-p3`): pacote `deko3d.pack` gerado (272 do cache GL + 4 bases uber, 0 falhas), `ShaderStore`, programa por draw (especializado > uber > pula), híbrido ligado |
| 4 | Anéis de vértice/uniform, texturas (upload, formatos), samplers | **parte 1 no código** (compila; sintaxe checada com o devkitA64): texturas RGBA8/LUT/samplers/descritores/despejo e streams por draw; falta: geometria persistente, ETC1→ETC2, Shadow2D, e medir no console |
| 5 | Estado de pipeline, alvos, display transfer, DRS, widescreen | **código pronto** (sintaxe e testes de host ok; build na Ryzen: ver abaixo); DRS e pós-processamento ficam para depois |
| 6 | UI TopScreen (UiRenderBackend) e ImGui | a fazer |
| 7 | Contadores no perf.txt equivalentes aos do GL (A/B) | a fazer |
| 8 | Estabilidade/memória | a fazer |

**Teste no console (24/10, Switch real, NRO p3 md5 `6e31a96d`, `deko3d.pack` em `shadercache/`,
`config/renderer`=deko3d):** tela preta como esperado; `logs/deko3d.txt`: `ready` (3 imagens de
3840 KiB, 2 quadros em voo), `276 precompiled stages in the pack`, quadros 1 e 60 apresentados
(acquire 48,8 ms somados em 60 quadros, 0 overflows); nenhum erro novo no log do jogo. Pacotes 2 e 3
validados no console.

**NRO p5 (24/10):** `~/oot3d-work/oot3d_native_game.deko3d-p5.nro`, md5
`411e4d9f6018d8838e77a530ff637a4f` (147 093 055 bytes), compilado na Ryzen do commit `66f9957e`
(11 passos, `-j 3`, `nice 10`, sem shard). Ainda **não testado no console**.

**Como testar o p5 no Switch:** `config/renderer` = `deko3d` e `shadercache/deko3d.pack` (o p3,
`~/oot3d-work/deko3d-p3.pack`). O que deve aparecer: a cena do jogo e os menus desenhados pela PICA
(tela de cima; o que o jogo põe na de baixo também, se apresentado), sem o HUD/UI TopScreen nem o
contador de FPS/ImGui (pacote 6), sem sombras projetadas (Shadow2D) e sem FXAA/SMAA/EASU. No
`logs/deko3d.txt`: `targets: ...` com os alvos criados, `pica: scanout shaders ready`, e a cada
1800 quadros `pica (frame): draws issued=...` (issued alto, `transfers`/`scanouts` > 0).
Diagnóstico rápido sem rebuild: geometria do avesso (paredes somem, vê-se o interior) →
`config/deko3d_cull_invert` com `1`; imagem de cabeça para baixo/espelhada → anotar (é o scanout:
`tools/oot3d/deko3d_scanout/scanout_vsh.glsl`); tela preta com `issued` > 0 e `scanouts` > 0 →
olhar `deko3d debug:` e `queue in error state` no log.

**Próximo passo:** testar o p5 no console; depois pacote 6 (UI TopScreen + ImGui no deko3d), DRS,
Shadow2D (programas com imagem no pacote) e medir o `pica_submit` contra o GL (pacote 7).

### Pacote 5 (o que entrou, 24/10, commit `66f9957e`)

- `gfx_deko3d_pica_pipeline.h` (puro, host): enums PICA → valores do deko3d (comparação, stencil,
  **operação lógica** — a ordem PICA difere —, blend, primitiva), plano de cull, chave/extensão de
  alvo (escala `config/resolution_scale(_docked)` como o GL, widescreen alarga só o alvo de cima
  480×400), viewport/scissor com as bordas do `PicaGlScale`, decodificação dos fills, plano do display
  transfer (mesmas contas e flip do GL, sem DRS) e posição do scanout na janela.
- `gfx_deko3d_pica_targets.{h,cpp}`: alvo = um memblock com cor RGBA8 (`UsageRender|Usage2DEngine`)
  + Z24S8 (`HwCompression`), limpo ao nascer (cor 0, depth 1, stencil 0) como o GL; imagem de display
  RGBA8 com slot de descritor próprio; imagem trocada de tamanho só é destruída 2 quadros depois.
- `gfx_deko3d_pica_draw.cpp`: `SubmitPicaDraw` (mesmas rejeições do GL; fills pendentes; produtor de
  Shadow2D pulado e contado; draw sem efeito pulado; shaders, UBO vértice 0 / fragmento 4, texturas
  1..3 + LUT 13, vértices com o constante (0,0,0,1) no slot 15, blend/logic op/máscara/depth/stencil/
  cull, sem depth clamp, viewport/scissor e posicionamento widescreen portado do GL sem os cantos),
  `SubmitPicaDisplayTransfer` (blit 2D com barreiras, cópia idêntica repetida pulada como o `gl_lean`),
  scanout (DKSH embutido, `tools/oot3d/deko3d_scanout/`; v invertido para a origem em cima; clear da
  janela fora do viewport; AlphaOverlay com blend), `ClearPicaRenderTarget`, `SubmitPicaMemoryFill`,
  `ResetPicaState`. Log novo em `logs/deko3d.txt`: `targets: ...` (32 primeiros), `pica (frame|shutdown):
  draws issued/skipped/shadow_producers/no_effect, transfers, scanouts, fills, clears, targets MiB`.
- **Bug do pacote 4 achado:** o atributo constante (0,0,0,1) nunca era alocado (`mConstantGpu`
  inválido) — todo atributo ausente leria endereço 0. Agora num bloco `CpuUncached` próprio (guarda).
- `oot3d_native_a32_window_setup.cpp`: a projeção widescreen do deko3d segue a regra do GL (antes
  alargava o frustum para 16:9 como o Vulkan).
- Decisões/fallbacks: **ETC1 → RGBA8 decodificado** (ETC2 direto fica para depois; o draw não
  precisa); **Shadow2D**: produtor pulado (nenhum programa do pacote usa imagem), consumidor lê
  0xFFFFFFFF (sem sombra); **DRS desligado**; **sem FXAA/SMAA/EASU/RCAS**; **sem os cantos do menu
  widescreen** e sem a amostragem nearest do menu realocado; UI TopScreen/ImGui = pacote 6.
- Chaves sem rebuild: `config/deko3d_cull_invert` (=1 inverte o sentido de face se a geometria sair
  do avesso), `config/gl_lean` (=0 recopia o display mesmo idêntico).
- Testes: `TestPipelineEnums`, `TestCullPlan`, `TestTargetsAndRects`, `TestFills`,
  `TestTransferAndScanout` + 6 guardas (83/83 ok).

### Pacote 3 (o que entrou, 24/10)

- `gfx_deko3d_programs.h` (puro): `PickPicaProgram` (especializado > uber > pula), contadores, e
  `PicaStageCache` (estágio resolvido uma vez por identidade da fonte canônica; ausência lembrada).
- `GfxRenderingAPIDeko3d` implementa `PicaProgramPreparationBackend` com
  `UsesAlwaysAvailablePrograms() = true` (o plano manda o programa paramétrico em todo draw);
  `PrepareNativePicaPrograms` só registra (nada a compilar). `SubmitPicaDraw` traduz para GL 4.3
  (mesma função do GL) só na primeira vez de cada texto e procura no pacote; log
  `programs (frame|shutdown)` a cada 1800 quadros.
- Pacote: `tools/oot3d/deko3d_shader_pack.py --src <shadercache> --glsl <bases> -o deko3d.pack`;
  bases uber com `oot3d_deko3d_parametric_glsl_dump <dir>` (alvo de host; sem o CMake, compila com
  g++ contra `liboot3d_native_pica_frontend.a` + `pica_gl_shader_dialect.cpp`). Gerado no notebook:
  `~/oot3d-work/deko3d-p3.pack` (md5 `66f481372245ab4a521694e2a7f500d6`, 1,4 MiB, 1 vertex + 275
  fragment). Chaves das bases: `a64d0c333b2643ec` (sem luz), `891e9717e2716c91` (luz),
  `e8e1dda8208db28e` (textura0 inteira), `5eff4c88329bdc4b` (luz + textura0 inteira).

### Pacote 4, parte 1 (o que entrou, 24/10)

- `gfx_deko3d_pica_plan.h` (puro, host): wrap/filtro/LOD como o GL (`ToPicaGlWrap`,
  `PicaGlMinFilter`, BASE_LEVEL = início da view, MAX_LEVEL limitado à cadeia), chave de textura
  igual à do GL (o estado do sampler faz parte), plano de mips RGBA8, slots de descritor, plano de
  vértices (todos os bindings num bloco do quadro, alinhados a 16; atributo ausente = buffer
  constante (0,0,0,1) no slot 15) e tamanho de UBO (múltiplo de 256, até 64 KiB).
- `gfx_deko3d_pica_resources.{h,cpp}`: arrays de descritores (4096 imagens, 512 samplers; imagem 0
  = branco 1×1, 1 = R32UI 0xFFFFFFFF), textura = um `DkMemBlock` próprio + `DkImage` RGBA8 com mips
  (pré-decodificadas pelo worker de planos quando houver, senão `DecodePicaTextureMips`) subidas
  por stream + `dkCmdBufCopyBufferToImage`; LUT de luz R32UI 256×24; despejo com a política do GL
  (`gfx_opengl_pica_residency.h`, orçamento `config/gl_texture_budget_mb`, 96 MiB) logo depois da
  espera do fence do slot; vértices/índices/uniforms por draw na memória de stream (uniform igual ao
  anterior no mesmo quadro é reaproveitado). Shadow2D → textura inteira de reserva (contado).
- Pendências do pacote 4: geometria persistente (`PicaGeometryRegistry`), ETC1→ETC2 direto
  (`config/etc2`), anisotropia, sub-alocador de imagens (hoje um memblock por textura: medir),
  stream que cresce e nunca encolhe (medir no console).

### Ryzen (24/10)

- Limpeza feita: a tarefa `EdenDeko3d` já não existia; no SD do Eden não havia `config\renderer`
  e o `.nro` era o original (mesmo md5 do `eden_deko3d_orig.nro`, que foi apagado).
- Build existente validado: NRO de `out/` com o md5 do p2 (`c8d4032d…`), 30 objetos do dia todos
  ELF válidos, `.ninja_log/.ninja_deps` íntegros.
- **Bug achado:** o `/root/agentes/deko3d/build.sh` da Ryzen ainda estava com `ninja -j12` e
  `nice -n 5` e sem a recusa com o Eden aberto (a cópia do repo já tinha `-j4`/`nice 10`).
  Substituído pela cópia do repo (`tools/oot3d/deko3d_ryzen_build.sh`, md5 `636b00a1…`). Conferir o
  md5 dos dois antes de cada build.
- Build do p3 (17 passos, só runtime, sem shard) ok: NRO md5 `6e31a96d79e229181cf7f4fd43c3edc4`.
- **NRO p3 ainda não está no notebook:** ficou em `/root/agentes/deko3d/out/oot3d_native_game.nro`
  e em `C:\Users\flavin\deko3d-p3.nro` (md5 `6e31a96d…`, 147 015 231 bytes). A cópia parcial está
  em `~/oot3d-work/oot3d_native_game.deko3d-p3.nro.parcial` (não usar). Quando a Ryzen voltar:
  `scp -O ryzen:deko3d-p3.nro ~/oot3d-work/oot3d_native_game.deko3d-p3.nro` e conferir o md5. O p3
  foi compilado no commit `3b12e0e1` (pacote 3, sem o pacote 4 parte 1).
- A Ryzen parou de responder (~13:51) no meio do `scp` do NRO e não voltou em 30 min (Windows OpenSSH: `scp` padrão dá
  "Connection closed"; `scp -O` funcionou até cair). Sem build nem Eden rodando nessa hora.
- **24/10 (p5):** `build.sh` agora com `ninja -j 3` e recusa se já houver um `ninja` rodando (md5
  `ac83157e…`, igual à cópia do repo). Fonte sincronizada por tar dos arquivos alterados desde `3b12e0e1`
  (inclui o pacote 4 parte 1, que nunca tinha ido) com `--touch`; scripts curtos via `scp -O` +
  `wsl -d Ubuntu -u root -- bash /mnt/c/Users/flavin/x.sh` (variáveis `$A` se perdem no `ssh '…'`).
- Regras: nunca build e Eden juntos; `-j 3` com `nice 10`; `.wslconfig` com `memory=6GB`; nada
  > 5 GB; Eden só teste curto e nunca por agente sem pedido.

## Mapa do que o GL faz hoje (pacote 1)

Quem chama o backend (`GfxRenderingAPI`, criado em `Fast3dWindow::InitWindowManager`, caso
`FAST3D_SDL_OPENGL`):

- **Thread de render** (`NativeRenderCommandStream`): `beginRendererFrame` (frame.cpp) faz
  `UpdateFramebufferParameters(0,…)`, `StartFrame`, `HasActiveFrame`, `StartDrawToFramebuffer(0)`,
  `SetClearColor`, `ClearFramebuffer`, `SetViewport/SetScissor`; depois o plano PICA chama
  `PublishPicaCompositionSequence`, `SubmitPicaDraw`, `SubmitPicaMemoryFill`,
  `ClearPicaRenderTarget`, `SubmitPicaDisplayTransfer` (com `Present` desenha o scanout na
  janela), `QueuePicaCompletion/TakePicaCompletions`; a UI TopScreen (`N64UiFast3dRenderer` →
  `Fast3dUiRenderBackend`) usa `NewTexture/SelectTexture/UploadTexture/SetSamplerParameters/
  DeleteTexture`, `LookupShader/CreateAndLoadNewShader/LoadShader` (só 2 programas: sólido e
  texturizado), `SetOot3dNativeTransform`, `DrawTriangles`; ImGui (`Fast3dGui`) desenha por
  último; `window.EndFrame()` → `Interpreter::EndFrame` → `rapi->EndFrame`,
  `wapi->SwapBuffersBegin` (SDL_GL_SwapWindow), `rapi->FinishRender`.
- **GL fora do backend:** `gfx_sdl2.cpp` (contexto/EGL/swap), `Fast3dGui.cpp` (ImGui OpenGL3),
  `oot3d_native_a32_window_setup.cpp` (contexto movido para a thread de render, glthread.txt),
  `oot3d_native_gpu_frame_timer.h` (glQueryCounter, só com `config/gpu_timing`),
  `oot3d_native_a32_window_frame.cpp` (glFlush só com `config/gl_pacing_flush`),
  `FlushDeferredWindowClear` (gancho do acquire → relógio de vsync reserva / timer de GPU).

Backend PICA GL (`gfx_opengl_pica.cpp`, 4,7 mil linhas) — o que precisa de equivalente:

| GL | O que é | deko3d |
|---|---|---|
| `PicaGlTarget` (cor RGBA8 + DS24S8 + imagem Shadow2D R32UI) por (namespace, end. cor, end. depth, tamanho, formatos) | alvos PICA na escala interna (`resolution_scale`), widescreen alarga o eixo Y do alvo de cima | `DkImage` cor + depth em heap de imagens; Shadow2D adiado (nenhum dos 271 programas do cache usa imagem) |
| `PicaGlDisplay` + `glBlitFramebuffer` | display transfer (cópia com filtro 2:1) | `dkCmdBufBlitImage` (motor 2D, filtro linear/nearest) |
| scanout + pós (FXAA/SMAA/EASU/RCAS) | desenha a imagem de display na janela (retrato→paisagem) | shader próprio de scanout (pós-processamento fica para depois) |
| programas (`GetPicaGlProgram`) | fonte GLSL 4.5 canônica → `TranslatePicaShaderToOpenGl43` → Mesa | mesma tradução → DKSH (uam); cache por hash da fonte de cada estágio |
| aneis `UniformRing/VertexRing/IndexRing` + `PicaGeometryRegistry` (geometria persistente) | upload por draw | anel por quadro em memória CPU-visível + buffers persistentes em heap |
| texturas (`GetPicaGlTexture`, pré-decodificadas pelo worker de planos, chave por hash de conteúdo) + LUT de luz (R32UI 256×24) | | `DkImage` RGBA8 (upload por staging + `dkCmdBufCopyBufferToImage`), descritores em pool |
| estado (blend, logic op, depth/stencil, cull, viewport/scissor, máscara de cor) com cache | | `DkColorState/DkBlendState/DkDepthStencilState/DkRasterizerState/DkColorWriteState`, viewports/scissors dinâmicos |
| DRS (`gl_pica_dynamic_scale.h`): sub-retângulo do alvo de cima | | mesma lógica (só aritmética; reaproveitar os helpers `PicaGlScale`) |
| widescreen (`gl_widescreen_*_policy.h`) | posicionamento por draw | mesma política (arquivos já sem GL) |
| fills (`SubmitPicaMemoryFill`), clear de alvo | | `dkCmdBufClearColor/ClearDepthStencil` com scissor do alvo inteiro |
| interpolação fps60 (`interp_exact_step`, `interp_carry_unmatched`) | acontece **antes** do backend (plano visual) | nada muda: o backend só recebe draws |

Programas vistos no cache do pacote (`pacote-mercado/.../shadercache/*.src`, 271 arquivos):
**1 vertex shader** (31 KB, o programa de vértice PICA do jogo) e **271 fragment shaders**
especializados (~5 KB). Ligações: UBO de vértice `binding=0`, UBO de fragmento `binding=4`,
`sampler2D` 1..3, `usampler2D pica_lighting_lut` 13. Usam `gl_FragCoord`, `textureSize`, 46 escrevem
`gl_FragDepth`, 41 têm `discard`. Nenhum usa imagem (Shadow2D).

## Decisões

- **Seleção em tempo de execução, mesmo NRO.** `WindowBackend` continua `FAST3D_SDL_OPENGL`
  (nada muda no resto do Ship); em `Fast3dWindow::InitWindowManager`, no Switch com
  `config/renderer` = `deko3d`, cria `GfxRenderingAPIDeko3d`. Sem o arquivo: GL de hoje.
- **Sem janela SDL no modo deko3d.** O SDL do Switch cria sempre uma superfície EGL na
  `nwindowGetDefault()` (`SWITCH_CreateWindow`), o que conflita com o swapchain do deko3d.
  No modo deko3d o `GfxWindowBackendSDL2` inicializa o vídeo do SDL (eventos, toque, applet)
  mas não cria janela nem contexto; tamanho da janela = 1280×720 (portátil) ou 1920×1080
  (dock na abertura), como hoje.
- **Convenções de coordenadas:** device deko3d com `DepthMinusOneToOne | OriginUpperLeft`. Para
  alvos fora da tela isso dá a mesma memória que o GL (linha 0 = y 0, NDC −1 → linha 0), então
  as contas de viewport/scissor/blit do GL valem 1:1 e os shaders são **os mesmos textos GL 4.3**
  que o Mesa compila (mesmo `z_gl = 2z−w`). Só o desenho na imagem do swapchain (linha 0 = topo
  da tela) inverte Y, num lugar só (scanout/UI/ImGui).
- **Shaders sem travada:** o backend implementa `PicaProgramPreparationBackend` com
  `UsesAlwaysAvailablePrograms() = true`: todo draw traz também o programa paramétrico (4 bases).
  Desenha com o especializado se o DKSH dele está no cache; senão com o paramétrico, e registra a
  fonte que faltou no SD para compilar depois (offline com o `uam` do devkitPro na Ryzen; no
  console com uam como biblioteca num worker, pacote 3b). Nunca compila na thread de render.
- **Atributos de vértice ausentes** leem (0,0,0,1) como no GL: buffer constante por instância.
- **Shadow2D (imagem R32UI)**: fora da primeira versão; draw produtor de sombra é pulado e
  contado (`deko_skipped_shadow`).
- **Sem ImGui na primeira versão** (contador de FPS, menu L3+R3 desligados no modo deko3d).

## Build na Ryzen (como retomar)

- Diretório próprio: `/root/agentes/deko3d` no WSL (`ssh ryzen`, `wsl -d Ubuntu -u root`).
- Para não recompilar os 256 shards: cópia do `build-switch` do notebook (estado do pisca,
  `62a6ac47`) em `/root/agentes/deko3d/build`, fonte em `/root/agentes/deko3d/src`, e o build roda
  num namespace de montagem (`unshare -m`) com bind de `src` em `/home/gustavo/projetos/TriAevum`
  e de `build` em `/home/gustavo/oot3d-work/build-switch` (mesmos caminhos ⇒ mesmas linhas de
  comando ⇒ só o runtime recompila). Os arquivos da fonte mantêm o mtime do checkout principal
  (só os alterados ficam novos).
- O `gen/cpp-lean` também vai do notebook (`/root/agentes/deko3d/gen`, bind em
  `/home/gustavo/oot3d-work/gen/cpp-lean`): o da Ryzen difere e fazia os shards recompilarem.
  `ninja -n` não serve para contar passos (a checagem de globs sempre "reroda o CMake"); o
  script vigia o log e mata o ninja se um shard começar. Alvo: `oot3d_native_game_nro`.
- Cópia do script no repo: `tools/oot3d/deko3d_ryzen_build.sh`. Sincronizar fonte: tar dos
  arquivos que diferem do `topscreen-3.0`, extraído com `--touch`.
- Antes de mandar à Ryzen, checar a sintaxe no notebook (leve, um arquivo por vez): pegar a linha
  do `gfx_opengl.cpp` em `ninja -t commands` do `~/oot3d-work/build-switch`, trocar o caminho do
  repo pelo do worktree, `-O3`→`-O0`, tirar `-w` e o `-MD … -o …`, e rodar com `-fsyntax-only` no
  arquivo deko3d (o `$(…)` precisa de `bash -c` por causa das aspas dos `-D`).
- Copiar o NRO: `wsl … cp out/oot3d_native_game.nro /mnt/c/Users/flavin/x.nro` e `scp -O ryzen:x.nro`
  (o `scp` padrão contra o OpenSSH do Windows fecha a conexão).
- Script `/root/agentes/deko3d/build.sh` grava `/root/agentes/deko3d/status.txt` e o NRO em
  `/root/agentes/deko3d/out/`; aborta se um shard começar a recompilar.

## Pacote 2 (o que entrou)

- `gfx_deko3d.h` (seleção pura `Deko3dRendererConfigSelects` + `Deko3dRendererSelected`),
  `gfx_deko3d_device.{h,cpp}` (device `DepthMinusOneToOne|OriginUpperLeft`, fila com zcull
  desligado, swapchain de 3 imagens RGBA8 1280×720/1920×1080, 2 slots por quadro com fence,
  memória de comando em fatias de 1 MiB + blocos extras, memória de stream por slot,
  acquire adiado ao primeiro uso da janela com o mesmo gancho do GL, log em
  `logs/deko3d.txt`), `gfx_deko3d.cpp`/`gfx_deko3d_api.h` (GfxRenderingAPI: clear da janela
  adiado, present no `FinishRender` depois do pacing do SDL; PICA/UI aceitos e contados, nada
  desenhado ainda; completions repassadas como no GL).
- Ganchos: `Fast3dWindow` (cria o deko3d se selecionado; senão GL), `gfx_sdl2` (sem janela SDL),
  `Fast3dGui` (sem backends ImGui), setup (não move contexto GL), frame (sem `glFlush`),
  `gl_frame_profiler` (desligado). `libdeko3d` ligado no CMake do Switch.
- Testes: `oot3d_deko3d_backend_tests` (seleção, chave e índice do pacote de shaders),
  guardas novas em `test_bug_guards.py`, `tests/switch/eden_deko3d.sh` (fumaça com
  `config/renderer` só durante o teste; print da tela).

## Créditos e licenças

Referência de desenho: renderizador deko3d do Aurora do "Alek's Ultimate NX Edition" (MIT) e o
SwitchWaker (centollOS, MIT), cuja camada de plataforma vem do SwitchWakerHD (MPL-2.0). Nenhum
código foi copiado até aqui; se for, entra com o cabeçalho e a licença originais.
deko3d e uam: devkitPro (fincs), zlib.
