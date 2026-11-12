#!/usr/bin/env python3
"""Converts a Ship of Harkinian save (file1.sav, JSON of the N64 SaveContext) into an OoT3D save00.bin.

Usage: soh_save_to_oot3d.py <file1.sav> <base OoT3D save00.bin> <output>

The OoT3D save00.bin is the SaveContext up to the checksum (0x14DC bytes, little-endian, layout from the
OoT3D Randomizer's z3D.h); the checksum (+0x14D8) is CRC-16/ARC of the whole file with the field zeroed, the same as
system.dat (oot3d_native_master_quest_unlock.h). The base file keeps what only the 3DS has (control
options, camera, boss statistics); the rest comes from SoH. OoT3D item IDs are the N64 ones.
A Master Quest save goes into an MQ slot: save03.bin to save05.bin (slots 0-2 are normal mode;
FUN_002e63c8, oot3d_native_master_quest_unlock.h). Left out: scarecrow songs, records (fishing, horse, Big Poe) and the menu's new-item marks.
"""
import json
import struct
import sys

SAVE_SIZE = 0x14DC
CHECKSUM = 0x14D8


def crc16_arc(data):
    crc = 0
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def store_checksum(save):
    save[CHECKSUM:CHECKSUM + 2] = b"\0\0"
    struct.pack_into("<H", save, CHECKSUM, crc16_arc(save))


def n64_name(chars):
    """N64 font (0-9, A-Z, a-z, 0x3E space) to text."""
    out = ""
    for c in chars:
        if c <= 0x09:
            out += chr(ord("0") + c)
        elif c <= 0x23:
            out += chr(ord("A") + c - 0x0A)
        elif c <= 0x3D:
            out += chr(ord("a") + c - 0x24)
        else:
            out += " "
    return out


def put_equips(save, offset, equips, inventory):
    # N64: B, C-left, C-down, C-right → 3DS: B, Y, X, I, II (II stays empty). A button shows the item that
    # is in its slot (SoH stored the Fairy Ocarina on a C with the Ocarina of Time in the slot).
    items = (equips["buttonItems"] + [255] * 4)[:4] + [255]
    slots = (equips["cButtonSlots"] + [255] * 3)[:3] + [255]
    for button, slot in enumerate(slots[:3], start=1):
        if slot < len(inventory) and inventory[slot] != 0xFF:
            items[button] = inventory[slot]
    save[offset:offset + 5] = bytes(v & 0xFF for v in items)
    save[offset + 5:offset + 9] = bytes(v & 0xFF for v in slots)
    struct.pack_into("<H", save, offset + 0x0A, equips["equipment"] & 0xFFFF)


def item_menu(items, has_bow):
    """Touch screen item grid: the slots Link has, in order (like the randomizer, the
    magic arrows only go in without the bow: with the bow they go along with it)."""
    arrows = {0x04, 0x0A, 0x10}
    owned = [slot for slot, item in enumerate(items) if item != 0xFF and not (has_bow and slot in arrows)]
    return bytes((owned + [0xFF] * 24)[:24])


