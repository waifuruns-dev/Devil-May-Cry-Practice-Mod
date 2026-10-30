#include "menu.h"
#include "config.h"
#include "input_block.h"
#include "log.h"
#include "presets.h"
#include "reset.h"
#include "savefile.h"
#include "mode.h"
#include "steam_redirect.h"
#include "dump.h"

#include <windows.h>
#include <imgui.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstring>

namespace {

enum class Page { Closed, Categories, Presets, Editor };

Page g_page = Page::Closed;
std::vector<PresetCategory> g_cats;
int g_catSel = 0;
int g_presetSel = 0;
bool g_scrollToSel = false;
std::wstring g_savePath;
std::string g_status;
bool g_statusOk = true;
bool g_waitRelease = false;  // keep blocking input until menu keys are let go

// ---- keys ------------------------------------------------------------------

struct Key {
    int vk;
    bool down = false;
    ULONGLONG nextRepeat = 0;
};
Key g_kMenu{0}, g_kUp{VK_UP}, g_kDown{VK_DOWN}, g_kEnter{VK_RETURN}, g_kBack{VK_BACK},
    g_kLeft{VK_LEFT}, g_kRight{VK_RIGHT}, g_kEsc{VK_ESCAPE}, g_kEdit{'E'}, g_kDel{VK_DELETE}, g_kSort{'S'}, g_kExport{'X'};

// True on the first press, and again on auto-repeat while held (for Up/Down).
bool Pressed(Key& k, bool focused, bool repeat = false) {
    const bool now = focused && (GetAsyncKeyState(k.vk) & 0x8000) != 0;
    const ULONGLONG t = GetTickCount64();
    bool fire = false;
    if (now && !k.down) { fire = true; k.nextRepeat = t + 350; }
    else if (now && repeat && t >= k.nextRepeat) { fire = true; k.nextRepeat = t + 60; }
    k.down = now;
    return fire;
}

bool AnyMenuKeyHeld() {
    for (int vk : {g_kMenu.vk, VK_UP, VK_DOWN, VK_RETURN, VK_BACK, VK_LEFT, VK_RIGHT, VK_ESCAPE, (int)'E', VK_DELETE, (int)'X'})
        if (GetAsyncKeyState(vk) & 0x8000) return true;
    return false;
}

// ---- actions ---------------------------------------------------------------

// Preset list order, S in the list cycles it. Kept while the game runs.
enum class SortMode { Mission, Name, Newest };
SortMode g_sort = SortMode::Mission;
const char* SortName(SortMode m) {
    return m == SortMode::Mission ? "mission" : m == SortMode::Name ? "name" : "newest first";
}
std::string LowerStr(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
void SortPresets() {
    for (PresetCategory& cat : g_cats) {
        std::stable_sort(cat.presets.begin(), cat.presets.end(), [](const Preset& a, const Preset& b) {
            switch (g_sort) {
                case SortMode::Name: {
                    const std::string la = LowerStr(a.name), lb = LowerStr(b.name);
                    if (la != lb) return la < lb;
                    return a.mission < b.mission;
                }
                case SortMode::Newest:
                    if (a.modified != b.modified) return a.modified > b.modified;
                    return a.mission < b.mission;
                default:
                    if (a.mission != b.mission) return a.mission < b.mission;
                    return LowerStr(a.name) < LowerStr(b.name);
            }
        });
    }
}
void ReloadPresets() {
    g_cats = presets::LoadAll(g_config.dataFolder + g_config.presetFolder);
    SortPresets();
}

// What the overlay shows as the category, and what personal bests are filed under: the preset's
// own name (falls back to its folder for a preset without one).
std::string PresetLabel(const Preset& p) { return p.name.empty() ? p.category : p.name; }

void Open() {
    ReloadPresets();  // re-read so edits show up
    if (g_catSel > (int)g_cats.size()) g_catSel = 0;
    g_savePath = mode::SavePath();  // the practice file: your real save is never written
    g_page = Page::Categories;
    g_scrollToSel = true;
}

void Close() {
    g_page = Page::Closed;
    g_waitRelease = true;
}

const char* CostumeName(int c);

void LoadPreset(const Preset& p) {
    std::vector<uint8_t> slot;
    std::string error;
    if (!builder::Build(p, slot, error)) {
        g_status = "Couldn't build: " + error;
        g_statusOk = false;
        return;
    }
    g_savePath = mode::SavePath();  // the practice file: your real save is never written
    char buf[256];

    // Costume: gameplay reads the costume byte live (LDK / Super Dante checks all over the game code),
    // and the load writes it, but the player model only changes on a title-screen load.
    const int loaded = reset::LoadedCostume();
    const bool costumeChange = loaded >= 0 && loaded != p.costume;

    // A different costume can't be loaded mid-mission: the model (and its Devil Trigger data) stays
    // the old costume's, and DT moves like Air Raid / Vortex then crash the game (Dante -> LDK).
    // Write it to the practice slot instead, for a title-screen load.
    if (reset::InGame() && costumeChange) {
        if (!savefile::WriteSlot(g_savePath, g_config.practiceSlot, slot, error)) {
            g_status = "Couldn't write the save: " + error;
            g_statusOk = false;
            return;
        }
        savefile::WriteSlotToMemory(g_config.practiceSlot, slot);
        g_config.category = PresetLabel(p);
        snprintf(buf, sizeof(buf), "%s is %s, you're playing as %s: costumes only change from the title. "
                 "Written to slot %d - quit to the title, then Load Game -> slot %d.",
                 PresetLabel(p).c_str(), CostumeName(p.costume), CostumeName(loaded), g_config.practiceSlot,
                 g_config.practiceSlot);
        g_status = buf;
        g_statusOk = false;
        dump::ShowMessage(buf, 7000);
        dlog::Write("[Menu] %s needs costume %d (playing %d): written to slot %d for a title load",
                    p.file.c_str(), p.costume, loaded, g_config.practiceSlot);
        Close();
        return;
    }

    // In a mission: load it straight into the game, no title screen needed.
    if (reset::InGame()) {
        if (!reset::LoadSlotNow(slot, error)) {
            g_status = "Couldn't load into the mission: " + error;
            g_statusOk = false;
            return;
        }
        std::string fileError;
        savefile::WriteSlot(g_savePath, g_config.practiceSlot, slot, fileError);  // keep slot 10 in sync
        savefile::WriteSlotToMemory(g_config.practiceSlot, slot);
        g_config.category = PresetLabel(p);
        snprintf(buf, sizeof(buf), "Loaded %s (M%02d). F3 restarts it.", PresetLabel(p).c_str(), p.mission);
        g_status = buf;
        g_statusOk = true;
        dlog::Write("[Menu] loaded %s mission %d into the running game", p.category.c_str(), p.mission);
        Close();
        return;
    }

    if (!savefile::WriteSlot(g_savePath, g_config.practiceSlot, slot, error)) {
        g_status = "Couldn't write the save: " + error;
        g_statusOk = false;
        return;
    }
    savefile::WriteSlotToMemory(g_config.practiceSlot, slot);
    g_config.category = PresetLabel(p);  // personal bests now file under this category
    snprintf(buf, sizeof(buf), "Ready: %s (M%02d) in slot %d. Load Game -> slot %d.",
             PresetLabel(p).c_str(), p.mission, g_config.practiceSlot, g_config.practiceSlot);
    g_status = buf;
    g_statusOk = true;
    dlog::Write("[Menu] wrote %s mission %d (%s) to slot %d", p.category.c_str(), p.mission, p.file.c_str(),
                g_config.practiceSlot);
}

// Export: the preset goes into slot 10 of your REAL save (through Steam, so the cloud copy gets it
// too), to play it without the mod. Also kept as a standalone copy in dmc1_exports\.
bool g_exportArmed = false;
ULONGLONG g_exportArmedAt = 0;

void ExportSelected() {
    const int cat = g_catSel - 1;
    if (cat < 0 || cat >= (int)g_cats.size()) return;
    const auto& list = g_cats[cat].presets;
    if (g_presetSel < 0 || g_presetSel >= (int)list.size()) return;
    const Preset& p = list[g_presetSel];
    const int slotNo = g_config.practiceSlot;
    char buf[512];
    if (!g_exportArmed || GetTickCount64() - g_exportArmedAt > 4000) {
        g_exportArmed = true;
        g_exportArmedAt = GetTickCount64();
        snprintf(buf, sizeof(buf), "Put %s into slot %d of your REAL save (it replaces what's there)? Press X again.",
                 PresetLabel(p).c_str(), slotNo);
        g_status = buf;
        g_statusOk = false;
        return;
    }
    g_exportArmed = false;
    std::vector<uint8_t> slot, file;
    std::string error;
    if (!builder::Build(p, slot, error)) { g_status = "Couldn't build: " + error; g_statusOk = false; return; }
    if (!steam_redirect::ReadRealSave(file) || file.size() < (size_t)slotNo * slot.size()) {
        g_status = "Couldn't read your real save through Steam (see the log).";
        g_statusOk = false;
        return;
    }
    std::copy(slot.begin(), slot.end(), file.begin() + (slotNo - 1) * slot.size());
    if (!steam_redirect::WriteRealSave(file)) {
        g_status = "Couldn't write your real save (see the log).";
        g_statusOk = false;
        return;
    }
    mode::RealSlotChanged(slotNo, slot);
    // A standalone copy too: dmc1_exports\<preset>\dmc1.sav (your save with this slot), to share.
    std::string name = PresetLabel(p);
    for (char& c : name) if (strchr("\\/:*?\"<>|", c)) c = '_';
    std::wstring dir = g_config.dataFolder + L"dmc1_exports";
    CreateDirectoryW(dir.c_str(), nullptr);
    const int n = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
    std::wstring wname(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, &wname[0], n);
    dir += L"\\" + wname;
    CreateDirectoryW(dir.c_str(), nullptr);
    FILE* f = _wfopen((dir + L"\\dmc1.sav").c_str(), L"wb");
    if (f) { fwrite(file.data(), 1, file.size(), f); fclose(f); }
    snprintf(buf, sizeof(buf), "Exported %s to slot %d of your real save (works with the mod off). Copy in dmc1_exports.",
             PresetLabel(p).c_str(), slotNo);
    g_status = buf;
    g_statusOk = true;
    dlog::Write("[Menu] exported %s to real save slot %d", p.file.c_str(), slotNo);
}

void LoadSelected() {
    // Category rows start at 1 (row 0 is "New preset").
    const int cat = g_catSel - 1;
    if (cat < 0 || cat >= (int)g_cats.size()) return;
    const auto& list = g_cats[cat].presets;
    if (g_presetSel < 0 || g_presetSel >= (int)list.size()) return;
    LoadPreset(list[g_presetSel]);
}

// Delete: press Delete twice on a preset. The file isn't erased: it's renamed to
// "<name>.json.deleted" next to the original, so it's out of the menu but can be restored by
// renaming it back.
bool g_deleteArmed = false;
ULONGLONG g_deleteArmedAt = 0;

void DeleteSelected() {
    const int cat = g_catSel - 1;
    if (cat < 0 || cat >= (int)g_cats.size()) return;
    auto& list = g_cats[cat].presets;
    if (g_presetSel < 0 || g_presetSel >= (int)list.size()) return;
    const Preset& p = list[g_presetSel];
    char buf[512];
    if (!g_deleteArmed || GetTickCount64() - g_deleteArmedAt > 4000) {
        g_deleteArmed = true;
        g_deleteArmedAt = GetTickCount64();
        snprintf(buf, sizeof(buf), "Delete M%02d %s? Press Delete again to confirm.", p.mission, p.name.c_str());
        g_status = buf;
        g_statusOk = false;
        return;
    }
    g_deleteArmed = false;
    int n = MultiByteToWideChar(CP_UTF8, 0, p.file.c_str(), (int)p.file.size(), nullptr, 0);
    std::wstring path(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, p.file.c_str(), (int)p.file.size(), &path[0], n);
    if (!MoveFileExW(path.c_str(), (path + L".deleted").c_str(), MOVEFILE_REPLACE_EXISTING)) {
        g_status = "Couldn't delete the file (see the log).";
        g_statusOk = false;
        dlog::Write("[Menu] couldn't delete %s (error %lu)", p.file.c_str(), GetLastError());
        return;
    }
    dlog::Write("[Menu] deleted %s (renamed to .json.deleted)", p.file.c_str());
    snprintf(buf, sizeof(buf), "Deleted M%02d %s.", p.mission, p.name.c_str());
    const std::string catName = g_cats[cat].name;
    ReloadPresets();
    // Stay in the same category if it still has presets; otherwise go back to the categories.
    int found = -1;
    for (int i = 0; i < (int)g_cats.size(); ++i) if (g_cats[i].name == catName) found = i;
    if (found < 0) { g_page = Page::Categories; g_catSel = 0; }
    else {
        g_catSel = found + 1;
        if (g_presetSel >= (int)g_cats[found].presets.size()) g_presetSel = (int)g_cats[found].presets.size() - 1;
    }
    g_status = buf;
    g_statusOk = true;
    g_scrollToSel = true;
}

// ---- preset editor -----------------------------------------------------------

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
    return out;
}
std::string Lower(std::string x) {
    for (char& c : x) c = (char)tolower((unsigned char)c);
    return x;
}

const int kDiffs[] = {2, 3, 5, 6};
const char* kDiffNames[] = {"Easy", "Normal", "Hard", "Dante Must Die"};
const char* kCostumes[] = {"Dante", "Legendary Dark Knight", "Super Dante"};
const char* kArms[] = {"Default", "Alastor", "Ifrit", "Force Edge", "Sparda", "Yamato"};
const char* kGuns[] = {"Default", "Handguns", "Shotgun", "Grenadegun", "Nightmare-Beta"};
struct MoveRow { const char* name; int bit; int needs; };  // needs = Lv1 bit for a Lv2 move
const MoveRow kMoveRows[] = {
    {"Stinger Lv1", 12, 0}, {"Stinger Lv2", 11, 12}, {"Round Trip", 13, 0}, {"Air Hike", 14, 0},
    {"Air Raid", 8, 0},     {"Vortex Lv1", 10, 0},   {"Vortex Lv2", 9, 10},  {"Kick 13 Lv1", 5, 0},
    {"Kick 13 Lv2", 4, 5},  {"Magma Drive", 6, 0},   {"Rolling Blaze", 7, 0}, {"Meteor Lv1", 3, 0},
    {"Meteor Lv2", 2, 3},   {"Inferno", 1, 0},
};
struct WeaponRow { const char* name; int id; };
const WeaponRow kWeaponRows[] = {
    {"Alastor", 6}, {"Ifrit", 7}, {"Force Edge", 0}, {"Sparda", 8}, {"Yamato", 9},
    {"Handguns", 5}, {"Shotgun", 1}, {"Grenadegun", 3}, {"Nightmare-Beta", 4}, {"Needlegun", 2},
};
struct ItemRow { const char* name; int id; };
const ItemRow kItemRows[] = {{"Vital Star", 0x0A}, {"Devil Star", 0x0B}, {"Holy Water", 0x0D}, {"Untouchable", 0x0C}};

bool g_typing = false;        // typing the preset name
std::string g_typingBackup;
bool g_typeKeys[256] = {};

struct Editor {
    std::string name;
    int mission = 2, diff = 1, costume = 0;
    int orbs = 0, hp = 10, dt = 3;
    std::string gauge = "full";
    uint16_t moves = 0;
    bool weapons[10] = {};
    int items[4] = {};
    int arm = 0, gun = 0;
    int topCore = -1, bottomCore = -1;  // -1 = mission's own (story) state
    int sel = 0;
    Page returnTo = Page::Categories;
} g_ed;

void BaseBars(int difficulty, int& hp, int& dt) {
    if (difficulty == 2) { hp = 15; dt = 6; } else { hp = 10; dt = 3; }
}

bool IsStoryWeapon(int id) {
    for (int w : builder::StoryWeapons(g_ed.mission)) if (w == id) return true;
    return false;
}

// Weapons that are always in the save: the mission's story weapons, and Yamato with the LDK costume.
bool IsLockedWeapon(int id) { return IsStoryWeapon(id) || (id == 9 && g_ed.costume == 1); }
bool Owns(int id) { return id >= 0 && id < 10 && (g_ed.weapons[id] || IsLockedWeapon(id)); }
const int kArmIds[] = {-1, 6, 7, 0, 8, 9};  // kArms index -> weapon id
const int kGunIds[] = {-1, 5, 1, 3, 4};     // kGuns index -> weapon id
int MoveWeapon(int bit) { return bit <= 7 ? 7 : 6; }  // bits 1-7 Ifrit, 8-14 Alastor

// Keep the setup possible: equipped weapons must be owned, moves need their weapon.
void Reconcile() {
    if (g_ed.arm > 0 && !Owns(kArmIds[g_ed.arm])) g_ed.arm = 0;
    if (g_ed.gun > 0 && !Owns(kGunIds[g_ed.gun])) g_ed.gun = 0;
    for (const MoveRow& m : kMoveRows)
        if (!Owns(MoveWeapon(m.bit))) g_ed.moves &= (uint16_t)~(1u << m.bit);
}

// DT gauge choices: Full, Empty, Route, then half-rune steps up to the bars.
std::vector<std::string> GaugeChoices() {
    std::vector<std::string> c = {"full", "empty", "route"};
    for (int v = 60; v < g_ed.dt * 120; v += 60) c.push_back(std::to_string(v));
    return c;
}
std::string GaugeLabel(const std::string& g) {
    if (g == "full") return "Full";
    if (g == "empty") return "Empty";
    if (g == "route") return "Route (from the route save)";
    char b[48];
    snprintf(b, sizeof(b), "%s (%.1f runes)", g.c_str(), atoi(g.c_str()) / 120.0);
    return b;
}

void EditorFrom(const Preset& p) {
    g_ed = Editor();
    g_ed.name = p.name;
    g_ed.mission = p.mission;
    for (int i = 0; i < 4; ++i) if (kDiffs[i] == p.difficulty) g_ed.diff = i;
    g_ed.costume = p.costume;
    g_ed.orbs = (int)p.orbs;
    int bh, bd;
    BaseBars(p.difficulty, bh, bd);
    g_ed.hp = p.hpBars >= 0 ? p.hpBars : bh;
    g_ed.dt = p.dtBars >= 0 ? p.dtBars : bd;
    g_ed.gauge = Lower(p.dtGauge);
    for (const std::string& m : p.moves) { int bit; if (builder::MoveBit(m, bit)) g_ed.moves |= (uint16_t)(1u << bit); }
    for (const MoveRow& r : kMoveRows) if (r.needs && (g_ed.moves & (1u << r.bit))) g_ed.moves |= (uint16_t)(1u << r.needs);
    for (int w : builder::StoryWeapons(p.mission)) if (w >= 0 && w < 10) g_ed.weapons[w] = true;
    for (int w : p.extraWeapons) if (w >= 0 && w < 10) g_ed.weapons[w] = true;
    for (int i = 0; i < 4; ++i) { auto it = p.items.find(kItemRows[i].id); if (it != p.items.end()) g_ed.items[i] = it->second; }
    for (int i = 1; i < 6; ++i) if (Lower(p.devilArm) == Lower(kArms[i])) g_ed.arm = i;
    for (int i = 1; i < 5; ++i) if (Lower(p.gun) == Lower(kGuns[i])) g_ed.gun = i;
    g_ed.topCore = p.topCore;
    g_ed.bottomCore = p.bottomCore;
    Reconcile();
}

void EditorNew() {
    Preset p;
    p.mission = 2;
    p.name = "Mission 2 (custom)";
    EditorFrom(p);
}

Preset EditorToPreset() {
    Preset p;
    p.category = Narrow(g_config.captureCategory);
    p.name = g_ed.name.empty() ? "Mission " + std::to_string(g_ed.mission) + " (custom)" : g_ed.name;
    p.mission = g_ed.mission;
    p.difficulty = kDiffs[g_ed.diff];
    p.costume = g_ed.costume;
    p.orbs = (uint32_t)g_ed.orbs;
    p.hpBars = g_ed.hp;
    p.dtBars = g_ed.dt;
    p.dtGauge = g_ed.gauge;
    for (const MoveRow& r : kMoveRows) if (g_ed.moves & (1u << r.bit)) p.moves.push_back(r.name);
    for (int id = 0; id < 10; ++id) if (g_ed.weapons[id] && !IsLockedWeapon(id)) p.extraWeapons.push_back(id);
    for (int i = 0; i < 4; ++i) if (g_ed.items[i] > 0) p.items[kItemRows[i].id] = g_ed.items[i];
    if (g_ed.arm > 0) p.devilArm = kArms[g_ed.arm];
    if (g_ed.gun > 0) p.gun = kGuns[g_ed.gun];
    p.topCore = g_ed.topCore;
    p.bottomCore = g_ed.bottomCore;
    return p;
}

enum RowKind { R_NAME, R_MISSION, R_DIFF, R_COSTUME, R_ORBS, R_HP, R_DT, R_GAUGE, R_TOPCORE, R_BOTCORE, R_ARM, R_GUN, R_MOVE, R_WEAPON, R_ITEM,
               R_LOAD, R_SAVE };
struct Row { RowKind kind; int idx; const char* section; };

std::vector<Row> EditorRows() {
    std::vector<Row> r;
    r.push_back({R_NAME, 0, "Preset"});
    r.push_back({R_MISSION, 0, nullptr});
    r.push_back({R_DIFF, 0, nullptr});
    r.push_back({R_COSTUME, 0, nullptr});
    r.push_back({R_ORBS, 0, "Stats"});
    r.push_back({R_HP, 0, nullptr});
    r.push_back({R_DT, 0, nullptr});
    r.push_back({R_GAUGE, 0, nullptr});
    if (g_ed.mission >= builder::kFirstCoreMission) {
        r.push_back({R_TOPCORE, 0, "Nightmare cores"});
        r.push_back({R_BOTCORE, 0, nullptr});
    }
    for (int i = 0; i < (int)(sizeof(kWeaponRows) / sizeof(kWeaponRows[0])); ++i) r.push_back({R_WEAPON, i, i == 0 ? "Weapons owned" : nullptr});
    r.push_back({R_ARM, 0, "Starting weapons"});
    r.push_back({R_GUN, 0, nullptr});
    for (int i = 0; i < (int)(sizeof(kMoveRows) / sizeof(kMoveRows[0])); ++i) r.push_back({R_MOVE, i, i == 0 ? "Moves" : nullptr});
    for (int i = 0; i < 4; ++i) r.push_back({R_ITEM, i, i == 0 ? "Items" : nullptr});
    r.push_back({R_LOAD, 0, ""});
    r.push_back({R_SAVE, 0, nullptr});
    return r;
}

void SetMission(int m) {
    // Drop the old mission's story weapons that the new one doesn't have; add the new one's.
    for (int id = 0; id < 10; ++id) if (IsStoryWeapon(id)) g_ed.weapons[id] = false;
    g_ed.mission = m;
    for (int w : builder::StoryWeapons(m)) if (w >= 0 && w < 10) g_ed.weapons[w] = true;
    Reconcile();
    if (g_ed.name.empty() || g_ed.name.find("(custom)") != std::string::npos)
        g_ed.name = "Mission " + std::to_string(m) + " (custom)";
}

void EditorChange(const Row& row, int dir, bool shift) {
    switch (row.kind) {
        case R_MISSION: { int m = g_ed.mission + dir; if (m < 2) m = 23; if (m > 23) m = 2; SetMission(m); break; }
        case R_DIFF: {
            int bhOld, bdOld; BaseBars(kDiffs[g_ed.diff], bhOld, bdOld);
            g_ed.diff = (g_ed.diff + dir + 4) % 4;
            int bh, bd; BaseBars(kDiffs[g_ed.diff], bh, bd);
            if (g_ed.hp == bhOld) g_ed.hp = bh;   // keep bars at base if they were at base
            if (g_ed.dt == bdOld) g_ed.dt = bd;
            break;
        }
        case R_COSTUME: g_ed.costume = (g_ed.costume + dir + 3) % 3; Reconcile(); break;
        case R_ORBS: g_ed.orbs = std::max(0, std::min(9999999, g_ed.orbs + dir * (shift ? 1000 : 100))); break;
        case R_HP: g_ed.hp = std::max(1, std::min(30, g_ed.hp + dir)); break;
        case R_DT: {
            g_ed.dt = std::max(0, std::min(10, g_ed.dt + dir));
            if (atoi(g_ed.gauge.c_str()) >= g_ed.dt * 120) g_ed.gauge = "full";
            break;
        }
        case R_TOPCORE: g_ed.topCore = (g_ed.topCore + 1 + dir + 4) % 4 - 1; break;   // -1..2
        case R_BOTCORE: g_ed.bottomCore = (g_ed.bottomCore + 1 + dir + 4) % 4 - 1; break;
        case R_GAUGE: {
            auto c = GaugeChoices();
            int i = 0;
            for (int k = 0; k < (int)c.size(); ++k) if (c[k] == g_ed.gauge) i = k;
            g_ed.gauge = c[(i + dir + (int)c.size()) % c.size()];
            break;
        }
        case R_ARM:  // skip devil arms you don't own
            for (int k = 0; k < 6; ++k) { g_ed.arm = (g_ed.arm + dir + 6) % 6; if (g_ed.arm == 0 || Owns(kArmIds[g_ed.arm])) break; }
            break;
        case R_GUN:  // skip guns you don't own
            for (int k = 0; k < 5; ++k) { g_ed.gun = (g_ed.gun + dir + 5) % 5; if (g_ed.gun == 0 || Owns(kGunIds[g_ed.gun])) break; }
            break;
        case R_MOVE: {
            const MoveRow& m = kMoveRows[row.idx];
            const bool on = !(g_ed.moves & (1u << m.bit));
            if (on && !Owns(MoveWeapon(m.bit))) {
                g_status = std::string(m.name) + " needs " + (MoveWeapon(m.bit) == 7 ? "Ifrit" : "Alastor") +
                           " (tick it under Weapons owned)";
                g_statusOk = false;
                break;
            }
            if (on) { g_ed.moves |= (uint16_t)(1u << m.bit); if (m.needs) g_ed.moves |= (uint16_t)(1u << m.needs); }
            else {
                g_ed.moves &= (uint16_t)~(1u << m.bit);
                for (const MoveRow& o : kMoveRows) if (o.needs == m.bit) g_ed.moves &= (uint16_t)~(1u << o.bit);
            }
            break;
        }
        case R_WEAPON: {
            const int id = kWeaponRows[row.idx].id;
            if (!IsLockedWeapon(id)) g_ed.weapons[id] = !g_ed.weapons[id];
            Reconcile();  // unticking an equipped weapon un-equips it and drops its moves
            break;
        }
        case R_ITEM: g_ed.items[row.idx] = std::max(0, std::min(30, g_ed.items[row.idx] + dir * (shift ? 5 : 1))); break;
        default: break;
    }
}

void EditorConfirm(const Row& row) {
    if (row.kind == R_NAME) {
        g_typing = true;
        g_typingBackup = g_ed.name;
        const std::string m = "Mission " + std::to_string(g_ed.mission);
        if (g_ed.name == m + " (custom)" || g_ed.name == m + " (captured)") g_ed.name.clear();  // start fresh
        for (int vk = 0; vk < 256; ++vk) g_typeKeys[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
        g_status = "Type a name. Enter to keep it, Esc to cancel.";
        g_statusOk = true;
        return;
    }
    if (row.kind == R_LOAD) {
        LoadPreset(EditorToPreset());
        return;
    }
    if (row.kind == R_SAVE) {
        const Preset p = EditorToPreset();
        std::vector<uint8_t> test;
        std::string error;
        if (!builder::Build(p, test, error)) { g_status = "Can't save: " + error; g_statusOk = false; return; }
        const std::wstring path = presets::UserPresetPath(p.mission, g_ed.name);
        if (!presets::Save(p, path, error)) {
            g_status = "Can't save: " + error; g_statusOk = false; return;
        }
        const std::wstring file = path.substr(path.find_last_of(L"\\") + 1);
        char buf[512];
        snprintf(buf, sizeof(buf), "Saved to presets\\%s\\%s", p.category.c_str(), Narrow(file).c_str());
        g_status = buf;
        g_statusOk = true;
        return;
    }
    EditorChange(row, 1, false);  // Enter toggles checkboxes / steps a value
}

std::string RowValue(const Row& row) {
    char b[96];
    switch (row.kind) {
        case R_NAME: {
            // The mission number is shown but isn't part of the editable name.
            snprintf(b, sizeof(b), "M%02d  ", g_ed.mission);
            const std::string shown = g_typing ? g_ed.name + "_" : (g_ed.name.empty() ? "(unnamed)" : g_ed.name);
            return std::string(b) + shown + (g_typing ? "" : "   [Enter to rename]");
        }
        case R_MISSION: snprintf(b, sizeof(b), "< %d >", g_ed.mission); return b;
        case R_DIFF: return std::string("< ") + kDiffNames[g_ed.diff] + " >";
        case R_COSTUME: return std::string("< ") + kCostumes[g_ed.costume] + " >";
        case R_ORBS: snprintf(b, sizeof(b), "< %d >", g_ed.orbs); return b;
        case R_HP: snprintf(b, sizeof(b), "< %d >", g_ed.hp); return b;
        case R_DT: snprintf(b, sizeof(b), "< %d >", g_ed.dt); return b;
        case R_GAUGE: return "< " + GaugeLabel(g_ed.gauge) + " >";
        case R_TOPCORE:
        case R_BOTCORE: {
            static const char* kCore[3] = {"Blue", "Green", "Red"};
            const bool top = row.kind == R_TOPCORE;
            const int v = top ? g_ed.topCore : g_ed.bottomCore;
            if (v >= 0) return std::string("< ") + kCore[v] + " >";
            return std::string("< ") + kCore[builder::StoryCore(g_ed.mission, top)] + " (mission's own) >";
        }
        case R_ARM: return std::string("< ") + kArms[g_ed.arm] + (g_ed.arm ? " >" : " (mission's own) >");
        case R_GUN: return std::string("< ") + kGuns[g_ed.gun] + (g_ed.gun ? " >" : " (mission's own) >");
        case R_MOVE: {
            const int bit = kMoveRows[row.idx].bit;
            if (!Owns(MoveWeapon(bit))) return MoveWeapon(bit) == 7 ? "[ ] needs Ifrit" : "[ ] needs Alastor";
            return (g_ed.moves & (1u << bit)) ? "[x]" : "[ ]";
        }
        case R_WEAPON: {
            const int id = kWeaponRows[row.idx].id;
            if (IsStoryWeapon(id)) return "[x] story";
            if (IsLockedWeapon(id)) return "[x] costume";
            return g_ed.weapons[id] ? "[x]" : "[ ]";
        }
        case R_ITEM: snprintf(b, sizeof(b), "< %d >", g_ed.items[row.idx]); return b;
        default: return "";
    }
}

const char* RowName(const Row& row) {
    switch (row.kind) {
        case R_NAME: return "Name";
        case R_MISSION: return "Mission";
        case R_DIFF: return "Difficulty";
        case R_COSTUME: return "Costume";
        case R_ORBS: return "Red orbs";
        case R_HP: return "HP bars";
        case R_DT: return "DT bars";
        case R_GAUGE: return "DT gauge";
        case R_TOPCORE: return "Top core";
        case R_BOTCORE: return "Bottom core";
        case R_ARM: return "Devil arm";
        case R_GUN: return "Gun";
        case R_MOVE: return kMoveRows[row.idx].name;
        case R_WEAPON: return kWeaponRows[row.idx].name;
        case R_ITEM: return kItemRows[row.idx].name;
        case R_LOAD: return reset::InGame() ? "> Load now (also writes slot 10)" : "> Write to slot 10";
        case R_SAVE: return "> Save as preset";
    }
    return "";
}

void DrawEditor() {
    const auto rows = EditorRows();
    const ImVec4 dim(0.65f, 0.65f, 0.65f, 1.0f);
    const ImVec4 head(0.85f, 0.65f, 0.35f, 1.0f);
    if (!ImGui::BeginTable("##editor", 2, ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.45f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.55f);
    for (int i = 0; i < (int)rows.size(); ++i) {
        const Row& row = rows[i];
        if (row.section) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (*row.section) ImGui::TextColored(head, "%s", row.section);
            else ImGui::Spacing();
        }
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        char id[32];
        snprintf(id, sizeof(id), "##row%d", i);
        ImGui::Selectable(id, i == g_ed.sel, ImGuiSelectableFlags_SpanAllColumns);
        if (i == g_ed.sel && g_scrollToSel) ImGui::SetScrollHereY(0.5f);
        ImGui::SameLine(0, 0);
        const bool action = row.kind == R_LOAD || row.kind == R_SAVE;
        const bool locked = (row.kind == R_WEAPON && IsLockedWeapon(kWeaponRows[row.idx].id)) ||
                            (row.kind == R_MOVE && !Owns(MoveWeapon(kMoveRows[row.idx].bit)));
        if (action) ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.0f), "%s", RowName(row));
        else if (locked) ImGui::TextColored(dim, "%s", RowName(row));
        else ImGui::TextUnformatted(RowName(row));
        ImGui::TableSetColumnIndex(1);
        const std::string v = RowValue(row);
        if (locked) ImGui::TextColored(dim, "%s", v.c_str());
        else ImGui::TextUnformatted(v.c_str());
    }
    ImGui::EndTable();
}

