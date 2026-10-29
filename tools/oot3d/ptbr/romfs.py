import struct
def u32(b,p): return struct.unpack_from('<I',b,p)[0]
def u64(b,p): return struct.unpack_from('<Q',b,p)[0]
def localizar(caminho, alvo):
    """→ (offset absoluto, tamanho) de um arquivo numa romfs Level-3 (com ou sem IVFC)."""
    with open(caminho,'rb') as f:
        base = 0x1000 if f.read(4) == b'IVFC' else 0
        f.seek(base); h = f.read(40)
        f.seek(base+u32(h,12)); dirs = f.read(u32(h,16))
        f.seek(base+u32(h,28)); files = f.read(u32(h,32))
        data = base + u32(h,36)
    nome = lambda b,p,n: b[p:p+n].decode('utf-16le')
    pastas = {}; p = 0
    while p < len(dirs):
        n = u32(dirs,p+20); pastas[p] = (u32(dirs,p), nome(dirs,p+24,n)); p += (24+n+3)&~3
    def caminho_de(d):
        partes = []
        while d in pastas and d != 0:
            partes.append(pastas[d][1]); d = pastas[d][0]
        return '/'.join(reversed(partes))
    p = 0
    while p < len(files):
        n = u32(files,p+28); nm = nome(files,p+32,n)
        cam = (caminho_de(u32(files,p)) + '/' + nm).lstrip('/')
        if cam == alvo: return data + u64(files,p+8), u64(files,p+16)
        p += (32+n+3)&~3
    raise FileNotFoundError(alvo)
