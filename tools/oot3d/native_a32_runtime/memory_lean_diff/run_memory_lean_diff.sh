#!/bin/bash
# NativeA32Memory differential test: current tree against a base revision. The
# base implementation is extracted from git, renamed to the OldMem namespace
# and linked into the same executable (extract_base.sh).
#
# The current side runs in the fastmem window (WINDOW=1, default; WINDOW=0:
# every typed access on the slow path); the base runs with its second core
# active where it has one, so both use the global exclusive monitor (fastmem
# phase 2). A base older than the second core diverges on exclusives.
#
# usage: run_memory_lean_diff.sh <output> [base-revision] [operations] [seed] [episodes]
#   default base: e4280d76 (fastmem phase 1, the last memory before phase 2)
#   NLOHMANN=<nlohmann_json include> (default: build-switch _deps)
set -e
OUT=${1:?output folder}
BASE=${2:-e4280d76}
OPS=${3:-2000000}
SEED=${4:-7}
EPISODES=${5:-200}
WINDOW=${WINDOW:-1}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../.." && pwd)
R=$ROOT/tools/oot3d/native_game_runtime
A=$ROOT/tools/oot3d/native_a32_runtime
NLOHMANN=${NLOHMANN:-$HOME/oot3d-work/build-switch/_deps/nlohmann_json-src/include}
mkdir -p "$OUT"
"$HERE/extract_base.sh" "$ROOT" "$BASE" "$OUT"
OLD_SOURCES=$(cat "$OUT/OLD_SOURCES")
INC="-I$OUT -I$R -I$A -I$A/upstream -I$A/upstream/recomp -I$A/mass_cpp -I$NLOHMANN"
FLAGS="-std=c++20 -O2 -fno-fast-math -ffp-contract=off -w"
CXX=${CXX:-g++}
$CXX $FLAGS $INC -c "$R/oot3d_native_a32_memory.cpp" -o "$OUT/new_memory.o"
$CXX $FLAGS $INC -c "$OUT/old_memory.cpp" -o "$OUT/old_memory.o"
$CXX $FLAGS $INC -c "$R/oot3d_memory_bus_abi.cpp" -o "$OUT/bus_abi.o"
$CXX $FLAGS $INC -c "$R/oot3d_switch_guest_window.cpp" -o "$OUT/guest_window.o"
$CXX $FLAGS $INC -c "$R/oot3d_switch_guest_window_host_alloc.cpp" -o "$OUT/guest_window_hook.o"
OLD_OBJECTS=""
for source in $OLD_SOURCES; do
    $CXX $FLAGS $INC -c "$source" -o "${source%.cpp}.o"
    OLD_OBJECTS="$OLD_OBJECTS ${source%.cpp}.o"
done
$CXX $FLAGS $INC -c "$HERE/memory_lean_diff.cpp" -o "$OUT/memory_lean_diff.o"
$CXX "$OUT/memory_lean_diff.o" "$OUT/new_memory.o" "$OUT/old_memory.o" "$OUT/bus_abi.o" \
    "$OUT/guest_window.o" "$OUT/guest_window_hook.o" $OLD_OBJECTS -pthread -o "$OUT/memory_lean_diff"
TRIAEVUM_FASTMEM_WINDOW=$WINDOW "$OUT/memory_lean_diff" "$OPS" "$SEED" "$EPISODES"
