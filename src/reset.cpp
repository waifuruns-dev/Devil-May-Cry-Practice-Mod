#include "reset.h"
#include "config.h"
#include "log.h"
#include "game_addrs.h"

#include "overlay.h"
#include "dump.h"

#include <windows.h>
#include <cmath>
#include <cstring>

namespace {

const uintptr_t& kSessionPtr = g_addrs.session;  // dmc1.exe+? -> session data (found at startup)
const uintptr_t& kStagePtr = g_addrs.stage;      // -> stage/event manager
constexpr uintptr_t kSaveInSession = 0x1E40; // save slot offset 0 lives here
constexpr uintptr_t kEventField = 0x1CA4;
constexpr uint32_t kEvInGame = 0x2000;
constexpr uint32_t kEvMissionStart = 0x800;
constexpr uint32_t kEvTeleport = 0x10000000;
constexpr uintptr_t kStagePhase = 0x06;       // u8, 0 during normal play (see Update)
constexpr uintptr_t kTrack = 0x270, kRoom = 0x274, kNextTrack = 0x288, kNextRoom = 0x28C;
constexpr size_t kSlotSize = 0x970;
constexpr size_t kStageStateStart = 0x940;    // slot offsets that overlap the stage manager's state
constexpr size_t kStageStateEnd = 0x950;
constexpr size_t kHeader = 0x10;             // checksum header; the game's own load skips it too

int g_loadedCostume = -1;              // costume of the loaded player model
std::vector<uint8_t> g_snapshot;             // save data as it was when the current mission started
std::vector<uint8_t> g_pendingSlot;          // what the in-progress restart is loading
int g_lastSerial = 0;
bool g_pending = false;
ULONGLONG g_pendingSince = 0;
uint32_t g_lastLoggedEv = 0;
uint32_t g_lastLoggedRoom = 0xFFFFFFFF;
bool g_keyDown = false;

// Player DT gauge lives on the character, not in the save data, so it's set separately.
const uintptr_t& kActorPtr = g_addrs.actor;      // -> player
constexpr uintptr_t kActorGauge = 0x1BCE;    // DT gauge, 120 per rune
constexpr size_t kSaveGauge = 0x230;
constexpr size_t kSaveDtBars = 0x625;
constexpr ULONGLONG kTimeoutMs = 30000;

// The game's room load stops partway after the first request and needs a second one
// (found in testing: pressing F3 twice restarts instantly). From the logs, the events go
//   request -> bit 0x8000 cleared -> bit 0x02000000 set (load stage, stalls here)
//   -> second request -> 0x00400000 -> mission start.
// The teleport bit is never cleared by the game, so the follow-up keys off the 0x02000000 stage.
// Exactly one follow-up: more requests mid-load can leave the room half-loaded.
constexpr uint32_t kEvLoadStage = 0x02000000;
constexpr uint32_t kEvRoomActive = 0x8000;   // cleared by the load, normally set while playing
constexpr ULONGLONG kFollowUpDelayMs = 250;
constexpr ULONGLONG kStuckMs = 400;           // no change for this long in the stall stage = stuck
bool g_followUpSent = false;
ULONGLONG g_loadStageSeenAt = 0;   // when the event field last changed (while pending)
uint32_t g_stuckEv = 0;
uint32_t g_requestTrack = 0, g_requestRoom = 0, g_firstEv = 0;
bool g_triedTitleBits = false;
std::vector<uint8_t> g_preRequestSave;    // live save data from just before a mission request
constexpr uint32_t kEvTitleLoadBits = 0x00050000;
ULONGLONG g_lastTriggerAt = 0;
uint32_t g_evBeforeRestart = 0;

// After a restart, make sure the event field ends up like a normal mission start.
ULONGLONG g_fixupUntil = 0;
ULONGLONG g_missionStartAt = 0;
bool g_roomBitRestored = false;
// Trace events for a few seconds after every mission start (also normal ones, for comparison).
ULONGLONG g_traceUntil = 0;
ULONGLONG g_traceFrom = 0;

bool Read(uintptr_t addr, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), out, n, &got) && got == n;
}
bool Write(uintptr_t addr, const void* data, size_t n) {
    SIZE_T done = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(addr), data, n, &done) && done == n;
}
uintptr_t Deref(uintptr_t moduleOffset) {
    if (!moduleOffset) return 0;  // not found for this game build
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uintptr_t p = 0;
    return Read(base + moduleOffset, &p, sizeof(p)) ? p : 0;
}

bool ReadEvents(uintptr_t session, uint32_t& ev) { return Read(session + kEventField, &ev, 4); }

// Really playing a mission: "in game" (0x2000) alone is also on at the title screen and main menu
// (events 10002808 there). During a mission 0x80000000 is on as well, and the stage is on a real
// track (1-5; the title screen is track 0 room 0).
bool InMission() {
    uintptr_t session = Deref(kSessionPtr), stage = Deref(kStagePtr);
    uint32_t ev = 0, track = 0;
    if (!session || !stage || !ReadEvents(session, ev) || !Read(stage + kTrack, &track, 4)) return false;
    return (ev & 0x80002000) == 0x80002000 && track >= 1 && track <= 5;
}

// The game only starts a room load (dmc1.exe+24F4D0) when, among other things,
// (session+0x1F80 & 0xA00) == 0. session+0x1F80 is save offset 0x140, and route saves for
// M5, M12, M18, M19 and M21 carry bit 0x200 there (their missions open with a scripted start), so
// a mid-mission request for them was ignored. Loading from the title screen goes another way.
// The bits are cleared for the request and put back as soon as the mission has started.
constexpr uintptr_t kLoadBlockField = 0x1F80;
constexpr uint32_t kLoadBlockBits = 0xA00;
uint32_t ClearLoadBlock(uintptr_t session) {
    uint32_t v = 0;
    if (!Read(session + kLoadBlockField, &v, 4) || !(v & kLoadBlockBits)) return 0;
    const uint32_t had = v & kLoadBlockBits;
    v &= ~kLoadBlockBits;
    Write(session + kLoadBlockField, &v, 4);
    return had;
}
void RestoreLoadBlock(uintptr_t session, uint32_t bits) {
    uint32_t v = 0;
    if (!bits || !Read(session + kLoadBlockField, &v, 4)) return;
    v |= bits;
    Write(session + kLoadBlockField, &v, 4);
}
uint32_t g_missionLoadBlock = 0;   // scripted-event bit to put back after a mission start
constexpr uintptr_t kHudHpBars = 0x98, kHudDtBars = 0xA3;  // in the HUD state ([g_addrs.hud])

