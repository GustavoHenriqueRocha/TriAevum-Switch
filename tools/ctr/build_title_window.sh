#!/usr/bin/env bash
# Monta (e opcionalmente roda) o executável com janela (oot3d_native_game) de
# um título CTR qualquer convertido por tools/ctr/ctr_convert.py, no host
# Linux, com o renderer Vulkan e o perfil de título escolhido pelo program id
# (ctr_title_profile.h: outro título = perfil genérico, sem os ganchos do
# OoT3D).
#
# Uso:
#   tools/ctr/build_title_window.sh --work ~/oot3d-work/conv/sm3dl \
#       [--jobs 2] [--no-build] [--run] [--seconds 20] \
#       [--shots <pasta>] [--shot-start 120] [--shot-interval 240] \
#       [--width 1280] [--height 720] [-- <argumentos extras do executável>]
#
# Tudo depois de `--` vai direto para o oot3d_native_game (por exemplo
# `-- --input-timeline entrada.json` ou `-- --frames 900`).
#
# <work> é a pasta do ctr_convert.py: extract/{code,exheader,romfs}.bin,
# cpp/ (shards do whole-AOT) e build/ (o mesmo build Ninja do boot probe:
# os objetos dos shards são reaproveitados, só o resto do runtime compila).
#
#  1. compila o alvo oot3d_native_game com build_title_boot_probe.sh
#     --target oot3d_native_game (nice, --jobs compilações, pausa a 85 °C);
#  2. com --run, roda a janela por --seconds segundos (padrão 20) em
#     <work>/window (config, save data, recursos vazios) e grava um PNG do
#     quadro apresentado a cada --shot-interval quadros a partir de
#     --shot-start em --shots (padrão <work>/shots). stderr em
#     <work>/window/stderr.log, resumo em <work>/window/summary.json.
#
# Variáveis: OPT (padrão -O0; os shards do probe são reaproveitados só com o
# mesmo OPT), TRIAEVUM_SHADER_PREPARATION (padrão sync: cada programa PICA é
# traduzido e compilado na hora do draw, com travadinhas, sem depender da
# família de programas pré-traduzida do OoT3D).
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
WORK="" JOBS=2 BUILD_STEP=1 RUN=0 SECONDS_LIMIT=20
SHOTS="" SHOT_START=120 SHOT_INTERVAL=240 WIDTH=1280 HEIGHT=720
EXTRA=()

usage() { sed -n '2,34p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-64}"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --work) WORK=$2; shift 2 ;;
    --jobs) JOBS=$2; shift 2 ;;
    --no-build) BUILD_STEP=0; shift ;;
    --run) RUN=1; shift ;;
    --seconds) SECONDS_LIMIT=$2; shift 2 ;;
    --shots) SHOTS=$2; shift 2 ;;
    --shot-start) SHOT_START=$2; shift 2 ;;
    --shot-interval) SHOT_INTERVAL=$2; shift 2 ;;
    --width) WIDTH=$2; shift 2 ;;
    --height) HEIGHT=$2; shift 2 ;;
    -h|--help) usage 0 ;;
    --) shift; EXTRA=("$@"); break ;;
    *) echo "opção desconhecida: $1" >&2; usage ;;
  esac
done
[ -n "$WORK" ] || { echo "falta --work" >&2; usage; }
WORK=$(realpath "$WORK")
EXTRACT=$WORK/extract BUILD=$WORK/build
SHOTS=${SHOTS:-$WORK/shots}
for f in "$EXTRACT/code.bin" "$EXTRACT/exheader.bin" "$EXTRACT/romfs.bin"; do
  [ -f "$f" ] || { echo "não existe: $f (rode o ctr_convert.py antes)" >&2; exit 66; }
done

if [ "$BUILD_STEP" = 1 ]; then
  "$REPO/tools/ctr/build_title_boot_probe.sh" --shards "$WORK/cpp" \
    --code-bin "$EXTRACT/code.bin" --exheader "$EXTRACT/exheader.bin" \
    --romfs "$EXTRACT/romfs.bin" --build "$BUILD" --jobs "$JOBS" \
    --target oot3d_native_game
fi
BIN=$BUILD/oot3d_native_game
MANIFEST=$BUILD/title/process.json
[ -x "$BIN" ] || { echo "não existe: $BIN" >&2; exit 66; }
# Sempre regerado (barato): pega seções do ExeFS extraídas depois do build.
mkdir -p "$BUILD/title"
EXEFS_ARGS=()
[ -d "$EXTRACT/exefs" ] && EXEFS_ARGS=(--exefs-dir "$EXTRACT/exefs")
python3 -I "$REPO/tools/oot3d/native_a32_runtime/build_process_manifest.py" \
  --exheader "$EXTRACT/exheader.bin" --code-bin "$EXTRACT/code.bin" \
  --romfs-image "$EXTRACT/romfs.bin" ${EXEFS_ARGS[@]+"${EXEFS_ARGS[@]}"} \
  --output "$MANIFEST" > /dev/null

RUN_DIR=$WORK/window
mkdir -p "$RUN_DIR/config" "$RUN_DIR/savedata" "$RUN_DIR/resources" "$SHOTS"
CMD=("$BIN"
     --a32-process-manifest "$MANIFEST"
     --resource-root "$RUN_DIR/resources"
     --config "$RUN_DIR/config/oot3d_native_game.json"
     --save-data "$RUN_DIR/savedata"
     --ui-profile oot3d
     --renderer vulkan
     --width "$WIDTH" --height "$HEIGHT"
     --max-seconds "$SECONDS_LIMIT"
     --output "$RUN_DIR/summary.json"
     --screenshot "$SHOTS/quadro.png"
     --screenshot-sequence
     --screenshot-start-frame "$SHOT_START"
     --screenshot-interval "$SHOT_INTERVAL"
     ${EXTRA[@]+"${EXTRA[@]}"})
if [ "$RUN" = 0 ]; then
  printf 'para rodar:'; printf ' %q' "${CMD[@]}"; printf '\n'
  exit 0
fi
echo "rodando ${SECONDS_LIMIT} s: $BIN (capturas em $SHOTS)"
cd "$RUN_DIR"
set +e
TRIAEVUM_SHADER_PREPARATION=${TRIAEVUM_SHADER_PREPARATION:-sync} \
  timeout --kill-after=10 $((SECONDS_LIMIT + 60)) "${CMD[@]}" \
  > "$RUN_DIR/stdout.log" 2> "$RUN_DIR/stderr.log"
STATUS=$?
set -e
echo "saída $STATUS; stderr: $RUN_DIR/stderr.log"
ls -1 "$SHOTS"/*.png 2>/dev/null || echo "nenhuma captura gravada"
exit "$STATUS"
