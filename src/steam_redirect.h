#pragma once
#include <string>
#include <vector>

// DMC1 reads and writes dmc1.sav through Steam (ISteamRemoteStorage, Steam Cloud). While the
// redirect is on, the game's reads and writes of dmc1.sav use the practice file next to the mod
// instead (Load Game reads the file each time), and Steam's own copy (your real save) isn't changed.
namespace steam_redirect {

// Hooks Steam's remote storage once the game has started Steam (waits for it; logs the result).
void Install(const std::wstring& practiceFile);

void SetActive(bool on);  // redirect dmc1.sav writes to the practice file
bool Active();
bool Installed();

// Your real dmc1.sav through Steam, bypassing the redirect (export to the real save).
bool ReadRealSave(std::vector<uint8_t>& out);
bool WriteRealSave(const std::vector<uint8_t>& data);

}  // namespace steam_redirect