// What the title screen's mission init does (dmc1.exe+24F9A3-24F9FA) and our in-game request
// otherwise skips. session+0x1F80..0x1F8F are per-mission run-time flags that happen to be inside
// the save; the game zeroes them on a fresh start. Leaving the saved ones in place (e.g. 0x200 at
// 0x1F80, which blocks every room load, or 0x10000000 at 0x1F88) is what broke doors and menus
// in M12/M18/M19/M21.
void MissionInitResets(uintptr_t session, bool log) {
    uint32_t old1F80 = 0, cb0 = 0;
    Read(session + 0x1F80, &old1F80, 4);
    Read(session + 0x1CB0, &cb0, 4);
    uint32_t cb4 = 0;
    const uint8_t zero16[16] = {};
    Write(session + 0x1CB4, &cb4, 4);
    if (uintptr_t stage = Deref(kStagePtr)) { uint32_t z = 0; Write(stage + 0x1FC, &z, 4); }
    Write(session + 0x1F80, zero16, 16);
    Write(session + 0x1851, zero16, 2);
    Write(session + 0x1CD4, zero16, 4);
    Write(session + 0x1832, zero16, 1);
    uint32_t v = 0;
    if (Read(session + 0x2068, &v, 4)) { v &= 0xA7FFFFFF; Write(session + 0x2068, &v, 4); }
    if (old1F80 & 0x2000) {
        uint32_t f84 = 0x10000000;
        Write(session + 0x1F84, &f84, 4);
    }
    if (cb0 & 0x10) { cb4 |= 4; Write(session + 0x1CB4, &cb4, 4); }
    if (log) dlog::Write("[Reset] mission-start resets applied (session+1F80 was %08X)", old1F80);
}
uint32_t g_roomLoadBlock = 0;      // same for a room restart

// After putting the bits back: the scripted start that normally clears them (M5's intro does)
// doesn't always run after a mid-mission load. M12, M18, M19, M21 kept bit 0x200 set, which blocks
// doors (room loads) and the menus. So for 15s after a restart, once no cutscene has been running
// for 2s (event bits 0x00C00000 off), clear the bits if they're still set.
ULONGLONG g_lbWatchUntil = 0, g_lbQuietSince = 0;
void ArmLoadBlockWatch() {
    g_lbWatchUntil = GetTickCount64() + 30000;
    g_lbQuietSince = 0;
}
void UpdateLoadBlockWatch(ULONGLONG now) {
    if (!g_lbWatchUntil) return;
    if (now > g_lbWatchUntil) { g_lbWatchUntil = 0; return; }
    uintptr_t session = Deref(kSessionPtr);
    uint32_t ev = 0, block = 0;
    if (!session || !Read(session + kEventField, &ev, 4) || !Read(session + kLoadBlockField, &block, 4)) return;
    if (!(block & kLoadBlockBits)) { g_lbWatchUntil = 0; return; }   // the game cleared it itself
    uint32_t lock = 0;
    Read(session + 0x1CB4, &lock, 4);
    if ((ev & 0x00C00000) || (lock & 0x00808000) == 0x00808000) {  // cutscene / scripted event running
        g_lbQuietSince = 0;
        return;
    }
    if (!g_lbQuietSince) g_lbQuietSince = now;
    if (now - g_lbQuietSince >= 2000) {
        block &= ~kLoadBlockBits;
        Write(session + kLoadBlockField, &block, 4);
        dlog::Write("[Reset] scripted-event bit cleared (no cutscene for 2s)");
        g_lbWatchUntil = 0;
    }
}

// Starting weapons after a restart / preset load: equipped once no cutscene or scripted event has
// been running for a moment. A switch during a mission's opening cutscene doesn't take (M3).
ULONGLONG g_equipUntil = 0, g_equipQuietSince = 0;
void EquipFromSave(const std::vector<uint8_t>& slot);
void UpdatePendingEquip(ULONGLONG now, const std::vector<uint8_t>& slot) {
    if (!g_equipUntil) return;
    if (now > g_equipUntil) {
        g_equipUntil = 0;
        dlog::Write("[Reset] starting weapons not equipped: a cutscene ran for over a minute");
        return;
    }
    uintptr_t session = Deref(kSessionPtr);
    uint32_t ev = 0, lock = 0;
    if (!session || !Read(session + kEventField, &ev, 4)) return;
    Read(session + 0x1CB4, &lock, 4);
    if ((ev & 0x00C00000) || (lock & 0x00808000) == 0x00808000) {
        if (g_equipQuietSince) dlog::Verbose("[Reset] cutscene running; weapons wait");
        g_equipQuietSince = 0;
        return;
    }
    if (!g_equipQuietSince) { g_equipQuietSince = now; return; }
    if (now - g_equipQuietSince < 300) return;
    g_equipUntil = 0;
    EquipFromSave(slot);
}

