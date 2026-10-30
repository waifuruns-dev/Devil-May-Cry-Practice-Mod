#include "overlay.h"
#include "mode.h"
#include "config.h"
#include "log.h"
#include "watch.h"
#include "pb.h"
#include "menu.h"
#include "stat_fix.h"
#include "reset.h"
#include "dump.h"
#include "criteria.h"
#include "capture.h"
#include "game_addrs.h"

#include <windows.h>
#include <algorithm>
#include <cfloat>
#include <imgui.h>
#include <string>
#include <vector>

namespace {

struct Term {
    AddressExpr addr;
    double sign = 1.0;  // +1 or -1
};

struct Entry {
    const WatchDef* def;
    std::vector<Term> terms;  // value shown = sum of signed terms
};

std::vector<Entry> g_entries;

bool ReadEntry(const Entry& e, double& value) {
    if (e.terms.empty()) return false;
    value = 0;
    for (const Term& t : e.terms) {
        double v = 0;
        if (!ReadWatch(t.addr, e.def->type, v)) return false;
        value += t.sign * v;
    }
    return true;
}

// Splits "A + B - C" into signed terms. Operators need a space on both sides, so
// "dmc1.exe+1234" and pointer hops like "-> -10" are left alone.
bool ParseTerms(const std::string& text, std::vector<Term>& out, const std::string& name) {
    out.clear();
    double sign = 1.0;
    size_t start = 0;
    for (;;) {
        size_t plus = text.find(" + ", start);
        size_t minus = text.find(" - ", start);
        size_t op = std::min(plus, minus);

        std::string piece = text.substr(start, op == std::string::npos ? std::string::npos : op - start);
        Term t;
        t.sign = sign;
        if (!ParseAddress(piece, t.addr)) {
            dlog::Write("Couldn't parse address '%s' for '%s'", piece.c_str(), name.c_str());
            return false;
        }
        out.push_back(t);

        if (op == std::string::npos) break;
        sign = (op == minus) ? -1.0 : 1.0;
        start = op + 3;
    }
    return true;
}

// ---- Mission timer --------------------------------------------------------
// Idle:     nothing started since launch (shows 0:00.00).
// Running:  the game wrote a new Start Frame -> show Counter - Start Frame.
// Finished: the game wrote a non-zero Final Time -> freeze on that exact value.
enum class TimerState { Idle, Running, Finished };

struct MissionTimer {
    bool ready = false;      // addresses parsed OK
    bool primed = false;     // first readings taken (launch values are ignored)
    AddressExpr counter, startFrame, finalTime;
    bool hasFlag = false;    // optional in-mission flag configured
    AddressExpr flag;
    TimerState state = TimerState::Idle;
    double lastStart = 0, lastFinal = 0;
    double shown = 0;        // frames to display

    // Room time. The game's room timer stops while paused, so it's only used to
    // spot room changes (it drops back toward 0); the time shown is counted from
    // the frame counter, which keeps running through pauses like the mission timer.
    bool hasRoom = false;
    AddressExpr roomTimer;
    double lastRoomRaw = 0;
    double roomStart = 0;    // counter value when the current room was entered
    double roomShown = 0;

    // Personal bests. The mission number is tracked while the run is going, and the
    // value from the last running frame is used when it finishes (in case the game
    // moves it on to the next mission right at the results screen).
    bool hasMission = false;
    AddressExpr missionAddr;
    int runMission = -1;     // mission of the current/last run, -1 = unknown
    bool practice = false;   // a room restart happened during this run: no PB
    bool abandoned = false;  // restart/preset load in progress: ignore final-time changes until the next start
    enum class Result { None, NewPb, Slower, Practice } result = Result::None;
    double resultDelta = 0;  // frames vs previous PB (negative = faster)

