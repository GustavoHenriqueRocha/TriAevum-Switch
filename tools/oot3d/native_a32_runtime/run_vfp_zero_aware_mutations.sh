#!/usr/bin/env bash
# Mutações do critério de zero exato de Oot3dVfpZeroAware*
# (oot3d_native_a32_vfp_aarch64.h): cada uma precisa ser DETECTADA pelo
# diferencial oot3d_native_a32_vfp_aarch64_tests.cpp (versão rápida, sob
# qemu-aarch64 com as flags dos shards do Switch). Sem mutação o teste passa.
# Uso: run_vfp_zero_aware_mutations.sh [diretório de saída]
set -uo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="${1:-${TMPDIR:-/tmp}/oot3d-vfp-zero-aware-mutations}"
devkit="${DEVKITPRO:-/opt/devkitpro}/devkitA64/bin"
qemu="$(command -v qemu-aarch64-static || command -v qemu-aarch64)"
mkdir -p "$out/native_a32_runtime"
ln -sfn "$here/../native_game_runtime" "$out/native_game_runtime"
cp "$here/oot3d_native_a32_vfp_aarch64_tests.cpp" "$out/native_a32_runtime/"
header="$here/oot3d_native_a32_vfp_aarch64.h"
# nome|expressão sed aplicada ao cabeçalho
mutations=(
  "nenhuma|"
  "soma_aceita_subnormal|/ExactZeroSum(std::uint32_t left,/,/^}/ s/return FiniteNotDenormal(left) \&\& FiniteNotDenormal(right) \&\&$/return true \&\&/"
  "produto_descartado_aceito|s/return !ExactZero(product) || ExactZeroProduct(left, right);/return true;/"
  "produto_aceita_subnormal|/ExactZeroProduct(std::uint32_t left,/,/^}/ s/return FiniteNotDenormal(left) \&\& FiniteNotDenormal(right) \&\&$/return/"
  "add_qualquer_zero|s/exact = ExactZeroSum(left, right);/exact = true;/"
  "sub_qualquer_zero|s/exact = ExactZeroSum(left, right ^ 0x80000000U);/exact = true;/"
  "mul_qualquer_zero|s/exact = ExactZeroProduct(left, right);/exact = true;/"
  "div_qualquer_zero|s/exact = FiniteNotDenormal(left) \&\&$/exact = true || /"
  "sqrt_qualquer_zero|s/exact = ExactZero(left);/exact = true;/"
  "mac_qualquer_zero|s/exact = exact \&\& ProductAgrees(product, left, right);/exact = true;/"
)
status=0
for item in "${mutations[@]}"; do
    name="${item%%|*}"; expr="${item#*|}"
    if [ -n "$expr" ]; then sed "$expr" "$header" > "$out/native_a32_runtime/oot3d_native_a32_vfp_aarch64.h"
    else cp "$header" "$out/native_a32_runtime/oot3d_native_a32_vfp_aarch64.h"; fi
    if [ -n "$expr" ] && cmp -s "$header" "$out/native_a32_runtime/oot3d_native_a32_vfp_aarch64.h"; then
        echo "ERRO $name: a mutação não alterou o cabeçalho"; status=1; continue
    fi
    "$devkit/aarch64-none-elf-g++" -std=c++20 -O3 -march=armv8-a+crc+crypto -mtune=cortex-a57 \
        -fno-fast-math -ffp-contract=off -static -nostartfiles -DOOT3D_VFP_TEST_QUICK \
        -I"$out/native_a32_runtime" -I"$here" -I"$here/upstream" -I"$here/upstream/recomp" \
        "$here/qemu_aarch64_test_runtime.cpp" "$here/oot3d_native_a32_vfp_ops.cpp" \
        "$here/upstream/recomp/a32_vfp_binary64.cpp" \
        "$out/native_a32_runtime/oot3d_native_a32_vfp_aarch64_tests.cpp" -o "$out/$name.elf" || { echo "ERRO $name: não compilou"; status=1; continue; }
    if "$qemu" "$out/$name.elf" > "$out/$name.txt" 2>&1; then result=passou; else result=falhou; fi
    if [ "$name" = nenhuma ]; then
        [ $result = passou ] && echo "OK   sem mutação: passa" || { echo "ERRO sem mutação: falhou"; status=1; }
    else
        [ $result = falhou ] && echo "OK   $name: detectada ($(grep -c '^za_' "$out/$name.txt") divergências mostradas)" || { echo "ERRO $name: NÃO detectada"; status=1; }
    fi
done
exit $status
