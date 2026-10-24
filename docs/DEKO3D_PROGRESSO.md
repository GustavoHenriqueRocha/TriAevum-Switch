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
| 2 | Init deko3d + swapchain + clear/apresentação atrás de `config/renderer` | a fazer |
| 3 | Shaders: GLSL→DKSH (uam), cache em disco, pré-compilação offline | a fazer |
| 4 | Anéis de vértice/uniform, texturas (upload, formatos), samplers | a fazer |
| 5 | Estado de pipeline, alvos, display transfer, DRS, widescreen | a fazer |
| 6 | UI TopScreen (UiRenderBackend) e ImGui | a fazer |
| 7 | Contadores no perf.txt equivalentes aos do GL (A/B) | a fazer |
| 8 | Estabilidade/memória | a fazer |

**Próximo passo:** pacote 2.

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
- Script `/root/agentes/deko3d/build.sh` grava `/root/agentes/deko3d/status.txt` e o NRO em
  `/root/agentes/deko3d/out/`; aborta se um shard começar a recompilar.

## Créditos e licenças

Referência de desenho: renderizador deko3d do Aurora do "Alek's Ultimate NX Edition" (MIT) e o
SwitchWaker (centollOS, MIT), cuja camada de plataforma vem do SwitchWakerHD (MPL-2.0). Nenhum
código foi copiado até aqui; se for, entra com o cabeçalho e a licença originais.
deko3d e uam: devkitPro (fincs), zlib.
