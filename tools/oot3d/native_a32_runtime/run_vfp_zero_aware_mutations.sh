#!/usr/bin/env bash
# Mutations of the exact-zero criterion of Oot3dVfpZeroAware*
# (oot3d_native_a32_vfp_aarch64.h): each must be DETECTED by the
# differential oot3d_native_a32_vfp_aarch64_tests.cpp (quick version, under
# qemu-aarch64 with the Switch shard flags). Without mutation the test passes.
# Usage: run_vfp_zero_aware_mutations.sh [output directory]
set -uo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="${1:-${TMPDIR:-/tmp}/oot3d-vfp-zero-aware-mutations}"
devkit="${DEVKITPRO:-/opt/devkitpro}/devkitA64/bin"
qemu="$(command -v qemu-aarch64-static || command -v qemu-aarch64)"
mkdir -p "$out/native_a32_runtime"
ln -sfn "$here/../native_game_runtime" "$out/native_game_runtime"
cp "$here/oot3d_native_a32_vfp_aarch64_tests.cpp" "$out/native_a32_runtime/"
header="$here/oot3d_native_a32_vfp_aarch64.h"
# name|sed expression applied to the header
mutations=(
  "none|"
  "sum_accepts_subnormal|/ExactZeroSum(std::uint32_t left,/,/^}/ s/return FiniteNotDenormal(left) \&\& FiniteNotDenormal(right) \&\&$/return true \&\&/"
  "discarded_product_accepted|s/return !ExactZero(product) || ExactZeroProduct(left, right);/return true;/"
  "product_accepts_subnormal|/ExactZeroProduct(std::uint32_t left,/,/^}/ s/return FiniteNotDenormal(left) \&\& FiniteNotDenormal(right) \&\&$/return/"
  "add_any_zero|s/exact = ExactZeroSum(left, right);/exact = true;/"
  "sub_any_zero|s/exact = ExactZeroSum(left, right ^ 0x80000000U);/exact = true;/"
  "mul_any_zero|s/exact = ExactZeroProduct(left, right);/exact = true;/"
  "div_any_zero|s/exact = FiniteNotDenormal(left) \&\&$/exact = true || /"
  "sqrt_any_zero|s/exact = ExactZero(left);/exact = true;/"
  "mac_any_zero|s/exact = exact \&\& ProductAgrees(product, left, right);/exact = true;/"
)
status=0
for item in "${mutations[@]}"; do
    name="${item%%|*}"; expr="${item#*|}"
    if [ -n "$expr" ]; then sed "$expr" "$header" > "$out/native_a32_runtime/oot3d_native_a32_vfp_aarch64.h"
    else cp "$header" "$out/native_a32_runtime/oot3d_native_a32_vfp_aarch64.h"; fi
    if [ -n "$expr" ] && cmp -s "$header" "$out/native_a32_runtime/oot3d_native_a32_vfp_aarch64.h"; then
        echo "ERROR $name: the mutation did not change the header"; status=1; continue
    fi
    "$devkit/aarch64-none-elf-g++" -std=c++20 -O3 -march=armv8-a+crc+crypto -mtune=cortex-a57 \
        -fno-fast-math -ffp-contract=off -static -nostartfiles -DOOT3D_VFP_TEST_QUICK \
        -I"$out/native_a32_runtime" -I"$here" -I"$here/upstream" -I"$here/upstream/recomp" \
        "$here/qemu_aarch64_test_runtime.cpp" "$here/oot3d_native_a32_vfp_ops.cpp" \
        "$here/upstream/recomp/a32_vfp_binary64.cpp" \
        "$out/native_a32_runtime/oot3d_native_a32_vfp_aarch64_tests.cpp" -o "$out/$name.elf" || { echo "ERROR $name: did not compile"; status=1; continue; }
    if "$qemu" "$out/$name.elf" > "$out/$name.txt" 2>&1; then result=passed; else result=failed; fi
    if [ "$name" = none ]; then
        [ $result = passed ] && echo "OK   no mutation: passes" || { echo "ERROR no mutation: failed"; status=1; }
    else
        [ $result = failed ] && echo "OK   $name: detected ($(grep -c '^za_' "$out/$name.txt") divergences shown)" || { echo "ERROR $name: NOT detected"; status=1; }
    fi
done
exit $status
