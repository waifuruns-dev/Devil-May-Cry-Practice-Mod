PRESETS
=======

Each folder here is a category. Each .json file in it is one mission's setup.
The preset's "name" is what the overlay shows and what personal bests are saved under.

Fields (only "mission" is required; everything else has a default):

  "name"          shown in the menu
  "mission"       2-23 (mission 1 is just New Game)
  "difficulty"    "easy", "normal", "hard", "dmd"                       default "normal"
  "costume"       "dante", "ldk", "super"                               default "dante"
  "orbs"          red orbs                                              default 0
  "hp_bars"       HP bars (10 base; Easy 15)                            default base
  "dt_bars"       DT bars (3 base; Easy 6)                              default base
  "dt_gauge"      "full", "empty", "route", or a number (120 per rune)   default "full"
  "moves"         list of: Stinger Lv1, Stinger Lv2, Round Trip, Air Hike, Air Raid,
                  Vortex Lv1, Vortex Lv2, Kick 13 Lv1, Kick 13 Lv2, Magma Drive,
                  Rolling Blaze, Meteor Lv1, Meteor Lv2, Inferno
                  (a Lv2 automatically includes its Lv1)
  "items"         { "Vital Star": 1, "Devil Star": 1, "Holy Water": 2, "Untouchable": 1 }
  "extra_weapons" list of: Force Edge, Shotgun, Needlegun, Grenadegun, Nightmare-Beta,
                  Handguns, Alastor, Ifrit, Sparda, Yamato
                  (the weapons the story gives you by that mission are always included)
  "equip"         { "devil_arm": "Alastor", "gun": "Shotgun" }   default: the mission's normal start
                  devil_arm: Alastor, Ifrit, Force Edge, Sparda, Yamato
                  gun: Handguns, Shotgun, Grenadegun, Nightmare-Beta
  "top_core"      "blue", "green" or "red"   Nightmare's cores (missions 17-23): blue = not
  "bottom_core"                              broken, green = broken once, red = broken twice.
                                             default: how the story route left them

Story progress (key items, doors, events) always comes from the mod's built-in table,
so a preset can't break the mission.

Starter categories:
  Clean                  no moves, no consumables, 0 orbs, base HP/DT, full DT gauge
  Any% Normal (route)    the loadout from the any% Normal route saves, mission by mission

Making presets in game
  Press F7 (CaptureKey in dmc1_practice.ini) during a mission to save your current loadout as a
  preset for that mission: presets\My Presets\Mxx.json (CaptureCategory sets the folder name).
  To record a whole route, press F7 right at the start of each mission as you play it.
  Capturing the same mission again replaces the file; the old one is kept as Mxx.json.bak.
  Rename the folder (e.g. "Any% Hard") to group your presets differently.
  The files are plain JSON in the format above, so you can edit them afterwards (e.g. set
  "dt_gauge" to "full", or change the name).

Preset editor (F2 menu)
  "+ New preset..." at the top of the category list opens the editor. In a preset list, press E
  to open the selected preset in the editor instead of loading it.
  Up/Down pick a row, Left/Right change it (Shift = bigger steps: orbs 1000, items 5), Enter ticks
  a box. Story weapons for the mission are always included (shown greyed out).
  "Name" (top row): Enter, type a name, Enter to keep it (Esc cancels). The mission number stays
  in front of it and isn't part of the name. Named presets save as "Mxx - Name.json", so you can keep
  several setups for one mission; unnamed ones save as Mxx.json.
  "Load now" loads it into the running mission (and writes slot 10); from the title screen it
  writes slot 10 for Load Game. "Save as preset" writes presets\My Presets\Mxx.json
  (CaptureCategory in the INI), keeping an existing file as Mxx.json.bak.

Deleting a preset (F2 menu)
  In a preset list, press Delete, then Delete again to confirm. The file isn't erased: it's renamed
  to "<file>.json.deleted" in the same folder. To bring it back, remove ".deleted" from the name.
