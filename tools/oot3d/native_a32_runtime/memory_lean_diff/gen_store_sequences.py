#!/usr/bin/env python3
"""Gera sequências de acessos à memória guest no formato do C++ gerado.

Cada função Seq_N imita um bloco do whole-AOT: registradores guest como
referências para state.R[i] (como o gerador faz), escritas WriteFast<T> de
todos os tamanhos em endereços sobrepostos e desalinhados, leituras
ReadFast<T>, LDM/STM com ReadWordsFast/WriteWordsFast e o fallback palavra a
palavra, exclusivos (LoadExclusive/StoreExclusive), flags, observações da
geração no meio da sequência e falhas (página só leitura, buraco, borda de
página), além de rajadas de acessos sobrepostos de tamanhos diferentes. Cada
Raw_N é uma sequência em linha reta só com StoreGuestValue/LoadGuestValue
(sem caminho lento entre os acessos). O mesmo texto é compilado contra a
memória atual e contra a antiga (store_sequence_side.cpp); o objetivo é pegar
qualquer reordenação ou encaminhamento errado introduzido pelo otimizador
(TBAA) nos acessos inlinados. Sensibilidade conferida: com um conjunto de
alias por tamanho (enums soltos em vez da união GuestCell) todas as execuções
divergem, no host e no devkitA64/qemu.

uso: gen_store_sequences.py <saida.inc> [funcoes] [operacoes] [semente]
"""
import random
import sys

SIZES = {8: "uint8_t", 16: "uint16_t", 32: "uint32_t", 64: "uint64_t"}
VALUE_REGS = list(range(0, 8))      # r0..r7: valores
BASE_REGS = list(range(8, 13))      # r8..r12: bases de endereço (nunca destino de ALU)


