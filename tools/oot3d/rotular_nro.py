#!/usr/bin/env python3
"""Regrava nome, versão e ícone de um NRO pronto para distinguir builds no menu do Switch.

Uso: rotular_nro.py <entrada.nro> [rótulo] [-o saída.nro]
Sem rótulo, usa o trecho do nome do arquivo: oot3d_native_game.<rótulo>.nro.
Não recompila: troca só a seção ASET (ícone + NACP) do fim do NRO; o código fica igual.
O ícone ganha uma faixa com o rótulo (precisa do ImageMagick, `magick`).
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

NACP_SIZE = 0x4000
NACP_LANG_ENTRY = 0x300  # nome 0x200 + autor 0x100, 16 idiomas
NACP_DISPLAY_VERSION = 0x3060
ASET_HEADER_SIZE = 0x38

# Cor da faixa por família de build: o mesmo rótulo sempre dá a mesma cor.
BAND_COLORS = ["#c62828", "#1565c0", "#2e7d32", "#ef6c00", "#6a1b9a", "#00838f", "#ad1457", "#4e342e"]


def label_from_name(path):
    base = os.path.basename(path)
    parts = base.split(".")
    if len(parts) >= 3 and parts[-1] == "nro":
        return ".".join(parts[1:-1])
    return os.path.splitext(base)[0]


def read_aset(data):
    if data[0x10:0x14] != b"NRO0":
        sys.exit("não é um NRO")
    nro_size = struct.unpack_from("<I", data, 0x18)[0]
    if data[nro_size:nro_size + 4] != b"ASET":
        sys.exit("NRO sem seção ASET")
    _, icon_off, icon_size, nacp_off, nacp_size, romfs_off, romfs_size = struct.unpack_from(
        "<I6Q", data, nro_size + 4)
    base = nro_size
    return (nro_size,
            data[base + icon_off:base + icon_off + icon_size],
            data[base + nacp_off:base + nacp_off + nacp_size],
            data[base + romfs_off:base + romfs_off + romfs_size])


def write_fixed(buf, offset, size, text):
    raw = text.encode("utf-8")[:size - 1]
    buf[offset:offset + size] = raw + b"\0" * (size - len(raw))


def relabel_nacp(nacp, label):
    nacp = bytearray(nacp.ljust(NACP_SIZE, b"\0"))
    for lang in range(16):
        entry = lang * NACP_LANG_ENTRY
        if nacp[entry] == 0 and lang != 0:
            continue
        write_fixed(nacp, entry, 0x200, f"OoT3D [{label}]")
    write_fixed(nacp, NACP_DISPLAY_VERSION, 0x10, label)
    return bytes(nacp)


def relabel_icon(icon, label):
    color = BAND_COLORS[sum(label.encode()) % len(BAND_COLORS)]
    font = subprocess.run(["fc-match", "-f", "%{file}", "sans:bold"],
                          capture_output=True, text=True).stdout or "Helvetica"
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "in.jpg")
        dst = os.path.join(tmp, "out.jpg")
        with open(src, "wb") as f:
            f.write(icon)
        subprocess.run(["magick", src, "-resize", "256x256!",
                        "(", "-size", "248x60", "-background", color, "-fill", "white",
                        "-font", font, "-gravity", "center", f"label:{label}",
                        "-bordercolor", color, "-border", "4x2", ")",
                        "-gravity", "south", "-composite", "-quality", "92", dst], check=True)
        with open(dst, "rb") as f:
            return f.read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("entrada")
    ap.add_argument("rotulo", nargs="?")
    ap.add_argument("-o", "--saida")
    args = ap.parse_args()
    label = args.rotulo or label_from_name(args.entrada)

    with open(args.entrada, "rb") as f:
        data = f.read()
    nro_size, icon, nacp, romfs = read_aset(data)
    icon = relabel_icon(icon, label)
    nacp = relabel_nacp(nacp, label)

    icon_off = ASET_HEADER_SIZE
    nacp_off = icon_off + len(icon)
    romfs_off = nacp_off + len(nacp)
    header = b"ASET" + struct.pack("<I6Q", 0, icon_off, len(icon), nacp_off, len(nacp),
                                   romfs_off if romfs else 0, len(romfs))
    out = args.saida or args.entrada
    with open(out + ".tmp", "wb") as f:
        f.write(data[:nro_size] + header + icon + nacp + romfs)
    os.replace(out + ".tmp", out)
    print(f"{out}: rótulo '{label}'")


if __name__ == "__main__":
    main()
