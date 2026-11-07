#!/usr/bin/env python3
"""The built-in twins of the grass rules' textures (grass_texture_twin_table.cpp).

A ground without a grass rule of its own takes the rule of its twin texture
(GrassTwinRules), which needs both textures' signatures. A rule texture's
signature exists only once it has been drawn, so a save that starts in an area
whose ground is a twin (the Forest Temple's entrance: its rule texture is drawn
in Sacred Forest Meadow) had no grass. This finds the twins offline, with the
runtime's own decode, signature and twin score (twin_table_tool.cpp), among
every texture of the game, and writes them as hashes and rule mappings only:
no pixels and no signatures are written.

  python3 tools/oot3d/deko3d_grass/make_twin_table.py <romfs.bin> <work dir> [--cxx g++]

Run it in the host-tests container (g++ 12). Rerun it when the preset's rules
(triaevum_product_graphics.inc) or the signature or score code change.
"""
import argparse
import json
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
RUNTIME = REPO / "runtime/three_ds_recomp"
PRESET = REPO / "tools/oot3d/native_game_runtime/triaevum_product_graphics.inc"
OUTPUT = RUNTIME / "src/fast/oot3d/grass_texture_twin_table.cpp"
sys.path.insert(0, str(REPO / "tools/oot3d/ptbr"))
import romfs  # noqa: E402

CHANNELS = ["Red", "Green", "Blue", "Alpha", "Luminance"]
# CMB (format, data type) -> PICA texture format.
PICA_FORMAT = {(0x6752, 0x1401): 0, (0x6754, 0x1401): 1, (0x6752, 0x8034): 2, (0x6754, 0x8363): 3,
               (0x6752, 0x8033): 4, (0x6758, 0x1401): 5, (0x6757, 0x1401): 7, (0x6756, 0x1401): 8,
               (0x6758, 0x6760): 9, (0x6757, 0x6761): 10, (0x6756, 0x6761): 11, (0x675A, 0): 12, (0x675B, 0): 13}
NIBBLES = [8, 6, 4, 4, 4, 4, 4, 2, 2, 2, 1, 1, 1, 2]
TOOL_SOURCES = [
    HERE / "twin_table_tool.cpp",
    RUNTIME / "src/fast/oot3d/grass_texture_twins.cpp",
    # Linked only (GrassTwinRules); this run rewrites it.
    RUNTIME / "src/fast/oot3d/grass_texture_twin_table.cpp",
    RUNTIME / "src/fast/oot3d/grass_texture_source_cache.cpp",
    RUNTIME / "src/fast/oot3d/texture_preview_artifact.cpp",
    RUNTIME / "src/fast/oot3d/grass_surface_extractor.cpp",
    RUNTIME / "src/fast/oot3d/grass_overlay_mask.cpp",
    RUNTIME / "src/fast/oot3d/grass_visibility.cpp",
    RUNTIME / "src/fast/oot3d/pica_texture_decode.cpp",
]


def preset_rules():
    text = PRESET.read_text().strip().removeprefix('R"json(').removesuffix(')json"')
    return json.loads(text)["Grass"]["Sources"]


def fnv1a(data):
    # The runtime's texture identity: FNV-1a of the encoded base level
    # (pica_grass_source.cpp BaseLevelContentHash).
    value = 1469598103934665603
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def base_level_size(fmt, width, height):
    return (((max(8, width) + 7) & ~7) * ((max(8, height) + 7) & ~7) * NIBBLES[fmt] + 1) // 2


def cmb_textures(data):
    """(width, height, PICA format, base level bytes, name) of every CMB in a file."""
    at = 0
    while (at := data.find(b"cmb ", at) + 1) > 0:
        start = at - 1
        if start + 0x44 > len(data):
            continue
        size, version = struct.unpack_from("<II", data, start + 4)
        if version != 6 or size < 0x44 or start + size > len(data):
            continue
        tex = start + struct.unpack_from("<I", data, start + 0x2C)[0]
        pixels = start + struct.unpack_from("<I", data, start + 0x40)[0]
        if data[tex:tex + 4] != b"tex ":
            continue
        for index in range(struct.unpack_from("<I", data, tex + 8)[0]):
            entry = tex + 0x0C + index * 0x24
            byte_count = struct.unpack_from("<I", data, entry)[0]
            width, height, cmb_format, data_type, offset = struct.unpack_from("<HHHHI", data, entry + 8)
            fmt = PICA_FORMAT.get((cmb_format, data_type))
            if fmt is None:
                continue
            base = base_level_size(fmt, width, height)
            payload = data[pixels + offset:pixels + offset + byte_count]
            if base <= len(payload):
                name = data[entry + 0x14:entry + 0x24].split(b"\0")[0].decode("latin1")
                yield width, height, fmt, payload[:base], name