    // Rank criteria inputs, frozen when the results screen comes up: pressing on through it adds
    // the mission's bonus orbs to the orb counter, which would change the grade afterwards.
    struct CritSample { int orbs, damage, penalty, bonus; };
    bool critFrozen = false;
    CritSample crit{};
} g_timer;

int ReadI32(uintptr_t a);
int ReadI16(uintptr_t a);

int g_missionStarts = 0;  // goes up each time a mission starts (used by the HP/DT fix)

void InitTimer() {
    const MissionTimerDef& d = g_config.missionTimer;
    g_timer = MissionTimer{};
    if (!d.enabled) return;
    g_timer.ready = ParseAddress(d.counter, g_timer.counter) &&
                    ParseAddress(d.startFrame, g_timer.startFrame) &&
                    ParseAddress(d.finalTime, g_timer.finalTime);
    if (!g_timer.ready) dlog::Write("[MissionTimer] couldn't parse one of Counter / StartFrame / FinalTime");

    if (!d.activeFlag.empty()) {
        if (!d.hasActiveValue && !d.hasInactiveValue)
            dlog::Write("[MissionTimer] ActiveFlag set but no ActiveValue/InactiveValue; ignoring it");
        else if (!ParseAddress(d.activeFlag, g_timer.flag))
            dlog::Write("[MissionTimer] couldn't parse ActiveFlag '%s'", d.activeFlag.c_str());
        else
            g_timer.hasFlag = true;
    }

    if (!d.roomTimer.empty()) {
        g_timer.hasRoom = ParseAddress(d.roomTimer, g_timer.roomTimer);
        if (!g_timer.hasRoom) dlog::Write("[MissionTimer] couldn't parse RoomTimer '%s'", d.roomTimer.c_str());
    }

    if (!d.missionNumber.empty()) {
        g_timer.hasMission = ParseAddress(d.missionNumber, g_timer.missionAddr);
        if (!g_timer.hasMission)
            dlog::Write("[MissionTimer] couldn't parse MissionNumber '%s'", d.missionNumber.c_str());
    }

    pb::Load(g_config.dataFolder + L"dmc1_practice_pbs.txt");
}

// True unless the optional flag says we're out of a mission (title screen etc).
bool InMission() {
    if (!g_timer.hasFlag) return true;
    const MissionTimerDef& d = g_config.missionTimer;
    double v;
    if (!ReadWatch(g_timer.flag, d.activeType, v)) return true;  // can't tell: don't stop the timer
    int value = (int)v;
    if (d.hasActiveValue) return value == d.activeValue;
    return value != d.inactiveValue;
}

void UpdateTimer() {
    MissionTimer& t = g_timer;
    if (!t.ready) return;

    double counter, start, final;
    if (!ReadWatch(t.counter, ValueType::I32, counter) ||
        !ReadWatch(t.startFrame, ValueType::I32, start) ||
        !ReadWatch(t.finalTime, ValueType::I32, final))
        return;

    double roomRaw = 0;
    bool roomOk = t.hasRoom && ReadWatch(t.roomTimer, ValueType::I32, roomRaw);

    // Whatever is in memory at launch is leftover; only react to changes after that.
    if (!t.primed) {
        t.lastStart = start;
        t.lastFinal = final;
        t.lastRoomRaw = roomRaw;
        t.roomStart = counter;
        t.primed = true;
        return;
    }

    if (start != t.lastStart) {
        // New mission began.
        t.state = TimerState::Running;
        t.lastStart = start;
        t.roomStart = counter;
        t.result = MissionTimer::Result::None;
        t.runMission = -1;
        t.practice = false;
        t.abandoned = false;
        t.critFrozen = false;
        ++g_missionStarts;
        dlog::Write("[MissionTimer] mission started (start frame %.0f)", start);
    }

    if (roomOk) {
        if (roomRaw < t.lastRoomRaw) {
            // Room timer went back down: new room. Back-date the start by however far
            // the game's room timer has already counted, so both start from the door.
            t.roomStart = counter - roomRaw;
        }
        t.lastRoomRaw = roomRaw;
    }

    if (t.state == TimerState::Running && !t.abandoned && final != t.lastFinal && final != 0) {
        // Results screen: the game wrote its official time. Freeze on it.
        t.state = TimerState::Finished;
        t.shown = final;
        dlog::Write("[MissionTimer] mission finished (%.0f frames)", final);
        t.crit = {ReadI32(0x10284C), ReadI32(0x102848), ReadI16(0x102850), ReadI16(0x102880)};
        t.critFrozen = true;
        dlog::Write("[Criteria] frozen at the end: orbs %d, damage %d", t.crit.orbs, t.crit.damage);

        if (t.practice) {
            t.result = MissionTimer::Result::Practice;
            dlog::Write("[MissionTimer] room restart used this run: not counted for PBs");
        } else if (t.runMission >= 0) {
            double previous;
            bool isNew = pb::Submit(g_config.category, t.runMission, final, previous);
            t.resultDelta = previous >= 0 ? final - previous : 0;
            t.result = isNew ? MissionTimer::Result::NewPb : MissionTimer::Result::Slower;
        }
    }
    t.lastFinal = final;

    if (t.state == TimerState::Running && !InMission()) {
        // Left the mission without finishing (quit to title): stop, keep the last time in gray.
        t.state = TimerState::Idle;
        dlog::Write("[MissionTimer] left mission without finishing");
    }

    if (t.state == TimerState::Running) {
        double m;
        if (t.hasMission && ReadWatch(t.missionAddr, ValueType::U16, m)) t.runMission = (int)m;
        t.shown = counter - start > 0 ? counter - start : 0;
        t.roomShown = counter - t.roomStart > 0 ? counter - t.roomStart : 0;
    }
}

ImFont* g_timerFont = nullptr;
float g_uiScale = 1.0f;
inline float S(float v) { return v * g_uiScale; }

const ImGuiWindowFlags kOverlayFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings;

float CharWidth(ImFont* font, float size, char c) {
    char s[2] = {c, 0};
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, s).x;
}

