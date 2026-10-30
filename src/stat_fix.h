#pragma once

// After a mission starts, makes Max HP / Max DT match the bar lengths the save loaded.
// Loading a different save without restarting leaves the old maximums in memory otherwise.
namespace stat_fix {

void Init();

// missionStartSerial goes up by one each time a mission starts.
void Update(int missionStartSerial);

}  // namespace stat_fix
