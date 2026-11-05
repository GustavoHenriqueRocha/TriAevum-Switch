# Converte os materiais de todos os CMB do romfs em registradores do PICA
# (TEV, buffer, texturas, alpha test) no formato de entrada de
# oot3d_deko3d_register_glsl_dump: "<id> <reg>=<valor> ...", um por material único.
#
#   python3 tools/oot3d/cmb_tev_registers.py <pasta-romfs-extraida> <saida.txt>
import os
import struct
import sys

SOURCE = {0x8577: 0, 0x6210: 1, 0x6211: 2, 0x84C0: 3, 0x84C1: 4, 0x84C2: 5, 0x84C3: 6,
          0x8579: 13, 0x8576: 14, 0x8578: 15}
COLOR_OPERAND = {0x300: 0, 0x301: 1, 0x302: 2, 0x303: 3, 0x8580: 4, 0x8583: 5,
                 0x8581: 8, 0x8584: 9, 0x8582: 12, 0x8585: 13}
ALPHA_OPERAND = {0x302: 0, 0x303: 1, 0x8580: 2, 0x8583: 3, 0x8581: 4, 0x8584: 5, 0x8582: 6, 0x8585: 7}
COMBINE = {0x1E01: 0, 0x2100: 1, 0x0104: 2, 0x8574: 3, 0x8575: 4, 0x84E7: 5, 0x86AE: 6, 0x86AF: 7,
           0x6401: 8, 0x6402: 9}
SCALE = {1: 0, 2: 1, 4: 2}
STAGE_BASE = (0x0C0, 0x0C8, 0x0D0, 0x0D8, 0x0F0, 0x0F8)
PREVIOUS = 0x8578
# GL_NEVER..GL_ALWAYS -> PICA (NEVER, ALWAYS, EQUAL, NOTEQUAL, LESS, LEQUAL, GREATER, GEQUAL)
ALPHA_FUNCTION = {0x200: 0, 0x207: 1, 0x202: 2, 0x205: 3, 0x201: 4, 0x203: 5, 0x204: 6, 0x206: 7}


def stage_registers(comb: bytes) -> tuple[int, int, int, int]:
    h = struct.unpack_from('<18H', comb, 0)
    comb_rgb, comb_a, scale_rgb, scale_a, _, _ = h[0:6]
    src_rgb, op_rgb, src_a, op_a = h[6:9], h[9:12], h[12:15], h[15:18]
    source = (SOURCE[src_rgb[0]] | SOURCE[src_rgb[1]] << 4 | SOURCE[src_rgb[2]] << 8 |
              SOURCE[src_a[0]] << 16 | SOURCE[src_a[1]] << 20 | SOURCE[src_a[2]] << 24)
    operand = (COLOR_OPERAND[op_rgb[0]] | COLOR_OPERAND[op_rgb[1]] << 4 | COLOR_OPERAND[op_rgb[2]] << 8 |
               ALPHA_OPERAND[op_a[0]] << 12 | ALPHA_OPERAND[op_a[1]] << 16 | ALPHA_OPERAND[op_a[2]] << 20)
    combine = COMBINE[comb_rgb] | COMBINE[comb_a] << 16
    scale = SCALE[scale_rgb] | SCALE[scale_a] << 16
    return source, operand, combine, scale


def material_registers(c: bytes, base: int, combs: list[bytes]) -> dict[int, int]:
    count = struct.unpack_from('<I', c, base + 0x120)[0]
    indices = struct.unpack_from('<6H', c, base + 0x124)[:count]
    regs: dict[int, int] = {}
    buffer_bits = 0
    for stage, index in enumerate(indices):
        comb = combs[index]
        source, operand, combine, scale = stage_registers(comb)
        r = STAGE_BASE[stage]
        regs[r], regs[r + 1], regs[r + 2], regs[r + 4] = source, operand, combine, scale
        buf_rgb, buf_a = struct.unpack_from('<2H', comb, 8)
        if stage < 4:
            buffer_bits |= (buf_rgb == PREVIOUS) << (8 + stage) | (buf_a == PREVIOUS) << (12 + stage)
    regs[0x0E0] = buffer_bits
    used = struct.unpack_from('<I', c, base + 0x08)[0]
    regs[0x080] = (1 << min(used, 3)) - 1 if used else 0
    if c[base + 0x130]:
        function = ALPHA_FUNCTION.get(struct.unpack_from('<H', c, base + 0x132)[0], 1)
        regs[0x104] = 1 | function << 4 | c[base + 0x131] << 8
    return regs


def scan(d: bytes, out: dict[tuple, dict[int, int]], lit: set[tuple], errors: list[str], name: str) -> None:
    i = d.find(b'cmb ')
    while i >= 0:
        try:
            c = d[i:i + struct.unpack_from('<I', d, i + 4)[0]]
            mats, tex = struct.unpack_from('<2I', c, 0x28)
            if c[mats:mats + 4] == b'mats':
                n = struct.unpack_from('<I', c, mats + 8)[0]
                end = mats + 0xC + n * 0x15C
                combs = [c[end + k * 0x28:end + k * 0x28 + 0x28] for k in range((tex - end) // 0x28)]
                for m in range(n):
                    try:
                        regs = material_registers(c, mats + 0xC + m * 0x15C, combs)
                    except KeyError as missing:
                        errors.append(f'{name}: enum desconhecido 0x{missing.args[0]:x}')
                        continue
                    key = tuple(sorted(regs.items()))
                    out.setdefault(key, regs)
                    if c[mats + 0xC + m * 0x15C]:
                        lit.add(key)
        except struct.error:
            pass
        i = d.find(b'cmb ', i + 4)


def main() -> int:
    root, output = sys.argv[1], sys.argv[2]
    unique: dict[tuple, dict[int, int]] = {}
    errors: list[str] = []
    lit: set[tuple] = set()
    for directory, _, files in os.walk(root):
        for name in sorted(files):
            if name.endswith(('.zar', '.zsi', '.gar', '.cmb')):
                with open(os.path.join(directory, name), 'rb') as f:
                    scan(f.read(), unique, lit, errors, name)
    # <saida>.luz: ids dos materiais com luz de fragmento ligada em algum CMB.
    with open(output, 'w') as f, open(output + '.luz', 'w') as luz:
        for index, (key, regs) in enumerate(unique.items()):
            f.write(f'm{index:04d} ' + ' '.join(f'{r:x}={v:x}' for r, v in sorted(regs.items())) + '\n')
            if key in lit:
                luz.write(f'm{index:04d}\n')
    print(f'{len(unique)} materiais únicos ({len(lit)} com luz de fragmento), {len(errors)} com enum desconhecido')
    for line in sorted(set(errors))[:10]:
        print(' ', line)
    return 0


if __name__ == '__main__':
    sys.exit(main())
