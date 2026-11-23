#!/usr/bin/env python3
"""Casa funções A32 entre dois code.bin (ex.: OoT3D -> MM3D) para transferir endereços.

  casar_funcoes.py --origem-code A/code.bin --origem-exheader A/exheader.bin
                   --origem-inventario A.csv
                   --destino-code B/code.bin --destino-exheader B/exheader.bin
                   --destino-inventario B.csv
                   [--nomes revisado.csv] [--alvos alvos.csv]
                   [--saida pares.csv] [--saida-alvos alvos_mapeados.csv]

Só análise: lê bytes, não compila nem roda o jogo, sem dependência externa.

Corpo da função: de `entry` até `entry+size` (coluna size do inventário) ou,
sem size, até a próxima entrada do mesmo inventário (o inventário de
discover_functions.py não tem size). Use o mesmo descobridor nos dois lados
para os cortes serem simétricos.

Normalização de cada palavra A32:
  * B/BL/BLX imediato: só o opcode e a condição (o deslocamento muda com o layout);
  * palavra cujo valor cai dentro da imagem (base..base+code+bss): vira "ENDEREÇO"
    (literal pool com ponteiro para código/dados);
  * nível frouxo (loose) também apaga imediatos de load/store, de processamento de
    dados e de VFP: casa funções iguais com offsets de struct diferentes.

Casamento, do mais forte para o mais fraco (cada par é único dos dois lados):
  1. exato: hash do corpo normalizado;
  2. frouxo: hash do corpo frouxo;
  3. vizinhança: a partir de cada par já casado, entre os chamadores/chamados ainda
     soltos dos dois lados, o que tiver chave única (exato, frouxo ou os 8 primeiros
     frouxos + tamanho parecido) dentro da vizinhança; repete até não mudar;
  4. semelhança: compiladores diferentes (OoT3D 2011 x MM3D 2015) mudam registradores e
     ordem das instruções, então o hash não pega o código do jogo. Cada função vira um
     conjunto de traços que sobrevivem à recompilação: strings e constantes dos literal
     pools, floats de VLDR, imediatos >= 0x100 e as funções chamadas já casadas. Par =
     melhor candidato dos dois lados (mútuo), Jaccard ponderado por raridade >= 0,5 e
     folga >= 0,1 sobre o segundo, tamanho entre 1/2 e 2x. Alterna com a vizinhança.
Um endereço do meio de função (gancho) é transferido como entrada_casada + offset;
só é "offset_confiavel" na entrada ou em par exato/frouxo de mesmo tamanho.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import struct
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

PAGE = 0x1000
ADDRESS_TOKEN = 0xADD7E550
PREFIX_WORDS = 8
NIVEIS = ("exato", "frouxo", "vizinhanca", "semelhanca")
MIN_SCORE, MIN_MARGIN, MIN_FEATURES = 0.5, 0.1, 3
SMALL_BYTES = 24  # par exato/frouxo de função menor que isso pode ser coincidência


@dataclass
class Image:
    code: bytes
    base: int
    text_size: int
    limit: int  # fim (exclusivo) da faixa considerada endereço da imagem

    @classmethod
    def from_exheader(cls, code: bytes, exheader: bytes) -> "Image":
        text_address, _, text_size = struct.unpack_from("<3I", exheader, 0x10)
        bss = struct.unpack_from("<I", exheader, 0x3C)[0]
        return cls(code, text_address, text_size, text_address + len(code) + bss)

    def word(self, address: int) -> int:
        return struct.unpack_from("<I", self.code, address - self.base)[0]

    def inside(self, address: int, size: int = 4) -> bool:
        return self.base <= address and address - self.base + size <= len(self.code)

    def c_string(self, address: int) -> str | None:
        if address < self.base + self.text_size or not self.inside(address, 1):
            return None
        start = address - self.base
        end = self.code.find(b"\0", start, start + 96)
        text = self.code[start:end] if end >= 0 else b""
        if len(text) >= 4 and all(0x20 <= c < 0x7F for c in text):
            return text.decode()
        return None


@dataclass
class Func:
    entry: int
    size: int
    exact: str = ""
    loose: str = ""
    prefix: str = ""
    callees: list[int] = field(default_factory=list)
    callers: set[int] = field(default_factory=set)
    features: Counter = field(default_factory=Counter)


def is_branch_imm(word: int) -> bool:
    if word >> 28 == 0xF:
        return word & 0xFE000000 == 0xFA000000  # BLX imediato
    return word & 0x0E000000 == 0x0A000000  # B/BL


def branch_target(pc: int, word: int) -> int:
    offset = word & 0x00FFFFFF
    if offset & 0x00800000:
        offset -= 1 << 24
    target = pc + 8 + offset * 4
    if word >> 28 == 0xF:
        target += (word >> 23) & 2  # bit H do BLX (destino Thumb)
    return target


def normalize(word: int, image: Image, loose: bool) -> int:
    if image.base <= word < image.limit:
        return ADDRESS_TOKEN
    if is_branch_imm(word):
        return word & (0xFE000000 if word >> 28 == 0xF else 0xFF000000)
    if not loose or word >> 28 == 0xF:
        return word
    if word & 0x0C000000 == 0x04000000:  # LDR/STR(B) imediato ou registrador
        return word & ~0xFFF if not word & 0x02000000 else word
    if word & 0x0E000000 == 0x02000000:  # processamento de dados com imediato
        return word & ~0xFFF
    if word & 0x0E400090 == 0x00400090:  # LDRH/STRH/LDRD/LDRSB imediato
        return word & ~0xF0F
    if word & 0x0E000E00 == 0x0C000A00:  # VLDR/VSTR/VLDM/VSTM
        return word & ~0xFF
    return word


def digest(values) -> str:
    return hashlib.blake2b(struct.pack(f"<{len(values)}I", *values), digest_size=12).hexdigest()


def read_entries(path: Path, image: Image) -> dict[int, int | None]:
    """{entrada: size ou None} das linhas com entrada no .text (exclui data_pointer)."""
    entries: dict[int, int | None] = {}
    with path.open(newline="") as handle:
        for row in csv.DictReader(handle):
            if not row.get("entry") or row.get("status") == "data_pointer":
                continue
            entry = int(row["entry"], 16)
            if entry % 4 or not image.base <= entry < image.base + image.text_size:
                continue
            size = int(row["size"]) if row.get("size") else None
            entries[entry] = size
    return entries


def build_functions(image: Image, entries: dict[int, int | None]) -> dict[int, Func]:
    ordered = sorted(entries)
    text_end = image.base + image.text_size
    funcs: dict[int, Func] = {}
    for index, entry in enumerate(ordered):
        size = entries[entry]
        if size is None:
            end = ordered[index + 1] if index + 1 < len(ordered) else text_end
            size = end - entry
        size = max(4, min(size, text_end - entry)) & ~3
        funcs[entry] = Func(entry, size)
    for func in funcs.values():
        words = [image.word(func.entry + at) for at in range(0, func.size, 4)]
        exact = [normalize(w, image, False) for w in words]
        loose = [normalize(w, image, True) for w in words]
        func.exact = digest(exact)
        func.loose = digest(loose)
        func.prefix = digest(loose[:PREFIX_WORDS])
        func.features = static_features(image, func.entry, words)
        for at, word in enumerate(words):
            if word >> 28 != 0xF and word & 0x0F000000 == 0x0B000000:  # BL
                target = branch_target(func.entry + at * 4, word)
            elif word & 0xFE000000 == 0xFA000000:  # BLX imediato
                target = branch_target(func.entry + at * 4, word) & ~1
            elif word >> 28 != 0xF and word & 0x0F000000 == 0x0A000000:  # B (tail call)
                target = branch_target(func.entry + at * 4, word)
                if func.entry <= target < func.entry + func.size:
                    continue
            else:
                continue
            if target in funcs:
                func.callees.append(target)
    for func in funcs.values():
        for target in func.callees:
            funcs[target].callers.add(func.entry)
    return funcs


def static_features(image: Image, entry: int, words: list[int]) -> Counter:
    """Traços que sobrevivem à recompilação (ver docstring do módulo)."""
    features: Counter = Counter()
    for at, word in enumerate(words):
        pc = entry + at * 4 + 8
        if word >> 28 == 0xF:
            continue
        if word & 0x0F7F0000 == 0x051F0000:  # LDR rX, [pc, #±imm]
            literal = pc + (word & 0xFFF if word & 0x00800000 else -(word & 0xFFF))
            if not image.inside(literal):
                continue
            value = image.word(literal)
            text = image.c_string(value)
            if text is not None:
                features["s:" + text] += 1
            elif not image.base <= value < image.limit:
                features[f"c:{value:x}"] += 1
        elif word & 0x0F3F0E00 == 0x0D1F0A00:  # VLDR s/d, [pc, #±imm]
            offset = (word & 0xFF) * 4
            literal = pc + (offset if word & 0x00800000 else -offset)
            if image.inside(literal, 8 if word & 0x100 else 4):
                value = image.word(literal)
                if word & 0x100:
                    value |= image.word(literal + 4) << 32
                features[f"f:{value:x}"] += 1
        elif word & 0x0E000000 == 0x02000000:  # processamento de dados com imediato
            rotate = (word >> 8 & 0xF) * 2
            imm = word & 0xFF
            value = ((imm >> rotate) | (imm << (32 - rotate))) & 0xFFFFFFFF if rotate else imm
            if value >= 0x100:
                features[f"i:{value:x}"] += 1
    return features


def call_features(func: Func, rename) -> Counter:
    calls: Counter = Counter()
    for target in func.callees:
        name = rename(target)
        if name is not None:
            calls[f"k:{name:x}"] += 1
    return calls


def similarity_pairs(src: list[Func], dst: list[Func], src_calls, dst_calls) -> list[tuple[int, int]]:
    """Pares mútuos pelo Jaccard ponderado (peso = log(N/df) no destino)."""
    import math
    src_sets = {f.entry: f.features + src_calls(f) for f in src}
    dst_sets = {f.entry: f.features + dst_calls(f) for f in dst}
    df: Counter = Counter()
    index: dict[str, list[int]] = defaultdict(list)
    for entry, feats in dst_sets.items():
        for feat in feats:
            df[feat] += 1
            index[feat].append(entry)
    total = max(1, len(dst_sets))

    def weight(feat):
        return math.log(1 + total / df.get(feat, 1))

    def score(a, b):
        inter = sum(weight(k) * min(v, b[k]) for k, v in a.items() if k in b)
        union = sum(weight(k) * max(a.get(k, 0), b.get(k, 0)) for k in a.keys() | b.keys())
        return inter / union if union else 0.0

    sizes = {f.entry: f.size for f in (*src, *dst)}
    best_of_src: dict[int, tuple[float, int, float]] = {}
    best_of_dst: dict[int, tuple[float, int]] = {}
    for f in src:
        feats = src_sets[f.entry]
        if sum(feats.values()) < MIN_FEATURES:
            continue
        candidates = {e for k in feats if df.get(k, 0) <= 20 for e in index.get(k, ())}
        scored = sorted(((score(feats, dst_sets[e]), e) for e in candidates
                         if sum(dst_sets[e].values()) >= MIN_FEATURES
                         and 0.5 <= sizes[e] / f.size <= 2.0), reverse=True)
        if not scored:
            continue
        second = scored[1][0] if len(scored) > 1 else 0.0
        best_of_src[f.entry] = (scored[0][0], scored[0][1], second)
        for value, e in scored:
            if value > best_of_dst.get(e, (-1.0, 0))[0]:
                best_of_dst[e] = (value, f.entry)
    return [(a, b) for a, (value, b, second) in best_of_src.items()
            if value >= MIN_SCORE and value - second >= MIN_MARGIN and best_of_dst.get(b, (0, 0))[1] == a]


def unique_pairs(src: list[Func], dst: list[Func], key) -> list[tuple[int, int]]:
    by_src: dict[object, list[Func]] = defaultdict(list)
    by_dst: dict[object, list[Func]] = defaultdict(list)
    for func in src:
        by_src[key(func)].append(func)
    for func in dst:
        by_dst[key(func)].append(func)
    return [(a[0].entry, by_dst[k][0].entry) for k, a in by_src.items()
            if len(a) == 1 and len(by_dst.get(k, ())) == 1]


def similar_size(a: Func, b: Func) -> bool:
    return abs(a.size - b.size) <= max(16, max(a.size, b.size) // 4)


def match(src: dict[int, Func], dst: dict[int, Func], rounds: int = 4) -> dict[int, tuple[int, str]]:
    matches: dict[int, tuple[int, str]] = {}
    used: set[int] = set()

    def add(pairs, level):
        added = 0
        for a, b in pairs:
            if a not in matches and b not in used:
                matches[a] = (b, level)
                used.add(b)
                added += 1
        return added

    def free_src():
        return [f for e, f in src.items() if e not in matches]

    def free_dst():
        return [f for e, f in dst.items() if e not in used]

    keys = (lambda f: ("e", f.exact), lambda f: ("l", f.loose), lambda f: ("p", f.prefix))

    def propagate():
        changed = True
        while changed:
            changed = False
            for a, (b, _) in list(matches.items()):
                for neighbours in ("callees", "callers"):
                    na = [src[e] for e in set(getattr(src[a], neighbours)) if e not in matches]
                    nb = [dst[e] for e in set(getattr(dst[b], neighbours)) if e not in used]
                    for key in keys:
                        if not na or not nb:
                            break
                        pairs = [(x, y) for x, y in unique_pairs(na, nb, key)
                                 if key is not keys[2] or similar_size(src[x], dst[y])]
                        if add(pairs, "vizinhanca"):
                            changed = True
                            na = [f for f in na if f.entry not in matches]
                            nb = [f for f in nb if f.entry not in used]

    add(unique_pairs(free_src(), free_dst(), lambda f: f.exact), "exato")
    add(unique_pairs(free_src(), free_dst(), lambda f: f.loose), "frouxo")
    for _ in range(rounds):
        propagate()
        reverse = {b: a for a, (b, _) in matches.items()}
        pairs = similarity_pairs(free_src(), free_dst(),
                                 lambda f: call_features(f, lambda t: t if t in matches else None),
                                 lambda f: call_features(f, reverse.get))
        if not add(pairs, "semelhanca"):
            break
    propagate()
    return matches


def containing(funcs: dict[int, Func], address: int) -> Func | None:
    best = None
    for func in funcs.values():
        if func.entry <= address < func.entry + func.size and (best is None or func.entry > best.entry):
            best = func
    return best


def map_address(address: int, src: dict[int, Func], dst: dict[int, Func],
                matches: dict[int, tuple[int, str]]) -> dict[str, object]:
    func = containing(src, address)
    if func is None:
        return {"destino": "", "nivel": "fora_de_funcao", "offset_confiavel": False}
    if func.entry not in matches:
        return {"destino": "", "nivel": "sem_par", "offset_confiavel": False,
                "funcao_origem": f"0x{func.entry:08X}"}
    other, level = matches[func.entry]
    offset = address - func.entry
    return {"destino": f"0x{other + offset:08X}", "nivel": level,
            # Só exato/frouxo garantem a mesma sequência de instruções; na vizinhança e na
            # semelhança o corpo foi recompilado e o offset do meio não se preserva.
            "offset_confiavel": offset == 0 or (level in ("exato", "frouxo") and dst[other].size == func.size),
            "funcao_origem": f"0x{func.entry:08X}", "funcao_destino": f"0x{other:08X}"}


def order_consistency(matches: dict[int, tuple[int, str]]) -> float:
    """Fração dos pares cuja ordem de endereço concorda com o vizinho casado anterior e posterior.

    Ligadores mantêm a ordem dos objetos; par falso costuma quebrar a monotonia local.
    """
    pairs = sorted((a, b) for a, (b, _) in matches.items())
    if len(pairs) < 3:
        return 1.0
    good = sum(1 for i in range(1, len(pairs) - 1)
               if pairs[i - 1][1] < pairs[i][1] < pairs[i + 1][1])
    return good / (len(pairs) - 2)


def load_side(code_path: Path, exheader_path: Path, inventory: Path) -> tuple[Image, dict[int, Func]]:
    image = Image.from_exheader(code_path.read_bytes(), exheader_path.read_bytes())
    return image, build_functions(image, read_entries(inventory, image))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    for side in ("origem", "destino"):
        parser.add_argument(f"--{side}-code", type=Path, required=True)
        parser.add_argument(f"--{side}-exheader", type=Path, required=True)
        parser.add_argument(f"--{side}-inventario", type=Path, required=True)
    parser.add_argument("--nomes", type=Path, help="CSV com entry,name (ex.: inventário revisado do OoT3D)")
    parser.add_argument("--alvos", type=Path, help="CSV com address,item,name a transferir")
    parser.add_argument("--saida", type=Path)
    parser.add_argument("--saida-alvos", type=Path)
    args = parser.parse_args()

    _, src = load_side(args.origem_code, args.origem_exheader, args.origem_inventario)
    _, dst = load_side(args.destino_code, args.destino_exheader, args.destino_inventario)
    matches = match(src, dst)
    names: dict[int, str] = {}
    if args.nomes:
        with args.nomes.open(newline="") as handle:
            names = {int(r["entry"], 16): r["name"] for r in csv.DictReader(handle) if r.get("entry")}
    levels = Counter(level for _, level in matches.values())
    summary = {"funcoes_origem": len(src), "funcoes_destino": len(dst), "pares": len(matches),
               **{f"pares_{k}": levels.get(k, 0) for k in NIVEIS},
               "ordem_consistente": round(order_consistency(matches), 4)}
    if names:
        named = [e for e in src if e in names]
        summary["nomeadas_origem"] = len(named)
        summary["nomeadas_casadas"] = sum(1 for e in named if e in matches)
    if args.saida:
        with args.saida.open("w", newline="") as handle:
            writer = csv.writer(handle)
            writer.writerow(("origem", "destino", "nivel", "size_origem", "size_destino", "nome"))
            for a, (b, level) in sorted(matches.items()):
                writer.writerow((f"0x{a:08X}", f"0x{b:08X}", level, src[a].size, dst[b].size, names.get(a, "")))
    if args.alvos:
        with args.alvos.open(newline="") as handle:
            targets = list(csv.DictReader(handle))
        rows = []
        for row in targets:
            mapped = map_address(int(row["address"], 16), src, dst, matches)
            rows.append({**row, **mapped})
        counts = Counter(r["nivel"] for r in rows)
        summary["alvos"] = len(rows)
        summary["alvos_por_nivel"] = dict(counts)
        summary["alvos_offset_confiavel"] = sum(1 for r in rows if r["offset_confiavel"])
        if args.saida_alvos:
            fields = list(rows[0]) if rows else ["address"]
            for r in rows:
                fields += [k for k in r if k not in fields]
            with args.saida_alvos.open("w", newline="") as handle:
                writer = csv.DictWriter(handle, fieldnames=fields)
                writer.writeheader()
                writer.writerows(rows)
    json.dump(summary, sys.stdout, ensure_ascii=False, indent=1)
    print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
