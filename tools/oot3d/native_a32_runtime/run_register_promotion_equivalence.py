"""Differential test of two lowerings of the same generated program: register
promotion (generate_whole_aot_cpp --promote-registers) or, with --n4, sparse
block entries (--sparse-block-entries).

Extracts the same real translated functions (those in guest_overrides.json, the
functions containing the block overrides, extra entries via --entry and the
call closure of all of them) from two generated directories: the default
lowering (--baseline) and the variant (--promoted). Each side goes into a
namespace (Base / Promoted, or Base / Sparse with --n4) of the same executable,
and the guest_override_equivalence.cpp harness runs both from identical copies
of state, memory, budget, hooks and tracing, requiring everything equal at the
end: GPRs, CPSR, FPSCR, VFP, flow, BlocksRemaining/BlocksConsumed, stats,
hook sequence, write bytes and generations (global and per page) and the
traced write fingerprint.

--n4: the budget accounting and the BlockLimit exit points may differ, so the
harness (OOT3D_N4_EQUIVALENCE) runs each side to completion through the
registry's re-dispatch loop, resuming every BlockLimit exit with a new slice,
and compares the final guest-visible state plus the full hook sequence (hooks
only at gated PCs). The extraction then also takes the registry owner of every
dispatch entry of every extracted function (the function a resume enters).
The hook lists come from --hook-lists, written beforehand by
--write-hook-gates from the baseline (each case's entry, interior blocks and
some callee blocks, plus the real gates of --real-gates); generate the
--sparse-block-entries directory with that file as its --hook-gates.

By default the native override prologues are stripped (translated C++
only); --keep-overrides keeps both sides with the prologues (tests the
syncing of locals around the native call).

Host (default):
  python run_register_promotion_equivalence.py --baseline <gen-base> \\
      --promoted <gen-promo> --code ~/oot3d-work/extract/exefs/code.bin \\
      --nlohmann ~/oot3d-work/build-switch/_deps/nlohmann_json-src/include \\
      --output /tmp/regpromo-eq --trials 200000
N4:
  ... --baseline <gen-base> --write-hook-gates n4eq_gates.json \\
      [--real-gates whole_aot_hook_gates.json]
  (generate <gen-n4> with --sparse-block-entries --hook-gates n4eq_gates.json)
  ... --baseline <gen-base> --promoted <gen-n4> --n4 --hook-lists n4eq_gates.json ...
AArch64 under qemu-aarch64 (devkitA64, same options as the Switch shards):
  ... --qemu [--cxx /opt/devkitpro/devkitA64/bin/aarch64-none-elf-g++]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import apply_guest_overrides as overrides
from whole_aot_cpp import HOOK_GATES_FORMAT, load_hook_gates

HERE = Path(__file__).resolve().parent
RUNTIME = HERE.parent / "native_game_runtime"
CALL = re.compile(r"\bExecute_\w+_([0-9A-F]{8})\(frame, context, state")
# Same signature line as apply_guest_overrides.definition_pattern, any entry.
DEFINITION = re.compile(
    r"^(?:\[\[gnu::optimize\([^\]]*\)\]\] )?Oot3dWholeAotFlow (Execute_\w+_([0-9A-F]{8}))\($")
DISPATCH_CASE = re.compile(r"^    case 0x([0-9A-F]{8})U:$")
BLOCK_LABEL = re.compile(r"^block_([0-9A-F]{8}):$")
REGISTRY_ENTRY = re.compile(r"^    \{0x([0-9A-F]{8})U, Execute_\w+_([0-9A-F]{8})\},$")
DEVKIT_CXX = "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-g++"
# The Switch shards' CPU and floating-point options, without __SWITCH__ (no
# libnx headers under the minimal qemu runtime) and with thread_local made
# global (no TLS there; the test is single-threaded), as
# run_guest_override_equivalence.py --qemu-aarch64 does.
SWITCH_FLAGS = ("-march=armv8-a+crc+crypto -mtune=cortex-a57 -O3 -ffunction-sections "
                "-fdata-sections -finline-functions -fno-fast-math "
                "-ffp-contract=off -w -Dthread_local=")
# Gated PCs outside every call closure, hooked by the "outside" hook lists.
OUTSIDE_GATES = (0x00100000, 0x00600000)


def shards(generated: Path, keep: bool) -> list[list[str]]:
    result = []
    for path in sorted(generated.glob("oot3d_whole_aot_shard_*.cpp")):
        lines = path.read_text(encoding="utf-8").split("\n")
        result.append(lines if keep else overrides.strip(lines))
    return result


def index_definitions(texts: list[list[str]]) -> dict[int, tuple[int, int, str]]:
    """entry -> (shard, line, symbol) of the first definition, in the order
    apply_guest_overrides.find_definition searches."""
    found: dict[int, tuple[int, int, str]] = {}
    for shard, lines in enumerate(texts):
        for index, line in enumerate(lines):
            match = DEFINITION.match(line)
            if (match and index + 3 < len(lines) and lines[index + 3].rstrip().endswith(") {")
                    and int(match.group(2), 16) not in found):
                found[int(match.group(2), 16)] = (shard, index, match.group(1))
    return found


def dispatch_entries(body: list[str]) -> set[int]:
    """The `case` PCs of the function's entry switch(es), before its first
    block (--sparse-block-entries lists an uncharged entry in both)."""
    entries = set()
    for line in body:
        if BLOCK_LABEL.match(line):
            break
        match = DISPATCH_CASE.match(line)
        if match:
            entries.add(int(match.group(1), 16))
    return entries


def block_pcs(body: list[str]) -> list[int]:
    return [int(match.group(1), 16) for line in body if (match := BLOCK_LABEL.match(line))]


def registry_owners(generated: Path) -> dict[int, int]:
    """Dispatch entry -> owning function (kDispatchEntries of the registry)."""
    owners = {}
    text = (generated / "oot3d_whole_aot_generated.cpp").read_text(encoding="utf-8")
    for line in text.split("\n"):
        match = REGISTRY_ENTRY.match(line)
        if match:
            owners[int(match.group(1), 16)] = int(match.group(2), 16)
    if not owners:
        raise SystemExit(f"no dispatch table in {generated}")
    return owners


def extract(texts: list[list[str]], roots: list[int], owners: dict[int, int] | None = None,
            ) -> tuple[dict[int, list[str]], dict[int, str]]:
    """Bodies and symbols of the call closure of `roots`; with `owners`, also
    the owner of every dispatch entry of every extracted function."""
    definitions = index_definitions(texts)
    symbols: dict[int, str] = {}
    bodies: dict[int, list[str]] = {}
    pending = list(roots)
    while pending:
        entry = pending.pop()
        if entry in bodies:
            continue
        if entry not in definitions:
            raise SystemExit(f"definition of 0x{entry:08X} not found")
        shard, index, symbol = definitions[entry]
        lines = texts[shard]
        bodies[entry] = lines[index:overrides.function_end(lines, index) + 1]
        symbols[entry] = symbol
        pending.extend(int(callee, 16) for callee in CALL.findall("\n".join(bodies[entry])))
        if owners is not None:
            for pc in dispatch_entries(bodies[entry]):
                if pc not in owners:
                    raise SystemExit(f"0x{pc:08X} of 0x{entry:08X} is not in the registry")
                pending.append(owners[pc])
    return bodies, symbols


def load_cases(manifest: Path, extra: list[int]) -> list[tuple[int, int, str]]:
    cases: list[tuple[int, int, str]] = []
    for item in overrides.load_manifest(manifest):
        entry = int(item["entry"])
        name = str(item.get("name", ""))
        if "function" in item:
            function = int(item["function"])
            cases.append((entry, function, name))          # from the block
            cases.append((function, function, name.split(":")[0]))
        else:
            cases.append((entry, entry, name))
    cases += [(entry, entry, "extra") for entry in extra]
    seen: set[int] = set()
    return [case for case in cases if not (case[0] in seen or seen.add(case[0]))]


def spread(values: list[int], count: int) -> list[int]:
    """`count` evenly spaced items of `values` (all if fewer)."""
    if len(values) <= count:
        return list(values)
    return [values[i * (len(values) - 1) // (count - 1)] for i in range(count)]


def write_hook_gates(baseline: Path, cases: list[tuple[int, int, str]], real: Path | None,
                     output: Path) -> None:
    """Gate list for the N4 directory: the real gates, OUTSIDE_GATES and, per
    case, the hooked PCs (its entry, 8 spread blocks of its function, the
    real gates among its blocks and the entry plus a middle block of up to 3
    direct callees), chosen deterministically from the baseline."""
    real_gates = load_hook_gates(real) if real is not None else frozenset()
    texts = shards(baseline.expanduser(), False)
    bodies, _ = extract(texts, sorted({function for _, function, _ in cases}))
    inside: dict[str, list[str]] = {}
    for entry, function, _ in cases:
        body = bodies[function]
        blocks = block_pcs(body)
        hooks = {entry, *spread(sorted(blocks), 8), *(real_gates & set(blocks))}
        callees = list(dict.fromkeys(int(c, 16) for c in CALL.findall("\n".join(body))))
        for callee in callees[:3]:
            callee_blocks = sorted(block_pcs(bodies[callee]))
            hooks |= {callee, callee_blocks[len(callee_blocks) // 2]}
        inside[f"0x{entry:08X}"] = [f"0x{pc:08X}" for pc in sorted(hooks)]
    pcs = set(real_gates) | set(OUTSIDE_GATES)
    for hooks in inside.values():
        pcs |= {int(pc, 16) for pc in hooks}
    output.write_text(json.dumps({
        "format": HOOK_GATES_FORMAT,
        "pcs": [f"0x{pc:08X}" for pc in sorted(pcs)],
        "source": "run_register_promotion_equivalence.py --write-hook-gates"
                  + (f" (real gates: {real})" if real is not None else ""),
        "real_gates": len(real_gates),
        "inside": inside,
    }, indent=1) + "\n", encoding="utf-8")
    print(f"{output}: {len(pcs)} gates ({len(real_gates)} real), {len(inside)} cases")


def render_n4(base_bodies: dict[int, list[str]], sparse_bodies: dict[int, list[str]],
              symbols: dict[int, str], owners: dict[int, int], gates: list[int],
              hook_lists: dict[int, list[int]], cases: list[tuple[int, int, str]]) -> list[str]:
    """Dispatch table (pc -> owner on both sides), gate list and hook lists."""
    dispatch: set[int] = set()
    for entry, body in base_bodies.items():
        entries = dispatch_entries(body)
        if entries != dispatch_entries(sparse_bodies[entry]):
            raise SystemExit(f"0x{entry:08X}: different dispatch entries in the two directories")
        dispatch.update(entries)
    gate_set = set(gates)
    outside = sorted(gate_set - dispatch)
    out = [
        "struct N4DispatchEntry {",
        "    uint32_t Pc;",
        "    Oot3dWholeAotFlow (*Base)(Oot3dWholeAotFrame&, Oot3dWholeAotContext&,",
        "                              Oot3dAotArchitecturalState&, uint32_t);",
        "    Oot3dWholeAotFlow (*Sparse)(Oot3dWholeAotFrame&, Oot3dWholeAotContext&,",
        "                                Oot3dAotArchitecturalState&, uint32_t);",
        "};",
        "inline constexpr N4DispatchEntry kN4Dispatch[] = {",
    ]
    for pc in sorted(dispatch):
        symbol = symbols[owners[pc]]
        out.append(f"    {{0x{pc:08X}U, &Base::{symbol}, &Sparse::{symbol}}},")
    out += ["};", "inline constexpr uint32_t kN4HookGates[] = {"]
    out += [f"    0x{pc:08X}U," for pc in sorted(gate_set)]
    out += ["};", "inline constexpr uint32_t kN4OutsideHooks[] = {"]
    out += [f"    0x{pc:08X}U," for pc in outside] or ["    0U,"]
    out += ["};", "inline constexpr uint32_t kN4InsideHooks[] = {"]
    ranges = []
    first = 0
    for entry, _, _ in cases:
        hooks = sorted(pc for pc in hook_lists.get(entry, []) if pc in gate_set)
        out += [f"    0x{pc:08X}U," for pc in hooks]
        ranges.append((entry, first, len(hooks)))
        first += len(hooks)
    if first == 0:
        out.append("    0U,")
    out += [
        "};",
        "struct N4CaseHooks {",
        "    uint32_t Entry;",
        "    uint32_t First, Count;",
        "};",
        "inline constexpr N4CaseHooks kN4CaseHooks[] = {",
        *(f"    {{0x{entry:08X}U, {start}U, {count}U}}," for entry, start, count in ranges),
        "};",
        "",
    ]
    if not outside:
        # The placeholder 0 must not be hooked: an empty list asks for every block.
        raise SystemExit("no gated PC outside the call closure")
    return out


def signature(symbol: str) -> str:
    return (f"Oot3dWholeAotFlow {symbol}(Oot3dWholeAotFrame& frame, "
            "Oot3dWholeAotContext& context, Oot3dAotArchitecturalState& state, "
            "uint32_t entryPc);")


def render_parts(namespace: str, bodies: dict[int, list[str]], names: dict[int, str],
                 part_lines: int) -> list[str]:
    """The definitions of one side as separate translation units of about
    `part_lines` lines (a single one would be too large to compile: the
    closure of the hot functions has over a million lines per side)."""
    head = [
        "// Generated by run_register_promotion_equivalence.py; do not edit.",
        '#include "oot3d_native_whole_aot_runtime.h"',
        '#include "oot3d_native_a32_memory.h"',
        '#include "oot3d_native_guest_overrides.h"',
        "#include <atomic>",
        "namespace Oot3dNativeGame::GeneratedWholeAot {",
        "namespace a32 = oot3d::recomp::a32;",
        "Oot3dWholeAotFlow ExecuteOot3dWholeAotIndirect(uint32_t pc, Oot3dWholeAotFrame& frame,",
        "    Oot3dWholeAotContext& context, Oot3dAotArchitecturalState& state);",
        f"namespace {namespace} {{",
        *(signature(symbol) for _, symbol in sorted(names.items())),
    ]
    tail = [f"}} // namespace {namespace}", "} // namespace Oot3dNativeGame::GeneratedWholeAot", ""]
    parts: list[str] = []
    current: list[str] = []
    for entry in sorted(bodies):
        current += bodies[entry] + [""]
        if len(current) >= part_lines:
            parts.append("\n".join(head + current + tail))
            current = []
    if current:
        parts.append("\n".join(head + current + tail))
    return parts


def render_fixture(baseline: Path, promoted: Path, cases: list[tuple[int, int, str]],
                   keep: bool, n4: dict[int, list[int]] | None = None,
                   part_lines: int = 100000) -> tuple[str, list[str]]:
    """The fixture included by the harness (declarations and tables) and the
    translation units with the definitions of both sides."""
    roots = sorted({function for _, function, _ in cases})
    other = "Sparse" if n4 is not None else "Promoted"
    out = ["// Generated by run_register_promotion_equivalence.py; do not edit."]
    symbols = {}
    bodies = {}
    parts: list[str] = []
    owners = None
    if n4 is not None:
        owners = registry_owners(baseline.expanduser())
        if registry_owners(promoted.expanduser()) != owners:
            raise SystemExit("the two directories do not have the same dispatch table")
    for namespace, generated in (("Base", baseline), (other, promoted)):
        bodies[namespace], names = extract(shards(generated.expanduser(), keep), roots, owners)
        symbols[namespace] = names
        out.append(f"namespace {namespace} {{")
        out += [signature(symbol) for _, symbol in sorted(names.items())]
        out += [f"}} // namespace {namespace}", ""]
        parts += render_parts(namespace, bodies[namespace], names, part_lines)
    if symbols["Base"] != symbols[other]:
        raise SystemExit("the two directories do not have the same functions")
    print(f"extracted {len(symbols['Base'])} functions per side, {len(parts)} units")
    # Indirect calls leave the closure: the harness defines
    # ExecuteOot3dWholeAotIndirect (both sides take the same exit).
    out += [
        "struct OverrideCase {",
        "    uint32_t Entry;",
        "    const char* Name;",
        "    Oot3dWholeAotFlow (*Translated)(Oot3dWholeAotFrame&, Oot3dWholeAotContext&,",
        "                                    Oot3dAotArchitecturalState&, uint32_t);",
        "    bool (*Native)(Oot3dWholeAotFrame&, Oot3dWholeAotContext&,",
        "                   Oot3dAotArchitecturalState&, Oot3dWholeAotFlow&) noexcept;",
        "    Oot3dWholeAotFlow (*Patched)(Oot3dWholeAotFrame&, Oot3dWholeAotContext&,",
        "                                 Oot3dAotArchitecturalState&, uint32_t);",
        "    bool (*NativeBlock)(Oot3dWholeAotFrame&, Oot3dWholeAotContext&,",
        "                        Oot3dAotArchitecturalState&, Oot3dWholeAotFlow&, uint32_t&) noexcept;",
        "};",
        "inline constexpr OverrideCase kOverrideCases[] = {",
    ]
    for entry, function, name in cases:
        symbol = symbols["Base"][function]
        out.append(f'    {{0x{entry:08X}U, "{name}", &Base::{symbol}, nullptr, '
                   f"&{other}::{symbol}, nullptr}},")
    out += ["};", ""]
    if n4 is not None:
        manifest = json.loads((promoted.expanduser() / overrides.MANIFEST_NAME)
                              .read_text(encoding="utf-8"))
        base_manifest = json.loads((baseline.expanduser() / overrides.MANIFEST_NAME)
                                   .read_text(encoding="utf-8"))
        if not manifest.get("sparse_block_entries") or base_manifest.get("sparse_block_entries"):
            raise SystemExit("--n4 needs a default --baseline and a --sparse-block-entries --promoted")
        out += render_n4(bodies["Base"], bodies[other], symbols["Base"], owners,
                         [int(pc) for pc in manifest["hook_gates"]], n4, cases)
    return "\n".join(out), parts


def compile_all(cxx: str, flags: list[str], includes: list[Path],
                units: list[tuple[Path, Path, bool]], jobs: int, cache: Path | None) -> None:
    """Compiles (source, object, cacheable) units, `jobs` at a time. With
    `cache`, the objects of the cacheable units (the self-contained generated
    definitions) are reused by content: unit text, flags, compiler and every
    header they can include."""
    key = hashlib.sha256(" ".join([cxx, *flags]).encode())
    for directory in includes:
        for header in sorted(Path(directory).glob("*.h")):
            key.update(header.name.encode() + header.read_bytes())

    def build(unit: tuple[Path, Path, bool]) -> None:
        source, obj, cacheable = unit
        cached = None
        if cache is not None and cacheable:
            digest = key.copy()
            digest.update(source.read_bytes())
            cached = cache / f"{digest.hexdigest()}.o"
            if cached.is_file():
                shutil.copyfile(cached, obj)
                return
        subprocess.run([cxx, *flags, *(f"-I{p}" for p in includes), "-c", str(source),
                        "-o", str(obj)], check=True)
        if cached is not None:
            cache.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(obj, cached)

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        list(pool.map(build, units))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--promoted", type=Path,
                        help="the variant directory (with --n4: --sparse-block-entries)")
    parser.add_argument("--code", type=Path, help="code.bin")
    parser.add_argument("--nlohmann", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--manifest", type=Path, default=overrides.DEFAULT_MANIFEST)
    parser.add_argument("--entry", action="append", default=[],
                        type=lambda value: int(value, 0),
                        help="extra function entry (repeatable)")
    parser.add_argument("--keep-overrides", action="store_true")
    parser.add_argument("--trials", type=int, default=200000)
    parser.add_argument("--seed", type=lambda value: int(value, 0), default=0x5EED)
    parser.add_argument("--only", help="hex entries (comma separated) to test only those cases")
    parser.add_argument("--qemu", action="store_true",
                        help="compiles for AArch64 (devkitA64) and runs under qemu-aarch64")
    parser.add_argument("--cxx", default=None)
    parser.add_argument("--extra-flags", default="")
    parser.add_argument("--n4", action="store_true",
                        help="--promoted is a --sparse-block-entries directory (N4 comparison)")
    parser.add_argument("--hook-lists", type=Path,
                        help="with --n4: the file written by --write-hook-gates")
    parser.add_argument("--write-hook-gates", type=Path,
                        help="writes the N4 hook-gate/hook-list file from --baseline and exits")
    parser.add_argument("--real-gates", type=Path,
                        help="with --write-hook-gates: whole_aot_hook_gates.json to include")
    parser.add_argument("--jobs", type=int, default=2, help="parallel compiles")
    parser.add_argument("--object-cache", type=Path,
                        help="reuses the objects of unchanged generated units")
    args = parser.parse_args()

    cases = load_cases(args.manifest, args.entry)
    if args.write_hook_gates is not None:
        write_hook_gates(args.baseline, cases, args.real_gates, args.write_hook_gates)
        return 0
    if args.promoted is None or args.code is None or args.nlohmann is None or args.output is None:
        parser.error("--promoted, --code, --nlohmann and --output are required")
    hook_lists = None
    if args.n4:
        hook_lists = {}
        if args.hook_lists is not None:
            data = json.loads(args.hook_lists.read_text(encoding="utf-8"))
            hook_lists = {int(entry, 0): [int(pc, 0) for pc in pcs]
                          for entry, pcs in data.get("inside", {}).items()}

    out = args.output
    out.mkdir(parents=True, exist_ok=True)
    fixture, parts = render_fixture(args.baseline, args.promoted, cases, args.keep_overrides,
                                    hook_lists)
    (out / "guest_override_fixture.inc").write_text(fixture, encoding="utf-8")
    for stale in out.glob("fixture_part_*.cpp"):
        stale.unlink()
    part_sources = []
    for index, text in enumerate(parts):
        part_sources.append(out / f"fixture_part_{index:03d}.cpp")
        part_sources[-1].write_text(text, encoding="utf-8")
    upstream = HERE / "upstream"
    includes = [out, RUNTIME, HERE, upstream, upstream / "recomp", HERE / "mass_cpp",
                args.nlohmann.expanduser()]
    sources = [
        RUNTIME / "oot3d_native_guest_overrides.cpp",
        RUNTIME / "oot3d_native_a32_memory.cpp",
        RUNTIME / "oot3d_switch_guest_window.cpp",
        RUNTIME / "oot3d_memory_bus_abi.cpp",
        HERE / "oot3d_native_a32_vfp_ops.cpp",
        upstream / "recomp" / "a32_vfp_binary64.cpp",
        upstream / "recomp" / "a32_vfp_transport.cpp",
    ]
    mode_flags = ["-DOOT3D_N4_EQUIVALENCE=1"] if args.n4 else []
    if args.qemu:
        cxx = args.cxx or DEVKIT_CXX
        code = args.code.expanduser().resolve()
        main_unit = out / "register_promotion_qemu_main.cpp"
        main_unit.write_text("\n".join([
            "// Generated: harness under qemu-aarch64 (no argv or files).",
            "#define main GuestOverrideEquivalenceMain",
            '#include "guest_override_equivalence.cpp"',
            "#undef main",
            "asm(\".section .rodata\\n.balign 16\\n.global gRegPromoCode\\ngRegPromoCode:\\n"
            f".incbin \\\"{code}\\\"\\n.global gRegPromoCodeEnd\\ngRegPromoCodeEnd:\\n.text\\n\");",
            'extern "C" const unsigned char gRegPromoCode[], gRegPromoCodeEnd[];',
            "int main() {",
            "    gCode.assign(gRegPromoCode, gRegPromoCodeEnd);",
            f'    char a0[] = "x", a1[] = "-", a2[] = "{args.trials}", a3[] = "0x{args.seed:X}";',
            f'    char a4[] = "{args.only or ""}";',
            "    char* argv[] = {a0, a1, a2, a3, a4, nullptr};",
            f"    return GuestOverrideEquivalenceMain({5 if args.only else 4}, argv);",
            "}",
            "",
        ]), encoding="utf-8")
        flags = ["-std=c++20", *SWITCH_FLAGS.split(), "-static", "-nostartfiles",
                 "-DOOT3D_GUEST_OVERRIDE_EQ_EMBEDDED_CODE=1", *mode_flags,
                 *args.extra_flags.split()]
        units = [main_unit, HERE / "qemu_aarch64_test_runtime.cpp", *sources]
        exe = out / "register_promotion_equivalence.elf"
        builds = [(source, out / (source.stem + ".aarch64.o"), source in part_sources)
                  for source in [*part_sources, *units]]
        compile_all(cxx, flags, includes, builds, args.jobs, args.object_cache)
        subprocess.run([cxx, *flags, *(str(obj) for _, obj, _ in builds), "-o", str(exe)],
                       check=True)
        qemu = shutil.which("qemu-aarch64-static") or shutil.which("qemu-aarch64")
        if qemu is None:
            raise SystemExit("qemu-aarch64 not found")
        return subprocess.run([qemu, str(exe)]).returncode

    cxx = args.cxx or "g++"
    flags = ["-std=c++20", "-O2", "-fno-fast-math", "-ffp-contract=off", "-w", *mode_flags,
             *args.extra_flags.split()]
    builds = [(source, out / (source.stem + ".o"), source in part_sources)
              for source in [*part_sources, HERE / "guest_override_equivalence.cpp", *sources]]
    compile_all(cxx, flags, includes, builds, args.jobs, args.object_cache)
    exe = out / "register_promotion_equivalence"
    subprocess.run([cxx, *(str(obj) for _, obj, _ in builds), "-o", str(exe)], check=True)
    command = [str(exe), str(args.code.expanduser()), str(args.trials), str(args.seed)]
    if args.only:
        command.append(args.only)
    return subprocess.run(command).returncode


if __name__ == "__main__":
    sys.exit(main())