// Name entry: reads the keyboard directly (the game gets no input while the menu is open).
void TypeName(bool focused, bool enter, bool backspace, bool esc) {
    if (esc) { g_ed.name = g_typingBackup; g_typing = false; g_status = "Name unchanged."; return; }
    if (enter) {
        while (!g_ed.name.empty() && g_ed.name.back() == ' ') g_ed.name.pop_back();
        g_typing = false;
        g_status = g_ed.name.empty() ? "Unnamed: saves as Mxx.json." : "Name set. Save as preset to keep it.";
        g_statusOk = true;
        return;
    }
    if (backspace && !g_ed.name.empty()) {
        // Remove one UTF-8 character.
        size_t i = g_ed.name.size() - 1;
        while (i > 0 && (g_ed.name[i] & 0xC0) == 0x80) --i;
        g_ed.name.erase(i);
    }
    BYTE state[256] = {};
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) state[VK_SHIFT] = 0x80;
    if (GetKeyState(VK_CAPITAL) & 1) state[VK_CAPITAL] = 0x01;
    for (int vk = 0x20; vk < 0xE0; ++vk) {
        if (vk == VK_RETURN || vk == VK_BACK || vk == VK_ESCAPE || (vk >= VK_PRIOR && vk <= VK_HELP)) continue;
        if (vk >= VK_LWIN && vk <= 0x2F) continue;
        if (vk >= VK_F1 && vk <= 0xB9) continue;  // function keys, num/scroll lock, media keys
        const bool down = focused && (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (down && !g_typeKeys[vk] && g_ed.name.size() < 40) {
            wchar_t ch[4];
            const int n = ToUnicode(vk, MapVirtualKeyW(vk, MAPVK_VK_TO_VSC), state, ch, 4, 0);
            if (n == 1 && ch[0] >= 32) {
                char utf8[8];
                const int len = WideCharToMultiByte(CP_UTF8, 0, ch, 1, utf8, sizeof(utf8), nullptr, nullptr);
                g_ed.name.append(utf8, len);
            }
        }
        g_typeKeys[vk] = down;
    }
}

