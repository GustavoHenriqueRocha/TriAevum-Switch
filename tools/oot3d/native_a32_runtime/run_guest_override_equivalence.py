"""Compila e roda o teste diferencial das substituições nativas.

Extrai do C++ gerado (sem prólogos de substituição) cada função listada em
guest_overrides.json e o fecho das funções que ela chama, renomeia as
listadas para Translated_<ENTRADA>, gera guest_override_fixture.inc e compila
(substituições de bloco: a função que contém o bloco entra inteira e uma
cópia dela com o prólogo do bloco vira Patched_<BLOCO>; as duas rodam a
partir do bloco)
guest_override_equivalence.cpp com os mesmos cabeçalhos do runtime e as
mesmas opções de ponto flutuante do build (-fno-fast-math -ffp-contract=off).

Com --qemu-aarch64 o mesmo teste é compilado com o devkitA64 (as opções de
ponto flutuante e de CPU dos shards do Switch, VFP nativo de ARM64 nas duas
versões) e roda sob qemu-aarch64, com o code.bin embutido no binário.

Exemplo:
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
    """Definições (texto) do fecho de chamadas e símbolo de cada entrada."""
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
            raise SystemExit(f"definição de 0x{entry:08X} não encontrada em {generated}")
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
    # Substituição de bloco: uma cópia da função traduzida com o prólogo do
    # bloco (Patched_<BLOCO>) contra a original, as duas começando no bloco.
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
        "// Gerado por run_guest_override_equivalence.py; não editar.",
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
        "    // Substituição de bloco: a função com o prólogo do bloco (Native nulo).",
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
    parser.add_argument("--code", type=Path, required=True, help="code.bin canônico")
    parser.add_argument("--nlohmann", type=Path, required=True,
                        help="diretório include do nlohmann_json")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, default=overrides.DEFAULT_MANIFEST)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--trials", type=int, default=1000000)
    parser.add_argument("--seed", type=lambda value: int(value, 0), default=0x5EED)
    parser.add_argument("--only", help="entrada hex para testar só uma função")
    parser.add_argument("--extra-flags", default="", help="opções extras do compilador")
    parser.add_argument("--qemu-aarch64", metavar="QEMU",
                        help="qemu-aarch64 (user mode): compila com --qemu-cxx e roda nele")
    parser.add_argument("--qemu-cxx",
                        default="/opt/devkitpro/devkitA64/bin/aarch64-none-elf-g++")
    parser.add_argument("--codegen-cxx",
                        help="compilador ARM64 (ex.: aarch64-none-elf-g++): só compila a "
                             "versão traduzida e a nativa e conta as instruções geradas")
    parser.add_argument("--codegen-flags",
                        default="-march=armv8-a+crc+crypto -mtune=cortex-a57 -O3 "
                                "-ffunction-sections -fdata-sections -fPIC -D__SWITCH__ "
                                "-finline-functions -fno-fast-math -ffp-contract=off -w",
                        help="opções da regra dos shards no build.ninja do Switch")
    args = parser.parse_args()

    manifest = overrides.load_manifest(args.manifest)
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
        # O teste e as substituições sempre; o suporte só se faltar ou mudar.
        stale = (not obj.exists() or source.name.startswith(("guest", "oot3d_native_guest"))
                 or obj.stat().st_mtime < source.stat().st_mtime)
        if stale:
            subprocess.run([args.cxx, *flags, *(f"-I{p}" for p in includes), "-c",
                            str(source), "-o", str(obj)], check=True)
        objects.append(str(obj))
    exe = args.output / "guest_override_equivalence"
    # As opções extras (ex.: -fsanitize=address,undefined) valem na ligação.
    subprocess.run([args.cxx, *args.extra_flags.split(), *objects, "-o", str(exe)], check=True)
    command = [str(exe), str(args.code.expanduser()), str(args.trials), str(args.seed)]
    if args.only:
        command.append(args.only)
    return subprocess.run(command).returncode


def run_qemu(args, fixture: Path) -> int:
    """Compila para AArch64 (devkitA64, runtime mínimo de qemu_aarch64_test_runtime)
    e roda sob qemu-aarch64. Parâmetros fixos no binário (sem argv)."""
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
        RUNTIME / "oot3d_memory_bus_abi.cpp",
        HERE / "oot3d_native_a32_vfp_ops.cpp",
        upstream / "recomp" / "a32_vfp_binary64.cpp",
        upstream / "recomp" / "a32_vfp_transport.cpp",
        embed,
    ]
    includes = [out, RUNTIME, HERE, upstream, upstream / "recomp",
                HERE / "mass_cpp", args.nlohmann.expanduser()]
    only = int(args.only, 16) if args.only else 0
    # As do build dos shards do Switch; thread_local vira global (o runtime
    # mínimo não tem TLS e o teste tem uma thread só).
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
    """Conta instruções ARM64 de cada função traduzida e da substituição."""
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
    print("entrada     traduzida(instr/bl)  nativa(instr/bl)  nome")
    for item in manifest:
        entry = int(item["entry"])
        a = counts.get(f"Translated_{entry:08X}", {"instr": 0, "bl": 0})
        b = counts.get(f"Override_{entry:08X}", {"instr": 0, "bl": 0})
        print(f"0x{entry:08X}  {a['instr']:6d}/{a['bl']:<4d}        {b['instr']:6d}/{b['bl']:<4d}     "
              f"{item.get('name', '')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
