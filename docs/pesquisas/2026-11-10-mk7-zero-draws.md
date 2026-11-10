# MK7 no boot probe: 600 quadros apresentados e nenhum draw

Data: 2026-11-10 · Título(s): Mario Kart 7 (genérico) · Commits: 36f8cbf2, 346be52f

## Pergunta
Depois do 56d1be46 o probe do MK7 chegava a 600/600 vblanks, mas com 0 draw
packets: só limpezas de tela e cópias para o display, e as 15 threads esperando.
O que o jogo esperava?

## Fontes
- Repro (Ryzen, via trava): `tools/ctr/build_title_boot_probe.sh --shards $M/cpp
  --code-bin $M/extract/code.bin --exheader $M/extract/exheader.bin --romfs
  $M/extract/romfs.bin --build $M/build --jobs 12 --vblanks 600
  [--complete-gpu-work] --run`, com `M=/root/agentes/conv/mk7`.
- `report.json`: o último SVC de cada thread, `stack_words` e `memory_snapshots`;
  disassembly do code.bin com o capstone (base 0x100000).
- A janela entrega os quadros de áudio em `DeliverDspAudioFrames`
  (`oot3d_native_a32_window_frame.cpp`); o host só os conta (`AdvanceSystemTicks`
  e `TakePendingDspAudioFrames` em `oot3d_native_a32_ctr_host.cpp`).

## Conclusão
- A thread principal (0) ficava parada desde o SVC 1107 de 5498, em
  `ArbitrateAddress 0x6a6a7c`, dentro de `0x3d9abc -> 0x27dfd8 -> 0x2750b0`.
  Ali ela espera a resposta de uma fila de mensagens (o Receive é 0x59ef98, o Send
  é 0x59f074): o pedido foi para a fila 0x6a69c4 (contagem 1, nenhum leitor) e a
  resposta volta pela fila em +0xb4.
- Quem drena a fila é 0x274f60, chamada só por 0x275e94/0x275ee4. Essa função
  não tem chamador direto: o ponteiro dela vai para 0x225808 em 0x27627c, que
  registra um callback por quadro de áudio (buffers de 0xa0 = 160 amostras).
- A thread de som (14) espera a interrupção do DSP (evento 0x1f8040). O probe
  nunca entregava os quadros de áudio, então essa interrupção nunca chegava, a
  fila nunca era drenada e a principal esperava para sempre.
- Com os quadros de áudio entregues (36f8cbf2), 600 vblanks:

| probe | SVCs | draws | ProcessCommandList | commits tela de baixo |
|---|---|---|---|---|
| 56d1be46 | 5498 | 0 | 2 | 0 |
| 36f8cbf2 | 13470 | 12 | 150 | 20 |
| 36f8cbf2 + `--complete-gpu-work` | 37590 | 6588 | 3986 | 568 |

  Foram 2043 quadros de áudio, e a principal roda até o fim.

## Decisão
O probe faz o mesmo que a janela antes de cada VBlank: mix HLE, sinal do DSP e
as threads que ele acorda. A guarda está em `test_bug_guards.py`. Para títulos
fora do OoT3D, o probe deve rodar com `--complete-gpu-work`: sem GPU, as
interrupções P3D/PPF não chegam e o jogo segura a próxima lista.

## Alternativas rejeitadas
- Sinalizar só o evento do DSP, sem o mix HLE: o jogo lê o estado que o mix
  escreve na memória compartilhada, e a janela já faz o mix. Copiar a janela é
  o caminho conhecido.

## Em aberto
- Teste sem ROM desde 346be52f: a entrega virou `DeliverPendingDspAudioFrames`
  (`ctr_dsp_audio_frames.h`) e o `TestDspAudioFrameDeliveryWakesSoundThread`
  acorda uma thread que espera a pipe de áudio (falhou com a função sem entregar).
- Ainda não se sabe o que a tela mostra: a janela não roda no WSLg do Ryzen.
  O executável compila (`build_title_window.sh --work /root/agentes/conv/mk7
  --jobs 12`), mas `--no-build --run` cai antes do primeiro quadro. Com o driver
  padrão (Wayland) dá SIGSEGV em `wl_proxy_get_version`, dentro do
  `SDL_CreateWindow`, chamado por `Fast::GfxWindowBackendSDL2::Init` (stdout.log
  do CrashHandler + addr2line). Com `SDL_VIDEODRIVER=x11` sai com "X connection
  to :0 broken". `--complete-gpu-work` é só do probe.
- Seguem como stub: frd:u, boss:U, nim:aoc e ndm:u. Nenhum deles bloqueia hoje.
