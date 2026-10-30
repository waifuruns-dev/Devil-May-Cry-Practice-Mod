"""DMC1 HD Collection save editor (prototype of the save builder).

Format notes: docs/SAVE_FORMAT.md

Usage examples:
  python dmc1_save.py info dmc1.sav
  python dmc1_save.py clean dmc1.sav out.sav --from-slot 4 --to-slot 9 --orbs 123456
"""
import argparse
import struct

SLOT_SIZE = 0x970
SLOT_COUNT = 10

DIFFICULTY = {2: "Easy", 3: "Normal", 5: "Hard", 6: "Dante Must Die"}
COSTUME = {0: "Dante", 1: "Legendary Dark Knight", 2: "Super Dante"}

# Starting max HP / DT bars on a new game, by difficulty.
# Easy and Hard are confirmed from fresh saves; Normal and DMD are assumed equal to Hard.
BASE_STATS = {2: (15, 6), 3: (10, 3), 5: (10, 3), 6: (10, 3)}

# Consumables removed by "clean": Vital Star, Devil Star, Untouchable, Holy Water.
# 0x10 (likely Yellow Orb) and 0x11 (unknown) are kept until we know what they are.
CONSUMABLE_IDS = {0x0A, 0x0B, 0x0C, 0x0D}

OFF_CHECKSUM = 0x08
OFF_SAVES = 0x20
OFF_MISSION = 0x24
OFF_DIFFICULTY = 0x26
OFF_COSTUME = 0x27
# Weapon IDs (item category 0). Guns are 1-5, devil arms 0 and 6-9.
WEAPONS = {
    0: "Force Edge",
    1: "Shotgun",
    2: "Needlegun",
    3: "Grenadegun",
    4: "Nightmare-Beta",
    5: "Handguns (Ebony & Ivory)",
    6: "Alastor",
    7: "Ifrit",
    8: "Sparda",
    9: "Yamato",
}

# Move bitfield (u16 at OFF_MOVES): bit -> name. All 14 purchasable moves (bits 1-14).
MOVE_BITS = {
    1: "Inferno",
    2: "Meteor Lv2",
    3: "Meteor Lv1",
    4: "Kick 13 Lv2",
    5: "Kick 13 Lv1",
    6: "Magma Drive",
    7: "Rolling Blaze",
    8: "Air Raid",
    9: "Vortex Lv2",
    10: "Vortex Lv1",
    11: "Stinger Lv2",
    12: "Stinger Lv1",
    13: "Round Trip",
    14: "Air Hike",
}
# Lv2 moves require their Lv1.
MOVE_REQUIRES = {2: 3, 4: 5, 9: 10, 11: 12}

# Starting equipment (written by the end-of-mission save; used at the next mission's start).
OFF_EQUIP_ARM = 0x235     # devil arm behaviour
OFF_EQUIP_GUN = 0x236
OFF_EQUIP_MODEL = 0x237   # devil arm model
DEVIL_ARMS = {"Alastor": (0, 0), "Ifrit": (1, 0), "Force Edge": (4, 0), "Sparda": (4, 1), "Yamato": (0, 2)}
GUNS = {"Handguns": 1, "Shotgun": 2, "Grenadegun": 3, "Nightmare-Beta": 4}

OFF_DT_CURRENT = 0x230   # u16 current DT gauge (200 per bar)
OFF_ITEM_COUNT = 0x23B   # number of non-key entries in the item list
OFF_ITEMS = 0x250        # 4-byte entries: [category, id, count, 0]
ITEMS_END = 0x2A0        # list area observed so far ends before this
OFF_HP_BAR = 0x624
OFF_DT_BAR = 0x625
OFF_BLUE_BOUGHT = 0x626
OFF_PURPLE_BOUGHT = 0x627
OFF_MOVES = 0x62A        # u16 bitfield
OFF_ORBS = 0x634


def checksum(slot: bytes) -> int:
    return sum(slot[0x0A:]) & 0xFFFF


def fix_checksum(slot: bytearray) -> None:
    struct.pack_into("<H", slot, OFF_CHECKSUM, checksum(slot))


def read_items(slot: bytes):
    """Returns (entries, tail): the real item list (first slot[0x23B] entries) and the stale
    entries left after it. The tail is ignored by the game and kept only so rewrites match."""
    n = slot[OFF_ITEM_COUNT]
    entries = [tuple(slot[OFF_ITEMS + 4 * i:OFF_ITEMS + 4 * i + 3]) for i in range(n)]
    tail = []
    off = OFF_ITEMS + 4 * n
    while off + 4 <= ITEMS_END and slot[off:off + 4] != b"\0\0\0\0":
        tail.append(tuple(slot[off:off + 3]))
        off += 4
    return entries, tail


