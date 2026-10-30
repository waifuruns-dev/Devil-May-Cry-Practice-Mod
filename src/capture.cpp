#include "capture.h"
#include "config.h"
#include "dump.h"
#include "log.h"
#include "game_addrs.h"
#include "overlay.h"
#include "presets.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>


namespace {

const uintptr_t& kSessionPtr = g_addrs.session;
const uintptr_t& kActorPtr = g_addrs.actor;
constexpr uintptr_t kSaveInSession = 0x1E40;
constexpr size_t kSlotSize = 0x970;

bool g_keyDown = false;

bool Read(uintptr_t addr, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), out, n, &got) && got == n;
}
uintptr_t Deref(uintptr_t moduleOffset) {
    if (!moduleOffset) return 0;  // not found for this game build
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uintptr_t p = 0;
    return Read(base + moduleOffset, &p, sizeof(p)) ? p : 0;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// Names match the preset format (presets\README.txt).
const char* kMoveNames[15] = {nullptr,       "Inferno",     "Meteor Lv2",  "Meteor Lv1", "Kick 13 Lv2",
                              "Kick 13 Lv1", "Magma Drive", "Rolling Blaze", "Air Raid", "Vortex Lv2",
                              "Vortex Lv1",  "Stinger Lv2", "Stinger Lv1", "Round Trip", "Air Hike"};
const char* kWeaponNames[10] = {"Force Edge", "Shotgun", "Needlegun", "Grenadegun", "Nightmare-Beta",
                                "Handguns",   "Alastor", "Ifrit",     "Sparda",     "Yamato"};
const char* ItemName(int id) {
    switch (id) {
        case 0x0A: return "Vital Star";
        case 0x0B: return "Devil Star";
        case 0x0C: return "Untouchable";
        case 0x0D: return "Holy Water";
    }
    return nullptr;
}
const char* DevilArmName(int melee, int form) {
    if (form == 1) return "Sparda";
    if (form == 2) return "Yamato";
    if (melee == 0) return "Alastor";
    if (melee == 1) return "Ifrit";
    if (melee == 4) return "Force Edge";
    return nullptr;
}
const char* GunName(int gun) {
    switch (gun) {
        case 1: return "Handguns";
        case 2: return "Shotgun";
        case 3: return "Grenadegun";
        case 4: return "Nightmare-Beta";
    }
    return nullptr;
}
const char* DifficultyName(int d) {
    switch (d) {
        case 2: return "easy";
        case 3: return "normal";
        case 5: return "hard";
        case 6: return "dmd";
    }
    return nullptr;
}
const char* CostumeName(int c) { return c == 1 ? "ldk" : c == 2 ? "super" : "dante"; }


void Capture() {
    const uintptr_t session = Deref(kSessionPtr), actor = Deref(kActorPtr);
    std::vector<uint8_t> s(kSlotSize);
    uint32_t events = 0;
    if (!session || !actor || !Read(session + kSaveInSession, s.data(), kSlotSize) ||
        !Read(session + 0x1CA4, &events, 4) || (events & 0x80002000) != 0x80002000) {
        dump::ShowMessage("Capture: start a mission first.", 3000);
        return;
    }
    int mission = 0;
    if (!overlay::CurrentMission(mission) || mission < 2 || mission > 23) {
        dump::ShowMessage("Capture: missions 2-23 only (mission 1 is New Game).", 3000);
        return;
    }

    Preset p;
    p.name = "Mission " + std::to_string(mission) + " (captured)";
    p.mission = mission;
    p.difficulty = DifficultyName(s[0x26]) ? s[0x26] : 3;
    p.costume = s[0x27] <= 2 ? s[0x27] : 0;
    memcpy(&p.orbs, &s[0x634], 4);
    p.hpBars = s[0x624];
    p.dtBars = s[0x625];

    // DT gauge from the player (the live value), as a number, or "full" when it's full.
    uint16_t gauge = 0;
    Read(actor + 0x1BCE, &gauge, 2);
    p.dtGauge = gauge >= s[0x625] * 120 ? std::string("full") : std::to_string(gauge);

    uint16_t moves;
    memcpy(&moves, &s[0x62A], 2);
    for (int bit = 14; bit >= 1; --bit)
        if (moves & (1u << bit)) p.moves.push_back(kMoveNames[bit]);

    // Item list: consumables (category 2) and weapons (category 0).
    const int count = s[0x23B];
    for (int i = 0; i < count && 0x250 + i * 4 + 3 < 0x2A0; ++i) {
        const uint8_t cat = s[0x250 + i * 4], id = s[0x251 + i * 4], n = s[0x252 + i * 4];
        if (cat == 2 && ItemName(id) && n > 0) p.items[id] = n;
        if (cat == 0 && id < 10) p.extraWeapons.push_back(id);
    }

    // Equipped weapons from the player (the save's copy only updates at a mission end).
    uint8_t melee = 0, gun = 0, form = 0;
    Read(actor + 0x1BCC, &melee, 1);
    Read(actor + 0x1C48, &gun, 1);
    Read(actor + 0x1C4C, &form, 1);
    if (const char* a = DevilArmName(melee, form)) p.devilArm = a;
    if (const char* g = GunName(gun)) p.gun = g;

    // Nightmare cores (from M17 on; the fights in M16 / M18 / M20 update the save copy).
    if (mission >= builder::kFirstCoreMission) builder::CoresFromByte(s[0x23A], p.topCore, p.bottomCore);

    const std::wstring path = presets::UserPresetPath(mission);
    std::string error;
    if (!presets::Save(p, path, error)) {
        dump::ShowMessage(("Capture failed: " + error).c_str(), 4000);
        return;
    }
    char msg[256];
    snprintf(msg, sizeof(msg), "Saved M%02d to presets\\%s", mission, Narrow(g_config.captureCategory).c_str());
    dump::ShowMessage(msg, 3000);
}

}  // namespace

void capture::Update(bool focused) {
    if (!g_config.captureKey) return;
    const bool down = focused && (GetAsyncKeyState(g_config.captureKey) & 0x8000) != 0;
    if (down && !g_keyDown) Capture();
    g_keyDown = down;
}
