# Mira por giroscópio no Switch

Data: 2026-11-08 · Título(s): OoT3D (genérico desligado) · Commits: portados de
JohnUzoka/TriAevum-Switch ee13d420 e fb26eb4f (0.1.0)

## Pergunta
Por que a mira por giroscópio (aim.source `automatic`, calibração e inversão em
L3+R3 > Controls) nunca funcionava no console, e como fazer o giroscópio somar
com o analógico direito sem estragar o mapeamento.

## Fontes
- O driver Switch do SDL2 do devkitPro não informa sensores: `host.ControllerMotion`
  vinha sempre vazio (mensagem do ee13d420).
- libnx `hidGetSixAxisSensorHandles`/`hidStartSixAxisSensor`/
  `hidGetSixAxisSensorStates` (LIFO de 17 entradas, ~200 Hz; aceleração em G,
  velocidade angular em rotações/s).
- Referencial Switch -> SDL de yuzu e Ryujinx; conferido também com Akira e
  borealis. Teste: `oot3d_switch_motion_tests` (TestMatchesSdlPath,
  TestRotationIsCoherent).
- Inversão do pitch depois de ~180° de giro pelo analógico:
  `three_ds_recomp_input_tests` (TestPhysicalGyroAfterSyntheticAim), visto falhar
  sem a correção com `got {36, 0, 0}, expected {-36, 0, 0}`.

## Conclusão
- Switch -> 3DS: acelerômetro `(-a.x, +a.z, +a.y)` G, giroscópio
  `360 * (-w.x, +w.z, +w.y)` graus/s; é a mesma rotação própria nos dois
  sensores e dá o mesmo que Switch -> SDL -> `ConvertSdlMotion`.
- Fonte: Pro Controller do jogador 1, senão o Joy-Con direito do par (o esquerdo
  se só ele estiver conectado), senão Handheld (Switch Lite).
- A média ponderada pelo tempo de todas as amostras desde a última leitura
  consumida pelo guest evita aliasing (o host lê por quadro apresentado, o guest
  consome só em alguns).
- A composição Automatic guardava o yaw sintético do lado do sensor; o modelo
  certo é `Yaw_mundo * físico * Offset_corpo`, com só o offset do corpo guardado.

## Decisão
- `oot3d_switch_motion.{h,cpp}` preenche a mesma observação que o SDL preencheria;
  calibração, inversão, sensibilidade e o anel HID do guest não mudam.
- O sensor só liga com o `aim.source` usando movimento de controle ou durante a
  calibração; para fora de foco e reinicia na volta ou quando os dados param.
- Perfil de título: `CtrTitleProfile::SwitchConsoleMotion` liga no OoT3D e
  desliga nos títulos genéricos (SM3DL, MK7), que não foram testados com ele.

## Alternativas rejeitadas
- Corrigir no SDL do devkitPro: exige rebuild do pacote do sistema; a leitura
  direta da libnx fica no port.
- Usar só a amostra mais recente da LIFO: aliasing com o anel do giroscópio do
  guest.

## Em aberto
- Joy-Con sozinho na horizontal: o referencial do estado processado não é
  documentado; fica sem suporte.
- No console, testado no Switch Lite (handheld); Pro Controller e par de
  Joy-Cons seguem o mesmo caminho, mas não foram medidos.
