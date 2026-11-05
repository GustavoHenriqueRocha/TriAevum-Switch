#!/usr/bin/env python3
"""Make a small copy of an Azahar/Citra texture pack for the Switch port.

The Switch never draws more than 2x the original resolution in the world and 3x
on the HUD (L3+R3 > Video > HD texture size; the HUD cap is fixed), so a 1080p
(4x) or 4K (8x) pack carries pixels the console reduces away every time it
loads them. This script reduces every texture to those sizes once, on the PC:
the SD card holds a fraction of the pack and the Switch decodes smaller PNGs.

    python3 shrink_texture_pack.py "OOT3D 4K 5.0a (1080p).zip" textures

PACK is the downloaded zip or a folder extracted from it. OUTPUT must be new or
empty; copy its contents (the folders and pack.json) to switch/oot3dre/textures/.

  --world N      world, character and effect textures, N x the original (2)
  --hud N        HUD, menus and maps (UI, MAPS folders), N x the original (3)
  --parts LIST   comma-separated: hud, models, world, or all (all)
                 hud = UI, MAPS; models = CHARACTERS, CREATURES, LINK;
                 world = everything else
  --language L   keep the UI/<folder> whose name contains L (english); the
                 English art is in UI itself, so the default keeps none of the
                 language folders; "all" keeps every one

A texture already within its size is copied unchanged; larger ones are
area-averaged with premultiplied alpha, so cut-out edges keep their colour.
Mip levels above 0 are dropped (the Switch builds its own). Only files under
load/textures/<title id>/ are read; nothing else in the archive is copied.

Needs Python 3.9+ and Pillow (pip install pillow). The output is your own copy
of the pack for your own console: the pack's license applies to it unchanged.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import io
import os
import re
import sys
import zipfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

TEXTURE_NAME = re.compile(r"^tex1_(\d+)x(\d+)_([0-9A-Fa-f]{16})_(\d+)_mip(\d+)\.png$")
TITLE_FOLDER = re.compile(r"(^|/)load/textures/([0-9A-Fa-f]{16})/")
HUD_FOLDERS = {"UI", "MAPS"}
MODEL_FOLDERS = {"CHARACTERS", "CREATURES", "LINK"}
PARTS = ("hud", "models", "world")


@dataclass(frozen=True)
class Job:
    relative: str        # path below the pack root, "/"-separated
    source: str          # zip member name or file path
    native: tuple[int, int] | None
    scale: int | None    # None: copy unchanged


def part_of(relative: PurePosixPath) -> str:
    top = relative.parts[0].upper() if len(relative.parts) > 1 else ""
    if top in HUD_FOLDERS:
        return "hud"
    if top in MODEL_FOLDERS:
        return "models"
    return "world"


def language_kept(relative: PurePosixPath, language: str) -> bool:
    # UI/<folder>/... holds one language's art (Henriko: UI/GERMAN; Vanilla
    # UI/HUD: UI/02_EU_ENGLISH). The same texture differs per language.
    if len(relative.parts) < 3 or relative.parts[0].upper() != "UI":
        return True
    return language == "all" or language.lower() in relative.parts[1].lower()


def target_size(width: int, height: int, native: tuple[int, int], scale: int) -> tuple[int, int]:
    limit_w, limit_h = native[0] * scale, native[1] * scale
    if width <= limit_w and height <= limit_h:
        return width, height
    factor = min(limit_w / width, limit_h / height)
    return max(1, round(width * factor)), max(1, round(height * factor))


def shrink_png(data: bytes, native: tuple[int, int], scale: int) -> bytes:
    from PIL import Image

    with Image.open(io.BytesIO(data)) as image:
        size = target_size(image.width, image.height, native, scale)
        if size == (image.width, image.height):
            return data
        if image.mode == "P":
            image = image.convert("RGBA" if "transparency" in image.info else "RGB")
        elif image.mode not in ("RGB", "RGBA", "L", "LA"):
            image = image.convert("RGBA")
        # BOX averages every source pixel a target pixel covers (any ratio);
        # Pillow resamples RGBA/LA with premultiplied alpha.
        reduced = image.resize(size, Image.Resampling.BOX)
        out = io.BytesIO()
        reduced.save(out, format="PNG", compress_level=6)
        return out.getvalue()


def find_root(names: list[str], folder: str | None = None) -> str:
    """The pack's load/textures/<title id>/ prefix (the one with pack.json), or
    FOLDER itself when it is already the texture folder (pack.json at its top)."""
    roots = {}
    for name in names:
        match = TITLE_FOLDER.search(name)
        if match:
            prefix = name[:match.end()]
            roots[prefix] = roots.get(prefix, False) or name == prefix + "pack.json"
    if not roots and folder is not None and folder.rstrip("/") + "/pack.json" in names:
        return folder.rstrip("/") + "/"
    if not roots:
        raise SystemExit("no load/textures/<title id>/ folder found: is this an Azahar/Citra texture pack?")
    with_pack = [root for root, has in roots.items() if has]
    candidates = with_pack or sorted(roots)
    if len(candidates) > 1:
        raise SystemExit("several texture folders found, extract the one for your game first: " +
                         ", ".join(candidates))
    return candidates[0]


def plan(names: list[str], root: str, args: argparse.Namespace) -> tuple[list[Job], dict[str, int]]:
    jobs: list[Job] = []
    skipped = {"other parts": 0, "other languages": 0, "mip levels": 0, "other files": 0}
    for name in names:
        if not name.startswith(root) or name.endswith("/"):
            continue
        relative = PurePosixPath(name[len(root):])
        if relative.name == "pack.json" and len(relative.parts) == 1:
            jobs.append(Job(str(relative), name, None, None))
            continue
        if relative.suffix.lower() != ".png":
            skipped["other files"] += 1
            continue
        part = part_of(relative)
        if part not in args.parts:
            skipped["other parts"] += 1
            continue
        if not language_kept(relative, args.language):
            skipped["other languages"] += 1
            continue
        match = TEXTURE_NAME.match(relative.name)
        if match and int(match.group(5)) > 0:
            skipped["mip levels"] += 1
            continue
        native = (int(match.group(1)), int(match.group(2))) if match else None
        scale = (args.hud if part == "hud" else args.world) if native else None
        jobs.append(Job(str(relative), name, native, scale))
    # A kept language folder's art replaces the same texture elsewhere (the
    # Switch would otherwise pick either).
    def localized(job: Job) -> bool:
        parts = PurePosixPath(job.relative).parts
        return len(parts) >= 3 and parts[0].upper() == "UI"
    names_localized = {PurePosixPath(job.relative).name for job in jobs if localized(job)}
    kept = [job for job in jobs if localized(job) or PurePosixPath(job.relative).name not in names_localized]
    skipped["replaced by language art"] = len(jobs) - len(kept)
    return kept, skipped


def run_job(pack: str, output: str, job: Job) -> tuple[str, int, int]:
    if os.path.isdir(pack):
        data = Path(job.source).read_bytes()
    else:
        with zipfile.ZipFile(pack) as archive:
            data = archive.read(job.source)
    result = data if job.scale is None else shrink_png(data, job.native, job.scale)
    destination = Path(output, *PurePosixPath(job.relative).parts)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(result)
    return job.relative, len(data), len(result)


def list_pack(pack: Path) -> list[str]:
    if pack.is_dir():
        return [p.as_posix() for p in sorted(pack.rglob("*")) if p.is_file()]
    with zipfile.ZipFile(pack) as archive:
        return archive.namelist()


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("pack", type=Path, help="the downloaded zip, or a folder extracted from it")
    parser.add_argument("output", type=Path, help="new folder for the small pack")
    parser.add_argument("--world", type=int, default=2, help="world/model size, x the original (2)")
    parser.add_argument("--hud", type=int, default=3, help="HUD/menu/map size, x the original (3)")
    parser.add_argument("--parts", default="all", help="hud,models,world or all (all)")
    parser.add_argument("--language", default="english", help="UI language folder to keep, or all (english)")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    args.parts = set(PARTS) if args.parts == "all" else {p.strip() for p in args.parts.split(",")}
    if not args.parts <= set(PARTS):
        parser.error(f"--parts takes {', '.join(PARTS)} or all")
    if args.world < 1 or args.hud < 1:
        parser.error("--world and --hud are at least 1")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        import PIL  # noqa: F401
    except ImportError:
        raise SystemExit("this needs Pillow: pip install pillow")
    if args.output.exists() and any(args.output.iterdir()):
        raise SystemExit(f"{args.output} is not empty")
    names = list_pack(args.pack)
    root = find_root(names, args.pack.as_posix() if args.pack.is_dir() else None)
    jobs, skipped = plan(names, root, args)
    if not any(job.scale for job in jobs):
        raise SystemExit("no textures selected")
    totals: dict[str, list[int]] = {}
    with concurrent.futures.ProcessPoolExecutor(max(1, args.jobs)) as pool:
        futures = [pool.submit(run_job, str(args.pack), str(args.output), job) for job in jobs]
        for done, future in enumerate(concurrent.futures.as_completed(futures), 1):
            relative, before, after = future.result()
            part = "pack.json" if relative == "pack.json" else part_of(PurePosixPath(relative))
            entry = totals.setdefault(part, [0, 0, 0])
            entry[0] += 1
            entry[1] += before
            entry[2] += after
            if done % 200 == 0 or done == len(futures):
                print(f"\r{done}/{len(futures)} files", end="", flush=True)
    print()
    mib = 1 << 20
    for part in (*PARTS, "pack.json"):
        if part in totals:
            count, before, after = totals[part]
            print(f"  {part:9} {count:5} files  {before / mib:8.1f} MiB -> {after / mib:7.1f} MiB")
    before = sum(t[1] for t in totals.values())
    after = sum(t[2] for t in totals.values())
    print(f"  total     {sum(t[0] for t in totals.values()):5} files  {before / mib:8.1f} MiB -> {after / mib:7.1f} MiB")
    left_out = ", ".join(f"{count} {what}" for what, count in skipped.items() if count)
    if left_out:
        print(f"  left out: {left_out}")
    print(f"Copy the contents of {args.output} to switch/oot3dre/textures/ on the SD card.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