// R3 quick switch: the game only unlocks it when it hands you a devil arm (session+0x2040 bits
// 0x20000 Alastor / 0x10000 Ifrit, session+0x1F9C bits 0x4 / 0x2). Whenever both Alastor and Ifrit
// are in the inventory, make sure those flags are set, whatever save or preset you came from.
ULONGLONG g_nextArmCheck = 0;
void EnsureDevilArmFlags(ULONGLONG now) {
    if (now < g_nextArmCheck) return;
    g_nextArmCheck = now + 1000;
    if (!InMission()) return;
    uintptr_t session = Deref(kSessionPtr);
    if (!session) return;
    uint8_t count = 0;
    uint8_t items[0x50] = {};
    if (!Read(session + kSaveInSession + 0x23B, &count, 1) ||
        !Read(session + kSaveInSession + 0x250, items, sizeof(items))) return;
    bool alastor = false, ifrit = false;
    for (int i = 0; i < count && i < 0x14; ++i) {
        if (items[i * 4] != 0) continue;
        if (items[i * 4 + 1] == 6) alastor = true;
        if (items[i * 4 + 1] == 7) ifrit = true;
    }
    if (!alastor || !ifrit) return;
    uint32_t give = 0, pickup = 0;
    if (!Read(session + 0x2040, &give, 4) || !Read(session + 0x1F9C, &pickup, 4)) return;
    if ((give & 0x30000) == 0x30000 && (pickup & 0x6) == 0x6) return;
    give |= 0x30000;
    pickup |= 0x6;
    Write(session + 0x2040, &give, 4);
    Write(session + 0x1F9C, &pickup, 4);
    dlog::Write("[Reset] own Alastor and Ifrit: R3 devil arm switch unlocked");
}

// The game keeps a second copy of the event field right after it (same bits in every dump).
constexpr uintptr_t kEventCopy = 0x1CA8;
constexpr uint32_t kEvSecretMission = 0x20;   // on while inside a secret mission
void ClearBitInCopy(uintptr_t session, uint32_t bit) {
    uint32_t v = 0;
    if (Read(session + kEventCopy, &v, 4) && (v & bit)) {
        v &= ~bit;
        Write(session + kEventCopy, &v, 4);
    }
}

bool TakeSnapshot() {
    uintptr_t session = Deref(kSessionPtr);
    if (!session) return false;
    std::vector<uint8_t> buf(kSlotSize);
    if (!Read(session + kSaveInSession, buf.data(), buf.size())) return false;
    g_snapshot.swap(buf);
    return true;
}

// Writes save data into the running game and asks it to start the mission from it.
bool Trigger(const std::vector<uint8_t>& slot, std::string& error, bool followUp = false) {
    uintptr_t session = Deref(kSessionPtr);
    uintptr_t stage = Deref(kStagePtr);
    if (!session || !stage) { error = "game data not found"; return false; }

    uint32_t ev = 0;
    if (!ReadEvents(session, ev)) { error = "couldn't read the event flags"; return false; }
    // A follow-up lands during the loading screen, where the "in game" bit may be off.
    if (!followUp && !InMission()) { error = "not in a mission"; return false; }

    // 1) Save data (everything after the checksum header, like the game's own load).
    if (!g_pending) {
        g_preRequestSave.resize(kSlotSize);
        if (!Read(session + kSaveInSession, g_preRequestSave.data(), kSlotSize)) g_preRequestSave.clear();
    }
    overlay::AbandonRun();  // whatever was running is void, and the save write mustn't look like a finish
    // Save data only, up to slot 0x940. The slot's tail overlaps the stage manager (0x102980):
    // writing it left junk in stage+0x10.. (M21 doors/menus broken, dumps 18/19).
    if (!Write(session + kSaveInSession + kHeader, slot.data() + kHeader, kStageStateStart - kHeader)) {
        error = "couldn't write the save data"; return false;
    }
    MissionInitResets(session, !followUp);
    // Retry count and carried-over time (save 0x844 / 0x848 = session+0x2684 / +0x2688). When the
    // count isn't 0, the mission start adds the carried time to the mission time (dmc1.exe+256538),
    // which is how the game keeps time across retries. A restart is a fresh attempt: zero both, or
    // the official time includes earlier attempts (41s run reported as 2:11).
    {
        const uint32_t zero[2] = {0, 0};
        Write(session + 0x2684, zero, 8);
    }
    // The scripted-event bit (0x200 at session+0x1F80) is what holds the player still during an
    // opening cutscene (M5). The reset above clears it like the title screen's init does, but a
    // title load then gets it back from its own path. Remember it and put it back once the mission
    // has started; the watcher clears it after the cutscene (M12/M19/M21 never clear it themselves).
    if (!followUp) {
        uint32_t saved1F80 = 0;
        memcpy(&saved1F80, &slot[0x140], 4);
        g_missionLoadBlock = saved1F80 & 0x200;
    }
    // Live HP / DT bar counts (HUD and max-gauge source). A title load sets them from the save;
    // a mid-mission load kept the previous mission's (DT runes carried over).
    {
        const uint8_t hpBars = slot[0x624], dtBars = slot[0x625];
        if (const uintptr_t hud = Deref(g_addrs.hud)) {
            Write(hud + kHudHpBars, &hpBars, 1);
            Write(hud + kHudDtBars, &dtBars, 1);
        }
    }

    // 2) Room load target = the current room (the mission start then picks its own start room).
    uint32_t track = 0, room = 0;
    if (!Read(stage + kTrack, &track, 4) || !Read(stage + kRoom, &room, 4) ||
        !Write(stage + kNextTrack, &track, 4) || !Write(stage + kNextRoom, &room, 4)) {
        error = "couldn't set the room"; return false;
    }

    // 3) "Next room load starts the mission" + "load the next room now".
    const uint32_t evBefore = ev;
    // Leaving a secret mission: its bit (0x20) stays on otherwise, and the next door ends the
    // "secret mission" with a clear screen. The game keeps a second copy at +0x1CA8.
    ev &= ~kEvSecretMission;
    ClearBitInCopy(session, kEvSecretMission);
    ev |= kEvMissionStart | kEvTeleport;
    if (!Write(session + kEventField, &ev, 4)) { error = "couldn't set the restart flags"; return false; }

    if (!g_pending) {
        g_requestTrack = track;
        g_requestRoom = room;
        g_firstEv = ev;
        g_triedTitleBits = false;
        g_pendingSince = GetTickCount64();
        g_followUpSent = false;
        g_loadStageSeenAt = 0;
        g_evBeforeRestart = evBefore;
    }
    g_pending = true;
    g_pendingSlot = slot;
    g_equipUntil = 0;
    g_lastTriggerAt = GetTickCount64();
    g_lastLoggedEv = ev;
    g_lastLoggedRoom = room;
    dlog::Write("[Reset] restart requested (track %u room %u, mission %u, events %08X)", track, room,
                slot[0x24] + slot[0x25], ev);
    return true;
}

