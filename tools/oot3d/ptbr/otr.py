"""Lê arquivos de um MPQ do SoH (.otr), incluindo os criptografados."""
import bz2, struct, sys, zlib
from io import BytesIO
from pathlib import Path
import mpyq
from mpyq import (MPQ_FILE_COMPRESS, MPQ_FILE_ENCRYPTED, MPQ_FILE_EXISTS, MPQ_FILE_FIX_KEY,
                  MPQ_FILE_SECTOR_CRC, MPQ_FILE_SINGLE_UNIT)


class Otr(mpyq.MPQArchive):
    def __init__(self, caminho):
        super().__init__(caminho, listfile=False)
        lista = self.ler("(listfile)")
        self.files = [l.strip() for l in lista.decode("utf-8", "replace").splitlines() if l.strip()] if lista else []

    def _dec(self, dados, chave):
        n = len(dados) // 4 * 4
        return self._decrypt(dados[:n], chave) + dados[n:] if n else dados

    def ler(self, nome):
        h = self.get_hash_table_entry(nome)
        if h is None:
            return None
        b = self.block_table[h.block_table_index]
        if not b.flags & MPQ_FILE_EXISTS or b.archived_size == 0:
            return b""
        self.file.seek(b.offset + self.header["offset"])
        dados = self.file.read(b.archived_size)
        chave = None
        if b.flags & MPQ_FILE_ENCRYPTED:
            base = nome.replace("/", "\\").split("\\")[-1]
            chave = self._hash(base, "TABLE")
            if b.flags & MPQ_FILE_FIX_KEY:
                chave = ((chave + b.offset) ^ b.size) & 0xFFFFFFFF

        def descomp(s):
            t = s[0]
            if t == 2:
                return zlib.decompress(s[1:])
            if t == 16:
                return bz2.decompress(s[1:])
            if t == 0:
                return s[1:]
            raise RuntimeError(f"compressão {t:#x} não suportada em {nome}")

        if b.flags & MPQ_FILE_SINGLE_UNIT:
            if chave is not None:
                dados = self._dec(dados, chave)
            if b.flags & MPQ_FILE_COMPRESS and b.size > b.archived_size:
                dados = descomp(dados)
            return dados
        setor = 512 << self.header["sector_size_shift"]
        n = (b.size + setor - 1) // setor
        n_pos = n + 1 + (1 if b.flags & MPQ_FILE_SECTOR_CRC else 0)
        cab = dados[:4 * n_pos]
        if chave is not None:
            cab = self._dec(cab, (chave - 1) & 0xFFFFFFFF)
        pos = struct.unpack(f"<{n_pos}I", cab)
        out, resta = BytesIO(), b.size
        for i in range(n):
            s = dados[pos[i]:pos[i + 1]]
            if chave is not None:
                s = self._dec(s, (chave + i) & 0xFFFFFFFF)
            if b.flags & MPQ_FILE_COMPRESS and len(s) < min(setor, resta):
                s = descomp(s)
            resta -= len(s)
            out.write(s)
        return out.getvalue()


if __name__ == "__main__":
    a = Otr(sys.argv[1])
    destino = Path(sys.argv[2]) if len(sys.argv) > 2 else None
    for f in a.files:
        d = a.ler(f)
        print(f"{len(d) if d is not None else -1:8}  {f}")
        if destino and d:
            p = destino / f.replace("\\", "/")
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(d)
