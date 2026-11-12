"""Wires the native overrides (guest_overrides.json) into the generated C++.

For each listed entry, inserts at the start of the translated function
Execute_<name>_<ENTRY> a prologue that calls
GuestOverrides::Override_<ENTRY> when the function is called at its own
entry; if the native version declines, the translation runs unchanged. Adds the
#include of oot3d_native_guest_overrides.h to the affected shards.

Block overrides (items with "function"): instead of the function entry,
the prologue goes right after the block_<ENTRY> label inside the
translated function <function>. The native version runs the hot loop/section and returns
the resume PC (one of those listed in "resume"), where the translated code
continues via goto, or an exit flow (block limit etc.). If it declines, the
translated block runs normally.

Idempotent ("guest-override" markers) and --remove undoes it. Run after
generate_whole_aot_cpp.py (or use its --guest-overrides option); only the
shards containing listed functions change, so the rebuild is small.

Items with "inline": true use a single-line prologue, appended to the
label line (block) or signature line (function), and the #include takes the blank line
after the shard's includes: no shard line changes number. So
GCC PGO (which checks, per function, the CFG checksum and the start-line
checksum) stays valid for every other function in the shard; only the function
receiving the prologue loses its profile. Older items stay in the
multi-line format, which is what the current PGO collection saw.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path
from typing import Iterable

INCLUDE_LINE = '#include "oot3d_native_guest_overrides.h" // guest-override'
# Inline format: the #include replaces a blank line and the prologue goes at the end
# of an existing line, between these markers (strip restores the line).
INLINE_INCLUDE_LINE = '#include "oot3d_native_guest_overrides.h" // guest-override-inline'
INLINE_BEGIN = " /*guest-override-inline*/ "
PROLOGUE_BEGIN = "    // guest-override begin"
PROLOGUE_END = "    // guest-override end"
DEFAULT_MANIFEST = Path(__file__).resolve().with_name("guest_overrides.json")
MANIFEST_NAME = "whole_aot_cpp_manifest.json"


# Block overrides read by load_manifest: block entry ->
# (entry of the function containing the block, resume PCs). apply() receives only
# the entries (so generate_whole_aot_cpp.py need not know about blocks) and
# looks up this table.
BLOCK_OVERRIDES: dict[int, tuple[int, tuple[int, ...]]] = {}
# Entries (function or block) with an inline prologue.
INLINE_OVERRIDES: set[int] = set()


def load_manifest(path: Path, include_disabled: bool = False) -> list[dict[str, object]]:
    """Manifest items. Those with "disabled" (the reason, as text) are not
    wired into the generated C++; the differential test reads them with include_disabled
    to keep proving equivalence."""
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("format") != "oot3d_guest_overrides_v1":
        raise ValueError(f"{path}: unknown format")
    entries = []
    for item in data["overrides"]:
        if item.get("disabled") and not include_disabled:
            continue
        entry = int(str(item["entry"]), 0)
        parsed = {**item, "entry": entry}
        if "function" in item:
            function = int(str(item["function"]), 0)
            resume = tuple(int(str(pc), 0) for pc in item.get("resume", []))
            if not resume:
                raise ValueError(f"{path}: block 0x{entry:08X} without resume PCs")
            parsed.update(function=function, resume=list(resume))
            BLOCK_OVERRIDES[entry] = (function, resume)
        if item.get("inline", False):
            INLINE_OVERRIDES.add(entry)
        entries.append(parsed)
    return entries


def definition_pattern(entry: int) -> re.Pattern[str]:
    # A giant function generated without a hot list (no PGO profile yet, e.g.
    # the PGO=generate build) carries whole_aot_cpp.GIANT_COLD_ATTRIBUTE at
    # the start of its signature line.
    return re.compile(
        r"^(?:\[\[gnu::optimize\([^\]]*\)\]\] )?Oot3dWholeAotFlow (Execute_\w+_%08X)\($" % entry)


def find_definition(lines: list[str], entry: int) -> tuple[int, str] | None:
    """Index of the definition's signature line (not the declaration's)."""
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
    """A multi-line prologue on a single line (without the markers)."""
    body = [line.strip() for line in lines if line not in (PROLOGUE_BEGIN, PROLOGUE_END)]
    return INLINE_BEGIN + " ".join(body)


def function_end(lines: list[str], index: int) -> int:
    return lines.index("}", index)


def patch_block(lines: list[str], entry: int, function: int,
                resume: Iterable[int], in_line: bool = False) -> bool:
    """Inserts the block prologue after the function's block_<entry> label."""
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
            f"block 0x{entry:08X} or resume missing in function 0x{function:08X}")
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
            # Inline prologues only: the include takes the first blank line
            # after the includes, without shifting the rest of the shard.
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
            "entries without a definition in the generated C++: "
            + ", ".join(f"0x{entry:08X}" for entry in sorted(missing)))
    # The generator manifest stores each file's SHA-256 and the product
    # audit checks those hashes: record the changed shards and the list of
    # overrides, so it is explicit in the manifest.
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
                        help="directory with oot3d_whole_aot_shard_*.cpp")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--remove", action="store_true",
                        help="removes all prologues and includes")
    args = parser.parse_args()
    entries = [item["entry"] for item in load_manifest(args.manifest)]
    located = apply(args.generated, entries, remove=args.remove)
    if args.remove:
        print("native overrides removed from the generated C++")
    for entry, name in sorted(located.items()):
        print(f"0x{entry:08X} -> {name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
