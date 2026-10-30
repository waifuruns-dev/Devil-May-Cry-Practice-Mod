# DMC1 HD Collection save format (dmc1.sav) — working notes

Reverse-engineered from the user's saves. "Confirmed" = matches every sample so far.

## File layout
- 10 slots × 0x970 (2416) bytes, starting at offset 0.
- 0x104-byte trailer after slot 10 (mostly zeros; not yet decoded).
- Slots can be swapped on disk while the game is at the main menu; the load screen re-reads the file.

## Checksum (confirmed)
- Slot offset `0x08`, u16 = sum of every byte from slot offset `0x0A` to the end of the slot, & 0xFFFF.
- Recompute after any edit.

## Memory mapping (likely)
- The in-game save block appears to be a copy of a slot:
  file offset X  ==  memory `[dmc1.exe+488B60] + X + 0x20`
  (autosplitter: total saves at +0x40, mission at +0x44 → file 0x20 / 0x24)

## Header fields
| Offset | Type | Meaning | Status |
|---|---|---|---|
| 0x20 | u16 | Total saves | very likely |
| 0x24 | u8  | Mission (0 = new game / before M1; 1–23 otherwise) | confirmed |
| 0x25 | u8  | 1 on the saves that start M8 and M14 (0x24 = 7 / 13): mission shown = 0x24 + 0x25. Autosplitter's "269" = 13 + 256. | confirmed from the 22 any% saves |
| 0x26 | u8  | Difficulty: 2 = Easy, 3 = Normal, 5 = Hard, 6 = Dante Must Die (4 not seen) | confirmed |
| 0x27 | u8  | Costume: 0 Dante, 1 Legendary Dark Knight, 2 Super Dante | confirmed |
| 0x2C | u32 | Total game time, frames (60/s) | very likely |
| 0x240–0x24F | f32s | Player position (non-zero only on mid-mission saves) | likely |

