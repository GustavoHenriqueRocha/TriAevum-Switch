#!/bin/bash
# Teste diferencial de NativeA32Memory: árvore atual contra uma revisão base
# (padrão 4fc0472, antes do modo enxuto). A implementação base é extraída do
# git, renomeada para o namespace OldMem e ligada no mesmo executável.
#
# uso: run_memory_lean_diff.sh <saida> [revisao-base] [operacoes] [semente] [episodios]
#   NLOHMANN=<include do nlohmann_json> (padrão: _deps do build-switch)
set -e
OUT=${1:?pasta de saída}
BASE=${2:-4fc0472}
OPS=${3:-2000000}
SEED=${4:-7}
EPISODES=${5:-200}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../.." && pwd)
R=$ROOT/tools/oot3d/native_game_runtime
A=$ROOT/tools/oot3d/native_a32_runtime
NLOHMANN=${NLOHMANN:-$HOME/oot3d-work/build-switch/_deps/nlohmann_json-src/include}
mkdir -p "$OUT"
git -C "$ROOT" show "$BASE:tools/oot3d/native_game_runtime/oot3d_native_a32_memory.h" > "$OUT/old_memory.h"
git -C "$ROOT" show "$BASE:tools/oot3d/native_game_runtime/oot3d_native_a32_memory.cpp" > "$OUT/old_memory.cpp"
sed -i 's/^namespace Oot3dNativeGame {/namespace OldMem {/; s|^} // namespace Oot3dNativeGame|} // namespace OldMem|; s|#include "oot3d_native_a32_memory.h"|#include "old_memory.h"|' \
    "$OUT/old_memory.h" "$OUT/old_memory.cpp"
INC="-I$OUT -I$R -I$A -I$A/upstream -I$A/upstream/recomp -I$A/mass_cpp -I$NLOHMANN"
FLAGS="-std=c++20 -O2 -fno-fast-math -ffp-contract=off -w"
CXX=${CXX:-g++}
$CXX $FLAGS $INC -c "$R/oot3d_native_a32_memory.cpp" -o "$OUT/new_memory.o"
$CXX $FLAGS $INC -c "$OUT/old_memory.cpp" -o "$OUT/old_memory.o"
$CXX $FLAGS $INC -c "$R/oot3d_memory_bus_abi.cpp" -o "$OUT/bus_abi.o"
$CXX $FLAGS $INC -c "$HERE/memory_lean_diff.cpp" -o "$OUT/memory_lean_diff.o"
$CXX "$OUT/memory_lean_diff.o" "$OUT/new_memory.o" "$OUT/old_memory.o" "$OUT/bus_abi.o" -o "$OUT/memory_lean_diff"
"$OUT/memory_lean_diff" "$OPS" "$SEED" "$EPISODES"
