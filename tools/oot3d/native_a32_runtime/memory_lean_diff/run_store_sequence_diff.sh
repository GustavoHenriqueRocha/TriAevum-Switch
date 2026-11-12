#!/bin/bash
# Differential of inlined guest access sequences (current memory vs
# base revision). The same generated functions (gen_store_sequences.py) are
# compiled against both memories at -O3 and run with random states and modes
# (trace, write observer, range watch).
#
# The current side runs in the fastmem window (WINDOW=1, default: its images
# claim the window and the inline paths load and store at window base +
# address; WINDOW=0: every access takes the slow path). The base runs with its
# second core active where it has one, so its exclusives use the global
# exclusive monitor, which the current memory uses with the second core on or
# off (fastmem phase 2, SWITCH_PORT.md "fastmem window"); a base older than
# the second core diverges on LDREX/STREX for that reason.
#
# usage: run_store_sequence_diff.sh <output> [base-revision] [trials] [seed] [functions] [operations]
#   default base: e4280d76 (fastmem phase 1, the last memory before phase 2)
#   TARGET=host (native g++, default) or TARGET=switch (devkitA64 with the shard
#   flags, run under qemu-aarch64; the window maps through qemu_mman.cpp)
#   WINDOW=1|0 (see above)
#   EXTRA=<additional flags for both sides>
#   GIT_ROOT=<repository the base revision comes from> (default: this tree)
set -e
OUT=${1:?output folder}
BASE=${2:-e4280d76}
TRIALS=${3:-200}
SEED=${4:-1}
FUNCS=${5:-100}
OPS=${6:-60}
TARGET=${TARGET:-host}
WINDOW=${WINDOW:-1}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../.." && pwd)
R=$ROOT/tools/oot3d/native_game_runtime
A=$ROOT/tools/oot3d/native_a32_runtime
NLOHMANN=${NLOHMANN:-$HOME/oot3d-work/build-switch/_deps/nlohmann_json-src/include}
mkdir -p "$OUT"
GIT_ROOT=${GIT_ROOT:-$ROOT}
"$HERE/extract_base.sh" "$GIT_ROOT" "$BASE" "$OUT"
OLD_SOURCES=$(cat "$OUT/OLD_SOURCES")
python3 "$HERE/gen_store_sequences.py" "$OUT/store_sequences.inc" "$FUNCS" "$OPS" "$SEED"
INC="-I$OUT -I$R -I$A -I$A/upstream -I$A/upstream/recomp -I$A/mass_cpp -I$NLOHMANN"
if [ "$TARGET" = switch ]; then
    CXX=${DEVKITPRO:-/opt/devkitpro}/devkitA64/bin/aarch64-none-elf-g++
    FLAGS="-std=c++20 -O3 -march=armv8-a+crc+crypto -mtune=cortex-a57 -ffunction-sections -fdata-sections -finline-functions -fno-fast-math -ffp-contract=off -w"
    WINDOW_FLAGS="-DOOT3D_GUEST_WINDOW_MMAP -I$HERE/qemu_sys"
    LINK="-static -nostartfiles -I$HERE/qemu_sys $A/qemu_aarch64_test_runtime.cpp $HERE/qemu_mman.cpp"
    RUN="$(command -v qemu-aarch64-static || command -v qemu-aarch64)"
else
    CXX=${CXX:-g++}
    FLAGS="-std=c++20 -O3 -fno-fast-math -ffp-contract=off -w"
    WINDOW_FLAGS=""
    LINK="-pthread"
    RUN=""
fi
FLAGS="$FLAGS ${EXTRA:-}"
$CXX $FLAGS $INC -c "$R/oot3d_native_a32_memory.cpp" -o "$OUT/new_memory.o" & p1=$!
$CXX $FLAGS $INC -c "$OUT/old_memory.cpp" -o "$OUT/old_memory.o"
wait $p1
$CXX $FLAGS $INC -c "$HERE/store_sequence_side.cpp" -o "$OUT/side_new.o" & p2=$!
$CXX $FLAGS $INC -DSEQ_OLD -c "$HERE/store_sequence_side.cpp" -o "$OUT/side_old.o"
wait $p2
$CXX $FLAGS $INC -c "$R/oot3d_memory_bus_abi.cpp" -o "$OUT/bus_abi.o"
$CXX $FLAGS $INC $WINDOW_FLAGS -c "$R/oot3d_switch_guest_window.cpp" -o "$OUT/guest_window.o"
$CXX $FLAGS $INC -c "$R/oot3d_switch_guest_window_host_alloc.cpp" -o "$OUT/guest_window_hook.o"
OLD_OBJECTS=""
for source in $OLD_SOURCES; do
    $CXX $FLAGS $INC -c "$source" -o "${source%.cpp}.o"
    OLD_OBJECTS="$OLD_OBJECTS ${source%.cpp}.o"
done
MAIN_DEFS="-DSEQ_WINDOW=\"$WINDOW\""
[ "$TARGET" = switch ] && MAIN_DEFS="$MAIN_DEFS -DSEQ_TRIALS=$TRIALS -DSEQ_SEED=$SEED"
$CXX $FLAGS $INC $MAIN_DEFS -c "$HERE/store_sequence_main.cpp" -o "$OUT/main.o"
$CXX $FLAGS $INC "$OUT/main.o" "$OUT/side_new.o" "$OUT/side_old.o" "$OUT/new_memory.o" \
    "$OUT/old_memory.o" "$OUT/bus_abi.o" "$OUT/guest_window.o" "$OUT/guest_window_hook.o" \
    $OLD_OBJECTS $LINK -o "$OUT/store_sequence_diff"
$RUN "$OUT/store_sequence_diff" "$TRIALS" "$SEED"