def write_items(slot: bytearray, non_key, key=()) -> None:
    """Writes the real item list. `key` (stale tail) is optional; the game ignores it."""
    entries = list(non_key) + list(key)
    if OFF_ITEMS + 4 * len(entries) > ITEMS_END:
        raise ValueError("item list too long")
    for off in range(OFF_ITEMS, ITEMS_END):
        slot[off] = 0
    for i, (cat, iid, cnt) in enumerate(entries):
        slot[OFF_ITEMS + 4 * i:OFF_ITEMS + 4 * i + 4] = bytes((cat, iid, cnt, 0))
    slot[OFF_ITEM_COUNT] = len(non_key)


def describe(slot: bytes) -> str:
    non_key, key = read_items(slot)
    return (
        f"mission {slot[OFF_MISSION] + slot[OFF_MISSION + 1]:2}  "
        f"{DIFFICULTY.get(slot[OFF_DIFFICULTY], slot[OFF_DIFFICULTY])}  "
        f"{COSTUME.get(slot[OFF_COSTUME], slot[OFF_COSTUME])}  "
        f"orbs {struct.unpack_from('<I', slot, OFF_ORBS)[0]}  "
        f"HP bars {slot[OFF_HP_BAR]}  DT bars {slot[OFF_DT_BAR]}  "
        f"moves {struct.unpack_from('<H', slot, OFF_MOVES)[0]:016b}  "
        f"items {non_key}  "
        f"checksum {'ok' if struct.unpack_from('<H', slot, OFF_CHECKSUM)[0] == checksum(slot) else 'BAD'}"
    )


def set_equipment(slot: bytearray, devil_arm: str, gun: str) -> None:
    """Sets the weapons the next mission starts with. The Needlegun is never equipped by the game."""
    slot[OFF_EQUIP_ARM], slot[OFF_EQUIP_MODEL] = DEVIL_ARMS[devil_arm]
    slot[OFF_EQUIP_GUN] = GUNS[gun]


def clean(slot: bytearray, orbs: int) -> None:
    """Strip purchased loadout: moves, consumables, extra HP/DT, purchase counters; set orbs."""
    diff = slot[OFF_DIFFICULTY]
    hp, dt = BASE_STATS.get(diff, (10, 3))

    struct.pack_into("<H", slot, OFF_MOVES, 0)
    slot[OFF_HP_BAR] = hp
    slot[OFF_DT_BAR] = dt
    # Keep the stored gauge within the new capacity (120 per rune).
    cur = struct.unpack_from("<H", slot, OFF_DT_CURRENT)[0]
    struct.pack_into("<H", slot, OFF_DT_CURRENT, min(cur, dt * 120))
    slot[OFF_BLUE_BOUGHT] = 0
    slot[OFF_PURPLE_BOUGHT] = 0
    struct.pack_into("<I", slot, OFF_ORBS, orbs)

    non_key, key = read_items(slot)
    non_key = [e for e in non_key if not (e[0] == 2 and e[1] in CONSUMABLE_IDS)]
    write_items(slot, non_key, key)

    fix_checksum(slot)


def main() -> None:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p_info = sub.add_parser("info")
    p_info.add_argument("file")
    p_clean = sub.add_parser("clean")
    p_clean.add_argument("file")
    p_clean.add_argument("out")
    p_clean.add_argument("--from-slot", type=int, required=True, help="1-10")
    p_clean.add_argument("--to-slot", type=int, required=True, help="1-10")
    p_clean.add_argument("--orbs", type=int, default=0)
    args = ap.parse_args()

    data = bytearray(open(args.file, "rb").read())
    slots = [data[i * SLOT_SIZE:(i + 1) * SLOT_SIZE] for i in range(SLOT_COUNT)]

    if args.cmd == "info":
        for i, s in enumerate(slots):
            print(f"slot {i + 1:2}: {describe(s)}")
        return

    src = bytearray(slots[args.from_slot - 1])
    clean(src, args.orbs)
    start = (args.to_slot - 1) * SLOT_SIZE
    data[start:start + SLOT_SIZE] = src
    open(args.out, "wb").write(data)
    print(f"slot {args.to_slot}: {describe(src)}")


if __name__ == "__main__":
    main()
