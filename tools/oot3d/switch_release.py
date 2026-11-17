#!/usr/bin/env python3
"""Release do port para Switch: release.json do atualizador + zip de instalação.

O stage é uma pasta igual a sdmc:/switch/oot3dre/ (NRO, shadercache/deko3d.pack,
resources.o2r, mods/, textures/, config/ padrão). Sai em <out>/:

- release.json (oot3dre_switch_release_v1, SWITCH_PORT.md) com os arquivos que o
  atualizador do jogo troca, e cada um deles como asset (nome = último componente);
- oot3dre-<versão>-switch.zip com o stage inteiro em switch/oot3dre/ (instalação
  nova: extrair na raiz do SD e pôr a dump .3ds em switch/oot3dre/);
- SHA256SUMS.txt.

Nunca entra: game/ (o jogo convertido da dump), saves, logs, a trava da importação,
o token do atualizador e o que é gerado do mod TopScreen (CC BY-NC-ND).

Canal: um release novo sai como "test" (pré-release no GitHub); depois do teste no
console, `promover` reescreve o release.json como "stable" e tira o pré-release.

    switch_release.py montar --stage S --out O --version 1.0.0 --build 1
    switch_release.py publicar --out O [--repo dono/nome]
    switch_release.py promover --out O [--repo dono/nome]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

FORMAT = "oot3dre_switch_release_v1"
REPO = "GustavoHenriqueRocha/oot3dre-pessoal"
NRO = "oot3d_native_game.nro"
# O que o atualizador troca (o resto só vem no zip).
UPDATER_FILES = (NRO, "shadercache/deko3d.pack", "resources.o2r")
FORBIDDEN_DIRS = ("game", "savedata", "savedata-backups", "savestates", "logs", "lab", "bench", "update")
FORBIDDEN_CONFIG = ("no_rom_import", "update_token", "atlas_overrides.o3tu", "font_coverage.zip")


def nacp_of(nro: bytes) -> tuple[str, str, str]:
    """(nome, autor, versão) da NACP embutida no NRO."""
    if nro[0x10:0x14] != b"NRO0":
        raise ValueError("não é um NRO")
    size = struct.unpack_from("<I", nro, 0x18)[0]
    if nro[size:size + 4] != b"ASET":
        raise ValueError("NRO sem assets (NACP)")
    offset, length = struct.unpack_from("<QQ", nro, size + 0x18)
    nacp = nro[size + offset:size + offset + length]
    if len(nacp) < 0x3070:
        raise ValueError("NACP curta")
    text = lambda b: b.split(b"\0", 1)[0].decode("utf-8")
    return text(nacp[0:0x200]), text(nacp[0x200:0x300]), text(nacp[0x3060:0x3070])


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def staged_files(stage: Path) -> list[str]:
    files = sorted(p.relative_to(stage).as_posix() for p in stage.rglob("*") if p.is_file())
    for rel in files:
        parts = rel.split("/")
        if parts[0] in FORBIDDEN_DIRS or (parts[0] == "config" and parts[-1] in FORBIDDEN_CONFIG):
            raise ValueError(f"{rel} não pode ir num release")
    return files


def write_manifest(out: Path, manifest: dict) -> None:
    (out / "release.json").write_text(json.dumps(manifest, indent=2) + "\n")


def build(stage: Path, out: Path, version: str, build_number: int, channel: str) -> dict:
    if build_number < 1:
        raise ValueError("build tem de ser > 0")
    if channel not in ("test", "stable"):
        raise ValueError("canal é test ou stable")
    files = staged_files(stage)
    if NRO not in files:
        raise ValueError(f"falta {NRO}")
    nro_version = nacp_of((stage / NRO).read_bytes())[2]
    if nro_version != version:
        raise ValueError(f"o NRO é da versão {nro_version}, não {version}")
    out.mkdir(parents=True, exist_ok=True)
    manifest = {"format": FORMAT, "version": version, "build": build_number, "channel": channel, "files": []}
    for rel in UPDATER_FILES:
        if rel not in files:
            continue
        source = stage / rel
        manifest["files"].append({"path": rel, "size": source.stat().st_size, "sha256": sha256(source)})
        shutil.copyfile(source, out / Path(rel).name)
    write_manifest(out, manifest)
    archive = out / f"oot3dre-{version}-switch.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
        for rel in files:
            z.write(stage / rel, "switch/oot3dre/" + rel)
    write_sums(out, manifest)
    return manifest


def write_sums(out: Path, manifest: dict) -> None:
    names = [Path(n).name for n in assets(out, manifest) if not n.endswith("SHA256SUMS.txt")]
    (out / "SHA256SUMS.txt").write_text("".join(f"{sha256(out / n)}  {n}\n" for n in names))


def promote(out: Path) -> dict:
    manifest = json.loads((out / "release.json").read_text())
    manifest["channel"] = "stable"
    write_manifest(out, manifest)
    write_sums(out, manifest)
    return manifest


def assets(out: Path, manifest: dict) -> list[str]:
    names = [Path(f["path"]).name for f in manifest["files"]]
    names += ["release.json", f"oot3dre-{manifest['version']}-switch.zip", "SHA256SUMS.txt"]
    return [str(out / n) for n in names]


def gh(*args: str) -> None:
    subprocess.run(["gh", *args], check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    m = sub.add_parser("montar")
    m.add_argument("--stage", type=Path, required=True)
    m.add_argument("--out", type=Path, required=True)
    m.add_argument("--version", required=True)
    m.add_argument("--build", type=int, required=True)
    for name in ("publicar", "promover"):
        p = sub.add_parser(name)
        p.add_argument("--out", type=Path, required=True)
        p.add_argument("--repo", default=REPO)
        p.add_argument("--notes", default="")
    args = parser.parse_args()
    if args.command == "montar":
        manifest = build(args.stage, args.out, args.version, args.build, "test")
        print(json.dumps(manifest, indent=2))
        return 0
    if args.command == "publicar":
        manifest = json.loads((args.out / "release.json").read_text())
        if manifest["channel"] != "test":
            raise SystemExit("release novo sai como test; promova depois do teste no console")
        tag = "v" + manifest["version"]
        gh("release", "create", tag, "--repo", args.repo, "--prerelease", "--title", tag,
           "--notes", args.notes or f"build {manifest['build']} (teste)", *assets(args.out, manifest))
        return 0
    manifest = promote(args.out)
    tag = "v" + manifest["version"]
    gh("release", "upload", tag, str(args.out / "release.json"), "--repo", args.repo, "--clobber")
    gh("release", "upload", tag, str(args.out / "SHA256SUMS.txt"), "--repo", args.repo, "--clobber")
    gh("release", "edit", tag, "--repo", args.repo, "--prerelease=false", "--latest")
    return 0


if __name__ == "__main__":
    sys.exit(main())
