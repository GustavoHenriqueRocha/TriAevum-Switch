#!/usr/bin/env bash
# Monta (e opcionalmente roda) o boot probe de um título CTR qualquer a partir
# dos shards do whole-AOT (whole_aot_cpp.py), no host Linux, em -O0.
#
# Uso:
#   tools/ctr/build_title_boot_probe.sh \
#       --shards ~/oot3d-work/jogos/sm3dl/cpp \
#       --code-bin ~/oot3d-work/jogos/sm3dl/extract/code.bin \
#       --exheader ~/oot3d-work/jogos/sm3dl/extract/exheader.bin \
#       --romfs ~/oot3d-work/jogos/sm3dl/extract/romfs.bin \
#       [--build ~/oot3d-work/build-ctr-sm3dl] [--jobs 2] [--run] \
#       [--vblanks 2] [--title-profile auto|oot3d|generic] [--configure-only]
#       [--complete-gpu-work]
#
# O que faz:
#  1. configura um build Ninja Debug só com o whole-AOT do título
#     (OOT3D_WHOLE_AOT_GENERATED_DIR=--shards, OOT3D_REBUILD_WHOLE_AOT=ON,
#     OOT3D_REGENERATE_WHOLE_AOT=OFF, fora do modo produto, sem o registro de
#     blocos A32 do OoT3D) e compila o alvo ctr_title_boot_probe em -O0 com
#     nice e no máximo --jobs compilações (padrão 2). Se o pacote da CPU chegar
#     a LIMITE (85 °C), pausa o build até RETOMA (75 °C);
#  2. gera o manifesto do processo (build_process_manifest.py: layout do
#     ExHeader, program id que escolhe o perfil de título, janela da RomFS);
#  3. com --run, roda o probe: relatório JSON em <build>/title/report.json e o
#     stderr (serviços sem HLE, SVCs não implementados, falhas) em
#     <build>/title/stderr.log. O resumo traz as fronteiras de quadro
#     (ctr_frame_boundary.h); com --complete-gpu-work o probe conclui o
#     trabalho adiado para a GPU (P3D/PSC/PPF) e o jogo passa do 1º quadro.
#
# Variáveis: OPT (flags de C++, padrão "-O0"), DEPS_FROM (diretório _deps de
# um build host já configurado, para não baixar as dependências de novo;
# padrão ~/oot3d-work/build-host-tests/_deps), HOST_DEPS (prefixo com
# nlohmann_json/tinyxml2/Vulkan headers, padrão ~/oot3d-work/host-deps),
# LIMITE/RETOMA (°C).
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SHARDS="" CODE_BIN="" EXHEADER="" ROMFS=""
BUILD="$HOME/oot3d-work/build-ctr-title"
JOBS=2 RUN=0 VBLANKS=2 PROFILE=auto CONFIGURE_ONLY=0 GPU_WORK=0
OPT=${OPT:--O0}
DEPS_FROM=${DEPS_FROM:-$HOME/oot3d-work/build-host-tests/_deps}
# Prefixo com nlohmann_json, tinyxml2 e cabeçalhos Vulkan (o host-deps do
# build host do notebook); vazio se o sistema já tem tudo.
HOST_DEPS=${HOST_DEPS:-$HOME/oot3d-work/host-deps}
LIMITE=${LIMITE:-85}
RETOMA=${RETOMA:-75}

usage() { sed -n '2,34p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-64}"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --shards) SHARDS=$2; shift 2 ;;
    --code-bin) CODE_BIN=$2; shift 2 ;;
    --exheader) EXHEADER=$2; shift 2 ;;
    --romfs) ROMFS=$2; shift 2 ;;
    --build) BUILD=$2; shift 2 ;;
    --jobs) JOBS=$2; shift 2 ;;
    --vblanks) VBLANKS=$2; shift 2 ;;
    --title-profile) PROFILE=$2; shift 2 ;;
    --run) RUN=1; shift ;;
    --configure-only) CONFIGURE_ONLY=1; shift ;;
    --complete-gpu-work) GPU_WORK=1; shift ;;
    -h|--help) usage 0 ;;
    *) echo "opção desconhecida: $1" >&2; usage ;;
  esac
done

for pair in "shards:$SHARDS" "code-bin:$CODE_BIN" "exheader:$EXHEADER" "romfs:$ROMFS"; do
  [ -n "${pair#*:}" ] || { echo "falta --${pair%%:*}" >&2; usage; }
done
SHARDS=$(realpath "$SHARDS"); CODE_BIN=$(realpath "$CODE_BIN")
EXHEADER=$(realpath "$EXHEADER"); ROMFS=$(realpath "$ROMFS")
mkdir -p "$BUILD"; BUILD=$(realpath "$BUILD")

for f in "$SHARDS/oot3d_whole_aot_generated.h" "$SHARDS/oot3d_whole_aot_generated.cpp" \
         "$CODE_BIN" "$EXHEADER" "$ROMFS"; do
  [ -f "$f" ] || { echo "não existe: $f" >&2; exit 66; }
done
NSHARDS=$(find "$SHARDS" -maxdepth 1 -name 'oot3d_whole_aot_shard_*.cpp' | wc -l)
echo "shards: $NSHARDS em $SHARDS"
[ -f "$SHARDS/whole_aot_cpp_manifest.json" ] || echo "aviso: sem whole_aot_cpp_manifest.json (só o modo produto exige)"