// Widest digit at this size. Every digit gets a slot this wide, so the text
// width never changes as the time ticks (no left-right shaking).
float DigitSlot(ImFont* font, float size) {
    float w = 0;
    for (char c = '0'; c <= '9'; ++c) {
        float cw = CharWidth(font, size, c);
        if (cw > w) w = cw;
    }
    return w;
}

// Width of `text` when laid out with fixed-width digit slots.
float FixedWidth(ImFont* font, float size, const std::string& text) {
    const float slot = DigitSlot(font, size);
    float w = 0;
    for (char c : text) w += (c >= '0' && c <= '9') ? slot : CharWidth(font, size, c);
    return w;
}

// Draws one line of big text centered in `width`, digits in fixed slots, with a
// soft drop shadow so it stays readable over bright scenes without a background box.
void DrawCenteredLine(const std::string& text, float size, float width, ImU32 color) {
    ImFont* font = g_timerFont ? g_timerFont : ImGui::GetFont();
    const float slot = DigitSlot(font, size);
    const float total = FixedWidth(font, size, text);
    const float shadow = size * 0.035f > 1.0f ? size * 0.035f : 1.0f;

    ImVec2 origin = ImGui::GetCursorScreenPos();
    float x = origin.x + (width - total) * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    for (char c : text) {
        char s[2] = {c, 0};
        float cw = CharWidth(font, size, c);
        bool digit = c >= '0' && c <= '9';
        float cell = digit ? slot : cw;
        float cx = x + (cell - cw) * 0.5f;  // center each digit in its slot
        dl->AddText(font, size, ImVec2(cx + shadow, origin.y + shadow), IM_COL32(0, 0, 0, 170), s);
        dl->AddText(font, size, ImVec2(cx, origin.y), color, s);
        x += cell;
    }

    ImGui::Dummy(ImVec2(width, size));
}

// "-1.20", "+0.85", "+1:02.40": a signed difference in seconds for the PB line.
std::string FormatDelta(double frames) {
    const double tps = g_config.ticksPerSecond > 0 ? g_config.ticksPerSecond : 60.0;
    const char sign = frames < 0 ? '-' : '+';
    double secs = (frames < 0 ? -frames : frames) / tps;
    long long cs = (long long)(secs * 100.0 + 0.5);
    char buf[32];
    if (cs < 6000) snprintf(buf, sizeof(buf), "%c%lld.%02lld", sign, cs / 100, cs % 100);
    else snprintf(buf, sizeof(buf), "%c%lld:%02lld.%02lld", sign, cs / 6000, (cs / 100) % 60, cs % 100);
    return buf;
}

// Third line under the timers: PB for this mission while playing, and the
// result (new PB / slower) once the mission is finished.
// Text and color of the PB line; false if there's nothing to show.
bool PbLine(std::string& text, ImU32& color) {
    if (!g_config.showPb || g_timer.runMission < 0) return false;
    const MissionTimer& t = g_timer;
    color = IM_COL32(200, 200, 200, 230);
    if (t.state == TimerState::Finished && t.result == MissionTimer::Result::NewPb) {
        text = t.resultDelta < 0 ? FormatDelta(t.resultDelta) + "  NEW PB" : "NEW PB";
        color = IM_COL32(255, 205, 70, 255);   // gold
    } else if (t.state == TimerState::Finished && t.result == MissionTimer::Result::Slower) {
        text = FormatDelta(t.resultDelta);
        color = IM_COL32(235, 90, 90, 255);    // red
    } else if (t.state == TimerState::Finished && t.result == MissionTimer::Result::Practice) {
        text = "No PB: room restart used";
        color = IM_COL32(150, 150, 150, 230);
    } else {
        double best;
        if (!pb::Get(g_config.category, t.runMission, best)) return false;
        text = "PB " + FormatValue(best, ValueType::I32, Format::Time, g_config.ticksPerSecond);
        if (t.practice) {
            text += "  (practice)";
            color = IM_COL32(150, 150, 150, 230);
        }
    }
    return true;
}

