"""Reads files from a SoH MPQ (.otr), including encrypted ones."""
import bz2, struct, sys, zlib
from io import BytesIO
from pathlib import Path
import mpyq
from mpyq import (MPQ_FILE_COMPRESS, MPQ_FILE_ENCRYPTED, MPQ_FILE_EXISTS, MPQ_FILE_FIX_KEY,
                  MPQ_FILE_SECTOR_CRC, MPQ_FILE_SINGLE_UNIT)


class Otr(mpyq.MPQArchive):
    def __init__(self, path):
        super().__init__(path, listfile=False)
        listing = self.read_entry("(listfile)")
        self.files = [l.strip() for l in listing.decode("utf-8", "replace").splitlines() if l.strip()] if listing else []

    def _dec(self, data, key):
        n = len(data) // 4 * 4
        return self._decrypt(data[:n], key) + data[n:] if n else data

    def read_entry(self, name):
        h = self.get_hash_table_entry(name)
        if h is None:
            return None
        b = self.block_table[h.block_table_index]
        if not b.flags & MPQ_FILE_EXISTS or b.archived_size == 0:
            return b""
        self.file.seek(b.offset + self.header["offset"])
        data = self.file.read(b.archived_size)
        key = None
        if b.flags & MPQ_FILE_ENCRYPTED:
            base = name.replace("/", "\\").split("\\")[-1]
            key = self._hash(base, "TABLE")
            if b.flags & MPQ_FILE_FIX_KEY:
                key = ((key + b.offset) ^ b.size) & 0xFFFFFFFF

        def decompress(s):
            t = s[0]
            if t == 2:
                return zlib.decompress(s[1:])
            if t == 16:
                return bz2.decompress(s[1:])
            if t == 0:
                return s[1:]
            raise RuntimeError(f"compression {t:#x} not supported in {name}")

        if b.flags & MPQ_FILE_SINGLE_UNIT:
            if key is not None:
                data = self._dec(data, key)
            if b.flags & MPQ_FILE_COMPRESS and b.size > b.archived_size:
                data = decompress(data)
            return data
        sector = 512 << self.header["sector_size_shift"]
        n = (b.size + sector - 1) // sector
        n_pos = n + 1 + (1 if b.flags & MPQ_FILE_SECTOR_CRC else 0)
        sector_table = data[:4 * n_pos]
        if key is not None:
            sector_table = self._dec(sector_table, (key - 1) & 0xFFFFFFFF)
        pos = struct.unpack(f"<{n_pos}I", sector_table)
        out, remaining = BytesIO(), b.size
        for i in range(n):
            s = data[pos[i]:pos[i + 1]]
            if key is not None:
                s = self._dec(s, (key + i) & 0xFFFFFFFF)
            if b.flags & MPQ_FILE_COMPRESS and len(s) < min(sector, remaining):
                s = decompress(s)
            remaining -= len(s)
            out.write(s)
        return out.getvalue()


if __name__ == "__main__":
    a = Otr(sys.argv[1])
    dest = Path(sys.argv[2]) if len(sys.argv) > 2 else None
    for f in a.files:
        d = a.read_entry(f)
        print(f"{len(d) if d is not None else -1:8}  {f}")
        if dest and d:
            p = dest / f.replace("\\", "/")
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(d)