def emit_function(rng, index, ops):
    out = []
    out.append(f"SEQ_FN(Seq_{index}) {{")
    for r in range(15):
        out.append(f"    uint32_t& r{r} = state.R[{r}];")
    out.append("    auto& flags = state.Flags;")
    pc = 0x1000
    for _ in range(ops):
        pc += 4
        kind = rng.choices(
            ["w", "r", "alu", "stm", "ldm", "ldrex", "strex", "obs", "flag",
             "rebase", "rw", "hazard"],
            weights=[30, 22, 14, 5, 5, 1, 1, 2, 3, 3, 8, 10])[0]
        base = f"r{rng.choice(BASE_REGS)}"
        imm = rng.choice([0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, rng.randrange(64),
                          0xFFC, 0xFFD, 0xFFE, 0xFFF, 0x1000 - 2])
        if kind == "w" or kind == "rw":
            size = rng.choice([8, 16, 32, 32, 64])
            t = SIZES[size]
            a = f"r{rng.choice(VALUE_REGS)}"
            if size == 64:
                b = f"r{rng.choice(VALUE_REGS)}"
                value = f"(uint64_t{{{b}}} << 32U) | {a}"
            else:
                value = f"static_cast<{t}>({a})"
            out.append("    {")
            out.append(f"        const uint32_t address = {base} + 0x{imm:X}U;")
            out.append(f"        if (!MEM.WriteFast<{t}>(address, {value})) {{ FAULT(0x{pc:X}U, address); }}")
            out.append("    }")
            if kind == "rw":
                # Leitura logo depois, possivelmente sobreposta à escrita.
                size2 = rng.choice([8, 16, 32, 64])
                t2 = SIZES[size2]
                d = rng.choice(VALUE_REGS)
                imm2 = max(0, imm + rng.choice([-3, -2, -1, 0, 0, 1, 2, 3]))
                out.append("    {")
                out.append(f"        const uint32_t address = {base} + 0x{imm2:X}U;")
                out.append(f"        {t2} value = 0;")
                out.append(f"        if (!MEM.ReadFast<{t2}>(address, &value)) {{ FAULT(0x{pc:X}U, address); }}")
                if size2 == 64:
                    out.append(f"        r{d} = static_cast<uint32_t>(value); r{(d + 1) % 8} = static_cast<uint32_t>(value >> 32U);")
                else:
                    out.append(f"        r{d} = value;")
                out.append("    }")
        elif kind == "r":
            size = rng.choice([8, 16, 32, 32, 64])
            t = SIZES[size]
            d = rng.choice(VALUE_REGS)
            out.append("    {")
            out.append(f"        const uint32_t address = {base} + 0x{imm:X}U;")
            out.append(f"        {t} value = 0;")
            out.append(f"        if (!MEM.ReadFast<{t}>(address, &value)) {{ FAULT(0x{pc:X}U, address); }}")
            if size == 64:
                out.append(f"        r{d} = static_cast<uint32_t>(value); r{(d + 1) % 8} = static_cast<uint32_t>(value >> 32U);")
            else:
                out.append(f"        r{d} = value;")
            out.append("    }")
        elif kind == "alu":
            d = rng.choice(VALUE_REGS)
            a = rng.choice(VALUE_REGS)
            b = rng.choice(VALUE_REGS)
            op = rng.choice(["+", "^", "-", "*", "|"])
            out.append(f"    r{d} = r{a} {op} (r{b} + 0x{rng.randrange(1 << 32):08X}U);")
        elif kind in ("stm", "ldm"):
            count = rng.choice([2, 3, 4, 5, 8])
            regs = rng.sample(VALUE_REGS, min(count, 8)) if count <= 8 else VALUE_REGS
            regs = (regs * 2)[:count]
            # STM/LDM: endereço alinhado como no gerador (pode cruzar página).
            imm = rng.choice([0, 4, 8, 16, 0xFF8, 0xFFC, 0xFF0])
            out.append("    {")
            out.append(f"        const uint32_t firstAddress = {base} + 0x{imm:X}U;")
            if kind == "stm":
                words = ", ".join(f"r{r}" for r in regs)
                out.append(f"        const uint32_t spanWords[{count}] = {{{words}}};")
                out.append(f"        if (!MEM.WriteWordsFast(firstAddress, spanWords, {count}U)) {{")
                for i in range(count):
                    out.append(f"            if (!MEM.WriteFast<uint32_t>(firstAddress + {4 * i}U, spanWords[{i}])) {{ FAULT(0x{pc:X}U, firstAddress + {4 * i}U); }}")
                out.append("        }")
            else:
                out.append(f"        uint32_t spanWords[{count}] = {{}};")
                out.append(f"        if (!MEM.ReadWordsFast(firstAddress, spanWords, {count}U)) {{")
                for i in range(count):
                    out.append(f"            if (!MEM.ReadFast<uint32_t>(firstAddress + {4 * i}U, &spanWords[{i}])) {{ FAULT(0x{pc:X}U, firstAddress + {4 * i}U); }}")
                out.append("        }")
                for i, r in enumerate(regs):
                    out.append(f"        r{r} = spanWords[{i}];")
            out.append("    }")
        elif kind == "ldrex":
            d = rng.choice(VALUE_REGS)
            out.append("    {")
            out.append(f"        const uint32_t address = ({base} + 0x{imm:X}U) & ~3U;")
            out.append("        uint64_t value = 0; uint32_t fault = 0;")
            out.append("        if (!MEM.LoadExclusive(address, 4U, &value, &exclusiveToken, &fault)) { FAULT(0x%XU, fault); }" % pc)
            out.append(f"        r{d} = static_cast<uint32_t>(value);")
            out.append("    }")
        elif kind == "strex":
            a = rng.choice(VALUE_REGS)
            d = rng.choice(VALUE_REGS)
            out.append("    {")
            out.append(f"        const uint32_t address = ({base} + 0x{imm:X}U) & ~3U;")
            out.append("        uint32_t fault = 0;")
            out.append(f"        const auto result = MEM.StoreExclusive(address, 4U, r{a}, exclusiveToken, &fault);")
            out.append(f"        if (result == a32::ExclusiveStoreResult::MemoryFault) {{ FAULT(0x{pc:X}U, fault); }}")
            out.append(f"        r{d} = result == a32::ExclusiveStoreResult::Success ? 0U : 1U;")
            out.append("    }")
        elif kind == "obs":
            out.append(f"    Observe(observations, MEM, {base});")
        elif kind == "flag":
            a = rng.choice(VALUE_REGS)
            b = rng.choice(VALUE_REGS)
            out.append(f"    Oot3dAotSetSubtractFlags(flags, r{a}, r{b}, static_cast<Oot3dAotFlagMask>(0xFU));")
            out.append(f"    if (flags.C()) {{ r{rng.choice(VALUE_REGS)} += 1U; }}")
        elif kind == "hazard":
            # Acessos sobrepostos de tamanhos diferentes na mesma base, sem
            # nada entre eles: escrita, escrita parcial por cima e leitura
            # (ou leitura entre as duas). Um otimizador que separasse os
            # tamanhos em conjuntos de alias diferentes encaminharia valores
            # velhos ou trocaria a ordem das escritas aqui.
            imm = rng.choice([0, 4, 8, 0x20, 0xFF8, 0xFFC])
            steps = []
            for _ in range(rng.choice([2, 3, 3, 4])):
                size = rng.choice([8, 16, 32, 64])
                delta = rng.choice([0, 0, 1, 2, 3, 4, 6])
                steps.append((rng.choice(["w", "w", "r"]), size, imm + delta))
            steps.append(("r", rng.choice([8, 16, 32, 64]), imm + rng.choice([0, 0, 1, 2, 4])))
            for step, size, offset in steps:
                t = SIZES[size]
                out.append("    {")
                out.append(f"        const uint32_t address = {base} + 0x{offset:X}U;")
                if step == "w":
                    a = f"r{rng.choice(VALUE_REGS)}"
                    value = (f"(uint64_t{{r{rng.choice(VALUE_REGS)}}} << 32U) | {a}"
                             if size == 64 else f"static_cast<{t}>({a} + 0x{rng.randrange(1 << 32):08X}U)")
                    out.append(f"        if (!MEM.WriteFast<{t}>(address, {value})) {{ FAULT(0x{pc:X}U, address); }}")
                else:
                    d = rng.choice(VALUE_REGS)
                    out.append(f"        {t} value = 0;")
                    out.append(f"        if (!MEM.ReadFast<{t}>(address, &value)) {{ FAULT(0x{pc:X}U, address); }}")
                    if size == 64:
                        out.append(f"        r{d} ^= static_cast<uint32_t>(value) + static_cast<uint32_t>(value >> 32U);")
                    else:
                        out.append(f"        r{d} = r{d} * 3U + value;")
                out.append("    }")
        elif kind == "rebase":
            b = rng.choice(BASE_REGS)
            a = rng.choice(VALUE_REGS)
            # Mantém a base nas páginas de teste: o valor vem da memória guest,
            # então a escolha do endereço depende de escritas anteriores.
            out.append(f"    r{b} = kSeqBases[(r{a} >> 3U) % kSeqBaseCount] + (r{a} & 0x1FU);")
    out.append("}")
    return "\n".join(out)