// Sets the player's current DT gauge to the restarted save's value (capped to its runes).
void ApplyGauge(const std::vector<uint8_t>& slot) {
    if (slot.size() != kSlotSize) return;
    uintptr_t actor = Deref(kActorPtr);
    if (!actor) return;
    uint16_t gauge;
    memcpy(&gauge, &slot[kSaveGauge], 2);
    const uint16_t cap = (uint16_t)(slot[kSaveDtBars] * 120);
    if (gauge > cap) gauge = cap;
    if (Write(actor + kActorGauge, &gauge, 2)) dlog::Write("[Reset] DT gauge set to %u", gauge);
}

// Starting weapons. Loading from the title sets them from save 0x235/0x236/0x237; a mission start
// inside a running game doesn't (Force Edge showed up instead of Alastor). The player's fields use
// the same values (checked in dumps): melee 0x1BCC = save 0x235, gun 0x1C48 = 0x236, form 0x1C4C = 0x237.
// What ApplyEquipment saw right at the start. The melee field can read back as the save's value a
// moment later while Ifrit is still the one equipped (M3 preset with Alastor), so EquipFromSave
// trusts this reading too.
uint8_t g_startMelee = 0xFF, g_startGun = 0xFF, g_startForm = 0xFF;

void ApplyEquipment(const std::vector<uint8_t>& slot) {
    if (slot.size() != kSlotSize) return;
    uintptr_t actor = Deref(kActorPtr);
    if (!actor) return;
    uint8_t melee = 0, gun = 0, form = 0;
    Read(actor + 0x1BCC, &melee, 1);
    Read(actor + 0x1C48, &gun, 1);
    Read(actor + 0x1C4C, &form, 1);
    g_startMelee = melee; g_startGun = gun; g_startForm = form;
    const uint8_t wantMelee = slot[0x235], wantGun = slot[0x236], wantForm = slot[0x237];
    dlog::Write("[Reset] weapons after start: melee %u gun %u form %u (save says %u %u %u)%s", melee, gun, form,
                wantMelee, wantGun, wantForm,
                (melee != wantMelee || gun != wantGun || form != wantForm) ? "  MISMATCH" : "");
    // Not written here: the weapon models are already built by now, so changing the fields gives
    // e.g. two shotguns with handgun behaviour, then a crash (M2 test). See EquipFromSave.
}

// Equips the save's starting weapons through the game's own weapon-switch functions (the ones
// DDMK's weapon switcher calls: dmc1.exe+2C4A00 sword form, +2C99C0 sword, +2C4C50 gun). Only
// writing the player's fields gave the old models with the new behaviour (two shotguns, crash).
void EquipFromSave(const std::vector<uint8_t>& slot) {
    if (slot.size() != kSlotSize) return;
    uintptr_t actor = Deref(kActorPtr);
    if (!actor) return;
    uint8_t melee = 0, gun = 0, form = 0;
    Read(actor + 0x1BCC, &melee, 1);
    Read(actor + 0x1C48, &gun, 1);
    Read(actor + 0x1C4C, &form, 1);
    const uint8_t wantMelee = slot[0x235], wantGun = slot[0x236], wantForm = slot[0x237];
    if (wantMelee > 9 || wantForm > 2 || wantGun == 0 || wantGun > 4) {
        dlog::Write("[Reset] save has no valid starting weapons (%u %u %u); left as is", wantMelee, wantGun, wantForm);
        return;
    }
    if (!g_addrs.fnForm || !g_addrs.fnGun || !g_addrs.fnMelee) {
        dlog::Write("[Reset] weapon functions not found for this game build; weapons left as they are");
        return;
    }
    using MeleeFn = void(__fastcall*)(uint32_t weapon, uint32_t playAura);
    using FormFn = void(__fastcall*)(uint32_t form);
    using GunFn = void(__fastcall*)(uint32_t weapon);
    const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const bool meleeWrong = melee != wantMelee || form != wantForm ||
                            (g_startMelee != 0xFF && (g_startMelee != wantMelee || g_startForm != wantForm));
    const bool gunWrong = gun != wantGun || (g_startGun != 0xFF && g_startGun != wantGun);
    if (g_startMelee != 0xFF) { melee = g_startMelee; form = g_startForm; gun = g_startGun; }
    g_startMelee = g_startGun = g_startForm = 0xFF;
    if (meleeWrong) {
        Write(actor + 0x1BCC, &wantMelee, 1);
        Write(actor + 0x1C4C, &wantForm, 1);
        reinterpret_cast<FormFn>(exe + g_addrs.fnForm)(wantForm);
        reinterpret_cast<MeleeFn>(exe + g_addrs.fnMelee)(wantMelee, 0);
    }
    if (gunWrong) {
        Write(actor + 0x1C48, &wantGun, 1);
        reinterpret_cast<GunFn>(exe + g_addrs.fnGun)(wantGun);
    }
    dlog::Write("[Reset] equipped from save: melee %u->%u form %u->%u gun %u->%u", melee, wantMelee, form, wantForm,
                gun, wantGun);
}