// PB for the mission being played (or last played) in the current category, as text.
std::string CurrentPbText() {
    int mission = g_timer.runMission;
    double m;
    if (mission < 0 && g_timer.hasMission && ReadWatch(g_timer.missionAddr, ValueType::U16, m)) mission = (int)m;
    double best;
    if (mission <= 0 || !pb::Get(g_config.category, mission, best)) return "--";
    return FormatValue(best, ValueType::I32, Format::Time, g_config.ticksPerSecond);
}

// Rank criteria for the mission being played: live value, target, points, and what the next
// bracket needs. Values are read straight from the game's mission stats.
int ReadI32(uintptr_t a) { int32_t v = 0; SIZE_T n; ReadProcessMemory(GetCurrentProcess(), (LPCVOID)a, &v, 4, &n); return v; }
int ReadI16(uintptr_t a) { int16_t v = 0; SIZE_T n; ReadProcessMemory(GetCurrentProcess(), (LPCVOID)a, &v, 2, &n); return v; }
int ReadU8(uintptr_t a) { uint8_t v = 0; SIZE_T n; ReadProcessMemory(GetCurrentProcess(), (LPCVOID)a, &v, 1, &n); return v; }

std::string Clock(int sec) {
    char b[16];
    if (sec < 0) sec = 0;
    snprintf(b, sizeof(b), "%d:%02d", sec / 60, sec % 60);
    return b;
}

