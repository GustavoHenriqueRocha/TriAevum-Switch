# Converte os materiais CGFX (CMDL -> MTOB) de todos os .szs (Yaz0 + NARC) de um
# romfs do Super Mario 3D Land em registradores do PICA, no mesmo formato de
# cmb_tev_registers.py: "<id> <reg>=<valor> ...", um por material único, mais o
# sidecar <saida>.luz com os ids que ligam a luz de fragmento. Assim
# deko3d_register_glsl_dump e deko3d_shader_predict.py funcionam sem mudança.
#
#   python3 -I tools/oot3d/cgfx_tev_registers.py <pasta-romfs> <saida.txt> [--census]
#
# Spike 05/11 (SM3DL): 1079 CGFX, 1066 modelos, 4579 materiais (4146 com luz de
# fragmento), 332 programas TEV únicos, 395 materiais únicos (289 com luz) -> 333 GLSL.
#
# Diferença para o CMB: o CGFX já guarda o fragment shader como comandos PICA
# prontos (valor + cabeçalho de comando), então não há tabela GL -> PICA; o script
# só copia os valores e confere os cabeçalhos. Layout (little-endian, ponteiros
# relativos ao próprio campo; validado nos 1078 .bcmdl do SM3DL em 05/11, ver
# validate() e o contador de anomalias impresso no fim):
#
#   CGFX  0x00 "CGFX" fffe | u16 tam. header (0x14) | u32 revisão | u32 tam. | u32 nº seções
#         DATA em <tam. header>: "DATA" u32 tam. | 16 × (u32 nº, s32 ptr p/ DICT); [0] = modelos
#   DICT  "DICT" u32 tam. | u32 n | raiz + n nós de 0x10 (u32 bit, u16 esq, u16 dir,
#         s32 ptr nome @+8, s32 ptr dado @+0xC)
#   CMDL  +0x00 u32 tipo (0x40000012 / 0x40000092) | +0x04 "CMDL" | +0x0C ptr nome
#         +0xBC u32 nº de materiais | +0xC0 ptr p/ DICT de MTOB
#   MTOB  +0x00 u32 0x08000000 | +0x04 "MTOB" | +0x0C ptr nome
#         +0x18 u32 flags (bit0 = luz de fragmento, bit4 = fog)
#         +0x1C u32 TexCoordConfig (0..5 = 0120, 0110, 0111, 0112, 0121, 0122:
#               coordenada usada por tex0/tex1/tex2/proctex)
#         +0x274/+0x278/+0x27C ptr TextureMapper 0..2 (0 = unidade desligada)
#         +0x280 ptr mapper procedural | +0x284 ptr SHDR | +0x288 ptr FragmentShader
#   FragmentShader (FS)
#         +0x00 float4 cor do buffer TEV | +0x10 u32 flags de luz de fragmento
#         +0x14 layer config | +0x18 fresnel | +0x1C bump tex | +0x20 bump mode
#         +0x24 renormaliza bump | +0x28 ptr tabelas LUT
#         +0x2C 6 estágios × 0x1C: u32 índice da cor constante, source, cabeçalho
#               (0x804F00C0 + base do estágio: escrita de 5 registradores seguidos),
#               operand, combine, const color, scale  (valores PICA crus)
#         +0xD4 valor/+0xD8 cabeçalho 0x000F0104 (alpha test)
#         +0xDC valor/+0xE0 cabeçalho 0x000F00FD (cor do buffer TEV)
#         +0xE4 valor/+0xE8 cabeçalho 0x000200E0 (só o byte 1: bits de update do buffer)
#         +0xEC valor/+0xF0 cabeçalho 0x000201C3 (byte 1 do config1 da luz)
#
# Registradores emitidos (iguais aos do CMB): 0xC0.. +0/+1/+2/+4 dos 6 estágios,
# 0xE0 (bits 8..15), 0x80 (unidades 0..2 com mapper; bit 13 quando tex2 usa a
# coordenada 1; bit 10 + bits 8..9 para textura procedural) e 0x104 só quando o
# alpha test está ligado. A cor constante (+3) fica de fora como no CMB.
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ctr_narc  # noqa: E402

STAGE_BASE = (0x0C0, 0x0C8, 0x0D0, 0x0D8, 0x0F0, 0x0F8)
CMDL_MATERIALS = 0xBC
MTOB_FLAGS, MTOB_TEXCOORD, MTOB_MAPPERS, MTOB_PROCTEX, MTOB_FRAGSHADER = 0x18, 0x1C, 0x274, 0x280, 0x288
FS_STAGES, FS_STAGE_SIZE, FS_TAIL = 0x2C, 0x1C, 0xD4
# TexCoordConfig -> (tex2 usa coordenada 1, coordenada da textura procedural)
TEXCOORD_CONFIG = {0: (False, 0), 1: (True, 0), 2: (True, 1), 3: (True, 2), 4: (False, 1), 5: (False, 2)}
VALID_SOURCES = {0, 1, 2, 3, 4, 5, 6, 0xD, 0xE, 0xF}
VALID_COLOR_OPERANDS = {0, 1, 2, 3, 4, 5, 8, 9, 0xC, 0xD}


