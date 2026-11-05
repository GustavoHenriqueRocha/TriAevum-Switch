# Uso: python3 tools/oot3d/romfs_extract.py <romfs.bin> <pasta-fora-do-repo>
# Extrai um romfs.bin (IVFC) do 3DS para uma pasta.
import struct, sys, os
src, out = sys.argv[1], sys.argv[2]
f = open(src, 'rb'); data = f.read()
assert data[:4] == b'IVFC', data[:4]
master_size = struct.unpack_from('<I', data, 0x08)[0]
lvl3_block = struct.unpack_from('<I', data, 0x4C)[0]
off = 0x60 + master_size
bs = 1 << lvl3_block
L3 = (off + bs - 1) // bs * bs
hdr = struct.unpack_from('<10I', data, L3)
_, dh_off, dh_len, dm_off, dm_len, fh_off, fh_len, fm_off, fm_len, fd_off = hdr
def dir_entry(o):
    p = L3 + dm_off + o
    parent, sib, child, file, nxt, nlen = struct.unpack_from('<6I', data, p)
    name = data[p+0x18:p+0x18+nlen].decode('utf-16le')
    return sib, child, file, name
def walk_files(fo, path):
    n = 0
    while fo != 0xFFFFFFFF:
        p = L3 + fm_off + fo
        parent, sib, doff, dlen, nxt, nlen = struct.unpack_from('<IIQQII', data, p)
        name = data[p+0x20:p+0x20+nlen].decode('utf-16le')
        start = L3 + fd_off + doff
        with open(os.path.join(path, name), 'wb') as o: o.write(data[start:start+dlen])
        n += 1; fo = sib
    return n
def walk_dir(do, path):
    sib, child, file, name = dir_entry(do)
    here = os.path.join(path, name) if name else path
    os.makedirs(here, exist_ok=True)
    n = walk_files(file, here)
    c = child
    while c != 0xFFFFFFFF:
        n += walk_dir(c, here); c = dir_entry(c)[0]
    return n
print('arquivos:', walk_dir(0, out))