void DrawCriteria() {
    if (!g_config.criteriaEnabled) return;
    MissionTimer& t = g_timer;
    int mission = t.runMission;
    double m;
    if (mission < 0 && t.hasMission && ReadWatch(t.missionAddr, ValueType::U16, m)) mission = (int)m;
    criteria::Mission c;
    if (!criteria::Get(mission, c)) return;

    const int diff = ReadU8(0x102066);  // save copy +0x26: 2 Easy, 3 Normal, 5 Hard, 6 DMD
    const bool dmd = diff == 6;
    const char* diffName = diff == 2 ? "Easy" : diff == 3 ? "Normal" : diff == 5 ? "Hard" : dmd ? "DMD" : "?";
    const bool live = t.state != TimerState::Idle;
    const double frames = live ? t.shown : 0;
    // After the end: the values from when the results screen came up (bonus orbs come later).
    const bool frozen = t.state == TimerState::Finished && t.critFrozen;
    const int orbs = frozen ? t.crit.orbs : ReadI32(0x10284C);
    const int damage = frozen ? t.crit.damage : ReadI32(0x102848);
    // The game keeps the item penalty as a negative number (-100 per item); use its size.
    const int penaltyRaw = frozen ? t.crit.penalty : ReadI16(0x102850);
    const int penalty = penaltyRaw < 0 ? -penaltyRaw : penaltyRaw;
    const int bonus = frozen ? t.crit.bonus : ReadI16(0x102880);
    const criteria::Score s = criteria::Compute(c, frames, g_config.ticksPerSecond, orbs, damage, penalty, bonus, dmd);

    char title[64];
    snprintf(title, sizeof(title), "Rank criteria  M%d %s", mission, diffName);
    ImGui::SeparatorText(title);
    if (!ImGui::BeginTable("##criteria", g_config.criteriaHints ? 4 : 3, ImGuiTableFlags_SizingFixedFit)) return;

    const ImVec4 dim(0.55f, 0.55f, 0.55f, 1.0f);
    auto ptsColor = [](int p) {
        return p >= 500 ? ImVec4(1.0f, 0.82f, 0.3f, 1.0f) : p >= 300 ? ImVec4(0.6f, 0.9f, 0.6f, 1.0f)
             : p > 0 ? ImVec4(0.9f, 0.9f, 0.9f, 1.0f) : ImVec4(0.9f, 0.4f, 0.4f, 1.0f);
    };
    auto row = [&](const char* name, const std::string& value, int pts, const std::string& hint, bool showPts = true) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(name);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(value.c_str());
        ImGui::TableSetColumnIndex(2);
        if (showPts) ImGui::TextColored(ptsColor(pts), "%d", pts);
        if (g_config.criteriaHints) {
            ImGui::TableSetColumnIndex(3);
            ImGui::TextColored(dim, "%s", hint.c_str());
        }
    };

    char buf[64];
    // Time
    std::string hint;
    if (live && s.timeLeftSec >= 0) {
        const int nextPts = s.timePts == 500 ? 300 : s.timePts == 300 ? 200 : s.timePts == 200 ? 100 : 0;
        snprintf(buf, sizeof(buf), "%s until %d", Clock(s.timeLeftSec + 1).c_str(), nextPts);
        hint = buf;
    }
    row("Time", (live ? Clock((int)(frames / g_config.ticksPerSecond)) : std::string("--")) + " / " + Clock(c.timeSec),
        s.timePts, hint);
    // Orbs
    hint.clear();
    if (s.orbsForNext > 0) { snprintf(buf, sizeof(buf), "+%d for %d", s.orbsForNext, s.nextOrbPts); hint = buf; }
    snprintf(buf, sizeof(buf), "%d / %d", orbs, c.orbs);
    row("Orbs", buf, s.orbPts, hint);
    // Damage
    hint.clear();
    if (s.damageToDrop > 0) {
        const int nextPts = s.damagePts == 500 ? 300 : s.damagePts == 300 ? 200 : s.damagePts == 200 ? 100 : 0;
        snprintf(buf, sizeof(buf), "%d more = %d", s.damageToDrop, nextPts);
        hint = buf;
    }
    row("Damage", std::to_string(damage), s.damagePts, hint);
    // Items / bonus
    row("Items", penalty ? "-" + std::to_string(penalty) : std::string("0"), -penalty, "", penalty != 0);
    row("Secret bonus", bonus ? "+" + std::to_string(bonus) : std::string("0"), bonus, "", bonus != 0);

    // Total + rank
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted("Total");
    ImGui::TableSetColumnIndex(1);
    const ImVec4 rankCol = s.rank == 'S' ? ImVec4(1.0f, 0.82f, 0.3f, 1.0f) : s.rank == 'A' ? ImVec4(0.6f, 0.9f, 0.6f, 1.0f)
                          : s.rank == 'D' ? ImVec4(0.9f, 0.4f, 0.4f, 1.0f) : ImVec4(0.9f, 0.9f, 0.9f, 1.0f);
    ImGui::TextColored(rankCol, "%d  %c", s.total, s.rank);
    // Special bonus (2000 orbs) at 1500+ points, on every difficulty.
    if (s.total >= 1500) {
        ImGui::TableSetColumnIndex(2);
        ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.3f, 1.0f), "SB");
    }
    if (g_config.criteriaHints) {
        ImGui::TableSetColumnIndex(3);
        if (s.nextRank) ImGui::TextColored(dim, "+%d for %c", s.pointsToNextRank, s.nextRank);
        else if (s.total < 1500) ImGui::TextColored(dim, "+%d for special bonus", 1500 - s.total);
        else ImGui::TextColored(dim, "special bonus");
    }
    ImGui::EndTable();
}

// Name of a key for the reminders: "F3" for function keys, otherwise what Windows calls it.
std::string KeyName(int vk) {
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    char name[64] = {};
    const UINT scan = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    if (scan && GetKeyNameTextA((LONG)(scan << 16), name, sizeof(name)) > 0) return name;
    char b[16];
    snprintf(b, sizeof(b), "key %02X", vk);
    return b;
}

