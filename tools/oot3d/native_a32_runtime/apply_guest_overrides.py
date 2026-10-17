"""Liga as substituições nativas (guest_overrides.json) no C++ gerado.

Para cada entrada listada, insere no início da função traduzida
Execute_<nome>_<ENTRADA> um prólogo que chama
GuestOverrides::Override_<ENTRADA> quando a função é chamada pela própria
entrada; se a versão nativa recusar, a tradução segue intacta. Acrescenta o
#include de oot3d_native_guest_overrides.h nos shards afetados.

Substituições de bloco (itens com "function"): em vez da entrada da função,
o prólogo vai logo depois do rótulo block_<ENTRADA> dentro da função
traduzida <function>. A versão nativa executa o laço/trecho quente e devolve
o PC de retomada (um dos listados em "resume"), para onde o código traduzido
segue com goto, ou um fluxo de saída (limite de blocos etc.). Se recusar, o
bloco traduzido roda normalmente.

É idempotente (marcadores "guest-override") e --remove desfaz. Rodar depois
de generate_whole_aot_cpp.py (ou usar a opção --guest-overrides dele); só os
shards que contêm funções listadas mudam, então a recompilação é pequena.

Itens com "inline": true usam o prólogo numa linha só, anexado à linha do
rótulo (bloco) ou da assinatura (função), e o #include ocupa a linha vazia
depois dos includes do shard: nenhuma linha do shard muda de número. Assim o
PGO do GCC (que confere, por função, o checksum do CFG e o da linha de
início) continua valendo para todas as outras funções do shard; só a função
que recebe o prólogo perde o perfil. Os itens antigos ficam no formato de
várias linhas, que é o que a coleta de PGO atual viu.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path
from typing import Iterable

INCLUDE_LINE = '#include "oot3d_native_guest_overrides.h" // guest-override'
# Formato em linha: o #include no lugar de uma linha vazia e o prólogo no fim
# de uma linha existente, entre estes marcadores (strip restaura a linha).
INLINE_INCLUDE_LINE = '#include "oot3d_native_guest_overrides.h" // guest-override-inline'
INLINE_BEGIN = " /*guest-override-inline*/ "
PROLOGUE_BEGIN = "    // guest-override begin"
PROLOGUE_END = "    // guest-override end"
DEFAULT_MANIFEST = Path(__file__).resolve().with_name("guest_overrides.json")
MANIFEST_NAME = "whole_aot_cpp_manifest.json"


# Substituições de bloco lidas por load_manifest: entrada do bloco ->
# (entrada da função que contém o bloco, PCs de retomada). apply() recebe só
# as entradas (assim generate_whole_aot_cpp.py não precisa saber de blocos) e
# consulta esta tabela.
BLOCK_OVERRIDES: dict[int, tuple[int, tuple[int, ...]]] = {}
# Entradas (de função ou de bloco) com prólogo em linha.
INLINE_OVERRIDES: set[int] = set()


def load_manifest(path: Path) -> list[dict[str, object]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("format") != "oot3d_guest_overrides_v1":
        raise ValueError(f"{path}: formato desconhecido")
    entries = []
    for item in data["overrides"]:
        entry = int(str(item["entry"]), 0)
        parsed = {**item, "entry": entry}
        if "function" in item:
            function = int(str(item["function"]), 0)
            resume = tuple(int(str(pc), 0) for pc in item.get("resume", []))
            if not resume:
                raise ValueError(f"{path}: bloco 0x{entry:08X} sem PCs de retomada")
            parsed.update(function=function, resume=list(resume))
            BLOCK_OVERRIDES[entry] = (function, resume)
        if item.get("inline", False):
            INLINE_OVERRIDES.add(entry)
        entries.append(parsed)
    return entries


def definition_pattern(entry: int) -> re.Pattern[str]:
    return re.compile(r"^Oot3dWholeAotFlow (Execute_\w+_%08X)\($" % entry)


def find_definition(lines: list[str], entry: int) -> tuple[int, str] | None:
    """Índice da linha da assinatura da definição (não da declaração)."""
    pattern = definition_pattern(entry)
    for index, line in enumerate(lines):
        match = pattern.match(line)
        if match and index + 3 < len(lines) and lines[index + 3].rstrip().endswith(") {"):
            return index, match.group(1)
    return None


def prologue(entry: int) -> list[str]:
    return [
        PROLOGUE_BEGIN,
        f"    if (entryPc == 0x{entry:08X}U && Oot3dGuestOverridesEnabled()) {{",
        "        Oot3dWholeAotFlow nativeFlow;",
        f"        if (GuestOverrides::Override_{entry:08X}(frame, context, state, nativeFlow)) {{",
        "            return nativeFlow;",
        "        }",
        "    }",
        PROLOGUE_END,
    ]


def block_prologue(entry: int, resume: Iterable[int]) -> list[str]:
    lines = [
        PROLOGUE_BEGIN,
        "    if (Oot3dGuestOverridesEnabled()) {",
        "        commitState();",
        "        Oot3dWholeAotFlow nativeFlow;",
        "        uint32_t resumePc = 0U;",
        f"        if (GuestOverrides::Override_{entry:08X}(frame, context, state, nativeFlow, resumePc)) {{",
        "            reloadState();",
    ]
    for pc in resume:
        lines.append(f"            if (resumePc == 0x{pc:08X}U) goto block_{pc:08X};")
    lines += [
        "            return nativeFlow;",
        "        }",
        "    }",
        PROLOGUE_END,
    ]
    return lines


def inline(lines: list[str]) -> str:
    """Um prólogo de várias linhas numa linha só (sem os marcadores)."""
    body = [line.strip() for line in lines if line not in (PROLOGUE_BEGIN, PROLOGUE_END)]
    return INLINE_BEGIN + " ".join(body)


def function_end(lines: list[str], index: int) -> int:
    return lines.index("}", index)


def patch_block(lines: list[str], entry: int, function: int,
                resume: Iterable[int], in_line: bool = False) -> bool:
    """Insere o prólogo de bloco depois do rótulo block_<entry> da função."""
    found = find_definition(lines, function)
    if found is None:
        return False
    index, _ = found
    end = function_end(lines, index)
    label = f"block_{entry:08X}:"
    labels = {f"block_{pc:08X}:" for pc in resume}
    body = lines[index:end]
    if label not in body or not labels <= set(body):
        raise SystemExit(
            f"bloco 0x{entry:08X} ou retomada ausente na função 0x{function:08X}")
    at = index + body.index(label)
    if in_line:
        lines[at] += inline(block_prologue(entry, resume))
    else:
        lines[at + 1:at + 1] = block_prologue(entry, resume)
    return True


def strip(lines: list[str]) -> list[str]:
    out: list[str] = []
    skipping = False
    for line in lines:
        if line == INLINE_INCLUDE_LINE:
            out.append("")
            continue
        if INLINE_BEGIN in line:
            line = line[:line.index(INLINE_BEGIN)]
        if line == PROLOGUE_BEGIN:
            skipping = True
            continue
        if skipping:
            if line == PROLOGUE_END:
                skipping = False
            continue
        if line == INCLUDE_LINE:
            continue
        out.append(line)
    return out


def patch_text(text: str, entries: Iterable[int]) -> tuple[str, list[int]]:
    lines = strip(text.split("\n"))
    patched: list[int] = []
    multiline = False
    for entry in sorted(set(entries)):
        in_line = entry in INLINE_OVERRIDES
        if entry in BLOCK_OVERRIDES:
            function, resume = BLOCK_OVERRIDES[entry]
            if patch_block(lines, entry, function, resume, in_line):
                patched.append(entry)
                multiline |= not in_line
            continue
        found = find_definition(lines, entry)
        if found is None:
            continue
        index, _ = found
        if in_line:
            lines[index + 3] += inline(prologue(entry))
        else:
            lines[index + 4:index + 4] = prologue(entry)
            multiline = True
        patched.append(entry)
    if patched:
        first_include = next(i for i, line in enumerate(lines) if line.startswith("#include"))
        if multiline:
            lines.insert(first_include + 1, INCLUDE_LINE)
        else:
            # Só prólogos em linha: o include ocupa a primeira linha vazia
            # depois dos includes, sem deslocar o resto do shard.
            blank = next(i for i in range(first_include, len(lines)) if lines[i] == "")
            lines[blank] = INLINE_INCLUDE_LINE
    return "\n".join(lines), patched


def apply(generated: Path, entries: Iterable[int], remove: bool = False) -> dict[int, str]:
    wanted = set() if remove else set(entries)
    located: dict[int, str] = {}
    changed: dict[str, str] = {}
    for path in sorted(generated.glob("oot3d_whole_aot_shard_*.cpp")):
        original = path.read_text(encoding="utf-8")
        if not wanted and "guest-override" not in original:
            continue
        text, patched = patch_text(original, wanted)
        for entry in patched:
            located[entry] = path.name
        if text != original:
            path.write_text(text, encoding="utf-8")
            changed[path.name] = hashlib.sha256(text.encode("utf-8")).hexdigest()
    missing = wanted - set(located)
    if missing:
        raise SystemExit(
            "entradas sem definição no C++ gerado: "
            + ", ".join(f"0x{entry:08X}" for entry in sorted(missing)))
    # O manifesto do gerador guarda o SHA-256 de cada arquivo e a auditoria do
    # produto confere esses hashes: registra os shards alterados e a lista de
    # substituições, para que fique explícito no manifesto.
    manifest_path = generated / MANIFEST_NAME
    if manifest_path.is_file():
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        files = manifest.setdefault("files", {})
        for name, digest in changed.items():
            if name in files:
                files[name] = digest
        if located:
            manifest["guest_overrides"] = [
                {"entry": entry, "file": name} for entry, name in sorted(located.items())
            ]
        else:
            manifest.pop("guest_overrides", None)
        manifest_path.write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return located


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generated", type=Path, required=True,
                        help="diretório com oot3d_whole_aot_shard_*.cpp")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--remove", action="store_true",
                        help="tira todos os prólogos e includes")
    args = parser.parse_args()
    entries = [item["entry"] for item in load_manifest(args.manifest)]
    located = apply(args.generated, entries, remove=args.remove)
    if args.remove:
        print("substituições nativas removidas do C++ gerado")
    for entry, name in sorted(located.items()):
        print(f"0x{entry:08X} -> {name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
