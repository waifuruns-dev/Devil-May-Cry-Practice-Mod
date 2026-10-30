#pragma once

struct ImFont;

namespace overlay {
void Init();   // parse addresses from the config once
void LoadFonts();  // after the ImGui context is created, and again when the UI scale changes

// UI scale: everything is laid out for a 1440-pixel-tall game window and scaled to the real
// height (or the INI's UiScale). SetDisplayHeight returns true if the scale changed enough that
// the fonts and style need rebuilding.
float UiScale();
ImFont* TitleFont();  // the big timer font (EB Garamond), for the splash
bool SetDisplayHeight(unsigned height);
void ApplyStyle();  // ImGui style at the current scale
void Update(bool gameFocused);  // every frame, even while hidden, so the timer never misses a start/stop
void Draw(bool showStats);      // between ImGui::NewFrame and ImGui::Render
bool WantsDraw();               // true while the practice menu is open, even if the overlay is hidden

// Mission time so far, in frames. False if no mission timer is running.
bool MissionElapsed(double& frames);
// Room restart: moves the game's mission start frame so the mission time reads `frames` again,
// and marks the run as practice (it can't set a PB).
bool RewindMission(double frames);
// Mission restart / preset load: the running mission is abandoned (no finish, no PB), and the
// game's final-time field changing from the save write is ignored until the next mission starts.
void AbandonRun();
// The mission being played (or last played). False if unknown.
bool CurrentMission(int& mission);
}  // namespace overlay
