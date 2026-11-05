# Super Mario 3D Land na janela do PC (primeira imagem)

Data: 2026-11-05 · Títulos: SM3DL (0004000000053F00), genérico · Branch: deko3d

## Pergunta
O executável com janela (oot3d_native_game, Vulkan no Linux) consegue rodar um
segundo jogo convertido pelo ctr_convert, só com o perfil de título genérico?

## Fontes
- Repro: `tools/ctr/build_title_window.sh --work ~/oot3d-work/conv/sm3dl --no-build --run --seconds 30`
  (ou `ctr_convert.py <jogo> --work ... --window --run`).
- Capturas: `~/oot3d-work/conv/sm3dl/shots/quadro_000060..000660.png`,
  `shots-longo/quadro_000900..002100.png`.
- Detalhes técnicos: `docs/GUIA_PORT_3DS_SWITCH.md` seção 8.3.

## Conclusão
- Sim: o logo "SUPER MARIO 3D LAND" sobre fundo amarelo aparece, desenhado pelo
  renderer real (1 vertex program + 3 fragment programs traduzidos em runtime).
- Em -O0 o jogo roda ~27 VBlanks/s (metade da velocidade real).
- Rodada de 150 s: 4269 VBlanks (~71 s de jogo), 235 mil draws, 4268 display
  transfers; o jogo NÃO sai do logo.

O que estava amarrado ao OoT3D e quebrava outro título:
- ganchos por PC da janela (16:9, PlayState, TopScreen, typed gameplay, núcleos
  manuais, guest_overrides) → campos `Oot3dGuestHooks`/`TopScreenUi` no perfil;
- lista de ganchos vazia mandava todo bloco pelo caminho lento (1 VBlank em 60 s)
  → callback nulo no genérico;
- detecção de idiomas lia menus do OoT na RomFS e abortava → genérico só inglês;
- `__aeabi_f2ulz` do RVCT (0x003A0D38) com literal após o BL deixava o CFG aberto
  → `ctr_library_natives.h`, rotina achada pelos bytes e rodada nativa;
- build Linux do branch deko3d não compilava (include de gfx_deko3d.h, vsync clock).

## Decisão
Tudo que é do OoT3D fica atrás do perfil de título, com guardas que garantem os
valores antigos para o OoT3D; tradução de shader sob demanda (`TRIAEVUM_SHADER_PREPARATION=sync`)
no genérico.

## Alternativas rejeitadas
- Pacote de shaders pré-compilado (`oot3d_pica_default.o3ps`) para o Mario: é da
  família do OoT, não serve.
- Corrigir `__aeabi_f2ulz` pelo endereço: não generaliza; a assinatura por bytes
  pega a mesma rotina em outro jogo compilado com RVCT.

## Em aberto
- O que segura o logo. Candidatos (hipóteses, sem suporte ainda): cecd:u Open e
  cfg:u 0x0003 em stub; jogo esperando entrada (nenhuma foi injetada). Frente
  "serviços" do workflow ctr-generico-frentes investiga os stubs.
- `0x0010748C`: a outra função com CFG aberto, ainda não exercitada.
- deko3d (Switch): a lista fixa `oot3d_translated_vertex_programs.h` ainda é
  exigida em `--pica-parametric-tev`; outro título falha lá.
- O Zelda não foi rodado na janela depois das mudanças: a garantia vem das
  guardas (149/149) e dos testes, não de execução.
