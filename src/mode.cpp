#include "mode.h"

#include <windows.h>
#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <vector>

#include "config.h"
#include "input_block.h"
#include "overlay.h"
#include "log.h"
#include "reset.h"
#include "savefile.h"
#include "steam_redirect.h"

namespace {

bool g_on = false;
bool g_restorePending = false;  // off, but your save goes back only at the title screen
bool g_keyDown = false;
std::wstring g_practiceFile;
std::vector<uint8_t> g_realSlots;  // the game's in-memory slots when the mod was turned on
std::string g_message;
ULONGLONG g_messageUntil = 0;
ULONGLONG g_splashStart = 0;  // the title card when the mod comes on
constexpr ULONGLONG kSplashMs = 3200;

void Say(const std::string& m, int ms = 5000) {
    g_message = m;
    g_messageUntil = GetTickCount64() + ms;
    dlog::Write("[Mode] %s", m.c_str());
}

// Your own save back: in the game's memory (what Load Game shows and what it saves) and on writes.
void RestoreRealSave() {
    // (Not read when the mod went on: take them from your save file, which the mod never changes.)
    if (g_realSlots.empty()) savefile::ReadSlotsFromFile(savefile::Locate(g_config.saveFile), g_realSlots);
    if (!g_realSlots.empty() && !savefile::WriteSlotsToMemory(g_realSlots))
        dlog::Write("[Mode] couldn't put your save back in the game's memory");
    g_realSlots.clear();
    steam_redirect::SetActive(false);
    g_restorePending = false;
}

void TurnOn() {
    if (!steam_redirect::Installed()) {
        Say("Can't turn the mod on: Steam's save storage wasn't found (see dmc1_practice.log).", 7000);
        return;
    }
    if (g_restorePending) {
        // Turned back on before reaching the title: the practice save is still the loaded one.
        g_restorePending = false;
        g_on = true;
        Say("Practice mod on (F10 turns it off).");
        return;
    }
    // The practice file starts as a copy of your save as it is now.
    const std::wstring real = savefile::Locate(g_config.saveFile);
    if (real.empty() || !savefile::CopyWhole(real, g_practiceFile)) {
        Say("Can't turn the mod on: couldn't copy your save to dmc1_practice.sav (see the log).", 7000);
        return;
    }
    if (!savefile::ReadSlotsFromMemory(g_realSlots)) {
        g_realSlots.clear();
        dlog::Write("[Mode] the game's in-memory save wasn't readable yet (it's put back from disk at the title)");
    }
    steam_redirect::SetActive(true);
    g_on = true;
    g_splashStart = GetTickCount64();
    Say("Practice mod on: the game now saves to dmc1_practice.sav, not your save. F10 turns it off.",
        (int)kSplashMs + 4000);
}

void TurnOff() {
    g_on = false;
    input_block::SetBlocked(false);
    if (reset::InGame()) {
        g_restorePending = true;
        Say("Practice mod off. Quit to the title screen: your own save comes back there (until then the "
            "practice save stays loaded).", 8000);
    } else {
        RestoreRealSave();
        Say("Practice mod off: your own save is back. F10 turns it on again.");
    }
}

}  // namespace

void mode::Init(const std::wstring& folder) {
    g_practiceFile = folder + L"dmc1_practice.sav";
    steam_redirect::Install(g_practiceFile);
    if (g_config.modOnAtStart) {
        g_on = false;  // turned on once Steam's storage is hooked (see Update)
        Say("Practice mod: turning on...", 3000);
    } else {
        Say("Practice mod is off (F10 turns it on).", 6000);
    }
}

void mode::Update(bool focused) {
    static bool autoOnDone = false;
    if (g_config.modOnAtStart && !autoOnDone && steam_redirect::Installed()) {
        autoOnDone = true;
        TurnOn();
    }
    const bool down = focused && (GetAsyncKeyState(g_config.modKey) & 0x8000) != 0;
    if (down && !g_keyDown) {
        if (g_on) TurnOff();
        else TurnOn();
    }
    g_keyDown = down;
    if (g_restorePending && !reset::InGame()) {
        RestoreRealSave();
        Say("Your own save is back (practice mod off).");
    }
}

bool mode::On() { return g_on; }

void mode::RealSlotChanged(int slot, const std::vector<uint8_t>& data) {
    const size_t size = data.size();
    if (g_realSlots.size() == 10 * size && slot >= 1 && slot <= 10)
        std::copy(data.begin(), data.end(), g_realSlots.begin() + (slot - 1) * size);
}
std::wstring mode::SavePath() { return g_practiceFile; }
bool mode::WantsDraw() {
    return g_on || GetTickCount64() < g_messageUntil || g_restorePending || GetTickCount64() - g_splashStart < kSplashMs;
}