def convert(soh, base):
    if len(base) != SAVE_SIZE or base[0x30:0x36] != b"ZELDAZ":
        raise SystemExit("the base save is not an OoT3D save00.bin")
    if struct.unpack_from("<H", base, CHECKSUM)[0] != crc16_arc(base[:CHECKSUM] + b"\0\0" + base[CHECKSUM + 2:]):
        raise SystemExit("base save checksum doesn't match")
    d = soh["sections"]["base"]["data"]
    inv = d["inventory"]
    s = bytearray(base)
    struct.pack_into("<iii", s, 0x00, d["entranceIndex"], d["linkAge"], d["cutsceneIndex"])
    struct.pack_into("<H", s, 0x0C, d["dayTime"] & 0xFFFF)
    s[0x0E] = 1 if d.get("isMasterQuest") else 0
    struct.pack_into("<iii", s, 0x10, d["nightFlag"], d["totalDays"], d["bgsDayCount"])
    name = n64_name(d["playerName"]).ljust(8)[:8]
    s[0x1C:0x2C] = name.encode("utf-16le")
    s[0x2C] = len(name.rstrip())
    struct.pack_into("<hhbbhHH", s, 0x42, d["healthCapacity"], d["health"], d["magicLevel"], d["magic"],
                     d["rupees"], d["swordHealth"] & 0xFFFF, d["naviTimer"] & 0xFFFF)
    s[0x4E] = d["isMagicAcquired"]
    s[0x50] = d["isDoubleMagicAcquired"]
    s[0x51] = d["isDoubleDefenseAcquired"]
    s[0x52] = d["bgsFlag"] & 0xFF
    items = [v & 0xFF for v in inv["items"][:24]]
    equipment = inv["equipment"]
    # OoT3D puts the iron boots and the hover boots in the item menu (slots 24 and 25).
    items += [0x45 if equipment & 0x2000 else 0xFF, 0x46 if equipment & 0x4000 else 0xFF]
    put_equips(s, 0x54, d["childEquips"], items)
    put_equips(s, 0x60, d["adultEquips"], items)
    struct.pack_into("<H", s, 0x7E, d["savedSceneNum"] & 0xFFFF)
    put_equips(s, 0x80, d["equips"], items)
    s[0x8C:0x8C + 26] = bytes(items)
    s[0xA6:0xA6 + 15] = bytes(v & 0xFF for v in inv["ammo"][:15])
    struct.pack_into("<HII", s, 0xB6, equipment & 0xFFFF, inv["upgrades"] & 0xFFFFFFFF,
                     inv["questItems"] & 0xFFFFFFFF)
    s[0xC0:0xC0 + 20] = bytes(v & 0xFF for v in inv["dungeonItems"][:20])
    s[0xD4:0xD4 + 19] = bytes(v & 0xFF for v in inv["dungeonKeys"][:19])
    struct.pack_into("<h", s, 0xE8, inv["gsTokens"])
    for index, flags in enumerate(d["sceneFlags"][:124]):
        struct.pack_into("<7I", s, 0xEC + index * 0x1C, *(flags[k] & 0xFFFFFFFF for k in
                         ("chest", "swch", "clear", "collect", "unk", "rooms", "floors")))
    fw = d["fw"]
    struct.pack_into("<8i", s, 0xE7C, fw["pos"]["x"], fw["pos"]["y"], fw["pos"]["z"], fw["yaw"],
                     fw["playerParams"], fw["entranceIndex"], fw["roomIndex"], fw["set"])
    struct.pack_into("<II", s, 0xE9C, fw["tempSwchFlags"] & 0xFFFFFFFF, fw["tempCollectFlags"] & 0xFFFFFFFF)
    # N64: gsFlags[6] u32, one byte per area (masks 0xFF << 8 * (area & 3)); 3DS: one byte per area.
    gs = [((d["gsFlags"][area >> 2] & 0xFFFFFFFF) >> (8 * (area & 3))) & 0xFF for area in range(22)]
    s[0xEB4:0xEB4 + 22] = bytes(gs)
    struct.pack_into("<14H", s, 0xEEC, *(v & 0xFFFF for v in d["eventChkInf"][:14]))
    struct.pack_into("<4H", s, 0xF08, *(v & 0xFFFF for v in d["itemGetInf"][:4]))
    struct.pack_into("<30H", s, 0xF10, *(v & 0xFFFF for v in d["infTable"][:30]))
    struct.pack_into("<I", s, 0xF50, d["worldMapAreaData"] & 0xFFFFFFFF)
    horse = d["horseData"]
    struct.pack_into("<5h", s, 0x1364, horse["scene"], horse["pos"]["x"], horse["pos"]["y"], horse["pos"]["z"],
                     horse["angle"])
    s[0x1370:0x1370 + 26] = bytes(items)
    has_bow = items[0x03] != 0xFF
    s[0x138A:0x138A + 24] = item_menu(items, has_bow)
    s[0x13A2:0x13A2 + 24] = item_menu(items, has_bow)
    store_checksum(s)
    return bytes(s), name, gs


def main(argv):
    if len(argv) != 4:
        raise SystemExit(__doc__)
    soh = json.load(open(argv[1]))
    base = open(argv[2], "rb").read()
    out, name, gs = convert(soh, base)
    open(argv[3], "wb").write(out)
    d = soh["sections"]["base"]["data"]
    print(f"{argv[3]}: {name.strip()}, {'child' if d['linkAge'] else 'adult'}, "
          f"{d['healthCapacity'] // 16} hearts, {d['inventory']['gsTokens']} skulltulas "
          f"({sum(bin(b).count('1') for b in gs)} flags), MQ={1 if d.get('isMasterQuest') else 0}, "
          f"scene {d['savedSceneNum']}, CRC {struct.unpack_from('<H', out, CHECKSUM)[0]:04x}")


if __name__ == "__main__":
    main(sys.argv)