// ---- drawing ---------------------------------------------------------------

const char* DifficultyName(int d) {
    switch (d) { case 2: return "Easy"; case 3: return "Normal"; case 5: return "Hard"; case 6: return "DMD"; }
    return "?";
}
const char* CostumeName(int c) {
    switch (c) { case 0: return "Dante"; case 1: return "Legendary Dark Knight"; case 2: return "Super Dante"; }
    return "?";
}

void DrawDetails(const Preset& p) {
    const ImVec4 dim(0.65f, 0.65f, 0.65f, 1.0f);
    ImGui::TextColored(dim, "%s  |  %s  |  %u orbs", DifficultyName(p.difficulty), CostumeName(p.costume), p.orbs);
    std::string bars = "HP " + (p.hpBars >= 0 ? std::to_string(p.hpBars) : std::string("base")) +
                       "  DT " + (p.dtBars >= 0 ? std::to_string(p.dtBars) : std::string("base")) +
                       "  DT gauge " + p.dtGauge;
    ImGui::TextColored(dim, "%s", bars.c_str());
    std::string moves = p.moves.empty() ? "none" : "";
    for (size_t i = 0; i < p.moves.size(); ++i) moves += (i ? ", " : "") + p.moves[i];
    ImGui::TextColored(dim, "Moves: %s", moves.c_str());
    std::string items;
    for (const auto& kv : p.items) {
        const char* n = kv.first == 0x0A ? "Vital Star" : kv.first == 0x0B ? "Devil Star"
                      : kv.first == 0x0C ? "Untouchable" : kv.first == 0x0D ? "Holy Water" : "item";
        items += (items.empty() ? "" : ", ") + std::string(n) + " x" + std::to_string(kv.second);
    }
    ImGui::TextColored(dim, "Items: %s", items.empty() ? "none" : items.c_str());
    if (p.mission >= builder::kFirstCoreMission) {
        static const char* kCore[3] = {"blue", "green", "red"};
        const int t = p.topCore >= 0 ? p.topCore : builder::StoryCore(p.mission, true);
        const int b = p.bottomCore >= 0 ? p.bottomCore : builder::StoryCore(p.mission, false);
        ImGui::TextColored(dim, "Nightmare cores: top %s, bottom %s", kCore[t], kCore[b]);
    }
    if (!p.devilArm.empty() || !p.gun.empty())
        ImGui::TextColored(dim, "Starts with: %s + %s", p.devilArm.empty() ? "default" : p.devilArm.c_str(),
                           p.gun.empty() ? "default" : p.gun.c_str());
}

}  // namespace

