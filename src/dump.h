#pragma once

// Debug memory dumps, for finding the room state (boss/room-cleared flags, entrance).
// Press DumpKey: the game's data memory is written to dumps\NNN\ next to the INI, one .bin per
// memory region, plus info.txt (room, events, region list). Compare dumps taken at room entry and
// after killing a boss / breaking a crystal etc.
namespace dump {

void Init();
void Update(bool gameFocused);
void Draw();          // short "Dump N saved" message on screen
bool WantsDraw();

// Shows a message on screen for a few seconds (used by other features too).
void ShowMessage(const char* text, unsigned ms);

}  // namespace dump
