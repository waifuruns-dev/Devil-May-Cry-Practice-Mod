#pragma once
#include <cstdint>
#include <string>

// Addresses inside dmc1.exe that move between game builds (offsets from the exe's base).
// Found at startup by searching the game's code for the instructions that use them, so the mod
// works on any build whose code still does the same things. 0 = not found (features that need it
// switch themselves off).
//
// Builds checked: PE timestamp 5AB4CAD1 (2018 Steam) and 6A72EAB6 (newer).
// Absolute addresses in the game's fixed memory block (0x100000...) are read through these
// pointers too, never hardcoded, so a build that moves that block still works.
struct GameAddrs {
    uintptr_t session = 0;     // -> session data (0x100200)
    uintptr_t stage = 0;       // -> stage / room manager (0x102980)
    uintptr_t actor = 0;       // -> player (0x102FD0)
    uintptr_t startFrame = 0;  // u32, frame counter value at mission start (mission timer)
    uintptr_t nextPos = 0;     // vec4, spawn position for the next room load
    uintptr_t saveBuf = 0;     // -> the game's in-memory dmc1.sav (+0x70 = slot 1; 0x139D40)
    uintptr_t hud = 0;         // -> HUD state (+0x98 HP bars, +0xA3 DT bars; 0x1378B0)
    uintptr_t fnForm = 0;      // void(uint32 form)            sword form (Sparda / Yamato)
    uintptr_t fnGun = 0;       // void(uint32 gun)
    uintptr_t fnMelee = 0;     // void(uint32 melee, uint32 aura)
};

extern GameAddrs g_addrs;

namespace game_addrs {

// Runs the search; true if everything was found. Logs what it found (only the final result when
// quiet). Newer builds keep their code encrypted until the game has started (Steam's protection),
// so the caller retries until this succeeds.
bool Resolve(bool quiet = false);

// Retries Resolve for up to `timeoutMs` (the code may still be encrypted right after launch).
void ResolveWithRetry(unsigned timeoutMs);

// For INI addresses: "@Session", "@Stage", "@Actor", "@StartFrame", "@NextPos", "@Hud", "@SaveBuf"
// -> offset. False if
// unknown or not found.
bool Lookup(const std::string& name, uintptr_t& offset);

}  // namespace game_addrs
