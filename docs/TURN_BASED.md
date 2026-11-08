# SMT NINE – Turn-Based Edition

Shin Megami Tensei NINE (Xbox, 2002, Japan only) on Windows, **in English**, with **traditional SMT turn-based battles** instead of the original real-time system.

This is a fan project built on [smt-nine-recomp](https://github.com/gymzatan/smt-nine-recomp) by gymzatan (a native PC recompilation) and the [SMT NINE English patch](https://github.com/negative-entrophy/smt-nine-english-patch) by negative-entrophy. **No game data is included.** You need your own unmodified Japanese disc image of SMT NINE.

## Install

1. Download this project (green **Code** button → *Download ZIP*, or `git clone`) and unpack it to a folder with **25 GB free**, for example `D:\Games\SMT-NINE-TurnBased`.
2. Double-click **`SMT NINE Turn-Based.cmd`** and click **Install / Patch my game...**
   (or double-click `Patch-My-Game.cmd` directly).
3. Select your **Japanese SMT NINE ISO** (Redump dump, AT-002). The installer:
   - checks for Python 3.14, Rustup and Visual Studio 2022 Build Tools (C++) and offers to install missing ones with `winget`,
   - downloads the English patcher from its official GitHub release and verifies it,
   - makes an English copy of your ISO (your ISO is never changed),
   - builds the PC game from your disc, with the turn-based mod included,
   - installs English and selects it.
4. When it says **Done!**, click **Play** in the launcher.

The first install takes a long time (often 1–3 hours, depending on your PC), because the whole game is translated to native PC code on your machine. You only do this once. Saves are kept in the `save` folder.

### Updating

Downloaded with git: click **Update Turn-Based Edition** in the launcher (runs `git pull` and rebuilds). Downloaded as ZIP: download the new ZIP, unpack it over the old folder, then run **Install / Patch my game...** again.

## Turn-based battles

Each turn works like classic SMT:

- **One action per party member per turn.** Commands are given in order: first demon → protagonist → next demon. After you confirm a command the menu moves on by itself. **B** goes back to the previous member to change their command.
- You can't jump to another member's menu or skip ahead.
- **The protagonist's turn** shows the main battle menu (Manual / Auto / **Item** / Summon). Items can only be used on his turn and use up his press turn. When you pick a healing item, a box opens straight away so you can pick **who** gets it (Up/Down, A = use, B = back).
- **Press-turn icons** at the top left: one icon per member of your party, and the enemy's icons below. Using an action takes away one icon.
- **Missing costs an extra press turn** (for you and for the enemy). Strong enemies can get extra actions.
- The battle waits while you choose. The turn starts after the last member's command.

### Hotkeys (keyboard, in game)

| Key | Effect |
| --- | --- |
| **T** | Turn-based mode on/off (off = original real-time battles) |
| **G** | Auto start on/off (off = start the turn yourself with L+R or **P**) |
| **H** | Items: pick one member / whole party (original) |
| **N** | Enemy extra press turns on/off |
| **D** | Debug line (for bug reports) |

Gamepad and keyboard buttons are set in **Settings** (Config.exe).

## Known issues

- Single-ally spells (e.g. **Dia**) can heal a different party member than the one you picked. Items are not affected.
- Critical hits don't give an extra press turn yet.
- The game's own "PHYSICAL PHASE / MAGICAL PHASE" captions are still shown.
- A "MISSED" message can sometimes appear for fights between story allies and enemies.

Please report problems on the GitHub issues page with a screenshot (press **D** first so the debug line is visible).

## Credits

- **smt-nine-recomp** – gymzatan and contributors (MIT). The PC recompilation this edition is built on.
- **SMT NINE English patch** – negative-entrophy (MIT), based on **smt-nine-tools** by RayCharlizard (MIT).
- **Turn-based mod** – Elivoltek.
- Shin Megami Tensei NINE © Atlus / SEGA. This is an unofficial fan project, not affiliated with Atlus or SEGA.
