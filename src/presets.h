#pragma once
#include "builder.h"

#include <string>
#include <vector>

struct PresetCategory {
    std::string name;
    std::vector<Preset> presets;  // sorted by mission
};

namespace presets {

// Reads every <root>/<category>/*.json. Bad files are skipped and logged.
std::vector<PresetCategory> LoadAll(const std::wstring& root);

// Writes a preset as JSON (the same format LoadAll reads). An existing file is kept as <file>.bak.
bool Save(const Preset& p, const std::wstring& path, std::string& error);

// presets\<CaptureCategory>\Mxx.json, or "Mxx - <name>.json" for a named preset (creates the folders).
std::wstring UserPresetPath(int mission, const std::string& name = "");

}  // namespace presets