// Reminders for the main keys, under everything else in the stats panel.
void DrawKeyHints() {
    if (!g_config.showKeyHints) return;
    struct Hint { int vk; bool on; const char* what; };
    const Hint hints[] = {
        {g_config.toggleKey, g_config.toggleKey != 0, "Show / hide overlay"},
        {g_config.menuKey, g_config.presetsEnabled && g_config.menuKey != 0, "Practice menu"},
        {g_config.resetKey, g_config.resetEnabled && g_config.resetKey != 0, "Restart mission"},
        {g_config.roomResetKey, g_config.resetEnabled && g_config.roomResetKey != 0, "Restart room"},
    };
    ImGui::SeparatorText("Keys");
    if (!ImGui::BeginTable("##keys", 2, ImGuiTableFlags_SizingFixedFit)) return;
    const ImVec4 key(1.0f, 0.82f, 0.3f, 1.0f), dim(0.65f, 0.65f, 0.65f, 1.0f);
    for (const Hint& h : hints) {
        if (!h.on) continue;
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextColored(key, "%s", KeyName(h.vk).c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::TextColored(dim, "%s", h.what);
    }
    ImGui::EndTable();
}

void DrawTimerWindow() {
    if (!g_timer.ready) return;

    ImU32 color;
    switch (g_timer.state) {
        case TimerState::Idle:     color = IM_COL32(140, 140, 140, 255); break;  // gray
        case TimerState::Running:  color = IM_COL32(255, 255, 255, 255); break;  // white
        case TimerState::Finished: color = IM_COL32(115, 230, 115, 255); break;  // green
        default:                   color = IM_COL32(255, 255, 255, 255); break;
    }

    const std::string main = FormatValue(g_timer.shown, ValueType::I32, Format::Time, g_config.ticksPerSecond);
    const std::string room = FormatValue(g_timer.roomShown, ValueType::I32, Format::Time, g_config.ticksPerSecond);

    // Width is the wider of the two lines, and never narrower than "0:00.00"
    // so the box doesn't twitch as digits change.
    ImFont* font = g_timerFont ? g_timerFont : ImGui::GetFont();
    float width = FixedWidth(font, S(g_config.timerSize), main);
    float minWidth = FixedWidth(font, S(g_config.timerSize), "0:00.00");
    if (width < minWidth) width = minWidth;
    // The PB line can be longer than the timers ("No PB: room restart used"): make room for it,
    // or it gets cut off at the window edge.
    std::string pbText;
    ImU32 pbColor = 0;
    const bool showPb = PbLine(pbText, pbColor);
    if (showPb) {
        const float pbWidth = FixedWidth(font, S(g_config.pbSize), pbText);
        if (pbWidth > width) width = pbWidth;
    }

    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(screen.x * 0.5f, S(g_config.timerPosY)), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(g_config.timerBackgroundAlpha);
    const bool boxed = g_config.timerBackgroundAlpha > 0.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, boxed ? ImVec2(S(22), S(10)) : ImVec2(S(10), S(8)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, boxed ? S(g_config.timerBorderSize) : 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(205, 35, 35, 255));  // red
    if (ImGui::Begin("##timers", nullptr, kOverlayFlags)) {
        // Drop shadow: soft dark rings around the box, heavier below it. Rings rather than filled
        // layers so the shadow doesn't darken the see-through box itself. Drawn on the background
        // list so it sits under every window.
        if (boxed && g_config.timerShadow) {
            const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
            const float rounding = ImGui::GetStyle().WindowRounding;
            ImDrawList* bg = ImGui::GetBackgroundDrawList();
            const float drop = S(8.0f);                     // how far the shadow reaches further below
            const ImVec2 lo(pos.x, pos.y), hi(pos.x + size.x, pos.y + size.y + drop);
            const int maxAlpha = 140;
            bg->AddRectFilled(ImVec2(lo.x, hi.y - drop), hi, IM_COL32(0, 0, 0, maxAlpha), rounding,
                              ImDrawFlags_RoundCornersBottom);
            const int rings = 18;
            const float step = S(2.0f);
            for (int k = 0; k < rings; ++k) {
                const float g = (k + 0.5f) * step;
                const float t = 1.0f - (float)k / rings;
                const int alpha = (int)(maxAlpha * t * t);
                bg->PathRect(ImVec2(lo.x - g, lo.y - g), ImVec2(hi.x + g, hi.y + g), rounding + g);
                bg->PathStroke(IM_COL32(0, 0, 0, alpha), ImDrawFlags_Closed, step + 0.6f);
            }
        }
        DrawCenteredLine(main, S(g_config.timerSize), width, color);
        if (g_timer.hasRoom) DrawCenteredLine(room, S(g_config.roomTimerSize), width, color);
        if (showPb) DrawCenteredLine(pbText, S(g_config.pbSize), width, pbColor);
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

}  // namespace

void overlay::Update(bool focused) {
    UpdateTimer();
    stat_fix::Update(g_missionStarts);
    reset::Update(focused, g_missionStarts);
    menu::Update(focused);
    dump::Update(focused);
    capture::Update(focused);
}

bool overlay::MissionElapsed(double& frames) {
    MissionTimer& t = g_timer;
    if (!t.ready || t.state != TimerState::Running) return false;
    double counter, start;
    if (!ReadWatch(t.counter, ValueType::I32, counter) || !ReadWatch(t.startFrame, ValueType::I32, start)) return false;
    frames = counter - start;
    return true;
}

bool overlay::RewindMission(double frames) {
    MissionTimer& t = g_timer;
    if (!t.ready || t.state != TimerState::Running) return false;
    double counter;
    uintptr_t addr;
    if (!ReadWatch(t.counter, ValueType::I32, counter) || !ResolveAddress(t.startFrame, addr)) return false;
    const int32_t start = (int32_t)(counter - frames);
    SIZE_T done = 0;
    if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(addr), &start, 4, &done) || done != 4)
        return false;
    t.lastStart = start;  // our own write, not a new mission
    t.practice = true;
    return true;
}

