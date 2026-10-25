#!/usr/bin/env python3
"""Regressão do rotular_nro.py: três ports no Switch apareciam iguais no menu (25/10)."""
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "oot3d"))
import rotular_nro  # noqa: E402


def check(name, condition):
    print(("ok   " if condition else "FAIL ") + name)
    return condition


def fake_nro(icon, nacp):
    code = bytearray(0x100)
    code[0x10:0x14] = b"NRO0"
    struct.pack_into("<I", code, 0x18, len(code))
    header = b"ASET" + struct.pack("<I6Q", 0, 0x38, len(icon), 0x38 + len(icon), len(nacp), 0, 0)
    return bytes(code) + header + icon + nacp


def main():
    icon = (ROOT / "res" / "switch" / "icon.jpg").read_bytes()
    nacp = bytearray(rotular_nro.NACP_SIZE)
    nacp[0:5] = b"Zelda"
    ok = True
    ok &= check("rótulo sai do nome do arquivo",
                rotular_nro.label_from_name("/x/oot3d_native_game.deko3d-p5b.nro") == "deko3d-p5b")
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "oot3d_native_game.pgo5o.nro"
        dst = Path(tmp) / "saida.nro"
        src.write_bytes(fake_nro(icon, bytes(nacp)))
        subprocess.run([sys.executable, str(ROOT / "tools/oot3d/rotular_nro.py"), str(src),
                        "-o", str(dst)], check=True, capture_output=True)
        nro_size, new_icon, new_nacp, romfs = rotular_nro.read_aset(dst.read_bytes())
        ok &= check("código do NRO intacto", dst.read_bytes()[:nro_size] == src.read_bytes()[:nro_size])
        ok &= check("nome no menu tem o rótulo", new_nacp.startswith(b"OoT3D [pgo5o]\0"))
        ok &= check("versão = rótulo", new_nacp[0x3060:0x3066] == b"pgo5o\0")
        ok &= check("ícone trocado e ainda JPEG", new_icon != icon and new_icon[:2] == b"\xff\xd8")
        ok &= check("sem romfs continua sem romfs", romfs == b"")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
