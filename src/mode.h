#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The mod as a whole on or off (F10; off when the game starts).
//
// Off: no overlay, no keys (except F10), nothing written to the game: plain DMC1.
// On: the game's save writes go to a practice file next to the mod (dmc1_practice.sav, a copy of
// your save at that moment) instead of your real dmc1.sav, and the presets write there too.
// Turning it off again puts your own save back in the game's memory; in a mission that waits for
// the title screen (the practice save stays loaded, and keeps saving to the practice file, until
// then).
namespace mode {

void Init(const std::wstring& folder);
void Update(bool focused);  // every frame
bool On();
std::wstring SavePath();    // the file the presets write: the practice file
// A slot exported to your real save: the copy the mod puts back when it turns off gets it too.
void RealSlotChanged(int slot, const std::vector<uint8_t>& data);
bool WantsDraw();
void Draw();                // short on-screen messages

}  // namespace mode
