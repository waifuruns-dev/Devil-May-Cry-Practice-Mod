#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Instant mission restart, without the title screen.
//
// How the game does it (found by reverse engineering, plus DDMK's names):
//   session  = [dmc1.exe+? (found at startup, game_addrs)] = 0x100200; the save data lives at session+0x1E40
//   events   = session+0x1CA4: bit 0x800 = "next room load starts the mission",
//                              bit 0x10000000 = "load the next room now" (teleport)
//   stage    = [dmc1.exe+60B148]: +0x270/+0x274 current track/room, +0x288/+0x28C next track/room
// Writing the wanted save data into the session and setting both bits makes the game run its own
// mission start, exactly like walking into a new mission.
namespace reset {

void Init();

// True while a mission is being played (the game's "in game" event bit is set).
bool InGame();

// Restarts the current mission from the snapshot taken when it started.
bool RestartMission(std::string& error);

// Loads a built save slot (0x970 bytes) straight into the running game and restarts with it.
bool LoadSlotNow(const std::vector<uint8_t>& slot, std::string& error);

// Room restart (RoomResetKey, F4) is handled inside Update as well: the room's state is recorded
// on entry and written back before reloading the same room.

// Costume of the player model that's actually loaded (0 Dante, 1 LDK, 2 Super), or -1 if not seen
// yet. The model is only loaded by a title-screen load; a mid-mission load can't change it.
int LoadedCostume();

// Call every frame. missionStartSerial goes up each time a mission starts.
void Update(bool gameFocused, int missionStartSerial);

}  // namespace reset
