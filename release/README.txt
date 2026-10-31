CINEMATIC COMBAT  (CombatMod)  -  version 0.9.0-beta
Single-player mod for Crimson Desert (PC). An ASI plugin.
=====================================================================

WHAT IT DOES
------------
 * SLOW MOTION - the game already slows down for some moments (finishers,
   Tiger's Fury, perfect dodges, explosions you cause ...). The mod stretches
   those moments: slower, with longer, smoother ramps in and out. It never
   invents slow motion for moments that have none (except the tower
   explosion option below).
 * LAUNCH (KNOCK-BACK) BOOST - enemies hit by launching attacks fly further.
   A "minimum launch" also makes weak or non-launching hits knock back.
 * HIT-STOP CONTROL (experimental, off by default) - deepen and lengthen the
   tiny impact pauses of each hit.
 * BLINDING FLASH MODES - choose how the Blinding Flash skill plays: the
   game's own slow motion (default), a fast dash chain with a slow finale, or
   smooth slow motion on every hit with a slow finale.
 * HIT REACTIONS - choose how the throw-back hit of Blinding Flash's finale
   (and, optionally, of Turning Slash) knocks the enemy: the game's own, a
   knock-down, a ground throw or a ragdoll fling. By default the finale of a
   Blinding Flash chain is a ground throw, so it is the same every time.
 * TOWER / EXPLOSIVE-ARROW SLOW MOTION - the game has none when you blow up a
   tower with an explosive arrow; the mod adds a short, deep slow motion.
 * CUSTOM TRIGGERS - for advanced users: write your own slow-motion rules in
   the ini file (no rule is active by default).
 * IN-GAME CONTROL PANEL - every setting lives in a small panel with sliders,
   presets, a help window and Save / Cancel. It pauses the game while open.

Only YOUR attacks are affected. Enemy attacks, enemy skills, and your own
rolls / dodges never trigger the mod. Nothing is written to the game's files;
the mod changes values in memory while the game runs. Close the game, remove
the files, and the game is exactly as before.

IMPORTANT: SINGLE-PLAYER ONLY. Do not use this (or any ASI mod) in any online
or multiplayer mode.


REQUIREMENTS
------------
 1. Crimson Desert for PC (Steam), one of the supported builds (see
    "SUPPORTED GAME BUILDS" below).
 2. An ASI loader in the game's bin64 folder. The mod is a .asi file and does
    NOT load by itself. The usual choice is "Ultimate ASI Loader" (by
    ThirteenAG): download the x64 version, rename it to version.dll and put it
    in bin64 next to CrimsonDesert.exe. (Search for the name on GitHub; no link
    is given here because links go stale.) If you use Definitive Mod Manager
    (DMM), its ASI tab can list and enable ASI plugins for you.
 3. Windows 10 / 11, 64-bit. No Visual C++ redistributable is needed.


INSTALL (3 STEPS)
-----------------
 1. Close the game. Make sure an ASI loader (version.dll) is already in
    <game folder>\bin64 (the folder that contains CrimsonDesert.exe).
 2. Drag these three files from the download into that bin64 folder:
        CombatMod.asi    CombatMod.ini    CombatMod.sigs
    (README.txt, LICENSE.txt and the other text files are optional.)
 3. Start the game. Load into the world and press  Ctrl+Enter  (or LB +
    DPad-Down on an Xbox controller). If the panel "Cinematic Combat" opens,
    the mod is running. The first hit of a fight teaches it who the player is.

The default Steam folder looks like:
   ...\steamapps\common\Crimson Desert\bin64
If you use DMM's ASI tab: install CombatMod.asi there, then copy CombatMod.ini
and CombatMod.sigs into the same bin64 folder.

All three files must stay together in the same folder. A missing
CombatMod.sigs means the mod cannot find the game's functions and does
nothing.


HOTKEYS
-------
 Open / close the panel     Ctrl+Enter        or  LB + DPad-Down (Xbox pad)
 (the key can be changed:   [UI] ToggleKeyboard in CombatMod.ini)
 In the panel (keyboard):   Up / Down = select a row, Left / Right = change
                            a slider (Shift = x5, PageUp / PageDown = x10,
                            Home / End = min / max), Space or Enter = press
                            a checkbox or button, Esc = close without saving.
 In the panel (mouse):      click checkboxes and buttons, click or drag a
                            slider, mouse wheel over a slider (Ctrl = x5).
 In the panel (controller): DPad Up / Down = select, DPad Left / Right or the
                            left stick = change a slider, A = checkbox /
                            button, B = close without saving, Start = Save
                            & Close. While the panel is open the game gets an
                            idle controller, so you cannot attack by accident.
 Closing the panel with the open / close hotkey discards unsaved changes.


THE PANEL
---------
Hover a row (or select it with keyboard / pad) and the help window on the
side explains it: its name, what it does, its range, default and current
value. The mouse needs the game in borderless or windowed mode; in exclusive
fullscreen the panel can stay hidden behind the game.

 General          Mod ON / OFF - master switch for Slow motion and Launch.
                  Pause game while open - freezes the world while you adjust.
 Slow motion      Enabled, Scale (lower = slower; 0.5 = twice as slow),
                  Ramp in / Ramp out (longer = smoother), Slow-motion cap
                  per chain (a safety limit: slow motion that has run
                  continuously this many seconds goes back to normal speed
                  until you pause for 1.5 s. It only SHORTENS slow motion,
                  it never makes it longer), Explosions and towers (also
                  stretch the slow motion of explosions YOU set off).
 Blinding Flash   Mode: Off (the game's own), Fast (fast dash chain, slow
                  finale) or Smooth (smooth slow motion on every hit, slow
                  finale); rows for the chosen mode appear below it.
 Hit-stop         Enabled, Strength, Length - experimental.
 Launch           Enabled, Strength (x), Minimum launch (a HIGH value makes
                  attacks that normally do not launch at all launch enemies),
                  Angle (experimental), Finale throw and Turning Slash throw
                  (the hit reaction: Native = the game's own, Knock, Throw,
                  Ragdoll; changed for those single hits only).
 Custom triggers  Tower and arrow explosions - slow motion for towers /
                  explosive arrows. Your own rules - the [Triggers] lines of
                  the ini.
 Presets          Vanilla (everything off), Cinematic (slow motion x0.5 with
                  1.6x ramps, launch x2), Heavy hits (hit-stop only), Extreme
                  (slow motion x0.1 with 3x ramps, launch x10). Except
                  Vanilla, a preset keeps your Minimum launch, your Blinding
                  Flash mode and your throw choices.
 Save & Close     Writes your settings into CombatMod.ini and closes.
 Close without    Undoes every change made since the panel was opened (live).
 saving
 Status           A read-out: is the player known, how many slow-motion events
                  were seen / changed, hook state.

Changes apply live while the panel is open. They are saved to the file only
with "Save & Close".


WHAT YOU GET ON THE FIRST START
-------------------------------
The shipped CombatMod.ini is the "Cinematic" look with a few extras:
   Slow motion ON, strength 0.5, ramps 1.6x, slow-motion cap per chain 6 s
   Explosions and towers ON, Blinding Flash mode: Off (game's own)
   Launch ON, strength x2, minimum launch 25
   Blinding Flash finale throw: ground throw (Turning Slash: the game's own)
   Hit-stop OFF, tower / arrow explosion slow motion ON
   Pause game while the panel is open ON, panel hotkey Ctrl+Enter
Not happy with it? Open the panel and click "Vanilla" then "Save & Close":
the game plays exactly as without the mod.


SETTINGS TABLE  (CombatMod.ini, section [Cinematic])
----------------------------------------------------
Key                      Shipped  Range      Meaning
SlowMo.Enabled           1        0 / 1      Slow-motion amplifier on / off
SlowMo.ScaleMul          0.5      0.05..2    Multiplies the game's slow-motion speed (lower = slower)
SlowMo.RampInMul         1.6      0.25..4    Length of the first part of each slow motion (slowing in)
SlowMo.RampOutMul        1.6      0.25..4    Length of the second part (easing back to normal)
SlowMo.MaxChainSeconds   6        1..30      Slow-motion cap per chain in real seconds (only shortens)
SlowMo.Gimmicks          1        0 / 1      Also stretch explosions / towers that you set off
BlindingFlash.Mode       0        0..2       0 = game's own, 1 = fast chain + slow finale, 2 = smooth per hit + slow finale
BlindingFlash.ChainSpeed 1        0.5..1     Mode 1: speed of the dashes (1 = normal; higher is locked, see Known limitations)
BlindingFlash.HitScale   0.5      0.2..1     Mode 2: slow-motion strength per hit (lower = slower)
BlindingFlash.HitRamp    1.5      0.5..3     Mode 2: ramp length per hit
BlindingFlash.FinaleScale 0.25    0.05..1    Modes 1 and 2: slow-motion strength of the finale
BlindingFlash.FinaleRamp 2.5      0.5..4     Modes 1 and 2: ramp length of the finale
Launch.Enabled           1        0 / 1      Launch boost on / off
Launch.Mult              2        0.5..20    Launch strength multiplier (1 = vanilla)
Launch.MinValue          25       0..60      Weak launches are raised to this BEFORE the multiplier; 0 = off
Launch.AngleMul          1        0.25..3    Launch angle multiplier (experimental)
Reaction.Finale          2        0..3       Hit reaction of the Blinding Flash finale: 0 = game's own, 1 = knock-down, 2 = ground throw, 3 = ragdoll fling
Reaction.TurningSlash    0        0..3       Same for Turning Slash hits (0 = game's own)
HitStop.Enabled          0        0 / 1      Hit-stop control on / off (experimental)
HitStop.ScaleMul         1        0.05..1    Lower = deeper impact dip
HitStop.DurMul           1        1..10      Longer impact pause
UI.PauseGame             1        0 / 1      Freeze the game while the panel is open
Triggers.Enabled         0        0 / 1      Your own rules from the [Triggers] section
Triggers.TowerBlast      1        0 / 1      Slow motion for tower / explosive-arrow explosions

Other sections: [UI] (Enabled, ToggleKeyboard, Alpha, StartOpen), [Advanced]
(rarely needed: skill lists that are never touched, the skill keys the hit
reactions look for, the tower curve and the chain-speed unlock; every line is
commented out and shows its default), [Triggers] (your own rules, with
an explanation in the file). [General] and [Telemetry] are developer sections:
leave them as shipped.

Edit the file with the game closed, or use the panel. If a value is out of
range or misspelled it is ignored (the default is used) and CombatMod.log says
so.


FAQ
---
Q: Do I need the ASI loader? A: Yes. See REQUIREMENTS.
Q: Is it safe for my save game? A: The mod never writes game or save files; it
   only changes values in memory while the game runs. Removing it restores
   the vanilla game. (Any mod carries risk; keep your own backups.)
Q: Will it get me banned? A: It is a single-player mod and does not touch any
   online feature, but nobody can promise that a game's anti-tamper system
   will never object to an ASI plugin. Do not use it online. Use at your own risk.
Q: Does it change enemy attacks or enemy slow motion? A: No. Only slow motion
   and launches caused by your own attacks. Your rolls / dodges never trigger it.
Q: The slow motion is too long / too strong. A: Open the panel: raise Scale
   towards 1, shorten Ramp in / out, or lower Slow-motion cap per chain. Or
   click Vanilla.
Q: Enemies fly too far / not far enough. A: Launch > Strength (x) and
   Minimum launch. Set Minimum launch to 0 for a plain multiplier. A high
   Minimum launch launches attacks that normally do not launch at all.
Q: Some attacks do not launch. A: Shielded or dying enemies are not launched
   by the game's own rules, and some skills have no launch at all.
Q: Does it work with other mods / ReShade / DMM? A: It only reads and changes
   memory; it does not touch game files, so file mods (DMM) do not conflict.
   Other ASI plugins that hook the same functions are not tested.
Q: Can I move the panel hotkey? A: Yes, [UI] ToggleKeyboard in CombatMod.ini.
Q: Where are the settings saved? A: CombatMod.ini next to CombatMod.asi.


TROUBLESHOOTING
---------------
Look at CombatMod.log (it is created next to CombatMod.asi and rewritten each
time the game starts). If you ask for help, attach it.

 * Nothing happens / the panel does not open:
     - Is version.dll (the ASI loader) in bin64, and are CombatMod.asi,
       CombatMod.ini and CombatMod.sigs in the SAME folder?
     - No CombatMod.log at all = the loader did not load the plugin.
     - Press Ctrl+Enter while the game window is in front. Another program may
       use that key: change [UI] ToggleKeyboard.
     - Use borderless or windowed mode (not exclusive fullscreen).
 * The game was updated and the mod does nothing: this is on purpose. The mod
   finds the game's functions by byte signatures and checks the game's data
   layout; when they do not match it stays inert instead of risking a crash.
   Open CombatMod.log: lines "sig ...: MISSING" or "AMBIGUOUS" mean the update
   moved something. Check the mod's page for an update that supports your game
   build. Meanwhile remove CombatMod.asi (or set Enabled=0 in [General]).
 * "Player: not known yet" in the Status area: attack once or twice; the mod
   learns who the player is from your attacks and changes nothing until then.
 * Slow motion works but Launch does nothing: only attacks that have a launch
   record are affected; the Status area counts the launch records seen.
 * The controller still moves the game while the panel is open: the game must
   read the pad through XInput; other controller APIs cannot be silenced.
 * The panel opens but the game does not pause (CombatMod.log has a line
   "WRITE REFUSED"): the mod checked the game's clock objects before
   touching them and they do not look right (a game update moved them). It
   refuses to write instead of risking a crash; nothing else is affected.
 * The first lines of CombatMod.log name the mod version and the game build
   (executable size and date). Please quote them when you ask for help.
 * Right after the game starts a feature may need a few seconds: hooks are
   installed in the background (up to 8 tries, 400 ms apart).
 * The ini was damaged, or "Save & Close" cannot write it (read-only file or
   a write-protected game folder): copy CombatMod.ini from the download again
   and make sure the file and folder are writable.


SUPPORTED GAME BUILDS
---------------------
Steam build IDs: 26308950, 26317560, 26329202.
(Look for "buildid" in steamapps\appmanifest_<number>.acf of the game, or ask
SteamDB.) An older build (for example 25477059) or a newer build that is not
listed is NOT supported by this file: the mod will usually stay inert.
You can tell by CombatMod.log. See CHANGELOG.txt for the history.


KNOWN LIMITATIONS
-----------------
 * Blinding Flash, Fast mode: the dash events of one chain share the game's
   global clock, so a speed above 1.0 would speed up everyone (enemies too);
   the mod cannot separate the player from enemies there. For that reason
   the chain speed is capped at 1.0. Advanced users can lift the cap with
   BlindingFlash.UnlockSpeedUp=1 in the [Advanced] section; this is untested.
 * Hit reactions (Finale throw, Turning Slash throw) change the reaction of
   your own hits for one call at a time. If the game decides the reaction at
   another moment the setting may have no visible effect; set it back to
   Native if you see anything odd.
 * Launch: shielded enemies and enemies that are dying are not launched by the
   game, so the boost has no effect on them.
 * Some skills (for example Turning Slash and Tiger's Fury) have special
   throw / launch handling in the game; the boost may do little or look
   different there. Explosive arrows and bombs are deliberately excluded.
 * An enemy's explosion shortly after one of your attacks cannot be told apart
   from yours and may be stretched too. Switch "Explosions and towers" off if
   that bothers you.
 * Ramp in / out and the launch angle are still being studied; treat them as
   experimental.
 * The panel needs borderless / windowed mode. Only XInput pads are
   silenced while it is open.
 * The mod does not support other games or the Xbox / console versions.


UNINSTALL
---------
Close the game and delete CombatMod.asi, CombatMod.ini, CombatMod.sigs (and
CombatMod.log) from bin64. If you installed it through DMM's ASI tab, remove it
there. Nothing else was changed. Keep or delete the ASI loader (version.dll)
as you like; other ASI mods may need it.


CREDITS AND LICENSE
-------------------
Cinematic Combat (CombatMod) by QuickkNastyy. Copyright (c) 2026 QuickkNastyy.
Open source under the MIT license. Source code and updates:
https://github.com/QuickkNastyy/CrimsonDesert-CinematicCombat
See LICENSE.txt.
Uses MinHook (BSD 2-Clause, Tsuda Kageyu) and HDE64 (Vyacheslav Patkov); see
THIRD_PARTY_NOTICES.txt. Thanks to the authors of the Ultimate ASI Loader and
to everyone who tested.
This is a fan-made mod. It is not made, endorsed or supported by the game's
developer or publisher. Crimson Desert is the property of its owners.