class CgfxError(ValueError):
    pass


def _u32(d: bytes, at: int) -> int:
    return struct.unpack_from('<I', d, at)[0]


def _ptr(d: bytes, at: int) -> int:
    rel = struct.unpack_from('<i', d, at)[0]
    return at + rel if rel else 0


def _name(d: bytes, at: int) -> str:
    p = _ptr(d, at)
    if not p:
        return ''
    return d[p:d.index(b'\0', p)].decode('latin1')


def dict_entries(d: bytes, at: int) -> list[tuple[str, int]]:
    if d[at:at + 4] != b'DICT':
        raise CgfxError(f'DICT esperado em 0x{at:x}')
    n = _u32(d, at + 8)
    if n > 0x10000:
        raise CgfxError(f'DICT com {n} entradas')
    out = []
    for i in range(1, n + 1):
        node = at + 0xC + i * 0x10
        out.append((_name(d, node + 8), _ptr(d, node + 0xC)))
    return out


def models(d: bytes) -> list[tuple[str, int]]:
    if d[:4] != b'CGFX':
        raise CgfxError('não é CGFX')
    data = struct.unpack_from('<H', d, 6)[0]
    if d[data:data + 4] != b'DATA':
        raise CgfxError('seção DATA ausente')
    count = _u32(d, data + 8)
    return dict_entries(d, _ptr(d, data + 0xC)) if count else []


def model_materials(d: bytes, cmdl: int) -> list[tuple[str, int]]:
    if d[cmdl + 4:cmdl + 8] != b'CMDL':
        raise CgfxError(f'CMDL esperado em 0x{cmdl:x}')
    if not _u32(d, cmdl + CMDL_MATERIALS):
        return []
    mats = dict_entries(d, _ptr(d, cmdl + CMDL_MATERIALS + 4))
    if len(mats) != _u32(d, cmdl + CMDL_MATERIALS):
        raise CgfxError('nº de materiais do CMDL não bate com o DICT')
    return mats


def validate(source: int, operand: int, combine: int, scale: int) -> list[str]:
    """Campos fora da faixa do PICA = offset errado ou layout diferente."""
    bad = []
    for shift in (0, 4, 8, 16, 20, 24):
        if source >> shift & 0xF not in VALID_SOURCES:
            bad.append('source')
    if source & 0xF000F000 or operand & 0xFF000000 or combine & 0xFFF0FFF0 or scale & 0xFFFCFFFC:
        bad.append('bits reservados')
    for shift in (0, 4, 8):
        if operand >> shift & 0xF not in VALID_COLOR_OPERANDS or operand >> (12 + shift) & 0xF > 7:
            bad.append('operand')
    if (combine & 0xF) > 9 or (combine >> 16 & 0xF) > 9:
        bad.append('combine')
    if (scale & 3) == 3 or (scale >> 16 & 3) == 3:
        bad.append('scale')
    return bad


def material_registers(d: bytes, mtob: int) -> tuple[dict[int, int], bool, list[str]]:
    """Registradores PICA do material, luz de fragmento ligada, anomalias."""
    if d[mtob + 4:mtob + 8] != b'MTOB':
        raise CgfxError(f'MTOB esperado em 0x{mtob:x}')
    anomalies: list[str] = []
    flags = _u32(d, mtob + MTOB_FLAGS)
    fs = _ptr(d, mtob + MTOB_FRAGSHADER)
    if not fs or fs + FS_TAIL + 0x20 > len(d):
        raise CgfxError('MTOB sem fragment shader')
    regs: dict[int, int] = {}
    for stage, base in enumerate(STAGE_BASE):
        at = fs + FS_STAGES + stage * FS_STAGE_SIZE
        _, source, header, operand, combine, _, scale = struct.unpack_from('<7I', d, at)
        if header != 0x804F0000 | base:
            raise CgfxError(f'cabeçalho do estágio {stage} = 0x{header:08x}')
        anomalies += validate(source, operand, combine, scale)
        regs[base], regs[base + 1], regs[base + 2], regs[base + 4] = source, operand, combine, scale
    tail = struct.unpack_from('<8I', d, fs + FS_TAIL)
    expected = (0x000F0104, 0x000F00FD, 0x000200E0, 0x000201C3)
    if tail[1::2] != expected:
        raise CgfxError('cabeçalhos de alpha test/buffer: ' + ' '.join(f'{h:08x}' for h in tail[1::2]))
    alpha, buffer = tail[0], tail[4]
    if buffer & ~0xFF00:
        anomalies.append('0xE0 fora do byte 1')
    regs[0x0E0] = buffer & 0xFF00
    texture = 0
    for unit in range(3):
        if _u32(d, mtob + MTOB_MAPPERS + unit * 4):
            texture |= 1 << unit
    config = _u32(d, mtob + MTOB_TEXCOORD)
    if config not in TEXCOORD_CONFIG:
        anomalies.append('TexCoordConfig')
        config = 0
    tex2_coord1, proc_coord = TEXCOORD_CONFIG[config]
    if tex2_coord1 and texture & 4:
        texture |= 1 << 13
    if _u32(d, mtob + MTOB_PROCTEX):
        texture |= 1 << 10 | proc_coord << 8
        anomalies.append('textura procedural (regs 0xA8.. não emitidos)')
    regs[0x080] = texture
    if alpha & ~0xFF71:
        anomalies.append('alpha test')
    if alpha & 1:
        regs[0x104] = alpha & 0xFF71
    return regs, bool(flags & 1), anomalies


