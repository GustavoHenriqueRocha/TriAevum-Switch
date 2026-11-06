"""Builds and runs the native-override differential test.

Extracts from the generated C++ (without override prologues) each function listed in
guest_overrides.json and the closure of the functions it calls, renames the
listed ones to Translated_<ENTRY>, generates guest_override_fixture.inc and compiles
(block overrides: the function containing the block is included whole and a
copy of it with the block prologue becomes Patched_<BLOCK>; both run starting
from the block)
guest_override_equivalence.cpp with the same runtime headers and the
same floating-point options as the build (-fno-fast-math -ffp-contract=off).

The host build links the fastmem window's allocator hook: with
TRIAEVUM_FASTMEM_WINDOW=1 the translated side's memory (the first image) takes
the window, so the generated code's inline paths load and store at window base
+ address while the overrides run on an ordinary heap image (unset: both on the
slow path).

With --qemu-aarch64 the same test is compiled with devkitA64 (the Switch shards'
floating-point and CPU options, native ARM64 VFP in both
versions) and runs under qemu-aarch64, with code.bin embedded in the binary.

Example:
  python run_guest_override_equivalence.py \
      --generated ~/oot3d-work/gen/cpp-opt \
      --code ~/oot3d-work/code-eur-canonical.bin \
      --nlohmann ~/oot3d-work/build-switch/_deps/nlohmann_json-src/include \
      --output /tmp/guest-override-eq --trials 2000000
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

import apply_guest_overrides as overrides

HERE = Path(__file__).resolve().parent
RUNTIME = HERE.parent / "native_game_runtime"
CALL = re.compile(r"\bExecute_\w+_([0-9A-F]{8})\(frame, context, state")


def extract(generated: Path, entries: list[int]) -> tuple[list[str], dict[int, str]]:
    """Definitions (text) of the call closure and the symbol of each entry."""
    shards = {
        path: overrides.strip(path.read_text(encoding="utf-8").split("\n"))
        for path in sorted(generated.glob("oot3d_whole_aot_shard_*.cpp"))
    }
    symbols: dict[int, str] = {}
    bodies: dict[int, list[str]] = {}
    pending = list(entries)
    while pending:
        entry = pending.pop()
        if entry in bodies:
            continue
        for lines in shards.values():
            found = overrides.find_definition(lines, entry)
            if found is None:
                continue
            index, symbol = found
            end = lines.index("}", index)
            bodies[entry] = lines[index:end + 1]
            symbols[entry] = symbol
            for callee in CALL.findall("\n".join(bodies[entry])):
                pending.append(int(callee, 16))
            break
        else:
            raise SystemExit(f"definition of 0x{entry:08X} not found in {generated}")
    return [line for entry in sorted(bodies) for line in bodies[entry] + [""]], symbols


def render_fixture(generated: Path, manifest: list[dict[str, object]]) -> str:
    functions = [int(item["entry"]) for item in manifest if "function" not in item]
    blocks = [item for item in manifest if "function" in item]
    roots = functions + [int(item["function"]) for item in blocks]
    body, symbols = extract(generated, roots)
    text = "\n".join(body)
    for entry in functions:
        text = re.sub(r"\b%s\(" % re.escape(symbols[entry]), f"Translated_{entry:08X}(", text)
    declarations = []
    for entry, symbol in sorted(symbols.items()):
        name = f"Translated_{entry:08X}" if entry in functions else symbol
        declarations.append(
            f"Oot3dWholeAotFlow {name}(Oot3dWholeAotFrame& frame, "
            "Oot3dWholeAotContext& context, Oot3dAotArchitecturalState& state, "
            "uint32_t entryPc);")
    # Block override: a copy of the translated function with the block
    # prologue (Patched_<BLOCK>) against the original, both starting at the block.
    patched_text = []
    for item in blocks:
        entry, function = int(item["entry"]), int(item["function"])
        lines = None
        for path in sorted(generated.glob("oot3d_whole_aot_shard_*.cpp")):
            candidate = overrides.strip(path.read_text(encoding="utf-8").split("\n"))
            if overrides.find_definition(candidate, function) is not None:
                lines = candidate
                break
        index, symbol = overrides.find_definition(lines, function)
        copy = lines[index:overrides.function_end(lines, index) + 1]
        overrides.patch_block(copy, entry, function, [int(pc) for pc in item["resume"]])
        copy[0] = copy[0].replace(symbol + "(", f"Patched_{entry:08X}(")
        patched_text += copy + [""]
        declarations.append(
            f"Oot3dWholeAotFlow Patched_{entry:08X}(Oot3dWholeAotFrame& frame, "
            "Oot3dWholeAotContext& context, Oot3dAotArchitecturalState& state, "
            "uint32_t entryPc);")
    cases = []
    for item in manifest:
        entry = int(item["entry"])
        if "function" in item:
            cases.append(
                "    {0x%08XU, \"%s\", &%s, nullptr, &Patched_%08X},"
                % (entry, item.get("name", ""), symbols[int(item["function"])], entry))
        else:
            cases.append(
                "    {0x%08XU, \"%s\", &Translated_%08X, "
                "&::Oot3dNativeGame::GuestOverrides::Override_%08X, nullptr},"
                % (entry, item.get("name", ""), entry, entry))
    return "\n".join([
        "// Generated by run_guest_override_equivalence.py; do not edit.",
        *declarations,
        "",
        text,
        *patched_text,
        "struct OverrideCase {",
        "    uint32_t Entry;",
        "    const char* Name;",
        "    Oot3dWholeAotFlow (*Translated)(Oot3dWholeAotFrame&, Oot3dWholeAotContext&,",
        "                                    Oot3dAotArchitecturalState&, uint32_t);",
        "    bool (*Native)(Oot3dWholeAotFrame&, Oot3dWholeAotContext&,",
        "                   Oot3dAotArchitecturalState&, Oot3dWholeAotFlow&) noexcept;",
        "    // Block override: the function with the block prologue (Native null).",
        "    Oot3dWholeAotFlow (*Patched)(Oot3dWholeAotFrame&, Oot3dWholeAotContext&,",
        "                                 Oot3dAotArchitecturalState&, uint32_t);",
        "};",
        "inline constexpr OverrideCase kOverrideCases[] = {",
        *cases,
        "};",
        "",
    ])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generated", type=Path, required=True)
    parser.add_argument("--code", type=Path, required=True, help="canonical code.bin")
    parser.add_argument("--nlohmann", type=Path, required=True,
                        help="nlohmann_json include directory")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, default=overrides.DEFAULT_MANIFEST)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--trials", type=int, default=1000000)
    parser.add_argument("--seed", type=lambda value: int(value, 0), default=0x5EED)
    parser.add_argument("--only", help="hex entry to test a single function")
    parser.add_argument("--extra-flags", default="", help="extra compiler options")
    parser.add_argument("--qemu-aarch64", metavar="QEMU",
                        help="qemu-aarch64 (user mode): compiles with --qemu-cxx and runs under it")
    parser.add_argument("--qemu-cxx",
                        default="/opt/devkitpro/devkitA64/bin/aarch64-none-elf-g++")
    parser.add_argument("--codegen-cxx",
                        help="ARM64 compiler (e.g. aarch64-none-elf-g++): only compiles the "
                             "translated and native versions and counts the generated instructions")
    parser.add_argument("--codegen-flags",
                        default="-march=armv8-a+crc+crypto -mtune=cortex-a57 -O3 "
                                "-ffunction-sections -fdata-sections -fPIC -D__SWITCH__ "
                                "-finline-functions -fno-fast-math -ffp-contract=off -w",
                        help="options from the shards rule in the Switch build.ninja")
    args = parser.parse_args()

    # Includes the disabled ones ("disabled"): they stay proven.
    manifest = overrides.load_manifest(args.manifest, include_disabled=True)
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = args.output / "guest_override_fixture.inc"
    fixture.write_text(render_fixture(args.generated.expanduser(), manifest), encoding="utf-8")
    if args.codegen_cxx:
        return codegen(args, manifest, fixture)
    if args.qemu_aarch64:
        return run_qemu(args, fixture)

    upstream = HERE / "upstream"
    sources = [
        HERE / "guest_override_equivalence.cpp",
        RUNTIME / "oot3d_native_guest_overrides.cpp",
        RUNTIME / "oot3d_native_a32_memory.cpp",
        RUNTIME / "oot3d_switch_guest_window.cpp",
        RUNTIME / "oot3d_switch_guest_window_host_alloc.cpp",
        RUNTIME / "oot3d_memory_bus_abi.cpp",
        HERE / "oot3d_native_a32_vfp_ops.cpp",
        upstream / "recomp" / "a32_vfp_binary64.cpp",
        upstream / "recomp" / "a32_vfp_transport.cpp",
    ]
    includes = [args.output, RUNTIME, HERE, upstream, upstream / "recomp",
                HERE / "mass_cpp", args.nlohmann.expanduser()]
    flags = ["-std=c++20", "-O2", "-fno-fast-math", "-ffp-contract=off", "-w",
             *args.extra_flags.split()]
    objects = []
    for source in sources:
        obj = args.output / (source.stem + ".o")
        # The test and the overrides always; support only if missing or changed.
        stale = (not obj.exists() or source.name.startswith(("guest", "oot3d_native_guest"))
                 or obj.stat().st_mtime < source.stat().st_mtime)
        if stale:
            subprocess.run([args.cxx, *flags, *(f"-I{p}" for p in includes), "-c",
                            str(source), "-o", str(obj)], check=True)
        objects.append(str(obj))
    exe = args.output / "guest_override_equivalence"
    # The extra options (e.g. -fsanitize=address,undefined) apply at link time.
    subprocess.run([args.cxx, *args.extra_flags.split(), *objects, "-o", str(exe)], check=True)
    command = [str(exe), str(args.code.expanduser()), str(args.trials), str(args.seed)]
    if args.only:
        command.append(args.only)
    return subprocess.run(command).returncode


def run_qemu(args, fixture: Path) -> int:
    """Compiles for AArch64 (devkitA64, minimal runtime from qemu_aarch64_test_runtime)
    and runs under qemu-aarch64. Parameters fixed in the binary (no argv)."""
    upstream = HERE / "upstream"
    out = args.output
    embed = out / "guest_override_code.S"
    embed.write_text("\n".join([
        "    .section .rodata",
        "    .balign 16",
        "    .global gOot3dEmbeddedCode",
        "gOot3dEmbeddedCode:",
        f'    .incbin "{args.code.expanduser().resolve()}"',
        "    .global gOot3dEmbeddedCodeEnd",
        "gOot3dEmbeddedCodeEnd:",
        "",
    ]), encoding="utf-8")
    sources = [
        HERE / "qemu_aarch64_test_runtime.cpp",
        HERE / "guest_override_equivalence.cpp",
        RUNTIME / "oot3d_native_guest_overrides.cpp",
        RUNTIME / "oot3d_native_a32_memory.cpp",
        RUNTIME / "oot3d_switch_guest_window.cpp",
        RUNTIME / "oot3d_memory_bus_abi.cpp",
        HERE / "oot3d_native_a32_vfp_ops.cpp",
        upstream / "recomp" / "a32_vfp_binary64.cpp",
        upstream / "recomp" / "a32_vfp_transport.cpp",
        embed,
    ]
    includes = [out, RUNTIME, HERE, upstream, upstream / "recomp",
                HERE / "mass_cpp", args.nlohmann.expanduser()]
    only = int(args.only, 16) if args.only else 0
    # Those from the Switch shards build; thread_local becomes global (the minimal
    # runtime has no TLS and the test is single-threaded).
    flags = ["-std=c++20", "-O2", "-march=armv8-a+crc+crypto", "-mtune=cortex-a57",
             "-fno-fast-math", "-ffp-contract=off", "-w", "-static", "-nostartfiles",
             "-Dthread_local=", "-DOOT3D_GUEST_OVERRIDE_QEMU=1",
             f"-DOOT3D_QEMU_TRIALS={args.trials}ULL", f"-DOOT3D_QEMU_SEED={args.seed}ULL",
             f"-DOOT3D_QEMU_ONLY={only}U", *args.extra_flags.split()]
    objects = []
    for source in sources:
        obj = out / (source.stem + ".a64.o")
        subprocess.run([args.qemu_cxx, *flags, *(f"-I{p}" for p in includes), "-c",
                        str(source), "-o", str(obj)], check=True)
        objects.append(str(obj))
    exe = out / "guest_override_equivalence.a64.elf"
    subprocess.run([args.qemu_cxx, *flags, *objects, "-o", str(exe)], check=True)
    return subprocess.run([args.qemu_aarch64, str(exe)]).returncode


def codegen(args, manifest, fixture: Path) -> int:
    """Counts ARM64 instructions of each translated function and of the override."""
    upstream = HERE / "upstream"
    includes = [args.output, RUNTIME, HERE, upstream, upstream / "recomp",
                HERE / "mass_cpp", args.nlohmann.expanduser()]
    unit = args.output / "guest_override_codegen.cpp"
    unit.write_text("\n".join([
        '#include "oot3d_native_whole_aot_runtime.h"',
        '#include "oot3d_native_a32_memory.h"',
        '#include "oot3d_native_guest_overrides.h"',
        "namespace Oot3dNativeGame::GeneratedWholeAot {",
        "namespace a32 = oot3d::recomp::a32;",
        '#include "guest_override_fixture.inc"',
        "}",
        "",
    ]), encoding="utf-8")
    tool = args.codegen_cxx
    objdump = re.sub(r"(g\+\+|c\+\+|gcc)$", "objdump", tool)
    flags = ["-std=c++20", *args.codegen_flags.split()]
    counts: dict[str, dict[str, int]] = {}
    for source in (unit, RUNTIME / "oot3d_native_guest_overrides.cpp"):
        obj = args.output / (source.stem + ".aarch64.o")
        subprocess.run([tool, *flags, *(f"-I{p}" for p in includes), "-c", str(source),
                        "-o", str(obj)], check=True)
        listing = subprocess.run([objdump, "-d", "-C", "--no-show-raw-insn", str(obj)],
                                 check=True, capture_output=True, text=True).stdout
        current = None
        for line in listing.splitlines():
            label = re.match(r"^[0-9a-f]+ <(.+)>:$", line)
            if label:
                name = label.group(1)
                match = re.search(r"(Translated|Override)_([0-9A-F]{8})", name)
                current = None
                if match and "(" in name:
                    current = f"{match.group(1)}_{match.group(2)}"
                    counts.setdefault(current, {"instr": 0, "bl": 0})
                continue
            if current and re.match(r"^\s+[0-9a-f]+:\s+\S", line):
                counts[current]["instr"] += 1
                if re.search(r"\sbl\s", line):
                    counts[current]["bl"] += 1
    print("entry       translated(instr/bl)  native(instr/bl)  name")
    for item in manifest:
        entry = int(item["entry"])
        a = counts.get(f"Translated_{entry:08X}", {"instr": 0, "bl": 0})
        b = counts.get(f"Override_{entry:08X}", {"instr": 0, "bl": 0})
        print(f"0x{entry:08X}  {a['instr']:6d}/{a['bl']:<4d}        {b['instr']:6d}/{b['bl']:<4d}     "
              f"{item.get('name', '')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
