#include "presets.h"
#include "log.h"
#include "config.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>

#include <nlohmann/json.hpp>

namespace {

using json = nlohmann::json;

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

bool ReadFile(const std::wstring& path, std::string& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    char buf[4096];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

int ParseDifficulty(const std::string& s) {
    std::string k = s;
    std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (k == "easy") return 2;
    if (k == "normal") return 3;
    if (k == "hard") return 5;
    if (k == "dmd" || k == "dante must die") return 6;
    return -1;
}

int ParseCostume(const std::string& s) {
    std::string k = s;
    std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (k == "dante") return 0;
    if (k == "ldk" || k == "legendary dark knight") return 1;
    if (k == "super" || k == "super dante") return 2;
    return -1;
}

bool ParsePreset(const std::string& text, Preset& p, std::string& error) {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        error = std::string("not valid JSON: ") + e.what();
        return false;
    }
    try {
        p.mission = j.at("mission").get<int>();
        p.name = j.value("name", "Mission " + std::to_string(p.mission));
        p.difficulty = ParseDifficulty(j.value("difficulty", "normal"));
        if (p.difficulty < 0) { error = "unknown difficulty"; return false; }
        p.costume = ParseCostume(j.value("costume", "dante"));
        if (p.costume < 0) { error = "unknown costume"; return false; }
        p.orbs = j.value("orbs", 0u);
        p.hpBars = j.value("hp_bars", -1);
        p.dtBars = j.value("dt_bars", -1);
        if (j.contains("dt_gauge"))
            p.dtGauge = j["dt_gauge"].is_number() ? std::to_string(j["dt_gauge"].get<int>())
                                                  : j["dt_gauge"].get<std::string>();
        if (j.contains("moves"))
            for (const auto& m : j["moves"]) p.moves.push_back(m.get<std::string>());
        if (j.contains("items"))
            for (auto it = j["items"].begin(); it != j["items"].end(); ++it) {
                int id;
                if (!builder::ItemId(it.key(), id)) { error = "unknown item '" + it.key() + "'"; return false; }
                p.items[id] = it.value().get<int>();
            }
        if (j.contains("extra_weapons"))
            for (const auto& w : j["extra_weapons"]) {
                int id;
                if (!builder::WeaponId(w.get<std::string>(), id)) { error = "unknown weapon '" + w.get<std::string>() + "'"; return false; }
                p.extraWeapons.push_back(id);
            }
        if (j.contains("equip")) {
            p.devilArm = j["equip"].value("devil_arm", "");
            p.gun = j["equip"].value("gun", "");
        }
        auto core = [&](const char* key, int& out) -> bool {
            if (!j.contains(key)) return true;
            std::string v = j[key].get<std::string>();
            for (char& c : v) c = (char)tolower((unsigned char)c);
            if (v == "blue") out = 0;
            else if (v == "green") out = 1;
            else if (v == "red") out = 2;
            else { error = std::string(key) + " must be blue, green or red"; return false; }
            return true;
        };
        if (!core("top_core", p.topCore) || !core("bottom_core", p.bottomCore)) return false;
        if (j.contains("save_set"))  // {"1c5": 0, ...}: hex save offset -> exact byte (research)
            for (auto it = j["save_set"].begin(); it != j["save_set"].end(); ++it) {
                const int off = (int)std::stoul(it.key(), nullptr, 16);
                if (off < 0x30 || off >= 0x940) { error = "save_set offset out of range"; return false; }
                p.saveSet[off] = it.value().get<int>() & 0xFF;
            }
        if (j.contains("save_or"))  // {"1c2": 128, ...}: hex save offset -> bits to set (research)
            for (auto it = j["save_or"].begin(); it != j["save_or"].end(); ++it) {
                const int off = (int)std::stoul(it.key(), nullptr, 16);
                if (off < 0x30 || off >= 0x940) { error = "save_or offset out of range"; return false; }
                p.saveOr[off] = it.value().get<int>() & 0xFF;
            }
    } catch (const std::exception& e) {
        error = std::string("bad field: ") + e.what();
        return false;
    }
    // Build once now so mistakes show up in the log, not when you press Load.
    std::vector<uint8_t> test;
    return builder::Build(p, test, error);
}

}  // namespace

std::vector<PresetCategory> presets::LoadAll(const std::wstring& root) {
    std::vector<PresetCategory> out;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        dlog::Write("[Presets] folder not found: %ls", root.c_str());
        return out;
    }
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        PresetCategory cat;
        cat.name = Narrow(fd.cFileName);
        const std::wstring dir = root + L"\\" + fd.cFileName;

        WIN32_FIND_DATAW ff;
        HANDLE hf = FindFirstFileW((dir + L"\\*.json").c_str(), &ff);
        if (hf != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring path = dir + L"\\" + ff.cFileName;
                std::string text, error;
                Preset p;
                p.category = cat.name;
                p.file = Narrow(path);
                p.modified = ((uint64_t)ff.ftLastWriteTime.dwHighDateTime << 32) | ff.ftLastWriteTime.dwLowDateTime;
                if (!ReadFile(path, text)) continue;
                if (ParsePreset(text, p, error)) cat.presets.push_back(p);
                else dlog::Write("[Presets] skipped %s: %s", p.file.c_str(), error.c_str());
            } while (FindNextFileW(hf, &ff));
            FindClose(hf);
        }
        std::sort(cat.presets.begin(), cat.presets.end(),
                  [](const Preset& a, const Preset& b) { return a.mission < b.mission; });
        if (!cat.presets.empty()) out.push_back(cat);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(out.begin(), out.end(), [](const PresetCategory& a, const PresetCategory& b) { return a.name < b.name; });
    size_t total = 0;
    for (const auto& c : out) total += c.presets.size();
    dlog::Write("[Presets] %zu categories, %zu presets", out.size(), total);
    return out;
}

