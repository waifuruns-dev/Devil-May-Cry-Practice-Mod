#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// A practice setup: which mission, plus the loadout to start it with.
// Fields marked "-1 / empty = default" fall back to the mission's normal values.
struct Preset {
    std::string category;        // folder name
    std::string name;
    std::string file;            // path it was loaded from (for messages)
    uint64_t modified = 0;       // file's last-write time (for sorting in the menu)
    int mission = 0;             // 2-23
    int difficulty = 3;          // 2 Easy, 3 Normal, 5 Hard, 6 DMD
    int costume = 0;             // 0 Dante, 1 Legendary Dark Knight, 2 Super Dante
    uint32_t orbs = 0;
    int hpBars = -1;             // -1 = base for the difficulty
    int dtBars = -1;
    std::string dtGauge = "full";  // full, empty, route, or a number
    std::vector<std::string> moves;
    std::map<int, int> items;    // consumable id -> count
    std::vector<int> extraWeapons;
    std::string devilArm;        // empty = the mission's normal starting weapon
    std::string gun;
    // Nightmare cores, carried from M16 to M18 and M20: -1 = the mission's story state,
    // 0 blue (not broken), 1 green (broken once), 2 red (broken twice). Used from mission 17 on.
    int topCore = -1;
    int bottomCore = -1;
    std::map<int, int> saveSet;  // research: save offset -> byte written into the built slot ("save_set")
    std::map<int, int> saveOr;   // research: save offset -> bits OR'ed into the built slot ("save_or")
};

namespace builder {

// Nightmare core states in the save (0x23A): top 0x1 (green) / 0x3 (red), bottom 0x4 / 0xC.
constexpr int kFirstCoreMission = 17;
int StoryCore(int mission, bool top);   // the story save's state for that mission (0-2)
int CoresByte(int top, int bottom);
void CoresFromByte(uint8_t v, int& top, int& bottom);

constexpr size_t kSlotSize = 0x970;

bool HasMission(int mission);

// Builds a complete save slot for the preset. Returns false and fills `error` on bad input.
bool Build(const Preset& p, std::vector<uint8_t>& slot, std::string& error);

// Weapons (ids 0-9) the story gives you by the start of this mission; always in a built save.
std::vector<int> StoryWeapons(int mission);

// Name lookups shared with the preset parser.
bool MoveBit(const std::string& name, int& bit);
bool ItemId(const std::string& name, int& id);
bool WeaponId(const std::string& name, int& id);

}  // namespace builder
