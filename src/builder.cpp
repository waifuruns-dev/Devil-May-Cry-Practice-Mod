// Save builder: fresh new-game template + the mission's story state + the preset's loadout.
// Same logic as tools/dmc1_builder.py, which was verified against real saves and in game.
// Offsets are documented in docs/SAVE_FORMAT.md.
#include "builder.h"

#include <algorithm>
#include <cstring>

namespace {

struct StoryByte { uint16_t off; uint8_t val; };
struct ItemEntry { uint8_t cat, id, count; };
struct MissionData {
    int mission;
    const StoryByte* story; int storyCount;
    const ItemEntry* items; int itemCount;   // story weapons, key items, kept misc items
    uint8_t equipArm, equipGun, equipModel;
    uint16_t routeGauge;
};

#include "builder_data.inc"

constexpr size_t OFF_CHECKSUM = 0x08;
constexpr size_t OFF_DIFFICULTY = 0x26;
constexpr size_t OFF_COSTUME = 0x27;
constexpr size_t OFF_DT_CURRENT = 0x230;
constexpr size_t OFF_CORES = 0x23A;
constexpr size_t OFF_ARM_FLAGS = 0x202;
constexpr size_t OFF_ARM_FLAGS2 = 0x15C;
constexpr size_t OFF_EQUIP_ARM = 0x235;
constexpr size_t OFF_EQUIP_GUN = 0x236;
constexpr size_t OFF_EQUIP_MODEL = 0x237;
constexpr size_t OFF_ITEM_COUNT = 0x23B;
constexpr size_t OFF_ITEMS = 0x250;
constexpr size_t ITEMS_END = 0x2A0;
constexpr size_t OFF_HP_BAR = 0x624;
constexpr size_t OFF_DT_BAR = 0x625;
constexpr size_t OFF_BLUE_BOUGHT = 0x626;
constexpr size_t OFF_PURPLE_BOUGHT = 0x627;
constexpr size_t OFF_MOVES = 0x62A;
constexpr size_t OFF_ORBS = 0x634;

const MissionData* FindMission(int mission) {
    for (const MissionData& m : kMissions)
        if (m.mission == mission) return &m;
    return nullptr;
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

struct Named { const char* name; int value; };

const Named kMoves[] = {
    {"inferno", 1}, {"meteor lv2", 2}, {"meteor lv1", 3}, {"kick 13 lv2", 4}, {"kick 13 lv1", 5},
    {"magma drive", 6}, {"rolling blaze", 7}, {"air raid", 8}, {"vortex lv2", 9}, {"vortex lv1", 10},
    {"stinger lv2", 11}, {"stinger lv1", 12}, {"round trip", 13}, {"air hike", 14},
};
// Lv2 moves need their Lv1.
const Named kMoveNeeds[] = {{"2", 3}, {"4", 5}, {"9", 10}, {"11", 12}};

const Named kItems[] = {
    {"vital star", 0x0A}, {"devil star", 0x0B}, {"untouchable", 0x0C}, {"holy water", 0x0D},
};

const Named kWeapons[] = {
    {"force edge", 0}, {"shotgun", 1}, {"needlegun", 2}, {"grenadegun", 3}, {"nightmare-beta", 4},
    {"handguns", 5}, {"alastor", 6}, {"ifrit", 7}, {"sparda", 8}, {"yamato", 9},
};

struct Arm { const char* name; uint8_t behaviour, model; };
const Arm kArms[] = {
    {"alastor", 0, 0}, {"ifrit", 1, 0}, {"force edge", 4, 0}, {"sparda", 4, 1}, {"yamato", 0, 2},
};
const Named kGuns[] = {{"handguns", 1}, {"shotgun", 2}, {"grenadegun", 3}, {"nightmare-beta", 4}};

template <size_t N>
bool Lookup(const Named (&table)[N], const std::string& name, int& out) {
    const std::string key = Lower(name);
    for (const Named& n : table)
        if (key == n.name) { out = n.value; return true; }
    // Also accept raw numbers like "0x0C" or "12".
    try {
        size_t used = 0;
        int v = std::stoi(name, &used, 0);
        if (used == name.size()) { out = v; return true; }
    } catch (...) {}
    return false;
}

void BaseStats(int difficulty, int& hp, int& dt) {
    // Easy confirmed 15/6, Hard 10/3; Normal and DMD matched Hard in every save seen.
    if (difficulty == 2) { hp = 15; dt = 6; }
    else { hp = 10; dt = 3; }
}

uint16_t Checksum(const std::vector<uint8_t>& s) {
    uint32_t sum = 0;
    for (size_t i = 0x0A; i < s.size(); ++i) sum += s[i];
    return (uint16_t)(sum & 0xFFFF);
}

void Put16(std::vector<uint8_t>& s, size_t off, uint16_t v) { memcpy(&s[off], &v, 2); }
void Put32(std::vector<uint8_t>& s, size_t off, uint32_t v) { memcpy(&s[off], &v, 4); }

}  // namespace

int builder::CoresByte(int top, int bottom) {
    static const int kTop[3] = {0x0, 0x1, 0x2}, kBottom[3] = {0x0, 0x4, 0x8};
    return kTop[std::max(0, std::min(top, 2))] | kBottom[std::max(0, std::min(bottom, 2))];
}
void builder::CoresFromByte(uint8_t v, int& top, int& bottom) {
    top = (v & 0x2) ? 2 : (v & 0x1) ? 1 : 0;
    bottom = (v & 0x8) ? 2 : (v & 0x4) ? 1 : 0;
}
int builder::StoryCore(int mission, bool top) {
    const MissionData* md = FindMission(mission);
    if (!md || mission < kFirstCoreMission) return 0;
    uint8_t v = kTemplateSlot[OFF_CORES];
    for (int i = 0; i < md->storyCount; ++i) if (md->story[i].off == OFF_CORES) v = md->story[i].val;
    int t, b;
    CoresFromByte(v, t, b);
    return top ? t : b;
}

bool builder::HasMission(int mission) { return FindMission(mission) != nullptr; }
std::vector<int> builder::StoryWeapons(int mission) {
    std::vector<int> out;
    if (const MissionData* md = FindMission(mission))
        for (int i = 0; i < md->itemCount; ++i)
            if (md->items[i].cat == 0 && md->items[i].id < 10) out.push_back(md->items[i].id);
    return out;
}
bool builder::MoveBit(const std::string& name, int& bit) { return Lookup(kMoves, name, bit); }
bool builder::ItemId(const std::string& name, int& id) { return Lookup(kItems, name, id); }
bool builder::WeaponId(const std::string& name, int& id) { return Lookup(kWeapons, name, id); }

bool builder::Build(const Preset& p, std::vector<uint8_t>& s, std::string& error) {
    const MissionData* md = FindMission(p.mission);
    if (!md) { error = "no story data for mission " + std::to_string(p.mission) + " (2-23 supported)"; return false; }
    if (p.difficulty != 2 && p.difficulty != 3 && p.difficulty != 5 && p.difficulty != 6) {
        error = "unknown difficulty"; return false;
    }
    if (p.costume < 0 || p.costume > 2) { error = "unknown costume"; return false; }

    s.assign(kTemplateSlot, kTemplateSlot + kSlotSize);

    // Story state for the mission.
    for (int i = 0; i < md->storyCount; ++i) s[md->story[i].off] = md->story[i].val;

    s[OFF_DIFFICULTY] = (uint8_t)p.difficulty;
    s[OFF_COSTUME] = (uint8_t)p.costume;
    Put32(s, OFF_ORBS, p.orbs);

    // HP / DT bars and the shop counters that go with them.
    int baseHp, baseDt;
    BaseStats(p.difficulty, baseHp, baseDt);
    const int hp = p.hpBars >= 0 ? p.hpBars : baseHp;
    const int dt = p.dtBars >= 0 ? p.dtBars : baseDt;
    if (hp < 1 || hp > 30 || dt < 0 || dt > 10) { error = "hp_bars / dt_bars out of range"; return false; }
    s[OFF_HP_BAR] = (uint8_t)hp;
    s[OFF_DT_BAR] = (uint8_t)dt;
    s[OFF_BLUE_BOUGHT] = (uint8_t)std::max(0, hp - baseHp);
    s[OFF_PURPLE_BOUGHT] = (uint8_t)std::max(0, dt - baseDt);

    // DT gauge: what you start the mission with. The gauge counts 120 per rune
    // (a full 3-rune gauge is 360, the minimum needed to trigger DT). Capped to bars x 120.
    const int cap = dt * 120;
    int gauge = cap;
    const std::string g = Lower(p.dtGauge);
    if (g == "empty") gauge = 0;
    else if (g == "route") gauge = md->routeGauge;
    else if (g != "full") {
        try { gauge = std::stoi(g); } catch (...) { error = "dt_gauge must be full, empty, route or a number"; return false; }
    }
    Put16(s, OFF_DT_CURRENT, (uint16_t)std::max(0, std::min(gauge, cap)));

    // Moves.
    uint16_t moves = 0;
    for (const std::string& name : p.moves) {
        int bit;
        if (!MoveBit(name, bit) || bit < 1 || bit > 14) { error = "unknown move '" + name + "'"; return false; }
        moves |= (uint16_t)(1u << bit);
    }
    for (const Named& need : kMoveNeeds) {
        int lv2 = std::stoi(need.name);
        if (moves & (1u << lv2)) moves |= (uint16_t)(1u << need.value);
    }
    Put16(s, OFF_MOVES, moves);

    // Starting equipment.
    s[OFF_EQUIP_ARM] = md->equipArm;
    s[OFF_EQUIP_GUN] = md->equipGun;
    s[OFF_EQUIP_MODEL] = md->equipModel;
    if (!p.devilArm.empty()) {
        const std::string key = Lower(p.devilArm);
        bool found = false;
        for (const Arm& a : kArms)
            if (key == a.name) { s[OFF_EQUIP_ARM] = a.behaviour; s[OFF_EQUIP_MODEL] = a.model; found = true; }
        if (!found) { error = "unknown devil arm '" + p.devilArm + "'"; return false; }
    }
    if (!p.gun.empty()) {
        int gun;
        if (!Lookup(kGuns, p.gun, gun)) { error = "unknown gun '" + p.gun + "' (Needlegun can't be equipped)"; return false; }
        s[OFF_EQUIP_GUN] = (uint8_t)gun;
    }

    // Item list: story weapons, key items and misc items from the mission, then the preset's extras.
    std::map<std::pair<int, int>, int> items;
    for (int i = 0; i < md->itemCount; ++i)
        items[{md->items[i].cat, md->items[i].id}] = md->items[i].count;
    if (p.costume == 1) items[{0, 9}] = 1;  // Legendary Dark Knight comes with Yamato
    for (int w : p.extraWeapons) {
        if (w < 0 || w > 9) { error = "weapon id out of range"; return false; }
        items[{0, w}] = 1;
    }
    for (const auto& kv : p.items) {
        if (kv.second <= 0) items.erase({2, kv.first});
        else items[{2, kv.first}] = std::min(kv.second, 255);
    }
    // ---- consistency checks: anything equipped or learned must go with a weapon you own ----
    auto owns = [&](int weaponId) { return items.count({0, weaponId}) != 0; };
    static const char* kWeaponName[10] = {"Force Edge", "Shotgun", "Needlegun", "Grenadegun", "Nightmare-Beta",
                                          "Handguns",   "Alastor", "Ifrit",     "Sparda",     "Yamato"};
    // Devil arm: model 1 = Sparda, 2 = Yamato; otherwise behaviour 0 Alastor, 1 Ifrit, 4 Force Edge.
    int armId = -1;
    if (s[OFF_EQUIP_MODEL] == 1) armId = 8;
    else if (s[OFF_EQUIP_MODEL] == 2) armId = 9;
    else if (s[OFF_EQUIP_ARM] == 0) armId = 6;
    else if (s[OFF_EQUIP_ARM] == 1) armId = 7;
    else if (s[OFF_EQUIP_ARM] == 4) armId = 0;
    if (armId < 0) { error = "invalid devil arm"; return false; }
    if (!owns(armId)) {
        error = std::string("starts with ") + kWeaponName[armId] + " equipped but doesn't own it";
        return false;
    }
    static const int kGunToId[5] = {-1, 5, 1, 3, 4};  // equip value -> weapon id
    const int gunVal = s[OFF_EQUIP_GUN];
    if (gunVal < 1 || gunVal > 4) { error = "invalid gun"; return false; }
    if (!owns(kGunToId[gunVal])) {
        error = std::string("starts with ") + kWeaponName[kGunToId[gunVal]] + " equipped but doesn't own it";
        return false;
    }
    // Moves: bits 1-7 are Ifrit's, 8-14 Alastor's.
    if ((moves & 0x00FE) && !owns(7)) { error = "has Ifrit moves but doesn't own Ifrit"; return false; }
    if ((moves & 0x7F00) && !owns(6)) { error = "has Alastor moves but doesn't own Alastor"; return false; }

    const size_t maxEntries = (ITEMS_END - OFF_ITEMS) / 4;
    if (items.size() > maxEntries) { error = "too many items for the save's item list"; return false; }
    std::fill(s.begin() + OFF_ITEMS, s.begin() + ITEMS_END, 0);
    size_t off = OFF_ITEMS;
    for (const auto& kv : items) {  // std::map keeps them sorted by (category, id), like the game
        s[off] = (uint8_t)kv.first.first;
        s[off + 1] = (uint8_t)kv.first.second;
        s[off + 2] = (uint8_t)kv.second;
        s[off + 3] = 0;
        off += 4;
    }
    s[OFF_ITEM_COUNT] = (uint8_t)items.size();

    // "Has the devil arm" flags (save 0x202, session+0x2042): 0x02 Alastor, 0x01 Ifrit. Set by the
    // game when it hands you the weapon (M3 / M10 in the story saves); the R3 quick switch between
    // Alastor and Ifrit checks them, not the item list. Keep them in line with what the preset owns.
    s[OFF_ARM_FLAGS] = (uint8_t)((s[OFF_ARM_FLAGS] & ~0x03) | (owns(6) ? 0x02 : 0) | (owns(7) ? 0x01 : 0));
    // Second set, written by the same "give weapon" code (session+0x1F9C): 0x04 Alastor, 0x02 Ifrit.
    // Without the Ifrit bit the R3 Alastor / Ifrit quick switch stays locked.
    s[OFF_ARM_FLAGS2] = (uint8_t)((s[OFF_ARM_FLAGS2] & ~0x06) | (owns(6) ? 0x04 : 0) | (owns(7) ? 0x02 : 0));

    // Nightmare cores.
    if (p.mission >= kFirstCoreMission && (p.topCore >= 0 || p.bottomCore >= 0)) {
        int top, bottom;
        CoresFromByte(s[OFF_CORES], top, bottom);
        if (p.topCore >= 0) top = std::min(p.topCore, 2);
        if (p.bottomCore >= 0) bottom = std::min(p.bottomCore, 2);
        s[OFF_CORES] = (uint8_t)((s[OFF_CORES] & 0xF0) | CoresByte(top, bottom));
    }

    for (const auto& kv : p.saveSet) s[kv.first] = (uint8_t)kv.second;
    for (const auto& kv : p.saveOr) s[kv.first] |= (uint8_t)kv.second;

    Put16(s, OFF_CHECKSUM, Checksum(s));
    return true;
}
