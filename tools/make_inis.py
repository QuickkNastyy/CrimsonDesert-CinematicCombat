#!/usr/bin/env python3
"""Regenerates dist\\CombatMod.ini (release), dist\\CombatMod.dev.ini and dist\\CombatMod.cinematic.ini from one template."""
import os

DIST = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'dist')

HEADER = """; CombatMod settings - place next to CombatMod.asi (bin64).
; Switches are 1/0. Everything under [Cinematic] can also be changed live (bridge: `cfg set <key> <value>`) and saved back here (`cfg save`).
"""

GENERAL = """
[General]
Enabled=1
; {bridge_comment}
BridgeEnabled={bridge}
BridgePort=6400
; 1 = initialise MinHook at startup (not needed: hooks are installed on demand, only for features that are switched on).
EnableHooks=0
; 1 = start the time-curve CSV sampler at load (developer tool).
SampleTimeCurve=0
; Wait this long before scanning signatures / opening the bridge (ms).
InitDelayMs=0
; Developer capability gates for the bridge commands (write/patch, trace/etrace/skills/amp-on, watch, key/keys). Keep 0 in a release.
AllowWrite={allow}
AllowTrace={allow}
AllowWatch={allow}
AllowInput={allow}
; call: runs game functions on a plugin thread - dangerous, keep 0.
AllowCall=0

[UI]
; The in-game control panel "Cinematic Combat" (opens with the hotkey below or LB + DPad-Down on an Xbox pad).
Enabled=1
ToggleKeyboard=Ctrl+Enter
; Window opacity 60..255.
Alpha=240
; (The panel has two close buttons: "Save & Close" writes the settings here, "Close without saving" undoes every change made since it opened.)
; 1 = open the panel by itself shortly after the game starts.
StartOpen=0

[Telemetry]
; Patch-diff database: one JSON file per session (event catalog, skill keys, launch records, slow-motion layouts, signatures) written to Dir,
; refreshed every 60 s. Compare two sessions after a game patch with tools/telemetry_diff.py. Costs a few KB/s of hook work and < MaxMB of disk.
Enabled=1
; Empty = CombatMod_telemetry next to the .asi
Dir=
MaxMB=20

[Skills]
; 1 = start skill-context capture at load (also started automatically when a feature needs it).
Enabled=0

[TimeAmp]
; Developer-only extras of the slow-motion engine (the user-facing values live in [Cinematic]). Uncomment to change.
; ContextWindowMs=800
; ContextAnyThread=1
; RequireCtx=0
; AllowKeys=10253,10258
; OnlyOwner=auto
; ScaleOverrides=0.6:0.35,0.1:0.04
"""

