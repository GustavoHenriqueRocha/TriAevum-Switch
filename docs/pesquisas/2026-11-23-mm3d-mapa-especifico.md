# MM3D: o que é específico do OoT3D no produto Switch e o que o upstream já tem

Data: 2026-11-23 · Título(s): OoT3D (produto Switch/deko3d), MM3D EUR Rev 1 0004000000125600 ·
Base: HEAD 1c22eabd (worktree deko3d) × origin/main 672d13f1 (coccofresco/TriAevum)

Estados: **confirmado** (conferido no código ou medido), **parcial** (parte conferida),
**sem suporte** (hipótese não testada).

## Pergunta

Para levar o Majora's Mask 3D (mesma engine da Grezzo) ao nosso produto Switch/deko3d:
(1) que peças do produto são específicas do OoT3D, onde estão e se dependem de endereço do code.bin;
(2) para cada uma, se o upstream (`origin/main:titles/mm3d`, projeto "Moonfall Curse") já tem o
equivalente, se ele é neutro ao renderer ou preso ao NRI/Vulkan, e o que falta escrever para o
Switch/deko3d.

O plano começou com um casamento de funções OoT3D→MM3D para transferir endereços. No meio da
tarefa o upstream mostrou ter o MM3D inteiro para a nossa ROM, e o casamento deixou de ser
necessário para o inventário. O resultado dele fica registrado abaixo (seção "Casamento de
funções") porque mede o que dá para transferir sem ajuda e valida o upstream.

## Fontes

- Nosso fork: `tools/oot3d/native_game_runtime/ctr_title_profile.h:92-101,133-138`,
  `tools/oot3d/native_a32_runtime/guest_overrides.json:5-28`,
  `tools/oot3d/native_game_runtime/oot3d_native_a32_window_shared.h:158-159`,
  `tools/oot3d/native_game_runtime/ctr_frame_boundary.h:51-54`,
  `tools/oot3d/native_a32_runtime/whole_aot_cpp.py:40-49` (lidos por mim). O restante das linhas
  do nosso lado veio de um relatório de agente só leitura no mesmo HEAD; conferi os itens acima e
  marquei os outros como parcial.
- Upstream (lido com `git show origin/main:<caminho>`, sem checkout):
  `docs/MOONFALL_CURSE.md` (785 linhas, 5819261b), `titles/mm3d/forge/supported_revisions.json`
  (ad0dd389), `tools/oot3d/native_game_runtime/triaevum_title_adapter.h` (33abd8ef),
  `tools/oot3d/native_game_runtime/triaevum_title_session.h` (8715d787),
  `titles/mm3d/adapter/mm3d_title_session.cpp` (3a80c2e7), `mm3d_title_contracts.h` (777ed1d8),
  `mm3d_timing_observer.h` (a1d2c7ef), `mm3d_frame_pacing.h/.md` (b8e9006c/4648c4bd),
  `mm3d_single_screen.h` (83274dc1), `mm3d_presentation_settings.h` (e0c5b8a6),
  `mm3d_a32_substitute.h` e `restoration/` (56747e4e, ec3a6ea8), `dekui/` (6ed69876),
  `shaders/` (141b91ae), `audio/NATIVE_AUDIO.md` (3e441baf), `a32/whole_aot_inventory.csv`
  (f754663d), `a32/thumb_ranges.csv` (757f90ae), `releases/0.5.0-alpha.1.md`.
- Divergência: `git merge-base HEAD origin/main` = 23eac6b0 (2026-10-15); desde lá, 350 commits
  nossos e 896 do upstream. A costura de título do upstream nasceu em 56747e4e (23/10) e
  10f00b83 (24/10), depois da nossa base.
- code.bin MM3D extraído por mim da ROM completa (1 GiB) em `~/oot3d-work/conv/mm3d-analise/`:
  5967872 bytes, sha256 6eec27b1…, igual ao da receita `mm3d-eur-local-6eec27b1` do upstream
  e ao `~/oot3d-work/conv/mm3d/extract/code.bin`.
- Casamento: `tools/ctr/casar_funcoes.py` e `tools/ctr/test_casar_funcoes.py` (10 testes, dado
  sintético). Saídas em `~/oot3d-work/conv/mm3d-analise/` (`pares-simetrico.csv`,
  `alvos-ovr-simetrico.csv`, `alvos-oot3d-simetrico.csv`). Para reproduzir:
  `python3 -I tools/ctr/casar_funcoes.py --origem-code ~/oot3d-work/code-eur-canonical.bin
  --origem-exheader ~/oot3d-work/extract/ncch0/exheader.bin --origem-inventario
  ~/oot3d-work/conv/mm3d-analise/oot3d-descoberta/inventory.csv --destino-code
  ~/oot3d-work/conv/mm3d-analise/code.bin --destino-exheader ~/oot3d-work/conv/mm3d-analise/exheader.bin
  --destino-inventario ~/oot3d-work/conv/mm3d/extract/inventory.csv --alvos ~/oot3d-work/conv/mm3d-analise/alvos-ovr.csv`

## Conclusão

### Fatos de base

- **Confirmado:** o upstream já porta o MM3D EUR v1.1 para a nossa ROM exata: a receita
  `supported_revisions.json` fixa o code.bin sha256 6eec27b1…, o mesmo do nosso dump. Ele é um
  produto desktop (Windows/Linux, release 0.5.0-alpha.1) sobre o renderer NRI; não há deko3d nem
  build Switch do MM3D lá.
- **Confirmado:** o upstream escolhe o título pelo **sha256 do code.bin**
  (`triaevum_title_adapter.h`, `ResolveTitleAdapter`: `oot3d_eur`, `mm3d` ou `generic`). Nós
  escolhemos pelo **program id** do ExHeader (`ctr_title_profile.h:133-138`). Nenhum arquivo da
  costura de título do upstream existe no nosso HEAD (`triaevum_title_adapter.h`,
  `triaevum_title_session.h`, `native_button_overlay.h`, `fast/extension/*`: 0 de 238 arquivos).
- **Confirmado:** o nosso conversor não gera o MM3D hoje. `ctr_convert.py` parou no `generate`
  com `LoweringError: instruction 0xFA06D22F at 0x0014EBC0 is unsupported` (um BLX para Thumb;
  `~/oot3d-work/conv/mm3d/convert.log`). O MM3D tem 1680 BLX imediatos para 122 destinos Thumb
  (o OoT3D tem 3). O upstream resolveu com decodificação e execução Thumb (85503360, 757f90ae,
  716b0ba6) e com `titles/mm3d/a32/thumb_ranges.csv` (223 funções Thumb, 21,3 KB).

### Peça por peça

| Peça do OoT3D (nosso fork) | Depende de endereço? | Equivalente no upstream | Neutro ou NRI? | O que falta para Switch/deko3d |
|---|---|---|---|---|
| Perfil de título `ctr_title_profile.h:36-138` (OoT3D por program id 0x33400/500/600; o resto cai em `GenericTitleProfile`) | não (interruptor) | `TitleAdapter::Mm3d` em `triaevum_title_adapter.h` (33abd8ef) + `TitleSession` (`triaevum_title_session.h`, 8715d787; 280 linhas, ~40 métodos virtuais) implementado em `titles/mm3d/adapter/mm3d_title_session.cpp` (697 linhas) | a interface é neutra; vários métodos devolvem tipos do caminho de extensão (`Fast::Extension::GrassInteractor`, packs de shader) | trazer o `TitleSession` para o nosso `window_setup` ou criar `CtrTitleKind::Mm3d` que chame os mesmos pedaços; decidir se o perfil passa a casar por sha256 como o upstream. **Confirmado** |
| 23 (hoje 24, 5 `disabled`) substituições nativas `guest_overrides.json:5-28`, `oot3d_native_guest_overrides.cpp:75-99` | sim, PCs do OoT3D | **não existe**: `titles/mm3d` não tem `guest_override`; `mm3d_a32_substitute.h` é outra coisa (executa a palavra que o mm3d-dekui troca num site) | — | são otimizações de desempenho, não são necessárias para bootar. Achar o par no MM3D e refazer a equivalência (`run_guest_override_equivalence.py`). O casamento já achou 8 das 24 (tabela abaixo). **Confirmado** |
| TopScreen 3.0 (ganchos em `oot3d_native_a32_window_shared.h:160-244`, `ui_topscreen/*`, 102 patches de layout, atlas `config/atlas_overrides.o3tu`) | sim, PCs e dados do OoT3D | o papel é do **mm3d-dekui** (tela única) + **Project Restoration** (HUD HD): `titles/mm3d/dekui/` (92 arquivos), `adapter/mm3d_single_screen.*`, `mm3d_guest_patches.*`, `restoration/`, `dekui/v110_address.h` (gerado por `a32/v110_address_map.py`; os 126 patches nativos do dekui traduzidos de v1.0 para v1.1) | lado guest neutro (patches por PC com palavras de guarda); o desenho de botões/glifos usa `native_button_overlay.h` + `fast/extension/native_ui_overlay_pass` (NRI); a captura da pausa (`dekui/pause_capture.h`) é interface abstrata (`BindPause`) com implementação no renderer | portar `mm3d_guest_patches`/`single_screen`/`restoration` como estão; escrever o overlay de botões e o `BindPause` no deko3d; o layout do HUD HD vem da RomFS (`restoration/package`, receitas BPS), neutro. Nada do nosso TopScreen serve. **Parcial** (não li o dekui inteiro) |
| Widescreen: hook de projeção `kMtx4x4BuildFrustum` 0x002FDE9C / ortho 0x00300AA8 (`window_shared.h:158-159`, `projection_policy.cpp:774-813`) | sim | `kMm3dMtx44Frustum` = 0x00122FE8 (`mm3d_title_contracts.h:18`, com palavra de guarda 0xED9F3A23), instalado em `mm3d_title_session.cpp:156,166` (`setup.PerspectiveFrustumEntry`, `FrustumIsSceneView = false`); `mm3d_presentation_settings.h` liga o widescreen do dekui e FOV 45/49 | o hook é neutro (A32 + estado); depende do `SceneView` do upstream | trazer o campo `PerspectiveFrustumEntry` para o nosso `projection_policy` (hoje fixo no OoT3D). **Confirmado** |
| Widescreen de menus `gl_widescreen_menu_policy.h` (670 linhas, só estado PICA) | não | não existe no upstream (é nosso, 3eb089f7) | neutro (OpenGL/deko3d) | no MM3D os menus são do dekui em 16:9; provavelmente desligar para o MM3D. **Sem suporte** (não testado) |
| fps60 / interpolação / pacing e fronteira de quadro (`ctr_frame_boundary.h:51-54`: GameState_Update 0x00417014, limitador 0x003FD27C/3A8/3B8; `oot3d_native_frame_rate.h:17-29`) | sim | `mm3d_timing_observer.h:13-17`: GameState_Update 0x106750, Player_Update 0x1F9C70, LimiterBegin 0x124EC0, Decision 0x124F6C, Commit 0x124F7C (com 4 palavras de guarda cada); `mm3d_frame_pacing.h:33` usa `GameplayTimingMode::Native30NoInterpolation` | neutro (política compartilhada `NativeFrameRatePolicy`) | o MM3D do upstream é **30 nativo sem interpolação**: não há fps60 para copiar. Para nós: fronteira de quadro pelos 3 PCs do limitador (ou `GspPresent`, que já funciona no perfil genérico); fps60 seria trabalho novo. **Confirmado** |
| Giroscópio `oot3d_switch_motion.*` (six-axis da libnx), gate `SwitchConsoleMotion` | não (só a substituição 0x002FA674 e o ícone 0x004FC648 do TopScreen) | o host CTR do upstream já escreve amostras de giroscópio (`oot3d_native_a32_ctr_host.h:159-176`); `titles/mm3d` não tem nada próprio | neutro | basta ligar `SwitchConsoleMotion` no perfil do MM3D. **Parcial** (não testei mira por giroscópio no MM3D) |
| Master Quest (`oot3d_native_master_quest_unlock.h`, `config/unlock_master_quest`) | flag no system.dat do OoT3D | não se aplica | — | desligar no perfil. **Confirmado** |
| Caminho `sdmc:/switch/oot3dre` (262 ocorrências, sem constante central; save de outros títulos já vai para `sdmc:/switch/triaevum/<id16>/`) | não | upstream é desktop: pasta de dados do Forge; não tem caminho Switch | — | centralizar a raiz por título (proposta em `docs/pesquisas/2026-11-05-fs-switch-generico.md:40`) e usar, por exemplo, `sdmc:/switch/triaevum/mm3d/`. **Parcial** (contagem do relatório) |
| `HOST_WRITTEN_TEXT_RANGES` (`whole_aot_cpp.py:40-49`, 8 palavras do OoT3D) | sim | o upstream não tem essa lista | — | aplicada a qualquer code.bin; no MM3D só deixa de dobrar 8 literais (inofensivo). Parametrizar por título. **Confirmado** |
| Shaders: família pré-traduzida `oot3d_native_vertex_binaries.cpp` + `deko3d.pack` | sim, hashes do romfs do OoT3D | `titles/mm3d/shaders/`: `mm3d_translated_vertex_programs.h` (6 programas GLSL traduzidos do romfs do MM3D: DebugShader, DummyShader, GzwShader, LayoutShader...) + `mm3d_native_vertex/fragment.spvpack` | o GLSL traduzido é neutro; os `.spvpack` são SPIR-V do NRI/Vulkan | usar o GLSL do upstream como fonte e montar um `deko3d.pack` do MM3D jogando (`tools/oot3d/deko3d_shader_pack.py`). **Parcial** |
| Áudio (layout de reverb EUR Rev0 do OoT3D, substituições 0x004A0338/0x004A022C) | sim | `titles/mm3d/audio/NATIVE_AUDIO.md`: o MM já roda o delay/reverb nativo antes do DSP HLE; endereços próprios (0x1628b0, 0x1d63bc) | neutro | nada para bootar; as duas substituições de áudio do OoT3D não têm par no casamento. **Confirmado** (doc) |
| PT-BR (`tools/oot3d/ptbr/`, patch offline de romfs) | não | não existe; o dekui tem preparação de mensagens (`dekui/setup/prepare_message_edits.py`) | neutro | tradução própria do MM3D; as ferramentas podem servir se os formatos forem os mesmos. **Sem suporte** |
| Iluminação, sombras, grama, composição "F1" (nossos `oot3d_native_a32_lighting.cpp`, `pica_composition`) | sim | `mm3d_native_a32_lighting.*`, `mm3d_pica_composition.*`, `gameplay/mm3d_light_arena.*`, `catalogs/` | presos ao caminho de extensão do NRI (`Fast::Extension`, que não existe no nosso fork) | fora do primeiro boot; avaliar depois. **Parcial** |

### Casamento de funções OoT3D→MM3D (encerrado)

Estado: `tools/ctr/casar_funcoes.py` funciona, com 10 testes de host verdes (dado sintético; os
testes foram vistos falhar com mutações e o do offset foi escrito antes da correção). Ficou como
está; não é mais o caminho para o inventário.

- **Confirmado (medido):** com a descoberta simétrica (14551 funções no OoT3D, 20784 no MM3D),
  1143 pares: 501 exatos, 91 frouxos, 144 por vizinhança, 407 por semelhança. Só 874 de 8984
  funções do OoT3D com 8+ instruções têm o prefixo de 8 instruções (frouxo) em algum lugar do
  MM3D: o compilador mudou registradores e ordem. O hash pega o SDK e pouco do jogo.
- **Confirmado (medido):** das 24 substituições, 8 acham par único: Runtime_Memcpy → 0x001F26D8,
  oot3d_translate_mtx3x4_local → 0x0020BBA4, FUN_003094cc → 0x001C24F0 (exatos), Mtx3x3_Multiply
  → 0x0061C230 (frouxo), PicaCommandWriter_UploadVertexFloatUniforms → 0x00141A34,
  Math_SmoothStepToF → 0x00222FD8, MeshCommandPacket_Submit → 0x00140E10, FUN_00303b24 →
  0x001362C4 (semelhança). Dos 585 PCs de código dos ganchos do OoT3D, 46 caem numa função com
  par; 26 têm offset confiável.
- **Confirmado (contra o upstream):** o par do limitador 0x003FD27C → 0x00124EC0 é o
  `LimiterBegin` do upstream (`mm3d_timing_observer.h:15`), e Mtx3x4_Multiply 0x0036C174 →
  0x0021B640 é a multiplicação citada em `mm3d_title_contracts.h:60`. Numa amostra de 10 pares
  fracos (seed 7) os 10 têm a mesma estrutura nos dois lados (conferido na desmontagem). A
  precisão parece boa; a cobertura é baixa.
- **Confirmado (contra o upstream):** faltaram pontos importantes: Mtx4x4_BuildFrustum (o
  upstream tem 0x00122FE8), GameState_Update (0x106750) e Player_Update (0x1F9C70) ficaram
  sem par.
- **Confirmado:** offset no meio de função só vale em par exato ou frouxo. Num par por
  semelhança de mesmo tamanho, 0x003FD3A8 + offset caiu num literal pool no MM3D (0x00124FE4),
  enquanto o upstream põe a decisão em 0x124F6C. Teste
  `test_hook_offset_not_trusted_across_recompilation`.
- **Confirmado:** os endereços do Project Restoration (`hooks/*.hks`) são da v1.0 e não batem
  com a nossa v1.1 (ex.: 0x1BDE78 é um dado na v1.1). O upstream traduziu v1.0→v1.1 com a mesma
  ideia de casamento por instrução mascarada (`a32/v110_address_map.py`: 363 trechos de
  deslocamento constante). Entre versões do mesmo compilador isso funciona; entre OoT3D e MM3D,
  não.

## Decisão sugerida

Não refazer o que o upstream já fez: trazer o MM3D dele para o nosso fork em vez de
reconstruir endereços. Ordem:

1. **Thumb no conversor**: trazer 85503360/757f90ae/716b0ba6 (decodificação e execução Thumb)
   e o `thumb_ranges.csv`. Sem isso não há C++ gerado do MM3D.
2. **Inventário do upstream**: usar `titles/mm3d/a32/whole_aot_inventory.csv` (18102 linhas,
   Ghidra + entradas descobertas em tempo de execução) e os demais insumos de `titles/mm3d/a32/`
   no lugar da nossa descoberta, e chegar ao boot probe com `--complete-gpu-work`.
3. **Perfil de título**: `CtrTitleKind::Mm3d` (program id 0x125600 ou sha256 6eec27b1), com
   fronteira de quadro pelo limitador (0x124EC0/0x124F6C/0x124F7C) ou `GspPresent`, MQ e TopScreen
   desligados, giroscópio ligado, raiz no SD própria.
4. **Costura `TitleSession`**: trazer `triaevum_title_session.h`/`triaevum_title_adapter.h` e
   `mm3d_title_session.cpp`, de início só com hooks neutros (frustum 0x00122FE8, timing,
   patches de guest do dekui e do Restoration). Deixar de fora o que depende de `Fast::Extension`.
5. **deko3d**: shaders a partir do GLSL de `mm3d_translated_vertex_programs.h` e pack montado
   jogando; overlay de botões e `BindPause` do dekui no deko3d.
6. Por último: substituições de desempenho (8 pares já achados), fps60 (trabalho novo) e PT-BR.

## Alternativas rejeitadas

- **Casamento OoT3D→MM3D como fonte de inventário/ganchos:** cobertura de 7,9% das funções e
  46 de 585 ganchos; não achou frustum, GameState_Update nem Player_Update, que o upstream já
  tem com palavras de guarda.
- **Endereços do Project Restoration direto:** são da v1.0; a v1.1 é outro layout.
- **Rebase do fork inteiro sobre o upstream:** são 896 commits do lado de lá contra 350 nossos,
  com renderer diferente (NRI × deko3d); é caro e arriscado para o OoT3D. Trazer só as peças acima
  (cherry-pick ou cópia com proveniência) parece mais barato. **Sem suporte**: não estimei o
  conflito de cada peça.

## Em aberto

- Quanto do `mm3d_title_session.cpp` compila sem `Fast::Extension` (não li os 697 linhas
  inteiros nem o `dekui/`).
- Se o `ctr_convert.py` do fork com os commits Thumb do upstream gera o MM3D com a nossa
  descoberta ou se é preciso o inventário Ghidra do upstream (o `MOONFALL_CURSE.md:143` relata
  13601 funções mantidas e 92,5% do texto coberto).
- Mira por giroscópio no MM3D com o nosso six-axis (não testado).
- As linhas exatas do nosso lado que vieram do relatório de agente e não conferi (UI, TG, VIS,
  LUZ, FOGO): marcadas como parcial.
