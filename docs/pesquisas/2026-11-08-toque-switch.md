# Tela de toque no Switch (HUD, inventário, mira por arraste, menu L3+R3)

Data: 2026-11-08 · Título(s): OoT3D (perfil TopScreen); genérico sem mudança · Commits: portados de
JohnUzoka/TriAevum-Switch 6ad3a19c, 88440a53, 4daa6243, 4a14e415, 164e6899 (0.1.0)

## Pergunta
Como usar a tela de toque do Switch no modo portátil com o deko3d: apertar os botões do HUD do
TopScreen, tocar itens do inventário e teclas da seleção de arquivo/criação de nome, mirar
arrastando e mexer no menu L3+R3, sem mudar o OoT3D com o controle nem os outros títulos.

## Fontes
- libnx `hidGetTouchScreenStates` (histórico de 17 amostras, painel 1280x720 no portátil).
- `oot3d_native_a32_window_input.cpp` (`ObserveSwitchTouchScreen`, `FeedHostMenuTouch`).
- `oot3d_top_screen_hud_touch.{h,cpp}`: acerto nos controles, rastreador de dedos e posse.
- `oot3d_host_menu_touch.{h,cpp}`: gesto do menu (folga de toque, ímã do X da barra de título).
- `oot3d_top_screen_page_layout.h`: posição das páginas da tela de baixo na tela de cima.
- Testes: `oot3d_top_screen_hud_touch_tests`, `oot3d_host_menu_touch_tests`,
  `oot3d_native_a32_input_tests` (`TestSwitchPageTouchLandsOnTheTappedElement`).

## Conclusão
- Sem janela SDL (deko3d), o SDL do Switch não reporta dedos: o toque tem de vir do HID.
- Cada dedo ganha dono quando encosta e o mantém até levantar: página nativa (um ponteiro, como
  o 3DS), controle do HUD, mira (o primeiro dedo fora de todos os controles), menu do host, ou
  nada. Assim um dedo nunca muda de papel no meio do gesto.
- O TopScreen desenha as páginas de 320 px da tela de baixo pixel por pixel em X + 40 da tela de
  cima (Itens alargada: grade em X + 63, sexta coluna em X + 80). Reduzir 400 para 320 errava uma
  casa: a primeira coluna de Itens (X nativo 32) chegava como X 76.
- O X da barra de título do ImGui tem 17 px em 720p (cerca de 1,6 mm no Lite); sem folga de 16 px
  e ímã de 24 px um dedo que rola ao levantar cancela o clique.

## Decisão
- Toque no HUD só no perfil TopScreen e só sem página nativa ou menu aberto; títulos genéricos
  (PiP da tela de baixo do SM3DL) seguem com o mapeamento deles, que tem precedência.
- A mira por arraste soma às taxas virtuais da fonte de mira (Automatic, Mouse, RightStick,
  DigitalLook) sem tomar a posse; desliga em `aim.touch_enabled` (`oot3d_controls.json` ou
  L3+R3 → Controls → Aiming), sensibilidade em `aim.touch_degrees_per_pixel` (padrão 0,14).

## Alternativas rejeitadas
- Eventos de dedo do SDL: não chegam sem janela (era o caminho antes do deko3d).
- Escalar a tela de cima inteira para a de toque: errava uma casa no inventário.

## Em aberto
- O minimapa não aceita toque.
- No modo TV não há tela de toque; nada muda lá.
