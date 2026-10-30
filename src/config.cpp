#include "config.h"
#include "log.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

Config g_config;

namespace {

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

bool ParseType(const std::string& s, ValueType& out) {
    std::string t = Lower(s);
    if (t == "u8" || t == "byte")  { out = ValueType::U8;  return true; }
    if (t == "i8")                 { out = ValueType::I8;  return true; }
    if (t == "u16")                { out = ValueType::U16; return true; }
    if (t == "i16" || t == "2")    { out = ValueType::I16; return true; }
    if (t == "u32")                { out = ValueType::U32; return true; }
    if (t == "i32" || t == "4")    { out = ValueType::I32; return true; }
    if (t == "f32" || t == "float"){ out = ValueType::F32; return true; }
    if (t == "text")               { out = ValueType::Text; return true; }
    return false;
}

bool ParseFormat(const std::string& s, Format& out) {
    std::string f = Lower(s);
    if (f == "int" || f.empty()) { out = Format::Int;   return true; }
    if (f == "time")             { out = Format::Time;  return true; }
    if (f == "float")            { out = Format::Float; return true; }
    if (f == "hex")              { out = Format::Hex;   return true; }
    return false;
}

int ParseInt(const std::string& s, int fallback) {
    try { return std::stoi(s, nullptr, 0); } catch (...) { return fallback; }
}

double ParseDouble(const std::string& s, double fallback) {
    try { return std::stod(s); } catch (...) { return fallback; }
}

}  // namespace

