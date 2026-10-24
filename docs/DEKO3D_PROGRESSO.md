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
| 3 | Shaders: GLSL→DKSH (uam), cache em disco, pré-compilação offline | em andamento: formato do pacote + ferramenta + testes; falta o carregador no console |
| 4 | Anéis de vértice/uniform, texturas (upload, formatos), samplers | a fazer |
| 5 | Estado de pipeline, alvos, display transfer, DRS, widescreen | a fazer |
| 6 | UI TopScreen (UiRenderBackend) e ImGui | a fazer |
| 7 | Contadores no perf.txt equivalentes aos do GL (A/B) | a fazer |
| 8 | Estabilidade/memória | a fazer |

**Próximo passo:** quando houver máquina liberada, rodar `tests/switch/eden_deko3d.sh` com o
NRO `~/oot3d-work/oot3d_native_game.deko3d-p2.nro` (md5 `c8d4032d274832e648eca615772affaa`) e
conferir `logs/deko3d.txt` (`deko3d: ready`, quadros apresentados). Em paralelo (só código):
pacote 3 — `gfx_deko3d_shaders.cpp` (carrega `shadercache/deko3d.pack`, DKSH inteiro na memória de
código como no Aurora, `control = NULL`; programa por par de identidades; uber quando falta;
registra o texto que faltou em `logs/deko3d_shaders/`).

### Pendências na Ryzen (24/10, ela reiniciou duas vezes)

- 1º crash: rodava só o Eden (tarefa agendada `EdenDeko3d`, teste do NRO p2, ~12:37); o build
  tinha terminado às 12:33. 2º crash: logo após voltar; do meu lado só havia um `ssh … echo ok`
  a cada 20 s esperando ela voltar.
- Limpar ao voltar: apagar a tarefa `EdenDeko3d` (`/SC ONCE /ST 23:59`: rodaria às 23:59!);
  no SD do Eden apagar `config\renderer` e voltar o `.nro` original de
  `%USERPROFILE%\eden_deko3d_orig.nro` (o script faz isso no fim, mas não chegou ao fim).
- Regras novas: nunca build e Eden juntos (o `build.sh` recusa com o Eden aberto e o
  `eden_deko3d.sh` recusa com `ninja` rodando); `-j 4` com `nice 10`; `.wslconfig` com
  `memory=6GB` (+ `wsl --shutdown`) antes do próximo build; nada > 5 GB; Eden só teste curto.
- Validar `/root/agentes/deko3d/build` (o build terminou antes do crash, mas conferir o md5 do
  NRO de `out/` e, na dúvida, recompilar o runtime).

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
