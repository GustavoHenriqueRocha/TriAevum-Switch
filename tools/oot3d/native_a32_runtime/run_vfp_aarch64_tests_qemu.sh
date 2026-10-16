#!/usr/bin/env bash
# Build the VFP differential tests for AArch64 with devkitA64 and the Switch
# whole-AOT code-generation flags, then run them under qemu-aarch64 user mode:
#   oot3d_native_a32_vfp_aarch64_tests  native AArch64 VFP vs software (~194M checks)
#   oot3d_native_a32_vfp_fast_tests     inline host-float fast paths vs software
# Usage: run_vfp_aarch64_tests_qemu.sh [output-directory]
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="${1:-${TMPDIR:-/tmp}/oot3d-vfp-aarch64-tests}"
devkit="${DEVKITPRO:-/opt/devkitpro}/devkitA64/bin"
cxx="$devkit/aarch64-none-elf-g++"
qemu="$(command -v qemu-aarch64-static || command -v qemu-aarch64)"
mkdir -p "$out"

flags=(
    -std=c++20 -O3 -march=armv8-a+crc+crypto -mtune=cortex-a57
    -fno-fast-math -ffp-contract=off -static -nostartfiles
    -I"$here" -I"$here/upstream" -I"$here/upstream/recomp"
)
sources=(
    "$here/qemu_aarch64_test_runtime.cpp"
    "$here/oot3d_native_a32_vfp_ops.cpp"
    "$here/upstream/recomp/a32_vfp_binary64.cpp"
)

status=0
for test in oot3d_native_a32_vfp_aarch64_tests oot3d_native_a32_vfp_fast_tests; do
    "$cxx" "${flags[@]}" "${sources[@]}" "$here/$test.cpp" -o "$out/$test.elf"
    echo "== $test"
    if "$qemu" "$out/$test.elf"; then
        echo "PASS $test"
    else
        echo "FAIL $test"
        status=1
    fi
done
exit "$status"