namespace {
const char* kWeaponNames[10] = {"Force Edge", "Shotgun", "Needlegun", "Grenadegun", "Nightmare-Beta",
                                "Handguns",   "Alastor", "Ifrit",     "Sparda",     "Yamato"};
const char* ItemName(int id) {
    switch (id) {
        case 0x0A: return "Vital Star";
        case 0x0B: return "Devil Star";
        case 0x0C: return "Untouchable";
        case 0x0D: return "Holy Water";
    }
    return nullptr;
}
}  // namespace

bool presets::Save(const Preset& p, const std::wstring& path, std::string& error) {
    nlohmann::ordered_json j;
    j["name"] = p.name;
    j["mission"] = p.mission;
    j["difficulty"] = p.difficulty == 2 ? "easy" : p.difficulty == 5 ? "hard" : p.difficulty == 6 ? "dmd" : "normal";
    j["costume"] = p.costume == 1 ? "ldk" : p.costume == 2 ? "super" : "dante";
    j["orbs"] = p.orbs;
    if (p.hpBars >= 0) j["hp_bars"] = p.hpBars;
    if (p.dtBars >= 0) j["dt_bars"] = p.dtBars;
    bool numeric = !p.dtGauge.empty() && std::isdigit((unsigned char)p.dtGauge[0]);
    if (numeric) j["dt_gauge"] = std::stoi(p.dtGauge);
    else j["dt_gauge"] = p.dtGauge;
    j["moves"] = p.moves;
    nlohmann::ordered_json items = nlohmann::ordered_json::object();
    for (const auto& kv : p.items)
        if (kv.second > 0 && ItemName(kv.first)) items[ItemName(kv.first)] = kv.second;
    j["items"] = items;
    nlohmann::ordered_json weapons = nlohmann::ordered_json::array();
    for (int w : p.extraWeapons)
        if (w >= 0 && w < 10) weapons.push_back(kWeaponNames[w]);
    j["extra_weapons"] = weapons;
    nlohmann::ordered_json eq = nlohmann::ordered_json::object();
    if (!p.devilArm.empty()) eq["devil_arm"] = p.devilArm;
    if (!p.gun.empty()) eq["gun"] = p.gun;
    j["equip"] = eq;
    static const char* kCoreNames[3] = {"blue", "green", "red"};
    if (p.mission >= builder::kFirstCoreMission) {
        if (p.topCore >= 0 && p.topCore <= 2) j["top_core"] = kCoreNames[p.topCore];
        if (p.bottomCore >= 0 && p.bottomCore <= 2) j["bottom_core"] = kCoreNames[p.bottomCore];
    }
    if (!p.saveSet.empty()) {
        nlohmann::ordered_json ss = nlohmann::ordered_json::object();
        for (const auto& kv : p.saveSet) {
            char key[8];
            snprintf(key, sizeof(key), "%x", kv.first);
            ss[key] = kv.second;
        }
        j["save_set"] = ss;
    }
    if (!p.saveOr.empty()) {
        nlohmann::ordered_json so = nlohmann::ordered_json::object();
        for (const auto& kv : p.saveOr) {
            char key[8];
            snprintf(key, sizeof(key), "%x", kv.first);
            so[key] = kv.second;
        }
        j["save_or"] = so;
    }

    const std::string text = j.dump(2) + "\n";
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        CopyFileW(path.c_str(), (path + L".bak").c_str(), FALSE);
    const std::wstring tmp = path + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) { error = "couldn't create the file"; return false; }
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    fclose(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        error = "couldn't write the file";
        return false;
    }
    dlog::Write("[Presets] saved mission %d to %s", p.mission, Narrow(path).c_str());
    return true;
}

std::wstring presets::UserPresetPath(int mission, const std::string& name) {
    const std::wstring root = g_config.dataFolder + g_config.presetFolder;
    const std::wstring dir = root + L"\\" + g_config.captureCategory;
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    // Mxx.json for unnamed presets (F7 capture, "Mission N (custom)"), "Mxx - Name.json" for named
    // ones, so several named setups for one mission can live side by side.
    std::string clean;
    for (char c : name) {
        if (strchr("\\/:*?\"<>|", c) || (unsigned char)c < 32) continue;
        clean += c;
    }
    while (!clean.empty() && (clean.back() == ' ' || clean.back() == '.')) clean.pop_back();
    // The automatic names ("Mission 5 (custom)", "Mission 5 (captured)") count as unnamed.
    const std::string m = "Mission " + std::to_string(mission);
    const bool unnamed = clean.empty() || clean == m + " (custom)" || clean == m + " (captured)";
    wchar_t prefix[16];
    swprintf(prefix, 16, L"M%02d", mission);
    if (unnamed) return dir + L"\\" + prefix + L".json";
    int n = MultiByteToWideChar(CP_UTF8, 0, clean.c_str(), (int)clean.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, clean.c_str(), (int)clean.size(), &w[0], n);
    return dir + L"\\" + prefix + L" - " + w + L".json";
}
