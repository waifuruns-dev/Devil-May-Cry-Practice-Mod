"""DMC1 save builder: any mission + any loadout, built from a fresh new-game template.

How it works
  1. make-table: for each mission baseline (a real save at the start of that mission), record every
     byte that differs from a fresh new-game save, EXCEPT the loadout fields this builder sets
     itself. That is the mission's "story state" (flags, area data, mission number, collectibles...).
     Key items and story weapons are recorded separately from the baseline's item list.
  2. build: fresh template + story state for mission N + key items/story weapons + chosen loadout,
     then the checksum.

Usage
  python dmc1_builder.py make-table FRESH.sav FRESH_SLOT BASELINE_DIR story_table.json
  python dmc1_builder.py build story_table.json FRESH.sav FRESH_SLOT OUT.sav \\
         --slot 1 --mission 5 --difficulty dmd [--costume dante] [--orbs 0] [--dt-gauge full]
"""
import argparse
import json
import os
import re
import struct

import dmc1_save as m

S = m.SLOT_SIZE

# Bytes the builder sets itself (or that are per-save bookkeeping). Never copied from a baseline.
LOADOUT = set()
for a, b in [
    (0x08, 0x0A),     # checksum
    (0x20, 0x22),     # total saves
    (0x26, 0x28),     # difficulty, costume
    (0x2C, 0x30),     # total game time
    (0x230, 0x232),   # current DT gauge
    (0x235, 0x238),   # starting equipment
    (0x23B, 0x23C),   # item count
    (0x250, 0x2A0),   # item list
    (0x624, 0x628),   # HP bars, DT bars, Blue/Purple bought
    (0x62A, 0x62C),   # moves
    (0x634, 0x638),   # red orbs
]:
    LOADOUT.update(range(a, b))

KEY_CATEGORIES = {1, 4, 5, 6}
STORY_WEAPON_CATEGORY = 0
# Consumables a clean preset removes; other category-2 items (0x0F, 0x10, 0x11) are kept from the
# baseline until we know what they are.
REMOVE_CONSUMABLES = {0x0A, 0x0B, 0x0C, 0x0D}

DIFFICULTY_BY_NAME = {"easy": 2, "normal": 3, "hard": 5, "dmd": 6}
COSTUME_BY_NAME = {"dante": 0, "ldk": 1, "super": 2}


def slot_of(path, slot_no):
    data = open(path, "rb").read()
    return data[(slot_no - 1) * S:slot_no * S]


def make_table(fresh: bytes, baseline_dir: str) -> dict:
    table = {}
    for name in sorted(os.listdir(baseline_dir)):
        mm = re.match(r"M(\d+)\.sav$", name)
        if not mm:
            continue
        base = open(os.path.join(baseline_dir, name), "rb").read()[0:S]
        mission = int(mm.group(1))
        story = {o: base[o] for o in range(S) if o not in LOADOUT and base[o] != fresh[o]}
        items, _ = m.read_items(base)
        table[str(mission)] = {
            "story": {f"{o:x}": v for o, v in story.items()},
            "weapons": [e[1] for e in items if e[0] == STORY_WEAPON_CATEGORY],
            "key_items": [list(e) for e in items if e[0] in KEY_CATEGORIES],
            "other_items": [list(e) for e in items
                            if e[0] == 2 and e[1] not in REMOVE_CONSUMABLES],
            "hp_base": base[m.OFF_HP_BAR] - base[m.OFF_BLUE_BOUGHT],
            "dt_base": base[m.OFF_DT_BAR] - base[m.OFF_PURPLE_BOUGHT],
            "equip": [base[m.OFF_EQUIP_ARM], base[m.OFF_EQUIP_GUN], base[m.OFF_EQUIP_MODEL]],
            "dt_gauge": struct.unpack_from("<H", base, m.OFF_DT_CURRENT)[0],
        }
    return table


def resolve_dt_gauge(setting: str, entry: dict) -> int:
    """'full' (default) = bars x 120, 'empty' = 0, 'route' = what the baseline save had, or a number.
    The gauge counts 120 per rune (DT triggers at 360 = 3 runes). Activation time = gauge x 200 / 120."""
    cap = entry["dt_base"] * 120
    if setting == "full":
        value = cap
    elif setting == "empty":
        value = 0
    elif setting == "route":
        value = entry["dt_gauge"]
    else:
        value = int(setting)
    return max(0, min(value, cap))


def build(fresh: bytes, entry: dict, difficulty: int, costume: int, orbs: int,
          dt_gauge: str = "full") -> bytearray:
    s = bytearray(fresh)

    # Story state for this mission.
    for off, val in entry["story"].items():
        s[int(off, 16)] = val

    # Clean loadout on top.
    s[m.OFF_DIFFICULTY] = difficulty
    s[m.OFF_COSTUME] = costume
    struct.pack_into("<I", s, m.OFF_ORBS, orbs)
    struct.pack_into("<H", s, m.OFF_MOVES, 0)
    s[m.OFF_HP_BAR] = entry["hp_base"]
    s[m.OFF_DT_BAR] = entry["dt_base"]
    s[m.OFF_BLUE_BOUGHT] = 0
    s[m.OFF_PURPLE_BOUGHT] = 0
    struct.pack_into("<H", s, m.OFF_DT_CURRENT, resolve_dt_gauge(dt_gauge, entry))
    s[m.OFF_EQUIP_ARM], s[m.OFF_EQUIP_GUN], s[m.OFF_EQUIP_MODEL] = entry["equip"]

    weapons = set(entry["weapons"])
    if costume == 1:
        weapons.add(9)  # Legendary Dark Knight comes with Yamato
    items = [(0, w, 1) for w in weapons]
    items += [tuple(e) for e in entry["key_items"]]
    items += [tuple(e) for e in entry["other_items"]]
    m.write_items(s, sorted(items))

    m.fix_checksum(s)
    return s


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("make-table")
    t.add_argument("fresh"); t.add_argument("fresh_slot", type=int)
    t.add_argument("baseline_dir"); t.add_argument("out")
    b = sub.add_parser("build")
    b.add_argument("table"); b.add_argument("fresh"); b.add_argument("fresh_slot", type=int)
    b.add_argument("out")
    b.add_argument("--slot", type=int, required=True)
    b.add_argument("--mission", type=int, required=True)
    b.add_argument("--difficulty", default="normal", choices=DIFFICULTY_BY_NAME)
    b.add_argument("--costume", default="dante", choices=COSTUME_BY_NAME)
    b.add_argument("--orbs", type=int, default=0)
    b.add_argument("--dt-gauge", default="full",
                   help="full (default), empty, route, or a number; capped to bars x 120")
    a = ap.parse_args()

    fresh = slot_of(a.fresh, a.fresh_slot)
    if a.cmd == "make-table":
        table = make_table(fresh, a.baseline_dir)
        json.dump(table, open(a.out, "w"), indent=1)
        print(f"{len(table)} missions -> {a.out}")
        return

    table = json.load(open(a.table))
    built = build(fresh, table[str(a.mission)], DIFFICULTY_BY_NAME[a.difficulty],
                  COSTUME_BY_NAME[a.costume], a.orbs, a.dt_gauge)
    data = bytearray(open(a.out, "rb").read()) if os.path.exists(a.out) else bytearray(open(a.fresh, "rb").read())
    data[(a.slot - 1) * S:a.slot * S] = built
    open(a.out, "wb").write(data)
    print(f"slot {a.slot}: {m.describe(built)}")


if __name__ == "__main__":
    main()
