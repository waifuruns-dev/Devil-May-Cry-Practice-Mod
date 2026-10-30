DMC1 PRACTICE MOD  v1.4.0
Devil May Cry HD Collection (PC) - Devil May Cry 1
Created by waifu
=================================================

A practice tool for individual-level (IL) runs: a mission timer, personal bests, live rank
criteria, instant mission and room restarts, and presets that drop you into any mission with
any loadout, without going through the title screen. It keeps its own practice save, so your
real save is never touched, and it's completely off until you press F10.


INSTALL
-------
1. Open the game folder: in Steam, right-click Devil May Cry HD Collection > Manage >
   Browse local files.
2. Copy everything from this zip into that folder (next to dmc1.exe):
     dinput8.dll, dmc1_practice.ini, dmc1_criteria.csv, README.txt, THIRD-PARTY-NOTICES.txt,
     fonts\, presets\
3. Start Devil May Cry 1 from the launcher as usual.

Font: the mod's text uses EB Garamond (fonts\, free under the SIL Open Font License, see
fonts\EBGaramond-OFL.txt). "Font" in dmc1_practice.ini picks any other font.

To uninstall, delete dinput8.dll (and the other files if you like). The mod's own files
(dmc1_practice.sav, dmc1_save_backups\, dmc1_exports\, dmc1_practice_pbs.txt, the logs) can be
deleted too.

ON / OFF (F10)
--------------
THE MOD STARTS OFF: the game is plain DMC1 until you press F10. F10 again turns it off. When it
comes on, a title card shows for a moment.

While the mod is on, a small "PRACTICE MOD" tag sits in the top-right corner (F1 doesn't hide
it), and the mod draws into the game's own frame before capture software sees it, so recordings
and streams always show it, also with OBS's "Capture third-party overlays" turned off. With the
mod off (F10) nothing is drawn and nothing in the game is changed.

Your saves:
- While the mod is on, the game loads and saves dmc1_practice.sav next to the mod (a copy of
  your save made when you turn it on), not your real dmc1.sav. Presets write there too (slot 10 of the
  practice save). Your real save isn't written at all, so Steam Cloud never sees a change.
- Turning the mod off puts your own save back. In a mission that happens when you quit to the
  title screen; until then the practice save stays loaded (and keeps saving to the practice file).
- Every time the game starts, your save files are also copied to dmc1_save_backups (one folder
  per start, the newest 20 kept). To restore one: close the game, copy the files from a backup
  folder into Steam\userdata\<your number>\631510\remote, and if Steam asks about a Cloud
  conflict, choose the local files.
- To play a preset without the mod (a challenge run, say), export it to your real save: X in the
  F2 list (see below).


KEYS
----
  F10  turn the whole mod on / off (off when the game starts)
  F1   show / hide the overlay
  F2   practice menu (see below)
  F3   restart the mission instantly, with the loadout you started it with
  F4   restart the room: back to how it was when you came in (door, HP, DT, items, orbs,
       enemies and bosses back, timer set back). Your equipped weapons stay as they are, so you
       can try different loadouts for a room. A mission where you used F4 isn't saved as a PB.
  F7   save your current loadout as a preset for the mission you're in

All keys can be changed in dmc1_practice.ini (ModOnAtStart = 1 turns the mod on at start).

The mod's text and panels scale with the game's resolution, so they cover the same part of the
screen at 1080p, 1440p or 4K. UiScale in dmc1_practice.ini sets a size of your own instead.


THE OVERLAY
-----------
Top of the screen: the mission timer (white while running, green and frozen on the game's own
official time at the results screen), the room timer under it, and your PB for the mission with
the result (NEW PB / +time) when you finish.

Bottom left: the preset you loaded, the mission number, your PB, and the rank criteria for the mission:
  Time / Orbs / Damage   live value, target, and points (500 / 300 / 200 / 100 / 0)
  Items, Secret bonus    item penalty and secret mission bonus
  Total                  projected score and rank (S 1000, A 800, B 500; Dante Must Die: S 700,
                         A 400, B 200), and "SB" at 1500 points (special bonus, 2000 orbs)
The grey column says what the next bracket needs: time left, orbs needed, damage until it drops.
Targets come from dmc.wiki and can be edited in dmc1_criteria.csv.
Under the criteria, a reminder of the F1 / F2 / F3 / F4 keys (ShowKeyHints = 0 in the INI
hides it).