// While a restart is pending, log what the game does with the flags and rooms (for debugging).
void TracePending() {
    uintptr_t session = Deref(kSessionPtr);
    uintptr_t stage = Deref(kStagePtr);
    uint32_t ev = 0, room = 0;
    if (session && ReadEvents(session, ev) && ev != g_lastLoggedEv) {
        dlog::Verbose("[Reset]   +%llums events %08X", GetTickCount64() - g_pendingSince, ev);
        g_lastLoggedEv = ev;
    }
    if (stage && Read(stage + kRoom, &room, 4) && room != g_lastLoggedRoom) {
        dlog::Verbose("[Reset]   +%llums room %u", GetTickCount64() - g_pendingSince, room);
        g_lastLoggedRoom = room;
    }
}

// ---------------------------------------------------------------------------------------------
// Room restart
//
// From memory dumps (mission 4): everything that makes a room "already done" (key items used,
// puzzle state, orbs picked up, mission stats) lives in the live save copy at session+0x1E40,
// the same 0x970 bytes the mission restart writes. The spawn point for the current room is
// kept in two places, set by the game on every room entry:
//   dmc1.exe+27E82A0  vec4 x, y, z, rotation (DDMK: "next position", read by the room load)
//   session+0x1D40    the same values
// So: snapshot those on room entry, and to restart write them back and reload the same room.
const uintptr_t& kNextPos = g_addrs.nextPos;
constexpr uintptr_t kSessionEntryPos = 0x1D40;
constexpr uintptr_t kRoomEventField = 0x1CD4;
constexpr size_t kRoomSaveEnd = 0x910;        // the game's own load copies 0x900 bytes from slot+0x10
constexpr uintptr_t kActorPos = 0x170;
constexpr uintptr_t kActorHp = 0x17A2;
constexpr uintptr_t kActorDtTime = 0x1BD2;
constexpr uintptr_t kStageRoomTimer = 0x18;   // stage+0x18: game's room timer (stops when paused / loading)

struct RoomSnap {
    bool valid = false;
    uint32_t track = 0, room = 0;
    std::vector<uint8_t> save;
    float nextPos[4] = {}, entryPos[4] = {};
    uint16_t hp = 0, gauge = 0, dtTime = 0;
    uint32_t roomEvent = 0;   // session+0x1CD4: bit 0x2000 cleared when Shadow died (room script state?)
    bool hasMissionTime = false;
    double missionFrames = 0;
} g_room;

uint32_t g_seenTrack = 0xFFFFFFFF, g_seenRoom = 0xFFFFFFFF;
bool g_roomSnapWanted = false;
ULONGLONG g_roomWantSince = 0;

bool g_roomPending = false;
ULONGLONG g_roomPendingSince = 0;
bool g_roomFollowUpSent = false;
bool g_roomSawUnload = false;      // 0x8000 went off: the load has begun
int32_t g_roomLastTimer = -1;
int g_roomTimerTicks = 0;          // room timer increments seen after the load began
ULONGLONG g_roomDoneAt = 0;        // load finished; finishing touches pending
bool g_roomKeyDown = false;

bool ReadCurrentRoom(uint32_t& track, uint32_t& room) {
    uintptr_t stage = Deref(kStagePtr);
    return stage && Read(stage + kTrack, &track, 4) && Read(stage + kRoom, &room, 4);
}

bool TakeRoomSnapshot() {
    if (!kNextPos) return false;  // spawn position not found for this game build
    uintptr_t session = Deref(kSessionPtr), actor = Deref(kActorPtr);
    const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    RoomSnap s;
    s.save.resize(kSlotSize);
    if (!session || !actor || !ReadCurrentRoom(s.track, s.room) ||
        !Read(session + kSaveInSession, s.save.data(), kSlotSize) ||
        !Read(exe + kNextPos, s.nextPos, 16) || !Read(session + kSessionEntryPos, s.entryPos, 16) ||
        !Read(actor + kActorHp, &s.hp, 2) || !Read(actor + kActorGauge, &s.gauge, 2) ||
        !Read(actor + kActorDtTime, &s.dtTime, 2) || !Read(session + kRoomEventField, &s.roomEvent, 4))
        return false;
    s.hasMissionTime = overlay::MissionElapsed(s.missionFrames);
    s.valid = true;
    g_room = std::move(s);
    dlog::Write("[Room] entry recorded: track %u room %u, spawn %.0f %.0f %.0f, HP %u, DT %u, mission %.0f frames",
                g_room.track, g_room.room, g_room.nextPos[0], g_room.nextPos[1], g_room.nextPos[2], g_room.hp,
                g_room.gauge, g_room.missionFrames);
    return true;
}

// Diagnostic (DebugReloadKey, F6): exactly what DDMK's "Reload Room" key does. Next room = current,
// spawn = where the player stands now, then the load flag. Nothing else is written.
void PlainReload(ULONGLONG now) {
    if (!kNextPos) return;
    uintptr_t session = Deref(kSessionPtr), stage = Deref(kStagePtr), actor = Deref(kActorPtr);
    const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uint32_t track, room, ev;
    float pos[4], rot;
    if (!session || !stage || !actor || !ReadCurrentRoom(track, room) || !ReadEvents(session, ev) ||
        !Read(actor + kActorPos, pos, 16) || !Read(actor + 0x124, &rot, 4)) {
        dlog::Write("[Debug] plain reload: game data not found");
        return;
    }
    pos[3] = rot;
    Write(stage + kNextTrack, &track, 4);
    Write(stage + kNextRoom, &room, 4);
    Write(exe + kNextPos, pos, 16);
    ev |= kEvTeleport;
    Write(session + kEventField, &ev, 4);
    dlog::Write("[Debug] plain reload requested (track %u room %u, events %08X)", track, room, ev);
    g_traceFrom = now;
    g_traceUntil = now + 8000;
    g_lastLoggedEv = ev;
}