namespace {

// Title card: "DEVIL MAY CRY PRACTICE MOD", "made by waifu" under it; fades in and out.
void DrawSplash() {
    const ULONGLONG t = GetTickCount64() - g_splashStart;
    if (!g_splashStart || t >= kSplashMs) return;
    const float ms = (float)t;
    float a = 1.0f;
    if (ms < 400) a = ms / 400.0f;
    else if (ms > kSplashMs - 700) a = (kSplashMs - ms) / 700.0f;
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float scale = overlay::UiScale();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = overlay::TitleFont() ? overlay::TitleFont() : ImGui::GetFont();

    const char* title = "DEVIL MAY CRY PRACTICE MOD";
    const char* by = "made by waifu";
    const float titleSize = 96.0f * scale, bySize = 40.0f * scale;
    const ImVec2 ts = font->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, title);
    const ImVec2 bs = font->CalcTextSizeA(bySize, FLT_MAX, 0.0f, by);
    const float cy = screen.y * 0.42f;
    const float bandTop = cy - ts.y * 0.5f - 36 * scale, bandBottom = cy + ts.y * 0.5f + bs.y + 52 * scale;

    // A dark band across the screen with thin red lines, like the timer box.
    const int bg = (int)(190 * a), red = (int)(230 * a);
    dl->AddRectFilledMultiColor(ImVec2(0, bandTop), ImVec2(screen.x * 0.5f, bandBottom), IM_COL32(0, 0, 0, 0),
                                IM_COL32(0, 0, 0, bg), IM_COL32(0, 0, 0, bg), IM_COL32(0, 0, 0, 0));
    dl->AddRectFilledMultiColor(ImVec2(screen.x * 0.5f, bandTop), ImVec2(screen.x, bandBottom), IM_COL32(0, 0, 0, bg),
                                IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, bg));
    const float lineW = 2.0f * scale, lx0 = screen.x * 0.2f, lx1 = screen.x * 0.8f;
    dl->AddLine(ImVec2(lx0, bandTop), ImVec2(lx1, bandTop), IM_COL32(170, 20, 20, red), lineW);
    dl->AddLine(ImVec2(lx0, bandBottom), ImVec2(lx1, bandBottom), IM_COL32(170, 20, 20, red), lineW);

    const ImVec2 tp(screen.x * 0.5f - ts.x * 0.5f, cy - ts.y * 0.5f);
    dl->AddText(font, titleSize, ImVec2(tp.x + 3 * scale, tp.y + 3 * scale), IM_COL32(0, 0, 0, (int)(200 * a)), title);
    dl->AddText(font, titleSize, tp, IM_COL32(235, 225, 210, (int)(255 * a)), title);
    const ImVec2 bp(screen.x * 0.5f - bs.x * 0.5f, cy + ts.y * 0.5f + 16 * scale);
    dl->AddText(font, bySize, bp, IM_COL32(190, 60, 60, (int)(255 * a)), by);
}

}  // namespace

// While the mod is on, a small tag that F1 doesn't hide: a recording always shows the mod is in use.
static void DrawWatermark() {
    if (!g_on) return;
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float s = overlay::UiScale();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const char* text = "PRACTICE MOD";
    const float size = 22.0f * s;
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const float pad = 8.0f * s, margin = 12.0f * s;
    const ImVec2 lo(screen.x - ts.x - 2 * pad - margin, margin), hi(screen.x - margin, margin + ts.y + 2 * pad);
    dl->AddRectFilled(lo, hi, IM_COL32(0, 0, 0, 150), 4.0f * s);
    dl->AddRect(lo, hi, IM_COL32(170, 20, 20, 220), 4.0f * s, 0, 1.5f * s);
    dl->AddText(font, size, ImVec2(lo.x + pad, lo.y + pad), IM_COL32(235, 225, 210, 230), text);
}

void mode::Draw() {
    DrawWatermark();
    DrawSplash();
    const bool msg = GetTickCount64() < g_messageUntil && GetTickCount64() - g_splashStart >= kSplashMs - 700;
    if (!msg && !g_restorePending) return;
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(screen.x * 0.5f, screen.y * 0.9f), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.75f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("##mode", nullptr, flags)) {
        ImGui::TextUnformatted(msg ? g_message.c_str()
                                   : "Practice mod off: quit to the title screen to get your own save back.");
    }
    ImGui::End();
}
