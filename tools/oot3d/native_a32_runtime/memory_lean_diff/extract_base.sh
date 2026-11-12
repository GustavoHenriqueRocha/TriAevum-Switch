#!/bin/bash
# Base implementation of NativeA32Memory for the differential tests
# (run_memory_lean_diff.sh, run_store_sequence_diff.sh): the memory header and
# source of <base-revision>, and, where the base has them, the second core's
# header and the guest window, all renamed into namespace OldMem.
#
# usage: extract_base.sh <git-root> <base-revision> <output>
# Writes old_memory.{h,cpp} (+ old_second_core.h, old_guest_window.{h,cpp})
# and OLD_SOURCES (the extra old sources to compile).
set -e
GIT_ROOT=$1
BASE=$2
OUT=$3
R=tools/oot3d/native_game_runtime
show() {  # path output: false when the base has no such file
    git -C "$GIT_ROOT" cat-file -e "$BASE:$1" 2>/dev/null || return 1
    git -C "$GIT_ROOT" show "$BASE:$1" > "$2"
}
show $R/oot3d_native_a32_memory.h "$OUT/old_memory.h"
show $R/oot3d_native_a32_memory.cpp "$OUT/old_memory.cpp"
rm -f "$OUT/old_second_core.h" "$OUT/old_guest_window.h" "$OUT/old_guest_window.cpp"
OLD_SOURCES=""
show $R/oot3d_native_a32_second_core.h "$OUT/old_second_core.h" || true
if show $R/oot3d_switch_guest_window.h "$OUT/old_guest_window.h"; then
    show $R/oot3d_switch_guest_window.cpp "$OUT/old_guest_window.cpp"
    OLD_SOURCES="$OUT/old_guest_window.cpp"
fi
for f in "$OUT"/old_*.h "$OUT"/old_*.cpp; do
    sed -i 's/^namespace Oot3dNativeGame {/namespace OldMem {/;
            s|^} // namespace Oot3dNativeGame$|} // namespace OldMem|;
            s/^namespace Oot3dNativeGame::\(SecondCore\|GuestWindow\) {/namespace OldMem::\1 {/;
            s|#include "oot3d_native_a32_memory.h"|#include "old_memory.h"|;
            s|#include "oot3d_native_a32_second_core.h"|#include "old_second_core.h"|;
            s|#include "oot3d_switch_guest_window.h"|#include "old_guest_window.h"|' "$f"
done
echo "$OLD_SOURCES" > "$OUT/OLD_SOURCES"