bool RestartRoom(std::string& error) {
    if (!kNextPos) { error = "not supported on this game build (spawn position not found)"; return false; }
    if (g_pending) { error = "a mission restart is in progress"; return false; }
    if (!g_room.valid) { error = "no room entry recorded yet"; return false; }
    uint32_t track, room;
    if (!ReadCurrentRoom(track, room)) { error = "game data not found"; return false; }
    if (track != g_room.track || room != g_room.room) { error = "room entry not recorded yet"; return false; }

    uintptr_t session = Deref(kSessionPtr), stage = Deref(kStagePtr);
    const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uint32_t ev = 0;
    if (!session || !stage || !ReadEvents(session, ev) || !InMission()) { error = "not in a mission"; return false; }

    // Only the save data itself. The 0x970-byte slot runs past it: slot bytes 0x940+ are the
    // stage manager (0x102980), whose state was captured mid-load. Writing those back made the game
    // ignore the reload (and broke doors / pause) in every room but the first.
    if (!Write(session + kSaveInSession + kHeader, g_room.save.data() + kHeader, kRoomSaveEnd - kHeader) ||
        !Write(exe + kNextPos, g_room.nextPos, 16) || !Write(session + kSessionEntryPos, g_room.entryPos, 16) ||
        !Write(session + kRoomEventField, &g_room.roomEvent, 4) ||
        !Write(stage + kNextTrack, &track, 4) || !Write(stage + kNextRoom, &room, 4)) {
        error = "couldn't write the room state"; return false;
    }
    if (const uint32_t had = ClearLoadBlock(session)) {
        g_roomLoadBlock |= had;
        dlog::Write("[Room] cleared load-block bits %03X for the reload", had);
    }
    const uint32_t before = ev;
    ev |= kEvTeleport;
    if (!Write(session + kEventField, &ev, 4)) { error = "couldn't set the load flag"; return false; }

    if (!g_roomPending) {
        g_roomPendingSince = GetTickCount64();
        g_roomFollowUpSent = false;
        g_roomSawUnload = false;
        g_roomTimerTicks = 0;
        g_roomLastTimer = -1;
        g_evBeforeRestart = before;
        g_pendingSince = g_roomPendingSince;  // for the trace timestamps
        g_lastLoggedEv = ev;
        g_lastLoggedRoom = room;
    }
    g_roomPending = true;
    g_roomDoneAt = 0;
    dlog::Write("[Room] restart requested (track %u room %u, events %08X, +1CD4 %08X)", track, room, ev, g_room.roomEvent);
    return true;
}

// Called when the room has reloaded: put the player's stats and the timer back.
void FinishRoomRestart() {
    uintptr_t actor = Deref(kActorPtr), session = Deref(kSessionPtr);
    if (session && g_roomLoadBlock) {
        RestoreLoadBlock(session, g_roomLoadBlock);
        dlog::Write("[Room] load-block bits %03X put back", g_roomLoadBlock);
        ArmLoadBlockWatch();
    }
    g_roomLoadBlock = 0;
    if (actor) {
        Write(actor + kActorHp, &g_room.hp, 2);
        Write(actor + kActorGauge, &g_room.gauge, 2);
        Write(actor + kActorDtTime, &g_room.dtTime, 2);
    }
    if (g_room.hasMissionTime && overlay::RewindMission(g_room.missionFrames))
        dlog::Write("[Room] mission timer set back to %.0f frames", g_room.missionFrames);
    uint32_t ev = 0;
    if (session && ReadEvents(session, ev)) {
        uint32_t fixed = ev & ~kEvTeleport;
        if (g_evBeforeRestart & kEvRoomActive) fixed |= kEvRoomActive;
        if (fixed != ev && Write(session + kEventField, &fixed, 4))
            dlog::Write("[Room] events %08X -> %08X", ev, fixed);
    }
    dlog::Write("[Room] room restarted after %llums (HP %u, DT %u)", GetTickCount64() - g_roomPendingSince,
                g_room.hp, g_room.gauge);
}

void UpdateRoom(bool focused, bool missionStarted, ULONGLONG now) {
    uintptr_t session = Deref(kSessionPtr);
    uint32_t ev = 0;
    const bool haveEv = session && ReadEvents(session, ev);

    // 1) Notice room entries (and mission starts) and take the snapshot once the player has spawned.
    uint32_t track, room;
    if (ReadCurrentRoom(track, room)) {
        if (track != g_seenTrack || room != g_seenRoom || missionStarted) {
            g_seenTrack = track;
            g_seenRoom = room;
            if (!g_roomPending) {
                g_roomSnapWanted = true;
                g_roomWantSince = now;
            }
        }
    }
    if (g_roomSnapWanted && !g_roomPending && !g_pending && haveEv && (ev & kEvInGame)) {
        bool spawned = false;
        uintptr_t actor = Deref(kActorPtr);
        float pos[4], next[4];
        const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (actor && kNextPos && Read(actor + kActorPos, pos, 16) && Read(exe + kNextPos, next, 16))
            spawned = fabsf(pos[0] - next[0]) < 400 && fabsf(pos[2] - next[2]) < 400;
        // Wait until the room has fully settled: the game clears the load flag ~250ms after a load.
        const bool ready = (ev & kEvRoomActive) && !(ev & kEvTeleport) && spawned && now - g_roomWantSince >= 300;
        if (ready || now - g_roomWantSince > 5000) {
            g_roomSnapWanted = false;
            TakeRoomSnapshot();
        }
    }

    // 2) A room restart in progress.
    if (g_roomPending) {
        TracePending();
        int32_t rt = -1;
        if (const uintptr_t stage = Deref(kStagePtr)) Read(stage + kStageRoomTimer, &rt, 4);
        if (haveEv && !(ev & kEvRoomActive) && !g_roomSawUnload) {
            g_roomSawUnload = true;
            g_roomLastTimer = rt;
            dlog::Verbose("[Room]   +%llums load started", now - g_roomPendingSince);
        }
        if (g_roomSawUnload && !g_roomDoneAt) {
            // Done the way the game finishes a reload itself (F6 log): 0x8000 comes back on, then
            // ~250ms later the game clears the load flag.
            if (haveEv && (ev & kEvRoomActive) && !(ev & kEvTeleport)) g_roomDoneAt = now;
            else if (now - g_roomPendingSince > 6000) {
                g_roomDoneAt = now;
                dlog::Verbose("[Room]   the game didn't finish the reload by itself; finishing anyway");
            }
        }
        // (No follow-up request: a plain room reload goes through on its own, F6 test.)
        if (g_roomDoneAt) {
            g_roomPending = false;
            FinishRoomRestart();
        } else if (now - g_roomPendingSince > 10000) {
            g_roomPending = false;
            if (haveEv && (ev & kEvTeleport)) {
                ev &= ~kEvTeleport;
                Write(session + kEventField, &ev, 4);
            }
            dlog::Write("[Room] the room didn't reload within 10 seconds; flag cleared");
        }
    }

    // Diagnostic plain reload.
    static bool debugDown = false;
    const bool dd = focused && g_config.debugReloadKey && (GetAsyncKeyState(g_config.debugReloadKey) & 0x8000) != 0;
    if (dd && !debugDown) PlainReload(now);
    debugDown = dd;

    // 3) Key.
    const bool down = focused && g_config.roomResetKey && (GetAsyncKeyState(g_config.roomResetKey) & 0x8000) != 0;
    if (down && !g_roomKeyDown && g_config.resetEnabled) {
        std::string error;
        if (!RestartRoom(error)) dlog::Write("[Room] not restarted: %s", error.c_str());
    }
    g_roomKeyDown = down;
}

