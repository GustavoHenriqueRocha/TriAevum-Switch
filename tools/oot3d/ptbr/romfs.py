import struct
def u32(b,p): return struct.unpack_from('<I',b,p)[0]
def u64(b,p): return struct.unpack_from('<Q',b,p)[0]
def locate(path, target):
    """→ (absolute offset, size) of a file in a Level-3 romfs (with or without IVFC)."""
    with open(path,'rb') as f:
        base = 0x1000 if f.read(4) == b'IVFC' else 0
        f.seek(base); h = f.read(40)
        f.seek(base+u32(h,12)); dirs = f.read(u32(h,16))
        f.seek(base+u32(h,28)); files = f.read(u32(h,32))
        data = base + u32(h,36)
    name = lambda b,p,n: b[p:p+n].decode('utf-16le')
    folders = {}; p = 0
    while p < len(dirs):
        n = u32(dirs,p+20); folders[p] = (u32(dirs,p), name(dirs,p+24,n)); p += (24+n+3)&~3
    def path_of(d):
        parts = []
        while d in folders and d != 0:
            parts.append(folders[d][1]); d = folders[d][0]
        return '/'.join(reversed(parts))
    p = 0
    while p < len(files):
        n = u32(files,p+28); nm = name(files,p+32,n)
        entry_path = (path_of(u32(files,p)) + '/' + nm).lstrip('/')
        if entry_path == target: return data + u64(files,p+8), u64(files,p+16)
        p += (32+n+3)&~3
    raise FileNotFoundError(target)


def list_files(path):
    """→ paths of every file in the romfs."""
    with open(path,'rb') as f:
        base = 0x1000 if f.read(4) == b'IVFC' else 0
        f.seek(base); h = f.read(40)
        f.seek(base+u32(h,12)); dirs = f.read(u32(h,16))
        f.seek(base+u32(h,28)); files = f.read(u32(h,32))
    name = lambda b,p,n: b[p:p+n].decode('utf-16le')
    folders = {}; p = 0
    while p < len(dirs):
        n = u32(dirs,p+20); folders[p] = (u32(dirs,p), name(dirs,p+24,n)); p += (24+n+3)&~3
    def path_of(d):
        parts = []
        while d in folders and d != 0:
            parts.append(folders[d][1]); d = folders[d][0]
        return '/'.join(reversed(parts))
    p = 0; out = []
    while p < len(files):
        n = u32(files,p+28); out.append((path_of(u32(files,p)) + '/' + name(files,p+32,n)).lstrip('/'))
        p += (32+n+3)&~3
    return out
