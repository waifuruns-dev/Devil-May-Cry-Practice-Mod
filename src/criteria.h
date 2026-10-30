#pragma once
#include <string>

// DMC1 mission rank criteria (source: dmc.wiki/dmc1:rank). Targets are the same on every
// difficulty; only the rank thresholds are lower on Dante Must Die.
namespace criteria {

struct Mission {
    int timeSec = 0;   // target clear time
    int orbs = 0;      // target red orbs found
};

// Loads overrides from a CSV (mission,time m:ss,orbs). Missing file = built-in table.
void Load(const std::wstring& path);
bool Get(int mission, Mission& out);

// Points for each category, and what it takes to reach the next bracket.
struct Score {
    int timePts = 0, orbPts = 0, damagePts = 0, penalty = 0, bonus = 0, total = 0;
    char rank = '-';
    int timeLeftSec = -1;      // seconds left in the current time bracket (-1 = last bracket)
    int orbsForNext = -1;      // orbs still needed for the next orb bracket (-1 = already max)
    int nextOrbPts = 0;
    int damageToDrop = -1;     // damage that would drop the damage bracket (-1 = already 0 pts)
    int pointsToNextRank = -1; // points needed for the next rank (-1 = already S)
    char nextRank = 0;
};

Score Compute(const Mission& m, double frames, double ticksPerSecond, int orbs, int damage, int penalty,
              int bonus, bool dmd);

}  // namespace criteria
