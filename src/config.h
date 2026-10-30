#pragma once
#include <string>
#include <vector>

// Text = not read from memory; the Address column is shown as-is (e.g. a category name).
enum class ValueType { U8, I8, U16, I16, U32, I32, F32, Text };
enum class Format { Int, Time, Float, Hex };

// One line of the [Watch] section: something to read from memory and show.
struct WatchDef {
    std::string group;
    std::string name;
    std::string address;  // e.g. "00102510", "dmc1.exe+1A2B40", "dmc1.exe+1A2B40 -> 10 -> 4"
    ValueType type = ValueType::I32;
    Format format = Format::Int;
};

// The live mission timer: starts when the game writes a new start frame,
// stops when the game writes the final mission time.
struct MissionTimerDef {
    bool enabled = false;
    std::string counter;     // frame counter that keeps running (Global Timer)
    std::string startFrame;  // frame the current mission started on
    std::string finalTime;   // written by the game at the results screen
    std::string roomTimer;   // optional: game's room timer, used only to detect entering a new room
    std::string missionNumber;  // optional: current mission number, used to file personal bests

    // Optional "are we in a mission" flag. When it says no while the timer is running
    // (e.g. quit to title), the timer stops and goes gray.
    std::string activeFlag;
    ValueType activeType = ValueType::U8;
    bool hasActiveValue = false;   int activeValue = 0;    // in mission when flag == this
    bool hasInactiveValue = false; int inactiveValue = 0;  // NOT in mission when flag == this
};

struct Config {
    std::wstring targetExe = L"dmc1.exe";  // overlay only runs inside this exe
    int toggleKey = 0x70;                  // F1
    int modKey = 0x79;                     // F10: the whole mod on / off
    bool modOnAtStart = false;
    bool showOnStart = true;
    int anchor = 2;                        // 0 TopLeft, 1 TopRight, 2 BottomLeft, 3 BottomRight
    float posX = 20.0f;                    // distance from the anchored screen edge
    float posY = 20.0f;
    float fontScale = 1.5f;
    float backgroundAlpha = 0.55f;
    double ticksPerSecond = 60.0;          // how fast "time" values count

    // Big timers at the top center of the screen.
    std::string font;                      // font for all text (.ttf / .otf); empty = fonts\EBGaramond-Regular.ttf
    std::string timerFont;                 // .ttf path; empty = Segoe UI Semibold from Windows
    float timerSize = 88.0f;               // mission timer height in pixels
    float roomTimerSize = 48.0f;           // room timer height in pixels
    float timerPosY = 16.0f;               // gap from the top of the screen
    float timerBackgroundAlpha = 0.55f;    // 0 = no box, just text with a shadow
    float timerBorderSize = 2.0f;          // red border around the timer box, px; 0 = none
    bool timerShadow = true;
    bool showKeyHints = true;
    float uiScale = 0.0f;                  // 0 = auto (game height / 1440), else a fixed multiplier              // key reminders at the bottom of the stats panel               // drop shadow under the timer box
    std::vector<std::wstring> chainLoad;   // other mod DLLs to load (e.g. the camera mod)
    // Practice settings. Category is what personal bests are filed under; presets will set it later.
    std::string category = "--";
    bool showPb = true;
    float pbSize = 34.0f;                  // PB / difference line under the room timer, in pixels
    std::wstring dataFolder;               // folder of dinput8.dll; set at startup, not from the INI

    // Preset loader.
    bool presetsEnabled = true;
    int menuKey = 0x71;                    // F2
    std::wstring presetFolder = L"presets";  // relative to the DLL's folder
    int captureKey = 0x76;                 // F7: save the current loadout as a preset
    std::wstring captureCategory = L"My Presets";
    std::wstring saveFile;                 // empty = find it through Steam
    int practiceSlot = 10;

    // After a mission starts, set Max HP / Max DT from the bar lengths (fixes hot-swap leftovers).
    bool statFix = true;
    std::string hpBarAddr = "@Session -> 2464";    // u8, HP bar length
    std::string dtBarAddr = "@Session -> 2465";    // u8, DT bar length
    std::string maxHpAddr = "@Actor -> 1BC8";    // u16
    std::string maxDtAddr = "@Actor -> 1BD4";    // u16, DT time max (200 per rune)
    std::string maxGaugeAddr = "@Actor -> 1BD0"; // u16, DT gauge max (120 per rune)
    std::string hpAddr = "@Actor -> 17A2";       // u16, current HP
    std::string dtAddr = "@Actor -> 1BCE";       // u16, current DT gauge (120 per rune)

    // Instant reset: restarts the current mission from how it began (or from the last preset).
    bool resetEnabled = true;
    int resetKey = 0x72;                   // F3
    int roomResetKey = 0x73;               // F4, restart the current room (0 = off)

    // Debug: memory dump key (0 = off).
    int dumpKey = 0;                       // research memory dump key (0 = off; F5 = 0x74)
    bool verboseLog = false;               // detailed tracing in the log
    int debugReloadKey = 0;                // DDMK-style plain room reload (test; 0 = off)

    // Rank criteria section in the stats panel.
    bool criteriaEnabled = true;
    bool criteriaHints = true;

    MissionTimerDef missionTimer;
    std::vector<WatchDef> watches;
};

// Loads the INI. Returns false if the file couldn't be opened (defaults are kept).
bool LoadConfig(const std::wstring& path, Config& out);

extern Config g_config;