def scan_cgfx(d: bytes, sink) -> int:
    """Chama sink(modelo, material, regs, luz, anomalias) para cada MTOB; devolve nº de modelos."""
    found = 0
    for model, cmdl in models(d):
        found += 1
        for material, mtob in model_materials(d, cmdl):
            regs, lit, anomalies = material_registers(d, mtob)
            sink(model, material, regs, lit, anomalies)
    return found


def tev_key(regs: dict[int, int]) -> tuple:
    return tuple(regs[b + k] for b in STAGE_BASE for k in (0, 1, 2, 4))


def active_stages(regs: dict[int, int]) -> int:
    """Estágios que fazem algo além de passar PREVIOUS adiante."""
    return sum(1 for b in STAGE_BASE
               if not (regs[b] & 0xF000F == 0xF000F and regs[b + 1] & 0xF00F == 0
                       and regs[b + 2] == 0 and regs[b + 4] == 0))


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) != 2:
        print('uso: cgfx_tev_registers.py <pasta-romfs> <saida.txt> [--census]', file=sys.stderr)
        return 2
    root, output = args
    unique: dict[tuple, dict[int, int]] = {}
    lit: set[tuple] = set()
    tev: collections.Counter = collections.Counter()
    tev_lit: set[tuple] = set()
    anomalies: collections.Counter = collections.Counter()
    errors: list[str] = []
    totals = collections.Counter()

    def sink(model, material, regs, is_lit, bad):
        key = tuple(sorted(regs.items()))
        unique.setdefault(key, regs)
        tev[tev_key(regs)] += 1
        totals['materiais'] += 1
        if is_lit:
            lit.add(key)
            tev_lit.add(tev_key(regs))
            totals['materiais com luz'] += 1
        for item in set(bad):
            anomalies[item] += 1

    for directory, _, files in os.walk(root):
        for name in sorted(files):
            if not name.endswith('.szs'):
                continue
            path = os.path.join(directory, name)
            try:
                content = ctr_narc.open_szs(path)
            except (ValueError, IndexError, struct.error) as error:
                errors.append(f'{name}: {error}')
                continue
            for inner, blob in content:
                if blob[:4] != b'CGFX':
                    continue
                totals['arquivos CGFX'] += 1
                try:
                    totals['modelos'] += scan_cgfx(blob, sink)
                except (CgfxError, IndexError, struct.error) as error:
                    errors.append(f'{name}/{inner}: {error}')
    os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)
    with open(output, 'w') as f, open(output + '.luz', 'w') as luz:
        for index, (key, regs) in enumerate(unique.items()):
            f.write(f'm{index:04d} ' + ' '.join(f'{r:x}={v:x}' for r, v in sorted(regs.items())) + '\n')
            if key in lit:
                luz.write(f'm{index:04d}\n')
    print(f'{len(unique)} materiais únicos ({len(lit)} com luz de fragmento), {len(errors)} falhas')
    if '--census' in sys.argv:
        print(', '.join(f'{k}={v}' for k, v in totals.items()))
        print(f'programas TEV únicos (só combiners)={len(tev)} (com luz de fragmento={len(tev_lit)})')
        print(f'TEV + texturas + buffer + alpha test={len(unique)}')
        stages = collections.Counter(active_stages(dict(zip(
            [b + k for b in STAGE_BASE for k in (0, 1, 2, 4)], key))) for key in tev)
        print('estágios ativos por programa:', sorted(stages.items()))
        print('anomalias:', dict(anomalies) or 'nenhuma')
    for line in errors[:10]:
        print(' ', line)
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