# key, label, range text, default, cinematic value
CINEMATIC = [
    ("SlowMo.Enabled", "Slow-motion amplifier on/off", "0/1", "0", "1"),
    ("SlowMo.ScaleMul", "Slow-motion strength: multiplies the native time scale (lower = slower)", "0.05..2", "1", "0.5"),
    ("SlowMo.RampInMul", "Ramp-in length multiplier (event timing A)", "0.25..4", "1", "1.6"),
    ("SlowMo.RampOutMul", "Ramp-out length multiplier (event timing B)", "0.25..4", "1", "1.6"),
    ("SlowMo.MaxChainSeconds", "Slow-motion cap per chain: slow motion that has been continuous for this many real seconds (hits < 1.5 s apart) stays at normal speed for the rest of the chain (it only shortens)", "1..30", "6", "6"),
    ("BlindingFlash.Mode", "Blinding Flash mode: 0 = off, 1 = fast chain + slow finale, 2 = smooth slow motion per hit + slow finale (works with every weapon)", "0..2", "0", "0"),
    ("BlindingFlash.ChainSpeed", "Mode 1: speed of the dashes between targets (1.0 = normal; above 1.0 needs the [Advanced] unlock)", "0.5..1", "1", "1"),
    ("BlindingFlash.HitScale", "Mode 2: multiplies the scale of each per-hit slow motion (lower = slower)", "0.2..1", "0.5", "0.5"),
    ("BlindingFlash.HitRamp", "Mode 2: multiplies the timings of each per-hit slow motion", "0.5..3", "1.5", "1.5"),
    ("BlindingFlash.FinaleScale", "Modes 1 and 2: multiplies the scale of the finale (lower = slower)", "0.05..1", "0.25", "0.25"),
    ("BlindingFlash.FinaleRamp", "Modes 1 and 2: multiplies the timings of the finale", "0.5..4", "2.5", "2.5"),
    ("SlowMo.Gimmicks", "Also amplify the slow motion of explosions / towers that YOU set off (a gimmick event right after a skill / arrow of yours)", "0/1", "1", "1"),
    ("Launch.Enabled", "Launch (knockback) boost on/off", "0/1", "0", "1"),
    ("Launch.Mult", "Launch strength multiplier", "0.5..20", "1", "2"),
    ("Launch.MinValue", "Minimum launch: attacks with less (or no) launch are raised to this BEFORE the multiplier; 0 = off (a high value makes attacks that normally do not launch at all launch enemies)", "0..60", "0", "0"),
    ("Reaction.Finale", "Hit reaction of the Blinding Flash finale: 0 = the game's own, 1 = knock-down, 2 = ground throw, 3 = ragdoll fling (for that single hit only)", "0..3", "0", "2"),
    ("Reaction.TurningSlash", "Hit reaction of Turning Slash hits shortly after you start it: 0 = the game's own, 1 = knock-down, 2 = ground throw, 3 = ragdoll fling", "0..3", "0", "0"),
    ("Launch.AngleMul", "Launch angle multiplier (experimental: scales the two angle floats of the launch record)", "0.25..3", "1", "1"),
    ("HitStop.Enabled", "Hit-stop control on/off (the tiny near-1.0 impact pauses)", "0/1", "0", "0"),
    ("HitStop.ScaleMul", "Hit-stop strength: multiplies the scale of an impact pause (lower = deeper dip)", "0.05..1", "1", "1"),
    ("HitStop.DurMul", "Hit-stop length: multiplies the duration of an impact pause", "1..10", "1", "1"),
    ("UI.PauseGame", "Freeze the game world while the control panel is open", "0/1", "1", "1"),
    ("Triggers.Enabled", "Custom slow-motion triggers on/off (rules in the [Triggers] section below)", "0/1", "0", "0"),
    ("Triggers.TowerBlast", "Slow motion for tower / explosive-arrow explosions (a built-in trigger on the player's arrow record 1091:5; curve in [Advanced])", "0/1", "0", "1"),
]


# the developer ini switches the tower slow motion on (the cinematic ini does through its preset column)
DEV_OVERRIDE = {"Triggers.TowerBlast": "1"}


