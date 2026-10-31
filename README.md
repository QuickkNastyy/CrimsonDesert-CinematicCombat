# Cinematic Combat (CombatMod) for Crimson Desert

Single-player ASI plugin: smoother slow motion on your own attacks, stronger knock-back, consistent
Blinding Flash / Turning Slash throws, tower-explosion slow motion, and an in-game settings panel
(**Ctrl+Enter**, pauses the game).

Not affiliated with or endorsed by Pearl Abyss. Single-player only.

## Install (players)

1. Download the loader: [version-x64.zip](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/x64-latest/version-x64.zip)
   (Ultimate ASI Loader, already named `version.dll`). Put `version.dll` in the game's `bin64` folder
   (the folder with `CrimsonDesert.exe`). Skip this if you already have it.
2. Download the mod zip from the Releases page (or the Nexus page when it is up) and drag
   `CombatMod.asi`, `CombatMod.ini` and `CombatMod.sigs` into the same `bin64` folder.
3. Start the game, load in, press **Ctrl+Enter**.

Uninstall: delete those three files. The plugin only changes memory while the game runs.

After a game update the plugin finds the game functions again by byte signatures; if something no
longer matches it does nothing (check `CombatMod.log`) instead of crashing.

## Build

Visual Studio 2026 (MSVC + the bundled CMake and Ninja, x64 tools, MASM):

    toolsuild.bat

Output: `build\CombatMod.asi`. See `release/README.txt` for the full user guide and settings, and
`tools/` for the offline test harness (`harness.bat`) and the release builder (`make_release.py`).

## Support

If you like it: a star on this repo is appreciated but never required.
[Buy me a coffee](https://buymeacoffee.com/quickknastyy). My other mod:
[Infinite Durability / No Cooldowns / Stamina / Spirit](https://www.nexusmods.com/crimsondesert/mods/2714).

## License

MIT, see `LICENSE`. MinHook (BSD-2-Clause) and HDE64 keep their own licenses
(`release/THIRD_PARTY_NOTICES.txt`).