bool menu::IsOpen() { return g_page != Page::Closed; }

void menu::Init() {
    g_kMenu.vk = g_config.menuKey;
    g_savePath = mode::SavePath();
}

void menu::Update(bool focused) {
    if (!g_config.presetsEnabled) return;

    if (Pressed(g_kMenu, focused)) {
        if (IsOpen()) Close(); else Open();
    }

    if (IsOpen()) {
        const bool up = Pressed(g_kUp, focused, true);
        const bool down = Pressed(g_kDown, focused, true);
        const bool left = Pressed(g_kLeft, focused, true);
        const bool right = Pressed(g_kRight, focused, true);
        const bool enterKey = Pressed(g_kEnter, focused);
        const bool backKey = Pressed(g_kBack, focused);
        const bool editKey = Pressed(g_kEdit, focused);
        const bool delKey = Pressed(g_kDel, focused);
        const bool sortKey = Pressed(g_kSort, focused);
        const bool exportKey = Pressed(g_kExport, focused);
        const bool esc = Pressed(g_kEsc, focused);
        const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        // Lists: Right also opens, Left also goes back. The editor uses Left/Right to change values.
        const bool enter = enterKey || (g_page != Page::Editor && right);
        const bool back = backKey || (g_page != Page::Editor && left);

        if (esc && !g_typing) Close();
        else if (g_page == Page::Categories) {
            const int n = (int)g_cats.size() + 1;  // row 0 = "New preset"
            if (up) { g_catSel = (g_catSel + n - 1) % n; g_scrollToSel = true; }
            if (down) { g_catSel = (g_catSel + 1) % n; g_scrollToSel = true; }
            if (enter) {
                if (g_catSel == 0) {
                    EditorNew();
                    g_ed.returnTo = Page::Categories;
                    g_page = Page::Editor;
                    g_status.clear();
                } else {
                    g_page = Page::Presets;
                    if (g_presetSel >= (int)g_cats[g_catSel - 1].presets.size()) g_presetSel = 0;
                }
                g_scrollToSel = true;
            }
            if (back) Close();
        } else if (g_page == Page::Presets) {
            const auto& list = g_cats[g_catSel - 1].presets;
            const int n = (int)list.size();
            if (up) { g_presetSel = (g_presetSel + n - 1) % n; g_scrollToSel = true; }
            if (down) { g_presetSel = (g_presetSel + 1) % n; g_scrollToSel = true; }
            if (enter) LoadSelected();
            if (editKey && n > 0) {
                EditorFrom(list[g_presetSel]);
                g_ed.returnTo = Page::Presets;
                g_page = Page::Editor;
                g_scrollToSel = true;
                g_status.clear();
            }
            if (delKey && n > 0) DeleteSelected();
            if (exportKey && n > 0) ExportSelected();
            if (up || down || enter || editKey || back || delKey) g_exportArmed = false;
            if (sortKey && n > 0) {
                const std::string keep = list[g_presetSel].file;   // stay on the same preset
                g_sort = (SortMode)(((int)g_sort + 1) % 3);
                SortPresets();
                const auto& sorted = g_cats[g_catSel - 1].presets;
                for (int i = 0; i < (int)sorted.size(); ++i)
                    if (sorted[i].file == keep) g_presetSel = i;
                g_scrollToSel = true;
                g_status = std::string("Sorted by ") + SortName(g_sort);
                g_statusOk = true;
                g_deleteArmed = false;
            }
            if (up || down || enter || editKey || back) g_deleteArmed = false;
            if (back) { g_page = Page::Categories; g_scrollToSel = true; }
        } else if (g_page == Page::Editor && g_typing) {
            TypeName(focused, enterKey, backKey, esc);
        } else if (g_page == Page::Editor) {
            const auto rows = EditorRows();
            const int n = (int)rows.size();
            if (up) { g_ed.sel = (g_ed.sel + n - 1) % n; g_scrollToSel = true; }
            if (down) { g_ed.sel = (g_ed.sel + 1) % n; g_scrollToSel = true; }
            if (left) EditorChange(rows[g_ed.sel], -1, shift);
            if (right) EditorChange(rows[g_ed.sel], +1, shift);
            if (enterKey) EditorConfirm(rows[g_ed.sel]);
            if (backKey) {
                ReloadPresets();  // show a new save
                if (g_catSel > (int)g_cats.size()) g_catSel = 0;
                g_page = g_ed.returnTo == Page::Presets && g_catSel > 0 ? Page::Presets : Page::Categories;
                if (g_page == Page::Presets && g_presetSel >= (int)g_cats[g_catSel - 1].presets.size()) g_presetSel = 0;
                g_scrollToSel = true;
            }
        } else if (back) {
            Close();
        }
    } else {
        // Keep the other keys' state current so a held key doesn't fire when the menu opens.
        for (Key* k : {&g_kUp, &g_kDown, &g_kEnter, &g_kBack, &g_kLeft, &g_kRight, &g_kEsc, &g_kEdit, &g_kDel, &g_kSort, &g_kExport}) Pressed(*k, focused);
    }

    if (g_waitRelease && !AnyMenuKeyHeld()) g_waitRelease = false;
    input_block::SetBlocked(IsOpen() || g_waitRelease);
}

