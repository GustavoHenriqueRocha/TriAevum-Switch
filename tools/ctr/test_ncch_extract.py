"""Testes do extrator de jogos de 3DS (ncch_extract.py) com imagens sintéticas."""
from __future__ import annotations

import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ncch_extract  # noqa: E402

MU = ncch_extract.MEDIA_UNIT


def blz(prefix: bytes) -> tuple[bytes, bytes]:
    """BLZ montado à mão: prefixo sem compressão + 152 zeros comprimidos.

    O decodificador lê de trás para a frente: um flag 0x00 com 8 literais
    zero, depois um flag 0xFF com 8 referências de 18 bytes à distância 3.
    Devolve (comprimido, esperado).
    """
    reading_order: list[bytes] = [b"\x00", *([b"\x00"] * 8), b"\xff", *([b"\x00\xf0"] * 8)]
    stream = b"".join(reversed(reading_order))
    compressed_size = len(stream) + 8
    plain = prefix + bytes(8 + 18 * 8)
    data_size = len(prefix) + compressed_size
    footer = struct.pack("<II", 8 << 24 | compressed_size, len(plain) - data_size)
    return prefix + stream + footer, plain


def ncch(code: bytes, romfs: bytes, *, compressed: bool, encrypted: bool = False) -> bytes:
    header = bytearray(0x200)
    header[0x100:0x104] = b"NCCH"
    struct.pack_into("<Q", header, 0x118, 0x0004000000123400)
    header[0x150:0x15A] = b"CTR-P-TEST"
    header[0x18F] = 0 if encrypted else 0x4
    exheader = bytearray(0x800)
    exheader[0xD] = 1 if compressed else 0
    stored = blz(code)[0] if compressed else code
    exefs = bytearray(0x200)
    struct.pack_into("<8sII", exefs, 0, b".code", 0, len(stored))
    exefs_body = bytes(exefs) + stored.ljust(-(-len(stored) // MU) * MU, b"\0")
    exefs_offset = 0x200 + 0x800
    romfs_offset = exefs_offset + len(exefs_body)
    struct.pack_into("<2I", header, 0x1A0, exefs_offset // MU, len(exefs_body) // MU)
    struct.pack_into("<2I", header, 0x1B0, romfs_offset // MU, len(romfs) // MU)
    return bytes(header) + bytes(exheader) + exefs_body + romfs


def ncsd(partition: bytes) -> bytes:
    header = bytearray(0x200)
    header[0x100:0x104] = b"NCSD"
    struct.pack_into("<2I", header, 0x120, 0x4000 // MU, len(partition) // MU)
    return bytes(header).ljust(0x4000, b"\0") + partition


class NcchExtractTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.prefix = bytes(range(256)) * 2 + b"fim"
        self.code = blz(self.prefix)[1]
        self.romfs = b"IVFC" + bytes(MU - 4)

    def extract(self, image: bytes) -> dict[str, object]:
        source = self.root / "jogo.3ds"
        source.write_bytes(image)
        return ncch_extract.extract(source, self.root / "saida")

    def test_cartridge_with_compressed_code(self) -> None:
        info = self.extract(ncsd(ncch(self.prefix, self.romfs, compressed=True)))
        self.assertEqual((self.root / "saida/code.bin").read_bytes(), self.code)
        self.assertEqual((self.root / "saida/romfs.bin").read_bytes(), self.romfs)
        self.assertEqual(info["product"], "CTR-P-TEST")
        self.assertEqual(info["program_id"], 0x0004000000123400)

    def test_bare_ncch_with_plain_code(self) -> None:
        self.extract(ncch(self.prefix, self.romfs, compressed=False))
        self.assertEqual((self.root / "saida/code.bin").read_bytes(), self.prefix)

    def test_encrypted_content_is_refused(self) -> None:
        with self.assertRaisesRegex(ncch_extract.ExtractError, "criptografado"):
            self.extract(ncsd(ncch(self.prefix, self.romfs, compressed=True, encrypted=True)))

    def test_unknown_format_is_refused(self) -> None:
        with self.assertRaisesRegex(ncch_extract.ExtractError, "CIA"):
            self.extract(b"\0" * 0x400)


if __name__ == "__main__":
    unittest.main()
