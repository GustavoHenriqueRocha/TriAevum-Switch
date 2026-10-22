# Testes de regressão do port Switch

Rodar antes de cada versão enviada ao console. **Regra: todo bug e todo erro
encontrado ganha um teste de regressão aqui** (unidade, guarda ou checagem no Eden),
para não acontecer de novo. O objetivo é que um bug já
corrigido nunca volte sem alguém perceber, e que as melhorias sejam medidas.

| Camada | Comando | Tempo | Pega |
|---|---|---|---|
| Guardas | `tests/switch/run.sh --quick` | segundos | correção de bug apagada por merge/refactor |
| Unidade (PC) | `tests/switch/run.sh` | ~5 min (com cache) | lógica do runtime: pacing, áudio, threads, watchdog, FS, pausa, contador de FPS |
| DSP AArch64 | (incluído em `run.sh`) | ~1 min | mixagem de áudio com as flags reais do Switch (devkitA64 + qemu) |
| Eden | `tests/switch/run.sh --eden build/oot3d_native_game.nro` | ~4 min | .nro que não abre, trava ou não passa de N quadros |
| Console | `tests/switch/perf_gate.py logs/perf.txt --mode 30` | — | queda de FPS, velocidade do jogo, travadinhas |

## Guardas (`test_bug_guards.py`)

Uma entrada por bug visto no console, apontando o arquivo da correção e o teste de
comportamento, quando existe. `--list` mostra todas. **Ao corrigir um bug novo,
acrescentar a guarda dele aqui.**

## Build de host (uma vez)

O CMake de host precisa de cabeçalhos que o Arch não tem por padrão. Sem sudo:

```sh
P=~/oot3d-work/host-deps   # Vulkan-Headers 1.4.357, nlohmann_json 3.12.0, tinyxml2 11.0.0
cmake -S . -B ~/oot3d-work/build-host-tests -G Ninja \
  -DCMAKE_PREFIX_PATH=$P -DVulkan_INCLUDE_DIR=$P/include \
  -DTHREE_DS_RECOMP_ENABLE_SSSR=OFF -DTHREE_DS_RECOMP_ENABLE_CACAO=OFF
```

`oot3d_native_audio_tests` pula (código 77) sem `OOT3D_TEST_EXTRACT_ROOT` apontando
para um romfs desempacotado. `oot3d_native_audio_dsp_mix_tests` no host só bate com
o digest guardado em build Release (`-O3 -ffast-math`); o que vale é a versão AArch64.

## Desempenho (`perf_gate.py`)

Referências em `perf_baseline.json` (final 4, CPU 1785 MHz, portátil):
30 FPS = 29,6 fps globais / 98,8% de velocidade; 60 FPS = 57,5 fps / 99,7%.
Só compara com o mesmo clock de CPU. Uma versão melhor vira a nova referência
com `--update`.

## Eden (`eden_smoke.sh`)

Usa o Eden da Ryzen (`D:\Emuladores\Eden-Windows-v0.2.1...`) numa tarefa agendada
interativa (`/IT`), porque um processo aberto pelo ssh não tem tela. A pasta
`%APPDATA%\eden\sdmc\switch\oot3dre` precisa ter `game/`, `config/` e
`resources.o2r`. O script fecha qualquer Eden aberto antes de rodar.

Ele reprova também quando: o SD do Eden não bate com o SD de referência (cópia
corrompida — não é bug do build), o `boot-status.txt` termina em `status=failed`
(mostra o `detail=`) ou o jogo altera arquivos que só deveria ler. Esses casos
vieram do bug de 2026-10-22: a rotação de log ao abrir do upstream (#72) usa
`rename`, que falha no Horizon, e o sink quebrado estragou a leitura do
`atlas_overrides.o3tu` ("payload hash mismatch").