def build(release, cinematic):
    out = HEADER
    if cinematic:
        out += "; PRESET 'cinematic': slow-motion x0.5 with 1.6x longer ramps, launch x2.\n"
    out += GENERAL.format(
        bridge="0" if release else "1",
        bridge_comment=("Release: the developer TCP bridge (127.0.0.1) is OFF." if release else "Developer build: the loopback TCP bridge is ON (python tools/bridge_client.py); it ALSO needs the marker file CombatMod.dev next to the .asi - without it BridgeEnabled=1 is ignored."),
        allow="0" if release else "1")
    out += "\n[Cinematic]\n"
    for key, label, rng, default, cine in CINEMATIC:
        val = cine if cinematic else default
        if not release and key in DEV_OVERRIDE:
            val = DEV_OVERRIDE[key]
        out += "; %s (%s; default %s)\n%s=%s\n" % (label, rng, default, key, val)
    out += """
[Advanced]
; Not shown in the panel. Skill keys (roll / evade skills) that never trigger the slow-motion amplifier; comma list, empty = none.
SlowMo.DenyKeys=10010,10031,10072,10278,10279,10086
; 1 = a custom trigger may start even while another time effect (a native slow motion) is running and replace it.
Triggers.Override=0
; Records that are NEVER boosted or raised by the launch boost: entries "key" (any of the 4 SkillKeys) or "key:slot" (first key + second key value).
; 1091:5 = the player's explosive arrow, 60002 = the bomb / tower gimmick. (The greatsword fire / ice hits 1091/2/1105 are not affected.)
Launch.DenyKeys=1091:5,60002
; Blinding Flash (skill key 10276): the finale of a chain is recognised by the ctx key0 of its event (1023 = Damage_Percent_IgnoreFatal on the weapons seen
; so far). Every other event of the chain is a per-hit event. Add the key0 values of other weapons here (live: `cfg set BlindingFlash.FinaleKeys 1023,1024`);
; `bf log` in the developer bridge shows the key tuples of the last chains per weapon. A chain without any of these keys is finale-less (bf_nofinale).
BlindingFlash.FinaleKeys=1023
; 1 = Blinding Flash Chain speed may go above 1.0. WARNING: that speeds up EVERYONE (enemies, NPCs) because the game has one global clock.
BlindingFlash.UnlockSpeedUp=0
; Skill keys whose use starts the Turning Slash window for Reaction.TurningSlash (10253; add the fist variant's key here).
Reaction.TurningSlashKeys=10253
; The built-in tower / explosive-arrow slow motion (Triggers.TowerBlast): curve scale, ramp in, hold, ramp out (seconds) and cooldown (ms).
; A = in + hold, B = out (provisional mapping onto the engine's time controller).
Triggers.TowerScale=0.1
Triggers.TowerIn=0.03
Triggers.TowerHold=0.35
Triggers.TowerOut=1.4
Triggers.TowerCooldownMs=3000
; Gimmick owners (bombs, towers: Gimmick_Active_Bomb = 60002) whose slow motion counts as yours when you used a skill / arrow shortly before.
SlowMo.GimmickKeys=60002
; ... "shortly before" = this many ms (200..5000).
SlowMo.GimmickWindowMs=1500

[Triggers]
; Custom slow-motion triggers (needs Triggers.Enabled=1): the game has NO native slow motion for some moments. A rule starts OUR OWN curve through the
; engine's time controller when the PLAYER's skill call carries one of the SkillKeys (never for NPCs). One line per rule:
;   Trigger<n> = key=<SkillKey list>|owner=player|scale=0.15|in=0.10|hold=0.5|out=0.45|cooldownMs=1500|firstOnly=1|delayMs=0|name=<text>
; The curve mapping is provisional: the engine only has (scale, A, B), so A = in + hold and B = out. Optional: lastOnly=1|idleMs=250 (fire on the LAST hit of a
; chain instead of the first), enabled=0. Find skill keys with `skills log` / `trigger log`.
; NOTE: the Blinding Flash finisher (SkillKey 10276) and bomb explosions (gimmick key 60002) already have native slow motion: the amplifier handles
; them (SlowMo.Enabled, SlowMo.Gimmicks). Tower / explosive-arrow explosions have NONE: Triggers.TowerBlast adds it with a built-in rule (key 1091:5).
; In rules, "key" matches any of the 4 SkillKeys and "key:slot" (e.g. 1091:5) matches only records whose first key is `key` and second key is `slot`.
; Example (disabled): Trigger1 = name=Example|enabled=0|key=12345|owner=player|scale=0.2|in=0.1|hold=0.4|out=0.3|cooldownMs=1500|firstOnly=1|delayMs=0
"""
    return out


def main():
    files = {
        'CombatMod.ini': build(True, False),
        'CombatMod.cinematic.ini': build(True, True),
        'CombatMod.dev.ini': build(False, False),
    }
    for name, text in files.items():
        with open(os.path.join(DIST, name), 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
        print('wrote', name)
    with open(os.path.join(DIST, 'CombatMod.dev'), 'w', encoding='ascii', newline=chr(10)) as f:   # developer marker: BridgeEnabled=1 only opens the bridge while this file sits next to the .asi
        f.write('developer marker: BridgeEnabled=1 opens the loopback bridge only while this file is next to CombatMod.asi. Never ship it.' + chr(10))
    print('wrote CombatMod.dev')


if __name__ == '__main__':
    main()
