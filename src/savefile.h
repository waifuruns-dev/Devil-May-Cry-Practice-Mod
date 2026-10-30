#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace savefile {

// Finds dmc1.sav: the INI override if set, otherwise Steam's logged-in user folder
// (Steam\userdata\<id>\631510\remote\dmc1.sav), otherwise the most recently changed one.
std::wstring Locate(const std::wstring& iniOverride);

// Writes one slot (1-10) into the save file. Makes a one-time backup first.
bool WriteSlot(const std::wstring& path, int slot, const std::vector<uint8_t>& data, std::string& error);

// Also puts the slot into the game's in-memory copy of the save file, so Load Game shows it
// without restarting the game. False if that copy couldn't be found.
bool WriteSlotToMemory(int slot, const std::vector<uint8_t>& data);

// The game's whole in-memory copy of the 10 slots (what Load Game shows and what the game saves).
bool ReadSlotsFromMemory(std::vector<uint8_t>& out);
bool WriteSlotsToMemory(const std::vector<uint8_t>& data);

// Plain file copy (the practice file starts as a copy of the real save).
bool CopyWhole(const std::wstring& from, const std::wstring& to);
bool ReadSlotsFromFile(const std::wstring& path, std::vector<uint8_t>& out);  // the 10 slots of a save file

}  // namespace savefile