bool overlay::CurrentMission(int& mission) {
    mission = g_timer.runMission;
    double m;
    if (mission < 0 && g_timer.hasMission && ReadWatch(g_timer.missionAddr, ValueType::U16, m)) mission = (int)m;
    return mission > 0;
}

void overlay::AbandonRun() {
    MissionTimer& t = g_timer;
    if (t.state == TimerState::Running) {
        t.state = TimerState::Idle;
        dlog::Write("[MissionTimer] run abandoned (restart / preset load)");
    }
    t.abandoned = true;
}

ImFont* overlay::TitleFont() { return g_timerFont; }

bool overlay::WantsDraw() { return menu::IsOpen() || dump::WantsDraw(); }

namespace {
std::string Narrow(const std::wstring& w) {
    std::string out;
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n > 1) { out.resize(n - 1); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &out[0], n, nullptr, nullptr); }
    return out;
}
bool FileExists(const std::string& path) {
    return !path.empty() && GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// The text font: the INI's Font if set, otherwise EB Garamond from the mod's fonts folder.
// Empty if neither is there.
std::string FindTextFont() {
    const std::string modFolder = Narrow(g_config.dataFolder);
    if (!g_config.font.empty()) {
        if (FileExists(g_config.font)) return g_config.font;
        if (FileExists(modFolder + g_config.font)) return modFolder + g_config.font;  // relative to the mod
        dlog::Write("Font not found: %s", g_config.font.c_str());
    }
    const std::string bundled = modFolder + "fonts\\EBGaramond-Regular.ttf";
    if (FileExists(bundled)) return bundled;
    return {};
}
}  // namespace

float overlay::UiScale() { return g_uiScale; }

bool overlay::SetDisplayHeight(unsigned height) {
    float target = g_config.uiScale > 0.0f ? g_config.uiScale : (height ? height / 1440.0f : 1.0f);
    if (target < 0.4f) target = 0.4f;
    if (target > 3.0f) target = 3.0f;
    if (fabsf(target - g_uiScale) < 0.02f) return false;
    dlog::Write("UI scale %.2f (window height %u)", target, height);
    g_uiScale = target;
    return true;
}

void overlay::ApplyStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImGui::StyleColorsDark(&style);
    style.WindowRounding = 6.0f;
    style.WindowBorderSize = 0.0f;
    style.WindowPadding = ImVec2(10, 8);
    style.ScaleAllSizes(g_uiScale);
}

void overlay::LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    const std::string textFont = FindTextFont();

    // Stats panel / menu font (the first font added is ImGui's default). Loaded at its final size
    // so it stays sharp; FontScale in the INI sizes it.
    ImFont* ui = nullptr;
    if (!textFont.empty()) {
        const float uiPx = 18.0f * g_config.fontScale * g_uiScale;
        ui = io.Fonts->AddFontFromFileTTF(textFont.c_str(), uiPx);
        if (ui) {
            io.FontGlobalScale = 1.0f;
            dlog::Write("Font: %s at %.0fpx", textFont.c_str(), uiPx);
        } else {
            dlog::Write("Couldn't load font %s; using the built-in one", textFont.c_str());
        }
    } else {
        dlog::Write("fonts\\EBGaramond-Regular.ttf not found; using the built-in font");
    }
    if (!ui) {
        io.Fonts->AddFontDefault();
        io.FontGlobalScale = g_config.fontScale * g_uiScale;
    }

    // Big timer font: loaded at its full pixel size so it stays sharp.
    std::vector<std::string> candidates;
    if (!g_config.timerFont.empty()) candidates.push_back(g_config.timerFont);
    if (!textFont.empty()) candidates.push_back(textFont);
    candidates.push_back("C:\\Windows\\Fonts\\seguisb.ttf");   // Segoe UI Semibold
    candidates.push_back("C:\\Windows\\Fonts\\segoeuib.ttf");  // Segoe UI Bold
    candidates.push_back("C:\\Windows\\Fonts\\segoeui.ttf");   // Segoe UI

    float px = S(g_config.timerSize > g_config.roomTimerSize ? g_config.timerSize : g_config.roomTimerSize);
    for (const std::string& path : candidates) {
        if (!FileExists(path)) continue;
        g_timerFont = io.Fonts->AddFontFromFileTTF(path.c_str(), px);
        if (g_timerFont) {
            dlog::Write("Timer font: %s at %.0fpx", path.c_str(), px);
            return;
        }
    }
    dlog::Write("No timer font found; using the built-in font (will look blocky at large sizes)");
}