def emit_raw_function(rng, index, ops):
    """Sequência em linha reta só com os auxiliares de acesso (sem caminho
    lento nem chamadas entre os acessos): é aqui que um conjunto de alias
    errado apareceria primeiro (valor velho encaminhado, escritas trocadas)."""
    out = [f"RAW_FN(Raw_{index}) {{"]
    for r in range(8):
        out.append(f"    uint32_t& r{r} = state.R[{r}];")
    for _ in range(ops):
        size = rng.choice([8, 16, 32, 64])
        t = SIZES[size]
        offset = rng.choice([0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 13])
        if rng.random() < 0.55:
            a = f"r{rng.choice(range(8))}"
            value = (f"(uint64_t{{r{rng.choice(range(8))}}} << 32U) | {a}"
                     if size == 64 else f"static_cast<{t}>({a} + 0x{rng.randrange(1 << 32):08X}U)")
            out.append(f"    RAW_STORE({t}, bytes + {offset}, {value});")
        else:
            d = rng.choice(range(8))
            if size == 64:
                out.append(f"    {{ const uint64_t v = RAW_LOAD({t}, bytes + {offset}); "
                           f"r{d} ^= static_cast<uint32_t>(v) * 5U + static_cast<uint32_t>(v >> 32U); }}")
            else:
                out.append(f"    r{d} = r{d} * 3U + RAW_LOAD({t}, bytes + {offset});")
    out.append("}")
    return "\n".join(out)


def main():
    path = sys.argv[1]
    functions = int(sys.argv[2]) if len(sys.argv) > 2 else 200
    ops = int(sys.argv[3]) if len(sys.argv) > 3 else 60
    seed = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    rng = random.Random(seed)
    parts = ["// Gerado por gen_store_sequences.py; não editar."]
    for index in range(functions):
        parts.append(emit_function(rng, index, ops))
    names = ", ".join(f"Seq_{i}" for i in range(functions))
    parts.append(f"constexpr SeqFn kSeqFunctions[] = {{{names}}};")
    for index in range(functions):
        parts.append(emit_raw_function(rng, index, ops))
    names = ", ".join(f"Raw_{i}" for i in range(functions))
    parts.append(f"constexpr RawFn kRawFunctions[] = {{{names}}};")
    with open(path, "w") as f:
        f.write("\n\n".join(parts) + "\n")


if __name__ == "__main__":
    main()
