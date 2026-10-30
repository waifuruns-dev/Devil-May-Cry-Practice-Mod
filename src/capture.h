#pragma once

// Preset capture: press CaptureKey in a mission to save your current loadout (difficulty, costume,
// orbs, HP/DT bars, DT gauge, moves, consumables, weapons, equipped weapons) as a preset for the
// mission you're in: presets\<CaptureCategory>\Mxx.json. Press it at a mission start to record a
// route's loadout mission by mission. An existing file is kept as Mxx.json.bak.
namespace capture {

void Update(bool gameFocused);

}  // namespace capture
