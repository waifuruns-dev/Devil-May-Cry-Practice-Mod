#pragma once
#include <string>

// Personal bests, one per (category, mission), saved to a small text file.
namespace pb {

void Load(const std::wstring& path);

// Best time in frames for this category + mission. False if none recorded yet.
bool Get(const std::string& category, int mission, double& frames);

// Records a finished run. Saves to disk if it beats the old best (or is the first).
// `previous` gets the old best (or -1 if there was none). Returns true for a new PB.
bool Submit(const std::string& category, int mission, double frames, double& previous);

}  // namespace pb