bool LoadConfig(const std::wstring& path, Config& out) {
    // Read the whole file (via _wfopen so non-ASCII install paths work everywhere).
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    std::string contents;
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) contents.append(chunk, n);
    fclose(f);
    std::istringstream file(contents);

    std::string section, line;
    int lineNo = 0;
    while (std::getline(file, line)) {
        ++lineNo;
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;

        if (line.front() == '[' && line.back() == ']') {
            section = Lower(line.substr(1, line.size() - 2));
            continue;
        }

        if (section == "overlay") {
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = Lower(Trim(line.substr(0, eq)));
            std::string val = Trim(line.substr(eq + 1));

            if (key == "targetexe")            out.targetExe = Widen(val);
            else if (key == "togglekey")       out.toggleKey = ParseInt(val, out.toggleKey);
            else if (key == "modkey")          out.modKey = ParseInt(val, out.modKey);
            else if (key == "modonatstart")    out.modOnAtStart = ParseInt(val, 0) != 0;
            else if (key == "showonstart")     out.showOnStart = ParseInt(val, 1) != 0;
            else if (key == "anchor") {
                std::string a = Lower(val);
                if (a == "topleft") out.anchor = 0;
                else if (a == "topright") out.anchor = 1;
                else if (a == "bottomleft") out.anchor = 2;
                else if (a == "bottomright") out.anchor = 3;
                else dlog::Write("INI line %d: unknown Anchor '%s'", lineNo, val.c_str());
            }
            else if (key == "posx")            out.posX = (float)ParseDouble(val, out.posX);
            else if (key == "posy")            out.posY = (float)ParseDouble(val, out.posY);
            else if (key == "fontscale")       out.fontScale = (float)ParseDouble(val, out.fontScale);
            else if (key == "backgroundalpha") out.backgroundAlpha = (float)ParseDouble(val, out.backgroundAlpha);
            else if (key == "tickspersecond")  out.ticksPerSecond = ParseDouble(val, out.ticksPerSecond);
            else if (key == "timerfont")       out.timerFont = val;
            else if (key == "font")            out.font = val;
            else if (key == "timersize")       out.timerSize = (float)ParseDouble(val, out.timerSize);
            else if (key == "roomtimersize")   out.roomTimerSize = (float)ParseDouble(val, out.roomTimerSize);
            else if (key == "timerposy")       out.timerPosY = (float)ParseDouble(val, out.timerPosY);
            else if (key == "timerbackgroundalpha")
                out.timerBackgroundAlpha = (float)ParseDouble(val, out.timerBackgroundAlpha);
            else if (key == "timerbordersize") out.timerBorderSize = (float)ParseDouble(val, out.timerBorderSize);
            else if (key == "timershadow")     out.timerShadow = ParseDouble(val, 1) != 0;
            else if (key == "uiscale") {
                std::string v = val;
                for (char& c : v) c = (char)tolower((unsigned char)c);
                out.uiScale = (v.empty() || v == "auto") ? 0.0f : (float)ParseDouble(val, 0.0);
            }
            else if (key == "showkeyhints")    out.showKeyHints = ParseDouble(val, 1) != 0;
            else if (key == "chainload" && !val.empty()) out.chainLoad.push_back(Widen(val));
        } else if (section == "practice") {
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = Lower(Trim(line.substr(0, eq)));
            std::string val = Trim(line.substr(eq + 1));

            if (key == "category")    out.category = val.empty() ? "--" : val;
            else if (key == "showpb") out.showPb = ParseInt(val, 1) != 0;
            else if (key == "pbsize") out.pbSize = (float)ParseDouble(val, out.pbSize);
        } else if (section == "presets") {
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = Lower(Trim(line.substr(0, eq)));
            std::string val = Trim(line.substr(eq + 1));

            if (key == "enabled")           out.presetsEnabled = ParseInt(val, 1) != 0;
            else if (key == "menukey")      out.menuKey = ParseInt(val, out.menuKey);
            else if (key == "presetfolder" && !val.empty()) out.presetFolder = Widen(val);
            else if (key == "capturekey")   out.captureKey = ParseInt(val, out.captureKey);
            else if (key == "capturecategory" && !val.empty()) out.captureCategory = Widen(val);
            else if (key == "savefile")     out.saveFile = Widen(val);
            else if (key == "practiceslot") out.practiceSlot = ParseInt(val, out.practiceSlot);
        } else if (section == "reset") {
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = Lower(Trim(line.substr(0, eq)));
            std::string val = Trim(line.substr(eq + 1));
            if (key == "enabled")       out.resetEnabled = ParseInt(val, 1) != 0;
            else if (key == "resetkey") out.resetKey = ParseInt(val, out.resetKey);
            else if (key == "roomresetkey") out.roomResetKey = ParseInt(val, out.roomResetKey);
        } else if (section == "criteria") {
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = Lower(Trim(line.substr(0, eq)));
            std::string val = Trim(line.substr(eq + 1));
            if (key == "enabled") out.criteriaEnabled = ParseInt(val, 1) != 0;
            else if (key == "showhints") out.criteriaHints = ParseInt(val, 1) != 0;
        } else if (section == "debug") {
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = Lower(Trim(line.substr(0, eq)));
            std::string val = Trim(line.substr(eq + 1));
            if (key == "dumpkey") out.dumpKey = ParseInt(val, out.dumpKey);
            else if (key == "debugreloadkey") out.debugReloadKey = ParseInt(val, out.debugReloadKey);
            else if (key == "verboselog") out.verboseLog = ParseInt(val, 0) != 0;
        } else if (section == "statfix") {
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = Lower(Trim(line.substr(0, eq)));
            std::string val = Trim(line.substr(eq + 1));

            if (key == "enabled")        out.statFix = ParseInt(val, 1) != 0;
            else if (key == "hpbar")     out.hpBarAddr = val;
            else if (key == "dtbar")     out.dtBarAddr = val;
            else if (key == "maxhp")     out.maxHpAddr = val;
            else if (key == "maxdt")     out.maxDtAddr = val;
            else if (key == "maxgauge")  out.maxGaugeAddr = val;
            else if (key == "hp")        out.hpAddr = val;
            else if (key == "dt")        out.dtAddr = val;
        } else if (section == "missiontimer") {
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = Lower(Trim(line.substr(0, eq)));
            std::string val = Trim(line.substr(eq + 1));

            if (key == "enabled")         out.missionTimer.enabled = ParseInt(val, 0) != 0;
            else if (key == "counter")    out.missionTimer.counter = val;
            else if (key == "startframe") out.missionTimer.startFrame = val;
            else if (key == "finaltime")  out.missionTimer.finalTime = val;
            else if (key == "roomtimer")  out.missionTimer.roomTimer = val;
            else if (key == "missionnumber") out.missionTimer.missionNumber = val;
            else if (key == "activeflag") out.missionTimer.activeFlag = val;
            else if (key == "activetype") {
                if (!ParseType(val, out.missionTimer.activeType))
                    dlog::Write("INI line %d: unknown ActiveType '%s'", lineNo, val.c_str());
            } else if (key == "activevalue" && !val.empty()) {
                out.missionTimer.hasActiveValue = true;
                out.missionTimer.activeValue = ParseInt(val, 0);
            } else if (key == "inactivevalue" && !val.empty()) {
                out.missionTimer.hasInactiveValue = true;
                out.missionTimer.inactiveValue = ParseInt(val, 0);
            }
        } else if (section == "watch") {
            // Group | Name | Address | Type | Format
            std::vector<std::string> parts;
            std::stringstream ss(line);
            std::string part;
            while (std::getline(ss, part, '|')) parts.push_back(Trim(part));
            if (parts.size() < 4) {
                dlog::Write("INI line %d: expected 'Group | Name | Address | Type | Format'", lineNo);
                continue;
            }
            WatchDef w;
            w.group = parts[0];
            w.name = parts[1];
            w.address = parts[2];
            if (!ParseType(parts[3], w.type)) {
                dlog::Write("INI line %d: unknown type '%s'", lineNo, parts[3].c_str());
                continue;
            }
            if (parts.size() >= 5 && !ParseFormat(parts[4], w.format)) {
                dlog::Write("INI line %d: unknown format '%s'", lineNo, parts[4].c_str());
                continue;
            }
            out.watches.push_back(w);
        }
    }
    return true;
}
