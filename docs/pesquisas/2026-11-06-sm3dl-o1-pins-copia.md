# SM3DL a -O1: janela 3× mais rápida e pins de cópia que perdiam a versão nova

Data: 2026-11-06 · Título(s): SM3DL (0004000000053F00), genérico (renderer Vulkan do PC,
apresentação paralela) · Continuação de `2026-11-06-sm3dl-stencil-sombra.md`.

## Pergunta

A janela Debug (-O0) do SM3DL roda a ~1/3 da velocidade, o que deixa cada rodada longa. Um
build otimizado resolve? E por que ele caiu com `raw PICA copy samples cannot be resolved: the
producer version is not registered in this namespace`, que o Debug nunca mostrou?

Repro: `OPT=-O1 tools/ctr/build_title_boot_probe.sh ... --build ~/oot3d-work/conv/sm3dl-o1/build
--target oot3d_native_game` e `tools/ctr/build_title_window.sh --work ~/oot3d-work/conv/sm3dl-o1
--no-build --run --seconds 300 -- --input-timeline ~/oot3d-work/conv/sm3dl/mapa_e_fase.json`
(apresentação paralela, o padrão).

## Fontes

- `build-o1.log`: a etapa 322/1017 parou em `_deps/nri-src/Source/Validation/SharedVal.h:61`
  (`-Werror=maybe-uninitialized`; o NRI liga `-Werror` em `target_compile_options(NRI_Shared PUBLIC)`).
- Rodadas -O1, `summary.json`/stderr: Debug 9143 vblanks em 560 s (16,3/s); -O1 paralelo 4200
  vblanks em 79 s (~53/s) até cair; -O1 serial (`TRIAEVUM_PARALLEL_PRESENTATION=0`) 10585 vblanks
  em 300 s (35/s), sem queda, até o riacho da W1-1.
- Log temporário (fora do commit) em `PublishGpuTextureCopySurface`,
  `RetireGpuTextureCopyProducers`, `SetPinnedPicaTextureCopies` e na falta do produtor.
- `runtime/three_ds_recomp/src/fast/backends/gfx_vulkan_pica_texture_copy.cpp`
  (`SetPinnedPicaTextureCopies`, `RetireGpuTextureCopyProducers`).

## Conclusão

1. **O -O1 tira a janela do 1/3 (confirmado):** ~53 vblanks/s na apresentação paralela, perto dos
   60 reais; os shards levaram a maior parte das ~1 h de build no notebook (2 jobs, pausa térmica).
2. **A queda depende do tempo (confirmado):** caiu em vblanks diferentes (480, 3840, 4560) e uma de
   cinco rodadas paralelas passou; a apresentação serial nunca caiu.
3. **Causa (confirmada no log):** `SetPinnedPicaTextureCopies` transformava cada completion id
   retido nos pares (namespace, id) **já registrados**. No quadro 253 o pin foi declarado com as
   referências 2036/2040/2044 quando só existiam as versões do namespace 1 e a 2:2036; logo depois
   a reprodução da apresentação publicou 2:2040 e 2:2044. No quadro 254 a varredura
   (`completed=253`) aposentou essas duas, ainda referenciadas, e o quadro 255 pediu 2:2040.
   No Debug a ordem entre a varredura e a próxima declaração de pin não abria essa janela.

## Decisão

- `PicaTextureCopyPins` (`fast/renderer3ds/pica_texture_copy_pins.h`): o pin guarda o completion
  id, que já era a identidade de todas as versões entre namespaces (comentário original), e vale
  também para versões capturadas depois da declaração.
- Teste `TestPicaTextureCopyPinCoversLaterVersions` (bridge tests), visto falhando com a regra antiga
  (pin só das versões existentes) e passando com a nova; guarda vista falhando por mutação (174/175).
- NRI: `-Wno-error=maybe-uninitialized` só no `NRI_Shared` com GCC (2bc1afc4), como o `/WX-` do
  clang-cl; mudar as flags globais recompilaria os 256 shards.

## Alternativas rejeitadas

- Usar só a apresentação serial no SM3DL: esconde o defeito, que é do renderer genérico, e roda a
  35 vblanks/s em vez de ~46.
- Declarar os pins de novo depois de cada execução: ainda deixaria a janela entre a publicação e a
  próxima declaração em outros caminhos (cópia crua no meio do quadro).
- Release completo: o script fixa `CMAKE_BUILD_TYPE=Debug`; `OPT=-O1` já dá a velocidade e mantém
  os asserts.

## Resultado com a correção

Três rodadas paralelas de 300 s com o mesmo roteiro: 13686, 13858 e 13751 vblanks (~45,8/s; 2,8× o
Debug e 1,3× a serial), nenhuma queda (antes, 4 de 5 caíram). Capturas em
`~/oot3d-work/conv/sm3dl/progresso/2026-11-06_02{07,12,17}_*_o1-pins-*/folha.png`: logo, abertura,
mapa do Mundo 1 e W1-1 até o riacho (o roteiro só segura direita, e o Mario fica no canal).

## Em aberto

- O ~53/s da primeira rodada era medido num trecho curto (só até a queda); o número de 300 s é o
  que vale.
- O roteiro de entrada não sai do riacho da W1-1: precisa de pulo no lugar certo para ir adiante.