void overlay::Init() {
    game_addrs::ResolveWithRetry(30000);  // before anything parses or uses exe addresses
    InitTimer();
    criteria::Load(g_config.dataFolder + L"dmc1_criteria.csv");
    stat_fix::Init();
    reset::Init();
    menu::Init();
    dump::Init();
    mode::Init(g_config.dataFolder);
    g_entries.clear();
    for (const WatchDef& w : g_config.watches) {
        Entry e{&w, {}};
        if (w.type != ValueType::Text && !ParseTerms(w.address, e.terms, w.name)) e.terms.clear();
        g_entries.push_back(e);
    }
    dlog::Write("Overlay ready with %zu watches", g_entries.size());
}

void overlay::Draw(bool showStats) {
    menu::Draw();
    dump::Draw();
    if (!showStats) return;

    DrawTimerWindow();

    if (g_entries.empty()) return;

    // Stats panel. Anchor to a screen corner; PosX/PosY are the gaps from those edges.
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const bool right = g_config.anchor == 1 || g_config.anchor == 3;
    const bool bottom = g_config.anchor == 2 || g_config.anchor == 3;
    ImVec2 pos(right ? screen.x - S(g_config.posX) : S(g_config.posX),
               bottom ? screen.y - S(g_config.posY) : S(g_config.posY));
    ImVec2 pivot(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowBgAlpha(g_config.backgroundAlpha);

    // Display only: the overlay never takes mouse or keyboard input from the game.
    if (!ImGui::Begin("DMC1 Practice", nullptr, kOverlayFlags)) {
        ImGui::End();
        return;
    }

    const ImVec4 dim(0.55f, 0.55f, 0.55f, 1.0f);
    std::string currentGroup;
    bool tableOpen = false;

    for (const Entry& e : g_entries) {
        const WatchDef& w = *e.def;

        if (!tableOpen || w.group != currentGroup) {
            if (tableOpen) ImGui::EndTable();
            currentGroup = w.group;
            if (!currentGroup.empty()) ImGui::SeparatorText(currentGroup.c_str());
            // Rows with no group sit directly under the main timer, without a header.
            const char* tableId = currentGroup.empty() ? "##ungrouped" : currentGroup.c_str();
            tableOpen = ImGui::BeginTable(tableId, 2, ImGuiTableFlags_SizingFixedFit);
            if (!tableOpen) continue;
            ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed);
            // Value column fits its widest entry (e.g. a long category name), at least 110px.
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed);
        }

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(w.name.c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::Dummy(ImVec2(S(110.0f), 0.0f));  // minimum value-column width so numbers right-align
        ImGui::SameLine(0, 0);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() - S(110.0f));

        double value = 0;
        bool isText = w.type == ValueType::Text;
        if (isText || ReadEntry(e, value)) {
            // "$category" in a text row shows the current practice category, "$pb" your best time
            // for the current mission in that category.
            std::string literal = w.address == "$category" ? g_config.category
                                : w.address == "$pb"       ? CurrentPbText()
                                                           : w.address;
            std::string text = isText ? (literal.empty() ? "--" : literal)
                                      : FormatValue(value, w.type, w.format, g_config.ticksPerSecond);
            // Right-align numbers so digits don't jump around as they change.
            float colWidth = ImGui::GetContentRegionAvail().x;
            float textWidth = ImGui::CalcTextSize(text.c_str()).x;
            if (colWidth > textWidth) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + colWidth - textWidth);
            ImGui::TextUnformatted(text.c_str());
        } else {
            ImGui::TextColored(dim, "--");
        }
    }
    if (tableOpen) ImGui::EndTable();

    DrawCriteria();
    DrawKeyHints();

    ImGui::End();
}
