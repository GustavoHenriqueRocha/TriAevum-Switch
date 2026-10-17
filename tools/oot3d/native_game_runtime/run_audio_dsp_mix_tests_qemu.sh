#!/usr/bin/env bash
# Build oot3d_native_audio_dsp_mix_tests for AArch64 with devkitA64 and the Switch release flags
# of the audio sources (-O3 -ffast-math, cortex-a57), then run it under qemu-aarch64 user mode.
# The digests must equal the AArch64 column of kExpected (the original audio_core).
# Usage: run_audio_dsp_mix_tests_qemu.sh [output-directory] [source-root]
#   source-root: a checkout whose tools/oot3d/{third_party/azahar_audio,native_game_runtime}
#   provide the DSP sources (default: this checkout). Pointing it at the pre-optimization tree
#   prints the reference digests.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
out="${1:-${TMPDIR:-/tmp}/oot3d-audio-dsp-mix-aarch64}"
root="${2:-$repo}"
devkit="${DEVKITPRO:-/opt/devkitpro}/devkitA64/bin"
cxx="$devkit/aarch64-none-elf-g++"
qemu="$(command -v qemu-aarch64-static || command -v qemu-aarch64)"
json="${NLOHMANN_JSON_INCLUDE:-}"
if [[ -z "$json" ]]; then
    for candidate in "$repo"/build*/_deps/nlohmann_json-src/include \
                     "$HOME"/oot3d-work/build-switch/_deps/nlohmann_json-src/include; do
        if [[ -f "$candidate/nlohmann/json.hpp" ]]; then json="$candidate"; break; fi
    done
fi
mkdir -p "$out"

audio="$root/tools/oot3d/third_party/azahar_audio"
# The flags the Switch build compiles the audio objects with (CMakeLists.txt release flags).
flags=(
    -std=gnu++20 -O3 -ffast-math -march=armv8-a+crc+crypto -mtune=cortex-a57
    -ffunction-sections -fdata-sections -fPIC -DNDEBUG -w -DOOT3D_QEMU_AARCH64_TEST
    -I"$root/tools/oot3d/native_game_runtime" -I"$audio" -I"$json"
)
objects=()
for source in "$audio/audio_core/codec.cpp" "$audio/audio_core/interpolate.cpp" \
              "$audio/audio_core/hle/filter.cpp" "$audio/audio_core/hle/mixers.cpp" \
              "$audio/audio_core/hle/source.cpp" \
              "$root/tools/oot3d/native_game_runtime/oot3d_native_a32_dsp_hle.cpp" \
              "$here/oot3d_native_audio_dsp_mix_tests.cpp"; do
    object="$out/$(basename "$source" .cpp).o"
    "$cxx" "${flags[@]}" -c "$source" -o "$object"
    objects+=("$object")
done
# Linked like the product (-fno-fast-math keeps crtfastmath out) with the qemu user-mode runtime.
"$cxx" -std=gnu++20 -O2 -march=armv8-a+crc+crypto -static -nostartfiles -fno-fast-math \
    "$repo/tools/oot3d/native_a32_runtime/qemu_aarch64_test_runtime.cpp" "${objects[@]}" \
    -o "$out/oot3d_native_audio_dsp_mix_tests.elf"
"$qemu" "$out/oot3d_native_audio_dsp_mix_tests.elf"