void menu::Draw() {
    if (!IsOpen()) return;

    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(screen.x * 0.5f, screen.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(screen.x * 0.42f, screen.y * 0.62f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.88f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing;
    if (!ImGui::Begin("##practice_menu", nullptr, flags)) { ImGui::End(); return; }

    const ImVec4 accent(0.85f, 0.25f, 0.25f, 1.0f);
    const ImVec4 dim(0.65f, 0.65f, 0.65f, 1.0f);

    ImGui::TextColored(accent, "PRACTICE LOADER");
    ImGui::SameLine();
    if (g_page == Page::Categories) ImGui::TextColored(dim, " -  choose a category");
    else if (g_page == Page::Editor) ImGui::TextColored(dim, " -  preset editor: %s", g_ed.name.c_str());
    else ImGui::TextColored(dim, " -  %s  (sorted by %s)", g_cats[g_catSel - 1].name.c_str(), SortName(g_sort));
    ImGui::Separator();

    const float footer = ImGui::GetTextLineHeightWithSpacing() * (g_page == Page::Presets ? 9.5f : 3.0f);
    ImGui::BeginChild("##list", ImVec2(0, -footer), false, ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav);
    if (g_page == Page::Categories) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.9f, 0.45f, 1.0f));
        ImGui::Selectable("+ New preset...", g_catSel == 0);
        ImGui::PopStyleColor();
        if (g_catSel == 0 && g_scrollToSel) ImGui::SetScrollHereY(0.5f);
        for (int i = 0; i < (int)g_cats.size(); ++i) {
            char label[256];
            snprintf(label, sizeof(label), "%s  (%zu missions)", g_cats[i].name.c_str(), g_cats[i].presets.size());
            ImGui::Selectable(label, i + 1 == g_catSel);
            if (i + 1 == g_catSel && g_scrollToSel) ImGui::SetScrollHereY(0.5f);
        }
    } else if (g_page == Page::Editor) {
        DrawEditor();
    } else if (g_page == Page::Presets) {
        const auto& list = g_cats[g_catSel - 1].presets;
        for (int i = 0; i < (int)list.size(); ++i) {
            char label[256];
            snprintf(label, sizeof(label), "M%02d   %s", list[i].mission, list[i].name.c_str());
            ImGui::Selectable(label, i == g_presetSel);
            if (i == g_presetSel && g_scrollToSel) ImGui::SetScrollHereY(0.5f);
        }
    }
    g_scrollToSel = false;
    ImGui::EndChild();

    ImGui::Separator();
    if (g_page == Page::Presets && !g_cats[g_catSel - 1].presets.empty())
        DrawDetails(g_cats[g_catSel - 1].presets[g_presetSel]);
    if (!g_status.empty())
        ImGui::TextColored(g_statusOk ? ImVec4(0.45f, 0.9f, 0.45f, 1.0f) : ImVec4(0.95f, 0.4f, 0.4f, 1.0f), "%s",
                           g_status.c_str());
    ImGui::TextColored(dim, "%s",
                       g_page == Page::Presets  ? "Up/Down select   Enter load   E edit   S sort   X export to your save   Delete delete   Backspace back   Esc close"
                       : g_page == Page::Editor ? "Up/Down select   Left/Right change (Shift = bigger steps)   "
                                                  "Enter toggle / confirm   Backspace back   Esc close"
                                                : "Up/Down select   Enter open   Esc close");
    ImGui::End();
}