Personal bests are kept per preset and mission in dmc1_practice_pbs.txt: loading a preset files
your times under its name, so "DMD LDK" and a Normal setup for the same mission don't mix.


PRACTICE MENU (F2)
------------------
Up/Down select, Enter choose / load, Backspace back, Esc close.

Pick a category, then a mission:
  In a mission       the preset loads straight away. F3 then restarts it.
  At the title       it's written to slot 10: choose Load Game > slot 10.
  E                  open the preset in the editor instead of loading it
  S                  sort the list: by mission, by name, or newest first
  Delete (twice)     delete it (the file is renamed to .json.deleted, so it can be restored)
  X (twice)          export it to your REAL save: it goes into slot 10 of your own dmc1.sav
                     (replacing what's there), so you can play it with the mod off, e.g. for a
                     challenge run. A copy of that save is also kept in dmc1_exports\<preset>\
                     to share. (The only time the mod writes your real save.)

"+ New preset..." opens the editor: mission, difficulty, costume, orbs, HP / DT bars, DT gauge,
Nightmare's cores (M17 on: blue / green / red, carried into the M18 and M20 fights), weapons
owned, starting weapons, moves and items. Left/Right change a value (Shift = bigger steps),
Enter ticks a box. "Name" lets you type a name, so you can keep several setups for one
mission. "Load now" loads it, "Save as preset" saves it to presets\My Presets\.
The editor won't let you make impossible setups (a weapon equipped that you don't own, moves
without their weapon), since those crash the game.

Presets are plain .json files in presets\<category>\. A category is just a folder for grouping
them. See presets\README.txt for the file format.


KNOWN LIMITATIONS
-----------------
- Costume: a preset with a different costume from the one you're playing can't load in the
  middle of a mission (the model's Devil Trigger data stays the old costume's, and moves like
  Air Raid crash the game). The menu writes it to slot 10 instead: quit to the title and choose
  Load Game > slot 10. After that, F3 and presets with the same costume load in-mission as usual.
- Mission 5's intro: after loading M5 from the menu in a mission, you can move during the opening
  cutscene. Loading M5 from the title screen plays it normally.
- At the title screen, presets go to slot 10 rather than loading directly. If you're already on
  the Load Game screen, back out and open it again to see the new slot.
- Mission 1 isn't supported (it's New Game).
- Restarts and mission switches take about 2-3 seconds of loading.
- F10 can't send you to the title screen by itself: turning the mod off in a mission keeps the
  practice save loaded until you quit to the title through the game's menu.


GAME VERSIONS
-------------
The mod finds the addresses it needs in the game's code when it starts, so it works on both the
2018 Steam release and the newer build (with Steam's protection the overlay may take a second to
appear). If something doesn't work, check dmc1_practice.log: the "[Addrs]" lines show what was
found. Please include that log (and your game version) when reporting a problem.


CREDITS
-------
Created by waifu.

Thanks to:
  serpentiem       DDMK, which the room and event research builds on
  Chris and Aisu   Cheat Engine tables
  Mysterion        the DMC1 autosplitter
  dmc.wiki         rank criteria

Built with Dear ImGui (Omar Cornut, MIT), MinHook (Tsuda Kageyu, BSD 2-Clause) and JSON for
Modern C++ (Niels Lohmann, MIT); their licenses are in THIRD-PARTY-NOTICES.txt.
Font: EB Garamond by Georg Duffner and Octavio Pardo (SIL Open Font License 1.1).


ABOUT HOW THIS WAS MADE
-----------------------
This mod was made with the help of AI: the code, the reverse engineering of the game's memory
and save format, and these documents were written with Claude (Anthropic's AI assistant),
directed and tested in-game by waifu.


VERSION HISTORY
---------------
1.4.0  The overlay is drawn before capture software sees the frame (shows in OBS with "Capture
       third-party overlays" off); "PRACTICE MOD" tag while the mod is on.
1.3.0  Export a preset to slot 10 of your real save (X in the F2 list).
1.2.0  Title card when the mod comes on; editor order: weapons owned, starting weapons, moves.
1.1.0  F10 turns the whole mod on / off (off at start); separate practice save (your real save is
       never written); save backups at every start.
1.0.0  First release.