## Inventory (confirmed from Devil Star purchase diff)
- 0x23B: u8 count of non-key inventory entries (13 → 14 when a Devil Star was added).
- 0x250: list of 4-byte entries `[category, id, count, 0]`, sorted by category then id.
  Only the first 0x23B entries are real. Entries after that are stale leftovers from earlier,
  longer lists (the game doesn't clear them) — the "category 9, count 0" entries seen earlier are these.

| Category | Meaning | IDs seen |
|---|---|---|
| 0 | Weapons, ids 0–9 (table below) | 0–9 |
| 1 | Key item (1:1 held from M16 onward) | 0, 1 |
| 4 | Key items carried into a single mission (4:6 → M4, 4:3 → M5, 4:5 → M7) | 3, 5, 6 |
| 5 | Key item (5:7 → M16) | 7 |
| 2 | Consumables | 0x0A Vital Star, 0x0B Devil Star, 0x0C Untouchable, 0x0D Holy Water (confirmed: route M10 preset shows 5 Holy Waters), 0x10 likely Yellow Orb, 0x11 ? |
| 6 | Key items (6:2 held M19–M23, 6:6 held M19–M22) | 2, 6 |
| 9 | Not real: only ever seen as stale entries past the end of the list | |

## Weapon IDs (category 0) — confirmed by adding one weapon at a time to a fresh M1 save
| ID | Weapon | | ID | Weapon |
|---|---|---|---|---|
| 0 | Force Edge | | 5 | Handguns (Ebony & Ivory) |
| 1 | Shotgun | | 6 | Alastor |
| 2 | Needlegun | | 7 | Ifrit |
| 3 | Grenadegun | | 8 | Sparda |
| 4 | Nightmare-β | | 9 | Yamato |

- Weapons added early (in mission 1) appear in the menu and can be used.
- Yamato comes with the Legendary Dark Knight costume (present on every LDK save, including a fresh one).
## Starting equipment (confirmed by writing values and loading)
- Only updated by the end-of-mission save; statue saves keep the old values. Applies to the next mission's start.
- 0x235 = devil arm behaviour, 0x237 = devil arm model, 0x236 = gun.

| Devil arm | 0x235 | 0x237 |
|---|---|---|
| Alastor | 0 | 0 |
| Ifrit | 1 | 0 |
| Force Edge | 4 | 0 |
| Yamato | 0 | 2 |
| Sparda | 4 | 1 (Force Edge behaviour + Sparda model, i.e. the transformed Force Edge) |

| Gun | 0x236 |
|---|---|
| Handguns | 1 |
| Shotgun | 2 |
| Grenadegun | 3 |
| Nightmare-β | 4 |
| Needlegun | 5 (never equipped by the game; don't write it) |

- 0x237 is the model: 0 = normal for that 0x235 value, 1 = Sparda, 2 = Yamato.
- 0x235 = 2 or 3 falls back to Alastor; 5 gives an unusable sword. Other mixed pairs (e.g. 4/2) give one weapon with another's model.
- Every real save seen (110) uses only: Alastor/Force Edge/Yamato with Handguns/Shotgun/Grenadegun.

## Moves / abilities (confirmed from Round Trip purchase diff)
- 0x62A–0x62B: bitfield of purchased moves (read as u16 at 0x62A).
- Round Trip = 0x62B bit 5 (0x20). 0x5F -> 0x7F when bought.
- Fully-unlocked saves read FE 7F (bits 1–14 of the u16 set): ~14 purchasable moves.
- Normal any% route save: 00 5F.
- Bit names (u16 at 0x62A, bit 0 = lowest), with shop prices seen on Normal. All 14 named.

| Bit | Mask | Move | Weapon | Price |
|---|---|---|---|---|
| 1 | 0x0002 | Inferno | Ifrit | 4850 |
| 2 | 0x0004 | Meteor Lv2 (needs Lv1) | Ifrit | 2700 |
| 3 | 0x0008 | Meteor Lv1 | Ifrit | 900 |
| 4 | 0x0010 | Kick 13 Lv2 (needs Lv1) | Ifrit | 2100 |
| 5 | 0x0020 | Kick 13 Lv1 | Ifrit | 700 |
| 6 | 0x0040 | Magma Drive | Ifrit | 500 |
| 7 | 0x0080 | Rolling Blaze | Ifrit | 3450 |
| 8 | 0x0100 | Air Raid | Alastor | 750 |
| 9 | 0x0200 | Vortex Lv2 (needs Lv1) | Alastor | 2400 |
| 10 | 0x0400 | Vortex Lv1 | Alastor | 1150 |
| 11 | 0x0800 | Stinger Lv2 (needs Lv1) | Alastor | 1400 |
| 12 | 0x1000 | Stinger Lv1 | Alastor | 350 |
| 13 | 0x2000 | Round Trip | Alastor | 1550 |
| 14 | 0x4000 | Air Hike | Alastor | 4000 |

- Bits 0 and 15 are never set, even on fully-upgraded saves.
- Normal any% route (0x5F00) = Stinger 1+2, Vortex 1+2, Air Raid, Air Hike.
- Nearby, not yet decoded: 0x620 (u16: 2/3), 0x62C and 0x630 (u32, identical pair:
  0x13EC / 0x10CC — NOT max HP; unchanged by a Blue Orb purchase).

## Vitality / Devil Trigger (confirmed from Blue Orb purchase diff)
| Offset | Type | Meaning |
|---|---|---|
| 0x624 | u8 | Max HP bar length (10 -> 11 on Blue Orb). Matches the in-game "HP Bar Length"; 25 on a maxed save. HP = 100 per unit. |
| 0x625 | u8 | Max DT bar length. Confirmed: game computes Max Devil Trigger = bars × 200 on a fresh load (3 bars → 600). After a hot-swap load without restarting, the old capacity sticks, so the mod must write Max Devil Trigger (mem 00104BA4) itself. |
| 0x230 | u16 | Current DT gauge, carried over from the previous mission. **120 per rune** (full 3 runes = 360, the minimum to trigger DT; M23 route 958 of 960 with 8 runes). Must stay ≤ bars × 120. Builder default: full. |
| 0x626 | u8 | Blue Orbs bought (0 -> 1); drives the Blue Orb price. |
| 0x627 | u8 | Purple Orbs bought (any% M23: base 3 bars + 5 bought = 8). |

## Other
- 0x634: u32 Red Orbs (confirmed).
- 0x844: u8, 0 -> 1 after both test purchases — likely "has used the shop" flag.
- 0x4D0–0x4E8: last mission's result stats (0x4D0 = time in frames).
- 0x504: u32, 0 → 1 on Devil Star purchase, but also 1 on fresh saves — meaning unclear.
- 0x7EA: 23 bytes, one per mission; FF = not cleared, small values = rank? (hypothesis)
- 0x950–0x96C: three repeated (u32, u32) pairs — likely checkpoint/area info.

## Fresh new-game values (from new Easy / Hard files, mission 1)
- Moves 0x62A–0x62B: 00 00 (nothing is unlocked by default).
- Max HP bar 0x624 / max DT bar 0x625: Easy 15 / 6, Hard 10 / 3. Base stats depend on difficulty
  (Normal presumably 10 / 3, like Hard). Blue/Purple bought 0x626 / 0x627: 0 / 0.
- Inventory: weapons 0 and 5 (likely Force Edge + handgun), Vital Star ×3 (Easy) / ×1 (Hard),
  item 0x10 ×1 (likely Yellow Orb).
- 0x504 is 1 here too, so it is NOT a purchase counter as first guessed.

## Noise seen in every mid-mission test save (not related to purchases)
- 0x143 / 0x14B / 0x14F, 0x240–0x24F (player position), 0x808 / 0x80C (cleared),
  0x848 (u16, different on every save). Safe to leave as the template has them.

## Still unknown
- (resolved) Current HP is NOT stored: no save holds bars × 100 anywhere, and a load gives full HP
  (Max HP = bars × 100, e.g. 10 bars → 1000/1000).
- Carry-over mission time (candidates around 0x2A / 0x32).

## Edited-save test results
- A slot built outside the game (moves cleared, consumables removed, DT/Purple reset, orbs = 123456,
  checksum recomputed) loads and plays normally. Moves and items show as refunded.
- Costume byte unaffected; DT capacity mismatch was a hot-swap leftover (fixed by a restart).

## Story state across the 22 any% Normal saves (M2–M23)
- Key items carried into each mission: see the category table above (only 7 distinct items; most key
  items are picked up and used within the same mission, so they never appear in a mission-start save).
- Other bytes that change from mission to mission (likely story/door/area flags), 175 bytes in:
  140-16A, 190-19B, 1B3, 1B9-1C5, 200-20B, 224, 22B, 450-451, 474-4BD, 641-64E, 653-657, 65F,
  6BC-708, 7BD-7D1, 850-857, 950-954, 95C-960, 968-96C.
- Weapons by mission start (any% route): M2 Force Edge + Handguns; Shotgun + Alastor by M3;
  Grenadegun by M9; Ifrit by M10; Needlegun by M13; Sparda by M19; M23 save has no Handguns/Sparda.

## Devil Trigger activation (from disassembly at dmc1.exe+2ECC79)
- Two scales (names from DDMK's PlayerActorData; actor = [dmc1.exe+60ACD0] = 0x102FD0):
  gauge 0x1BCE (00104B9E) / max 0x1BD0 (00104BA0): 120 per rune.
  DT time 0x1BD2 (00104BA2) / max 0x1BD4 (00104BA4): 200 per rune.
- On activation: DT time = gauge × 200 / 120. Full 3-rune gauge 360 → 600.
- Activation needs gauge ≥ 360 (0x168) = 3 runes.
- Early built saves wrote the gauge as bars × 200 (600 for 3 runes), which overfilled it: that was the long DT.

## Practice loader (in game)
- Confirmed working: F2 menu, Steam save auto-detect, slot 10 write, key blocking, preset loading,
  Max HP/DT fix, item names.

## Load-block bits (slot 0x140 = session+0x1F80)
- The room-load starter (dmc1.exe+24F4D0) only starts a load when
  `(event & 0x10A00000) == 0x10000000`, `(session+0x1F80 & 0xA00) == 0` and stage byte +6 == 0.
- Route saves for M5, M12, M18, M19 and M21 have bit 0x200 set at slot 0x140 (missions with a
  scripted start). A mid-mission mission-start request is ignored while it's set; the mod clears it
  for the request and restores it once the mission has started.
- Stage manager (dmc1.exe+60B148) byte +6 is the room-transition phase (0 = idle, 1-5 = steps,
  jump table at dmc1.exe+2529F0, task function dmc1.exe+2524C0).

## Retry count / carried time (slot 0x844 / 0x848 = session+0x2684 / +0x2688)
- MissionStart (dmc1.exe+2564A0): if [session+0x2684] != 0, the extra-time field dmc1.exe+4CB6850 is
  set from [session+0x2688], otherwise 0; then [session+0x2684] += 1. The final mission time includes
  that carried time. The mod zeroes both for every restart / preset load.

## Devil arm flags (0x202)
- 0x202 bit 0x02 = has Alastor, bit 0x01 = has Ifrit (session+0x2042; the game sets them with
  `or 0x20000` / `or 0x10000` on session+0x2040 when it gives you the weapon). Story saves: 2 from
  M3, 3 from M10. The R3 quick switch needs them; owning the weapon in the item list isn't enough.
  The builder sets them from the preset's weapons.
- 0x15C bit 0x04 = Alastor, bit 0x02 = Ifrit (session+0x1F9C), set by the same give-weapon code
  (dmc1.exe+255E66 / +256152 on the 2018 build). The R3 switch stays locked without the Ifrit bit
  (user test: only unlocked after picking Ifrit up). Story saves: 0x04 from M3, 0x06 from M10.

## Nightmare cores (0x23A)
- Low 4 bits, a 2-bit count per core: top core bits 0-1, bottom core bits 2-3.
  Top: 0 blue (not broken), 1 green (broken once), 2 red (broken twice).
  Bottom: 0 blue, 4 green, 8 red.
- All confirmed in game with M18 presets: 0 both blue, 1 top green, 2 top red, 4 bottom green,
  8 bottom red, 0xF both red (red wins when both bits of a core are set).
- Story route: 0 up to M16, 1 for M17-M20, 5 from M21. Carried from the M16 fight into M18 and
  M20 (M16 caps each core at one break). The builder writes it from "top_core" / "bottom_core"
  for missions 17+.
