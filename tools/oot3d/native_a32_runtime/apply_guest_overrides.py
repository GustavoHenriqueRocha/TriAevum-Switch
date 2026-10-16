"""Liga as substituições nativas (guest_overrides.json) no C++ gerado.

Para cada entrada listada, insere no início da função traduzida
Execute_<nome>_<ENTRADA> um prólogo que chama
GuestOverrides::Override_<ENTRADA> quando a função é chamada pela própria
entrada; se a versão nativa recusar, a tradução segue intacta. Acrescenta o
#include de oot3d_native_guest_overrides.h nos shards afetados.

É idempotente (marcadores "guest-override") e --remove desfaz. Rodar depois
de generate_whole_aot_cpp.py (ou usar a opção --guest-overrides dele); só os
shards que contêm funções listadas mudam, então a recompilação é pequena.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path
from typing import Iterable

INCLUDE_LINE = '#include "oot3d_native_guest_overrides.h" // guest-override'
PROLOGUE_BEGIN = "    // guest-override begin"
PROLOGUE_END = "    // guest-override end"
DEFAULT_MANIFEST = Path(__file__).resolve().with_name("guest_overrides.json")
MANIFEST_NAME = "whole_aot_cpp_manifest.json"


def load_manifest(path: Path) -> list[dict[str, object]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("format") != "oot3d_guest_overrides_v1":
        raise ValueError(f"{path}: formato desconhecido")
    entries = []
    for item in data["overrides"]:
        entry = int(str(item["entry"]), 0)
        entries.append({**item, "entry": entry})
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


def strip(lines: list[str]) -> list[str]:
    out: list[str] = []
    skipping = False
    for line in lines:
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
    for entry in sorted(set(entries)):
        found = find_definition(lines, entry)
        if found is None:
            continue
        index, _ = found
        lines[index + 4:index + 4] = prologue(entry)
        patched.append(entry)
    if patched:
        first_include = next(i for i, line in enumerate(lines) if line.startswith("#include"))
        lines.insert(first_include + 1, INCLUDE_LINE)
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