void ClearFlags() {
    uintptr_t session = Deref(kSessionPtr);
    uint32_t ev = 0;
    if (session && ReadEvents(session, ev) && (ev & (kEvMissionStart | kEvTeleport))) {
        ev &= ~(kEvMissionStart | kEvTeleport);
        Write(session + kEventField, &ev, 4);
    }
}

}  // namespace

void reset::Init() {}

bool reset::InGame() { return InMission(); }

int reset::LoadedCostume() { return g_loadedCostume; }

bool reset::RestartMission(std::string& error) {
    if (g_snapshot.size() != kSlotSize) { error = "no mission start recorded yet"; return false; }
    return Trigger(g_snapshot, error);
}

bool reset::LoadSlotNow(const std::vector<uint8_t>& slot, std::string& error) {
    if (slot.size() != kSlotSize) { error = "bad slot"; return false; }
    if (!Trigger(slot, error)) return false;
    g_snapshot = slot;  // F3 now returns to this preset
    return true;
}

void reset::Update(bool focused, int serial) {
    const ULONGLONG now = GetTickCount64();
    const bool missionStarted = serial != g_lastSerial;
    if (missionStarted) {
        g_lastSerial = serial;
        // A mission just started: remember its starting save data for F3.
        const bool restartedByUs = g_pending;
        if (g_pending) {
            g_pending = false;
            ClearFlags();  // make sure a later door doesn't restart the mission again
            if (g_missionLoadBlock) {  // the 0x00400000 stage was never seen: put it back now
                if (uintptr_t ses = Deref(kSessionPtr)) RestoreLoadBlock(ses, g_missionLoadBlock);
                dlog::Write("[Reset] scripted-event bit %03X put back after the start", g_missionLoadBlock);
                ArmLoadBlockWatch();
                g_missionLoadBlock = 0;
            }

            ApplyGauge(g_pendingSlot);
            ApplyEquipment(g_pendingSlot);
            dlog::Write("[Reset] mission restarted after %llums", now - g_pendingSince);
            g_fixupUntil = now + 5000;
            g_missionStartAt = now;
            g_roomBitRestored = false;
        }
        // A start that isn't ours (title-screen load, or the next mission after a results screen)
        // keeps or loads the player model for the save's costume: remember which one that is.
        if (!restartedByUs) {
            uint8_t costume = 0xFF;
            if (uintptr_t ses = Deref(kSessionPtr)) Read(ses + kSaveInSession + 0x27, &costume, 1);
            if (costume <= 2 && costume != g_loadedCostume) {
                g_loadedCostume = costume;
                dlog::Write("[Reset] player model costume: %u", costume);
            }
        }
        if (TakeSnapshot()) dlog::Write("[Reset] mission start recorded");
        g_traceFrom = now;
        g_traceUntil = now + 5000;
        uintptr_t session = Deref(kSessionPtr);
        uint32_t ev = 0;
        if (session && ReadEvents(session, ev)) {
            dlog::Verbose("[Reset]   start events %08X", ev);
            g_lastLoggedEv = ev;
        }
    }

    if (!g_pending) UpdatePendingEquip(now, g_pendingSlot);

    // Trace the event field for a few seconds after any mission start.
    if (!g_pending && now < g_traceUntil) {
        uintptr_t session = Deref(kSessionPtr);
        uint32_t ev = 0;
        if (session && ReadEvents(session, ev) && ev != g_lastLoggedEv) {
            dlog::Verbose("[Trace]   +%llums events %08X", now - g_traceFrom, ev);
            g_lastLoggedEv = ev;
        }
    }

    // After a restart: the load clears the "room active" bit (0x8000) and, unlike a normal
    // mission start, it doesn't come back (black screen, doors dead until a pause). If the game
    // hasn't set it again shortly after the start, restore it.
    if (!g_pending && now < g_fixupUntil && !g_roomBitRestored &&
        now - g_missionStartAt >= 500) {
        uintptr_t session = Deref(kSessionPtr);
        uint32_t ev = 0;
        if (session && ReadEvents(session, ev)) {
            if (!(ev & kEvRoomActive) && (g_evBeforeRestart & kEvRoomActive)) {
                ev |= kEvRoomActive;
                if (Write(session + kEventField, &ev, 4))
                    dlog::Write("[Reset] restored room-active bit, events now %08X", ev);
            }
            g_roomBitRestored = true;
            // Weapons: not while a cutscene runs (M3 opens with one; the switch was ignored).
            g_equipUntil = now + 60000;
            g_equipQuietSince = 0;
            // Stage manager byte +6: 0 in every normal-play dump, but 2 after a mission switch
            // (M3's door wouldn't open) and 5 when a reload was being ignored. Put it back to 0.
            uintptr_t stage = Deref(kStagePtr);
            uint8_t phase = 0;
            if (stage && Read(stage + kStagePhase, &phase, 1) && phase != 0) {
                const uint8_t zero = 0;
                if (Write(stage + kStagePhase, &zero, 1))
                    dlog::Write("[Reset] stage byte +6 was %u after the start; set to 0", phase);
            }
        }
    }

    if (g_pending) {
        TracePending();

        // Put the scripted-event bit back once the load is past the point where it would block it
        // (0x00400000 stage, just before the mission start runs). A title-screen load has it in
        // place when the mission start runs, and the mission's opening script reads it (M5 locks the
        // player during its intro, then clears it). Putting it back after the start was too late.
        if (g_missionLoadBlock) {
            uintptr_t ses = Deref(kSessionPtr);
            uint32_t e = 0;
            if (ses && ReadEvents(ses, e) && (e & 0x00400000)) {
                RestoreLoadBlock(ses, g_missionLoadBlock);
                dlog::Verbose("[Reset]   scripted-event bit %03X put back before the mission start", g_missionLoadBlock);
                ArmLoadBlockWatch();
                g_missionLoadBlock = 0;
            }
        }

        // The mission start moves the current room to the mission's start room straight away, then
        // waits for the next room to match it (that's what the second request used to fix).
        // Point the next room at it as soon as it changes.
        {
            uintptr_t stg = Deref(kStagePtr);
            uint32_t t = 0, r = 0, nt = 0, nr = 0;
            if (stg && Read(stg + kTrack, &t, 4) && Read(stg + kRoom, &r, 4) && Read(stg + kNextTrack, &nt, 4) &&
                Read(stg + kNextRoom, &nr, 4) && (t != g_requestTrack || r != g_requestRoom) && (nt != t || nr != r)) {
                Write(stg + kNextTrack, &t, 4);
                Write(stg + kNextRoom, &r, 4);
                dlog::Verbose("[Reset]   +%llums start room is track %u room %u: next room updated", now - g_pendingSince, t, r);
            }
        }

        // No reaction at all after 3s (shouldn't happen since the load-block fix; it was M5/M18):
        // give up, put the old save data back, clear the flags, and point to the title screen.
        {
        uintptr_t session = Deref(kSessionPtr);
        if (now - g_pendingSince > 3000 && g_lastLoggedEv == g_firstEv) {
            uintptr_t stg = Deref(kStagePtr);
            uint8_t hdr[16] = {};
            if (stg) Read(stg, hdr, 16);
            dlog::Verbose("[Reset]   no reaction after 3s (stage %02X%02X %02X%02X %02X%02X %02X%02X): giving up",
                        hdr[0], hdr[1], hdr[2], hdr[3], hdr[4], hdr[5], hdr[6], hdr[7]);
            if (session && g_preRequestSave.size() == kSlotSize)
                Write(session + kSaveInSession + kHeader, g_preRequestSave.data() + kHeader, kStageStateStart - kHeader);
            uint32_t e = 0;
            if (session && ReadEvents(session, e)) {
                e &= ~(kEvMissionStart | kEvTeleport);
                Write(session + kEventField, &e, 4);
            }
            g_pending = false;
            const int mission = g_pendingSlot.size() == kSlotSize ? g_pendingSlot[0x24] + g_pendingSlot[0x25] : 0;
            char msg[160];
            snprintf(msg, sizeof(msg), "Mission %d can't be loaded mid-mission yet.\nIt's in the practice slot: load it from the title screen.", mission);
            dump::ShowMessage(msg, 6000);
        }
        }

        // Follow-up request only if the load is really stuck: sitting in the 0x02000000 stage
        // (without 0x00400000) with no change for 1.5s. Sending it while a load was still going
        // caused double mission screens, broken textures and crashes on a mission switch.
        uintptr_t session = Deref(kSessionPtr);
        uint32_t ev = 0;
        if (session && ReadEvents(session, ev)) {
            if (ev != g_stuckEv || !g_loadStageSeenAt) { g_stuckEv = ev; g_loadStageSeenAt = now; }
        }
        const bool stuck = (g_stuckEv & kEvLoadStage) && !(g_stuckEv & 0x00400000) && g_loadStageSeenAt &&
                           now - g_loadStageSeenAt >= kStuckMs;
        if (!g_followUpSent && stuck) {
            std::string error;
            g_followUpSent = true;
            const std::vector<uint8_t> slot = g_pendingSlot;
            if (Trigger(slot, error, true))
                dlog::Write("[Reset] load stuck for %llums: follow-up request sent", kStuckMs);
            else dlog::Write("[Reset] follow-up request failed: %s", error.c_str());
        }

        if (GetTickCount64() - g_pendingSince > kTimeoutMs) {
            g_pending = false;
            ClearFlags();
            dlog::Write("[Reset] the game didn't restart within %llu seconds; flags cleared", kTimeoutMs / 1000);
        }
    }

    UpdateRoom(focused, missionStarted, now);
    UpdateLoadBlockWatch(now);
    if (!g_pending) EnsureDevilArmFlags(now);

    if (!g_config.resetEnabled) return;
    const bool down = focused && (GetAsyncKeyState(g_config.resetKey) & 0x8000) != 0;
    if (down && !g_keyDown) {
        std::string error;
        if (!RestartMission(error)) dlog::Write("[Reset] not restarted: %s", error.c_str());
    }
    g_keyDown = down;
}