# --- 1. configurar -----------------------------------------------------------
LAUNCHER=()
command -v ccache >/dev/null && LAUNCHER=(-DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_C_COMPILER_LAUNCHER=ccache)
DEPS=()
if [ -d "$DEPS_FROM" ]; then
  for dep in dxc ffx imgui monocypher ngx nri prism shadermake threadpool vma vulkan_headers zstd; do
    [ -d "$DEPS_FROM/$dep-src" ] && DEPS+=("-DFETCHCONTENT_SOURCE_DIR_${dep^^}=$DEPS_FROM/$dep-src")
  done
fi
if [ -n "$HOST_DEPS" ] && [ -d "$HOST_DEPS" ]; then
  DEPS+=("-DCMAKE_PREFIX_PATH=$HOST_DEPS")
  [ -f "$HOST_DEPS/include/vulkan/vulkan.h" ] && DEPS+=("-DVulkan_INCLUDE_DIR=$HOST_DEPS/include")
fi
mkdir -p "$BUILD/sem_registro_a32"   # vazio: sem oot3d_native_a32_aot (OoT3D)
echo "configurando $BUILD (OPT=$OPT)"
cmake -S "$REPO" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  "-DCMAKE_CXX_FLAGS_DEBUG=$OPT" "-DCMAKE_C_FLAGS_DEBUG=$OPT" \
  "${LAUNCHER[@]}" "${DEPS[@]}" \
  -DOOT3D_WHOLE_AOT_GENERATED_DIR="$SHARDS" \
  -DOOT3D_REBUILD_WHOLE_AOT=ON \
  -DOOT3D_REGENERATE_WHOLE_AOT=OFF \
  -DOOT3D_WHOLE_AOT_PRODUCT_MODE=OFF \
  -DOOT3D_DIRECT_AOT_PLUGIN=OFF \
  -DOOT3D_REQUIRE_WHOLE_AOT=ON \
  -DOOT3D_WHOLE_AOT_LEAN_STATS=OFF \
  -DOOT3D_WHOLE_AOT_PROFILE_MODE=none \
  -DOOT3D_WHOLE_AOT_THINLTO=OFF \
  -DOOT3D_WHOLE_AOT_MAX_PARALLEL_COMPILES="$JOBS" \
  -DOOT3D_A32_AOT_GENERATED_DIR="$BUILD/sem_registro_a32" \
  -DTHREE_DS_RECOMP_ENABLE_SSSR=OFF -DTHREE_DS_RECOMP_ENABLE_CACAO=OFF \
  > "$BUILD/configure.log" 2>&1 || { tail -30 "$BUILD/configure.log"; exit 1; }
[ "$CONFIGURE_ONLY" = 1 ] && { echo "configurado: $BUILD"; exit 0; }

# --- 2. compilar (nice, -j JOBS, teto de temperatura) --------------------------
temp() { sensors 2>/dev/null | awk '/^(Package id 0|Tctl):/ {gsub(/[+°C]/, "", $2); gsub(/[+°C]/, "", $4); v=($1=="Tctl:")?$2:$4; print int(v); exit}'; }
# Regera o build.ninja antes (o glob dos shards força um re-run do CMake e o
# dry-run contaria 0 etapas).
ninja -C "$BUILD" build.ninja > /dev/null 2>&1 || true
ETAPAS=$(cd "$BUILD" && ninja -n ctr_title_boot_probe | grep -c -E 'Building|Linking' || true)
echo "etapas: $ETAPAS, $JOBS jobs, teto ${LIMITE} °C"
START=$(date +%s)
setsid nice -n 19 ninja -C "$BUILD" -j "$JOBS" ctr_title_boot_probe > "$BUILD/build.log" 2>&1 &
PID=$!
PGID=$(ps -o pgid= "$PID" | tr -d ' ')
PAUSADO=0
while kill -0 "$PID" 2>/dev/null; do
  T=$(temp); T=${T:-0}
  if [ "$PAUSADO" = 0 ] && [ "$T" -ge "$LIMITE" ]; then
    kill -STOP -- "-$PGID" 2>/dev/null || true; PAUSADO=1; echo "$(date +%T) ${T} °C: pausado"
  elif [ "$PAUSADO" = 1 ] && [ "$T" -le "$RETOMA" ]; then
    kill -CONT -- "-$PGID" 2>/dev/null || true; PAUSADO=0; echo "$(date +%T) ${T} °C: retomado"
  fi
  sleep 2
done
if ! wait "$PID"; then
  grep -E 'error|FAILED' "$BUILD/build.log" | head -40
  echo "build falhou: $BUILD/build.log" >&2
  exit 1
fi
echo "compilado em $(( $(date +%s) - START )) s: $BUILD/ctr_title_boot_probe"

# --- 3. manifesto do processo -------------------------------------------------
OUT="$BUILD/title"
mkdir -p "$OUT"
python3 -I "$REPO/tools/oot3d/native_a32_runtime/build_process_manifest.py" \
  --exheader "$EXHEADER" --code-bin "$CODE_BIN" --romfs-image "$ROMFS" \
  --output "$OUT/process.json"

# --- 4. rodar -----------------------------------------------------------------
CMD=("$BUILD/ctr_title_boot_probe" --manifest "$OUT/process.json"
     --output "$OUT/report.json" --save-data "$OUT/savedata"
     --vblanks "$VBLANKS" --title-profile "$PROFILE")
[ "$GPU_WORK" = 1 ] && CMD+=(--complete-gpu-work)
if [ "$RUN" = 1 ]; then
  echo "rodando: ${CMD[*]}"
  set +e
  "${CMD[@]}" > /dev/null 2> >(tee "$OUT/stderr.log" >&2)
  STATUS=$?
  set -e
  echo "saída $STATUS (0 = parou numa espera de SVC, 2 = falha/limite); relatório: $OUT/report.json"
  exit "$STATUS"
fi
echo "para rodar: ${CMD[*]}"
