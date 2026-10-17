"""Diferencial da promoção de registradores (generate_whole_aot_cpp --promote-registers).

Extrai as mesmas funções traduzidas reais (as de guest_overrides.json, as
funções que contêm as substituições de bloco, entradas extras com --entry e o
fecho das chamadas de todas elas) de dois diretórios gerados: um sem a
promoção (--baseline) e um com ela (--promoted). Cada lado vai para um
namespace (Base / Promoted) do mesmo executável, e o harness de
guest_override_equivalence.cpp roda os dois a partir de cópias idênticas de
estado, memória, orçamento, ganchos e rastreio, exigindo tudo igual no fim:
GPRs, CPSR, FPSCR, VFP, fluxo, BlocksRemaining/BlocksConsumed, estatísticas,
sequência de ganchos, bytes e gerações de escrita (global e por página) e a
impressão digital de escrita rastreada.

Por padrão os prólogos das substituições nativas são retirados (só C++
traduzido); --keep-overrides mantém os dois lados com os prólogos (testa a
sincronização dos locais em volta da chamada nativa).

Host (padrão):
  python run_register_promotion_equivalence.py --baseline <gen-base> \\
      --promoted <gen-promo> --code ~/oot3d-work/extract/exefs/code.bin \\
      --nlohmann ~/oot3d-work/build-switch/_deps/nlohmann_json-src/include \\
      --output /tmp/regpromo-eq --trials 200000
AArch64 sob qemu-aarch64 (devkitA64, mesmas opções dos shards do Switch):
  ... --qemu [--cxx /opt/devkitpro/devkitA64/bin/aarch64-none-elf-g++]
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

import apply_guest_overrides as overrides

HERE = Path(__file__).resolve().parent
RUNTIME = HERE.parent / "native_game_runtime"
CALL = re.compile(r"\bExecute_\w+_([0-9A-F]{8})\(frame, context, state")
DEVKIT_CXX = "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-g++"
SWITCH_FLAGS = ("-march=armv8-a+crc+crypto -mtune=cortex-a57 -O3 -ffunction-sections "
                "-fdata-sections -D__SWITCH__ -finline-functions -fno-fast-math "
                "-ffp-contract=off -w")


def shards(generated: Path, keep: bool) -> list[list[str]]:
    result = []
    for path in sorted(generated.glob("oot3d_whole_aot_shard_*.cpp")):
        lines = path.read_text(encoding="utf-8").split("\n")
        result.append(lines if keep else overrides.strip(lines))
    return result


def extract(texts: list[list[str]], roots: list[int]) -> tuple[list[str], dict[int, str]]:
    symbols: dict[int, str] = {}
    bodies: dict[int, list[str]] = {}
    pending = list(roots)
    while pending:
        entry = pending.pop()
        if entry in bodies:
            continue
        for lines in texts:
            found = overrides.find_definition(lines, entry)
            if found is None:
                continue
            index, symbol = found
            bodies[entry] = lines[index:overrides.function_end(lines, index) + 1]
            symbols[entry] = symbol
            pending.extend(int(callee, 16) for callee in CALL.findall("\n".join(bodies[entry])))
            break
        else:
            raise SystemExit(f"definição de 0x{entry:08X} não encontrada")
    return [line for entry in sorted(bodies) for line in bodies[entry] + [""]], symbols


def render_fixture(baseline: Path, promoted: Path, cases: list[tuple[int, int, str]],
                   keep: bool) -> str:
    roots = sorted({function for _, function, _ in cases})
    out = ["// Gerado por run_register_promotion_equivalence.py; não editar."]
    symbols = {}
    uses_indirect = False
    for namespace, generated in (("Base", baseline), ("Promoted", promoted)):
        body, names = extract(shards(generated.expanduser(), keep), roots)
        symbols[namespace] = names
        text = "\n".join(body)
        uses_indirect |= "ExecuteOot3dWholeAotIndirect(" in text
        out.append(f"namespace {namespace} {{")
        for entry, symbol in sorted(names.items()):
            out.append(f"Oot3dWholeAotFlow {symbol}(Oot3dWholeAotFrame& frame, "
                       "Oot3dWholeAotContext& context, Oot3dAotArchitecturalState& state, "
                       "uint32_t entryPc);")
        out += [text, f"}} // namespace {namespace}", ""]
    if symbols["Base"] != symbols["Promoted"]:
        raise SystemExit("os dois diretórios não têm as mesmas funções")
    if uses_indirect:
        # Chamadas indiretas saem do fecho: os dois lados recebem o mesmo fluxo.
        out[1:1] = [
            "Oot3dWholeAotFlow ExecuteOot3dWholeAotIndirect(uint32_t pc, Oot3dWholeAotFrame&,",
            "    Oot3dWholeAotContext& context, Oot3dAotArchitecturalState&) {",
            "    ++context.Stats.UnsupportedExits;",
            "    return {Oot3dWholeAotFlowKind::Unsupported, pc, 0U};",
            "}",
        ]
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
        "};",
        "inline constexpr OverrideCase kOverrideCases[] = {",
    ]
    for entry, function, name in cases:
        symbol = symbols["Base"][function]
        out.append(f'    {{0x{entry:08X}U, "{name}", &Base::{symbol}, nullptr, '
                   f"&Promoted::{symbol}}},")
    out += ["};", ""]
    return "\n".join(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--promoted", type=Path, required=True)
    parser.add_argument("--code", type=Path, required=True, help="code.bin")
    parser.add_argument("--nlohmann", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, default=overrides.DEFAULT_MANIFEST)
    parser.add_argument("--entry", action="append", default=[],
                        type=lambda value: int(value, 0),
                        help="entrada de função extra (repetível)")
    parser.add_argument("--keep-overrides", action="store_true")
    parser.add_argument("--trials", type=int, default=200000)
    parser.add_argument("--seed", type=lambda value: int(value, 0), default=0x5EED)
    parser.add_argument("--only", help="entrada hex para testar só um caso")
    parser.add_argument("--qemu", action="store_true",
                        help="compila para AArch64 (devkitA64) e roda sob qemu-aarch64")
    parser.add_argument("--cxx", default=None)
    parser.add_argument("--extra-flags", default="")
    args = parser.parse_args()

    cases: list[tuple[int, int, str]] = []
    for item in overrides.load_manifest(args.manifest):
        entry = int(item["entry"])
        name = str(item.get("name", ""))
        if "function" in item:
            function = int(item["function"])
            cases.append((entry, function, name))          # a partir do bloco
            cases.append((function, function, name.split(":")[0]))
        else:
            cases.append((entry, entry, name))
    cases += [(entry, entry, "extra") for entry in args.entry]
    seen = set()
    cases = [case for case in cases if not (case[0] in seen or seen.add(case[0]))]

    out = args.output
    out.mkdir(parents=True, exist_ok=True)
    (out / "guest_override_fixture.inc").write_text(
        render_fixture(args.baseline, args.promoted, cases, args.keep_overrides),
        encoding="utf-8")
    upstream = HERE / "upstream"
    includes = [out, RUNTIME, HERE, upstream, upstream / "recomp", HERE / "mass_cpp",
                args.nlohmann.expanduser()]
    sources = [
        RUNTIME / "oot3d_native_guest_overrides.cpp",
        RUNTIME / "oot3d_native_a32_memory.cpp",
        RUNTIME / "oot3d_memory_bus_abi.cpp",
        HERE / "oot3d_native_a32_vfp_ops.cpp",
        upstream / "recomp" / "a32_vfp_binary64.cpp",
        upstream / "recomp" / "a32_vfp_transport.cpp",
    ]
    if args.qemu:
        cxx = args.cxx or DEVKIT_CXX
        code = args.code.expanduser().resolve()
        main_unit = out / "register_promotion_qemu_main.cpp"
        main_unit.write_text("\n".join([
            "// Gerado: harness sob qemu-aarch64 (sem argv nem arquivos).",
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
                 "-DOOT3D_GUEST_OVERRIDE_EQ_EMBEDDED_CODE=1", *args.extra_flags.split()]
        units = [main_unit, HERE / "qemu_aarch64_test_runtime.cpp", *sources]
        exe = out / "register_promotion_equivalence.elf"
        objects = []
        for source in units:
            obj = out / (source.stem + ".aarch64.o")
            subprocess.run([cxx, *flags, *(f"-I{p}" for p in includes), "-c", str(source),
                            "-o", str(obj)], check=True)
            objects.append(str(obj))
        subprocess.run([cxx, *flags, *objects, "-o", str(exe)], check=True)
        qemu = shutil.which("qemu-aarch64-static") or shutil.which("qemu-aarch64")
        if qemu is None:
            raise SystemExit("qemu-aarch64 não encontrado")
        return subprocess.run([qemu, str(exe)]).returncode

    cxx = args.cxx or "g++"
    flags = ["-std=c++20", "-O2", "-fno-fast-math", "-ffp-contract=off", "-w",
             *args.extra_flags.split()]
    objects = []
    for source in [HERE / "guest_override_equivalence.cpp", *sources]:
        obj = out / (source.stem + ".o")
        subprocess.run([cxx, *flags, *(f"-I{p}" for p in includes), "-c", str(source),
                        "-o", str(obj)], check=True)
        objects.append(str(obj))
    exe = out / "register_promotion_equivalence"
    subprocess.run([cxx, *objects, "-o", str(exe)], check=True)
    command = [str(exe), str(args.code.expanduser()), str(args.trials), str(args.seed)]
    if args.only:
        command.append(args.only)
    return subprocess.run(command).returncode


if __name__ == "__main__":
    sys.exit(main())