def collect(rom):
    textures = {}
    with open(rom, "rb") as handle:
        for path in romfs.list_files(rom):
            if not path.endswith((".zsi", ".zar", ".gar", ".cmb")):
                continue
            offset, size = romfs.locate(rom, path)
            handle.seek(offset)
            for width, height, fmt, base, name in cmb_textures(handle.read(size)):
                key = (fnv1a(base), width, height)
                entry = textures.setdefault(key, {"format": fmt, "base": base, "uses": []})
                if path not in entry["uses"]:
                    entry["uses"].append(path)
    return textures


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("romfs", type=Path)
    parser.add_argument("work", type=Path, help="scratch directory (the game's textures are written there)")
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)

    rules = preset_rules()
    textures = collect(args.romfs)
    for rule in rules:
        target = rule["Target"]
        if (int(target["Rgba8Hash"], 16), target["Width"], target["Height"]) not in textures:
            sys.exit(f"rule {rule['RuleId']}: texture {target['Rgba8Hash']} is not in {args.romfs}")
    rules_file = args.work / "rules.txt"
    rules_file.write_text("".join(
        f"{r['Target']['Rgba8Hash']} {r['Target']['Width']} {r['Target']['Height']} {CHANNELS.index(r['Channel'])}\n"
        for r in rules))
    textures_file = args.work / "textures.bin"
    with open(textures_file, "wb") as out:
        for (value, width, height), entry in sorted(textures.items()):
            out.write(struct.pack("<QHHBI", value, width, height, entry["format"], len(entry["base"])))
            out.write(entry["base"])

    tool = args.work / "twin_table_tool"
    subprocess.run([args.cxx, "-std=c++20", "-O2", "-I", str(RUNTIME / "include"), "-o", str(tool),
                    *map(str, TOOL_SOURCES), "-lpthread"], check=True)
    result = subprocess.run([tool, rules_file, textures_file], check=True, capture_output=True, text=True)
    sys.stderr.write(result.stderr)

    rule_ids = {(int(r["Target"]["Rgba8Hash"], 16), r["Target"]["Width"], r["Target"]["Height"]): r["RuleId"]
                for r in rules}
    rows = []
    for line in result.stdout.split("\n"):
        if not line:
            continue
        twin, tw, th, rule, rw, rh, channel, score, scale, offset = line.split()
        # %.9g floats: the exact single-precision values, as C++ literals.
        rows.append((int(twin, 16), int(tw), int(th), int(rule, 16), int(rw), int(rh), int(channel),
                     *(repr(float(value)) + "F" for value in (score, scale, offset))))
    rows.sort()
    lines = []
    for twin, tw, th, rule, rw, rh, channel, score, scale, offset in rows:
        uses = textures[(twin, tw, th)]["uses"]
        where = ", ".join(Path(p).name for p in uses[:3]) + (f" (+{len(uses) - 3})" if len(uses) > 3 else "")
        lines.append(f"    // rule {rule_ids[(rule, rw, rh)]}; {where}\n"
                     f"    {{0x{twin:016x}ULL, {tw}, {th}, 0x{rule:016x}ULL, {rw}, {rh}, "
                     f"GrassSampleChannel::{CHANNELS[channel]}, {score}, {scale}, {offset}}},\n")
    OUTPUT.write_text(
        "// Generated by tools/oot3d/deko3d_grass/make_twin_table.py from the game's RomFS; do not edit.\n"
        "// The twins of the preset's grass rule textures (texture hashes and rule mappings only).\n"
        '#include "fast/oot3d/grass_texture_twins.h"\n\n'
        "namespace Fast::Oot3d {\n"
        "namespace {\n\n"
        "// Twin hash, size; rule texture hash, size; the rule's channel; twin score; sample scale, offset.\n"
        "constexpr GrassKnownTwin kKnownTwins[] = {\n" + "".join(lines) + "};\n\n"
        "} // namespace\n\n"
        "std::span<const GrassKnownTwin> GrassKnownTwins() noexcept {\n"
        "    return kKnownTwins;\n"
        "}\n\n"
        "} // namespace Fast::Oot3d\n")
    print(f"{len(rows)} twins of {len(rules)} rule textures among {len(textures)} textures -> {OUTPUT}")


if __name__ == "__main__":
    main()
