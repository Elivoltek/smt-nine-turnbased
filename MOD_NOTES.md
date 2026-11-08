# SMT NINE turn-based mod: research notes

Goal: turn-based battles where the player picks each demon's skill and target,
then look at EXP from random encounters.

## Workflow

- `mod_rebuild.cmd` (repo root): recompiles changed runtime sources and relinks the
  English package only. No ISO or code regeneration needed. Linking takes 10-30 min.
- Play with the desktop shortcut "Shin Megami Tensei NINE" (Steam, for controllers).
- Instant states (F1-F5) do not survive a rebuild. Use ordinary saves.

## Research tools (added to vendor/xboxrecomp)

- F6 first press: start tracing. Opens `runtime/Trace/game_log_<pid>.txt`.
- F6 later presses: save `runtime/Trace/segment_NNN_<pid>.txt` = every guest
  function entered since the previous F6, with call counts.
- F7: save `runtime/Trace/memory_NNN_<pid>.bin` = full 64 MB guest RAM.
- Hook point: `recomp_cont_enter()` in `kernel/recomp_continuation.c`; every
  generated function calls it on entry with its guest address.

## Findings so far

- `sub_0014BA4B` is the game's debug printf, compiled to `xor eax,eax; ret` in
  retail; it is called from ~130 places. `kernel/mod_debug_log.c` formats its
  arguments into game_log while tracing is on (behavior unchanged).
- `sub_001974E8` prints the battle log header
  `BattleLog -Field %d Turn %d EngunNum %d Engun %d`, reading int16 globals at
  guest 0x587446 (turn - 1?), 0x587452, 0x587454. Target type strings nearby:
  ENEMY-SINGLE, ENEMY-FRONT_AND_NEAR, ENEMY-LINE, ENEMY-ALL, SELF,
  PLAYER-SINGLE, PLAYER-LINE, PLAYER-ALL, ALL (0x33B1AC-0x33B210).
- String "BattleID" is used at recomp_0054.c (around lines 67750/67840).

## Trace run 1 (2026-10-29, pid 9568)

- Tools work: 4 segments + 2 memory snapshots + game_log.
- Segment 3 (inside battle, ~28 s): 586 functions ran that did not run in the
  field before or after. Hottest: 0x165B13, 0x1C3695, 0x23EA5, 0x2377F, 0x23166,
  0x28EA3, 0x51445, 0x8E5E7, 0x13C13/0x13D40/0x24554, 0xAF161, 0x182EE8.
- Guest int16 0x587446 went 0 -> 1 between battle start and mid-battle (the
  BattleLog "Turn" value is this + 1). 0x587454 also 0 -> 1, 0x587450 0 -> 272.
  The battle engine counts turns internally.
- game_log during battle only shows "Chant 30" / "Chant 7" (caller 0x19C53A).
- Per Wikipedia, NINE awards no battle EXP by design (stats come from jewels),
  so random-encounter EXP would be a new feature, not a bug fix.

## Trace run 2: character table and skills (memory_003/004)

- Character table: base 0x589104, record size 0x264, indexed by int16
  (e.g. `SMEM16(0x589100)` * 0x264 + 0x589104). Up to ~200 entries.
  - entry 0 = Cait Sith (id 0x8C), entry 1 = Sphinx (id 0x39), entry 199 =
    protagonist (id 0x190; HP also referenced directly as 0x5A6E0C).
  - +0x000 int16 demon/character id
  - +0x002 int16 command mode? (-1 = AUTO; 0 = player-chosen skill)
  - +0x00A int16 selected skill id (Sphinx 292 = Rapid Fire, protagonist 253 =
    Tackle, Cait Sith on AUTO = 0). Matches the on-screen command column.
  - +0x14C int16 HP, +0x14E int16 MP (confirmed against screen values).
- Skill table in RAM (English build) near 0x449000-0x44C000: records of 0x70
  bytes: int16 id, then 32-byte name (twice), floats, description pointer.
  Rapid Fire id 0x124 at 0x44B6A8; Sealing Heaven Blade 0x125 at 0x44B718.
- Battle screen (user screenshot): left = party HP/MP, right = current command
  per member (skill name or AUTO) with a blue charge gauge.

## Battle loop (static analysis)

- Battle scene per-frame function: 0x1B68AF (called via vtable). Battle
  manager object pointer at guest 0x679C7C (allocated 0x15D00 bytes).
- 0x18BA1F = BattleManager::Update(int, float dt) (thiscall, ecx = manager);
  accumulates dt into manager+0x15648 and +0x22A8; calls per-unit update
  0x1C22B9 (7 units x frames in run 1).
- 0x18C068 = BattleManager::Draw(int, float dt) (calls D3D-range 0x2Exxxx).
- dt comes from float 0x5571F8, int arg from 0x557234.

## Step 1 build: battle wait (time stop)

- `kernel/mod_battle.c`: on entry to 0x18BA1F, if frozen, overwrite the guest
  dt argument ([esp+8]) with 0.0f. Toggle: P key, or LT+RT both > 200 (GameCube
  L+R full press). Auto-clears when no battle frame for 1.5 s.
- With tracing on (F6), every 8th battle frame is appended to
  `runtime/Trace/battle_rec_<pid>.bin`: 32-byte header ("BT9M", version 2,
  frame, manager address, manager size, party count, pad state, frozen flag),
  then the manager object (0x15D00 bytes), then 7 party records
  (entries 0-5 and 199, 0x264 bytes each).

## Trace run 3 (pid 21172): command menu

- Battle with menu closed (seg 2) vs menu open (seg 3): BattleManager::Update
  0x18BA1F keeps running at full rate while the menu is open (566 calls in 25 s),
  so retail battles do not pause for the menu.
- Functions that run only while the command menu is open: 0x19396E, 0x191824
  (about every 2nd frame), 0x19355D, 0x19A6AD, 0xA7AF1/0xA771D/0xA7BB5 (UI).
- Party records confirmed: Sphinx HP 225 -> 214, skill 292 set when chosen;
  Cait Sith skill 258; manager pointer 0x679C7C is 0 after the battle.

## Step 1b: auto wait

- `recomp_mod_battle_menu()` marks the frame when 0x19396E/0x191824 run;
  Update's dt is zeroed while the menu ran within the last 4 battle frames.
  Manual toggle (P / L+R triggers) still works on top.

## Live watch (20:50, screenshots): the battle is already turn-based inside

- On-screen battle text: "TURN n / 99", then "PHYSICAL PHASE 1" and
  "MAGICAL PHASE" per turn; pairing lines like "PLAYER-MAIN VS ENEMY-MAIN" and
  "ENEMY-MAIN VS PLAYER-SUB1"; per action "Gaki -> Cait Sith / Saliva LV 1",
  "Sphinx <DEFENSE>", "You -> Gaki / Tackle LV 1"; corners "ENEMY SKILL /
  PLAYER SKILL PA 1.0 MA 1.0", "P-REINFORCEMENT 1 / E-REINFORCEMENT 0".
- Turns took ~16-24 s each (turn 1 ~20:50:18, turn 2 ~20:50:42, turn 3 ~20:50:58).
- Party members in that battle also included Azumi and Baraki (story allies).

## Step 1c: turn pause

- When int16 0x587446 (turn index) changes or a new battle manager appears,
  `g_mod_battle_frozen` is set: each turn starts paused; the window notice says
  "TURN n - set commands, then L+R (or P) to start the turn".

## Live test 2 (21:27) and step 1d: full freeze

- Pause notice and menus worked while paused (Manual/Auto/Item/Summon/Retreat,
  per-demon list, Skill -> Rapid Fire). But HP still dropped and damage numbers
  appeared while "BATTLE PAUSED": zeroing Update's dt does not stop unit logic
  (chant/action counters are not all dt-based).
- Call structure (static + trace): battle frame 0x1B68AF calls once-per-battle
  init 0x18CD3E, then per frame Update 0x18BA1F and Draw 0x18C068. Inside Update:
  per-unit update 0x1C22B9 (thiscall, ret 16; 7 units/frame), menu logic
  0x1958EE -> 0x1950E9. Menu draw: Draw -> 0x195FD2 -> 0x19532F. Actions are
  callback-driven (e.g. Chant handler 0x19C526 is only referenced as a pointer).
- Fix: `config/recomp_types.h` RECOMP_ABI_CALL skips 0x1C22B9 (pops 20 bytes)
  while `g_mod_battle_skip` is set (paused or menu open). Every other call
  site folds to the original. mod_rebuild.py copies the header into the
  package's generated folder, so the whole game code recompiles once.
- T key toggles turn-based mode (turn-start pause) on/off.

## Step 2: auto menu (2026-10-30)

- User: A opens the battle menu (Manual / Auto / Item / Summon / Retreat).
- `input/xinput_device.c` `mod_inject()`: when `g_mod_tap_a` is set, port 0
  reports A at 255 for 6 polls then released for 6, with new packet numbers.
- `mod_battle.c`: after a turn-start freeze, tap A 20 battle frames later
  (90 for a new battle) if still paused and the command menu is not open.

## Live test 3 (2026-10-30 21:48): turn pause does not hold

- Auto menu fired (~2 s after "TURN 2 / 99" the Manual/Auto/... menu was
  open), so turn detection works, but the turn's actions kept playing while
  the menu was open; user confirms demons and allies kept attacking.
- Skipping 0x1C22B9 + zero dt does not stop the action presentation. The
  battle text ("X -> Y / Skill LV n") is drawn with sprite glyphs (strings
  such as "PHYSICAL PHASE" are not in the XBE). sub_001974E8 (BattleLog dump,
  only reached through a vtable) never ran in any trace: debug-only.
- Hypothesis: actions/presentation are driven by a scripted sequence or by
  other per-frame calls in Update (0x195D3D, 0x17F566, 0x188A56, 0x169C74,
  0x19F4E6, 0x17BFBD, 0x18ACDD) or Draw, not by unit timers.

## Research tool: battle timeline

- While tracing (F6) and a battle manager exists, the first 32 calls of each
  function per F6 segment are written to runtime/Trace/battle_events_<pid>.bin
  (9 x u32: "EV9M", wall ms, battle frame, function, call no, ecx, arg0-2),
  to line up action starts / damage / phase changes with screenshots.

## Goal agreed with user (2026-10-30)

- SMT-style: one action at a time; before each of the player's own units
  (protagonist + demons) acts, pause and let the player pick skill / item /
  target / pass. Story allies (Baraki, Azumi, ...) and enemies act in turn
  order on AI, also one action at a time, not in real time.

## Timeline run (2026-10-30 22:45, pid 20336) + screenshots: battle script

- Battle flow is a script interpreter: 0x169C74 (cdecl, called once per frame
  from Update 0x18BA1F) reads the int16 opcode at 0x587438 and calls the
  handler from the table at 0x3CA288 (8-byte entries: opcode, handler).
  Handlers get ecx = current script position (0x5600xx).
  Opcodes: 0x00 168392, 0x20 16842B, 0x28 16849E, 0x30 168904 (pairing
  setup), 0x50 16D81B (chant), 0x60 16FB34 (physical actions), 0x70 16859D,
  0x80 16FDFC (magical actions), 0x90 165111, 0x110 169B17, 0x120 16C548,
  0x1F0 1633C3, 0x250 16D4C7, 0x140 = battle end (checked after the call).
- Opcode 0x60: first frame 0x16C065 builds the action queue at 0x572FA0
  (0x18-byte entries: +0 unit ptr, +6 int16 state (0 = pending), +8 skill,
  +0xC target, +0x12 skill copy; count u32 at 0x557668). Then per action:
  0x16F6C1 selects the next pending entry, next frame 0x16FB21 performs it
  (0x16DD28; action counter int16 0x587440++), 0x169A63 finishes it.
- Battle units: 0x3D4 bytes each. Player main party 0x5D2488 + n*0x3D4
  (n < 5), story allies (PLAYER-SUB1) from 0x5D37AC, enemy main 0x5CEB18.
  0x1715E9(unit, ...) is called per unit during pairing setup 0x168904.
- Matches screenshots: 22:45:28 TURN 1, 22:45:30 first action (Cait Sith ->
  Gaki Jumping Kick), 22:45:32 You -> Gaki Tackle, 22:45:38 pairing ENEMY-MAIN
  VS PLAYER-SUB1 (Azumi, Baraki), 22:46:07 battle result (Bead).

## Step 3 (v2 turn mode): hold the script before player action phases

- recomp_types.h skip bit 2 = interpreter 0x169C74 (pops 4). mod_battle.c:
  hold when opcode == 0x60, the pairing contains a player-main unit, and the
  queue is not built yet; tap A to open the menu; L+R / P releases; also
  hold while the command menu is open. No dt/unit freezing any more.

## Live test 4 (2026-10-31 13:35): v2 works

- Battle held at TURN 1, menu auto-opened, user set Sphinx Rapid Fire ->
  Gaki, protagonist (shown as エリアス) Tackle, Cait Sith Spin Slash while
  nothing moved. Command submenu: Skill / Guard / Attack Magic /
  Heal/Support / Return; target select follows. User: "works really well".
- Battle menu party order seen: Sphinx (char entry 1), protagonist (entry 199),
  Cait Sith (entry 0).

## Requested next (user, 2026-10-31)

- Guided command flow: after a member's command, move to the next member;
  when all chose, end input and start the phase automatically.
- Pass / skip press turn.
- Items (medicine etc.) usable on demons and the protagonist.
- Press turns: player and enemies get press turns; enemies more if fast and
  strong (they attack often in real time); a critical hit grants one more
  turn, for both sides. Press-turn icons: original crystal design drawn for this mod (mod_assets/turn_icon_*.png)
  (purple = your side, pink = enemy).

## Research tool: queue blocks

- In battle_events_<pid>.bin, besides 36-byte "EV9M" records, every
  0x16F6C1 call writes a "QU9M" block: tag, battle frame, queue count,
  32 x 0x18 queue bytes from 0x572FA0, 0x7000 bytes of units from 0x5CE000.

## Research run 2026-10-31 14:15 (pid 11752): queue contents

- Queue entry (0x18): +0 unit, +4 int16 (-1 player-attack phase / 0 later),
  +6 int16 state (0 pending, -1 done), +8 int16 skill, +0xC target unit,
  +0x10 int16 (0x83 or 0), +0x12 skill copy (set when selected), +0x14 int16
  1 pending / 0 done. Capacity 10 (slot 10 = 0x573090 is other data).
- Each pairing queues only the attacking side: "PLAYER-MAIN VS ENEMY-MAIN"
  queued Sphinx (unit 0x5D2488, skill 292) and protagonist (0x5D285C, 253),
  later Cait Sith (0x5D2C30, 258); "ENEMY-MAIN VS PLAYER-SUB1" queued Gaki
  (0x5CEB18, skill 279 -> ally 0x5D37AC) and ally 0x5D3B80 (289).
- Unit -> character record: 0x17203A(?, record+2, unit) during pairing
  setup, in menu order. Unit +0x19A HP, +0x19C MP (Gaki 390/242).
- Battle Item menu: Medicine x5, Hiranya x5 seen. A heal effect followed.
- "Break" + 73 damage seen ~14:17:55; unique event then: 0x1C2EC0
  (ecx 0x1055AE4, arg 0x5CEB18). Crit flag not identified yet.

## Step 4 (v3): guided input + enemy press turns

- input: g_mod_taps sequence (1 A, 2 B, 3 down, 4 up).
- Guided: while holding, a change of a member's +0x2/+0xA triggers down+A
  (next member) or B,B after the last; turn starts when the menu has been
  closed 20 frames after any change. G toggles.
- Enemy press turns: first 0x16F6C1 of an enemy-led queue appends copies of
  enemy entries: +2 if enemy HP >= 3x party average HP, +1 if >= 1.5x.
  N toggles; notice shows "+n extra actions".

## v4 (2026-10-31): guided menu, single-target items, press-turn bar

- Battle manager menu state (from battle_rec_11752): +0x1564C screen (-1 closed,
  0 top/submenus, 2 member list, 3 target list), +0x15650 depth, +0x1565C cursor,
  +0x1565E member being commanded. Commands are written to the member's
  character record by 0x18F5E1 (target confirmed), 0x193842 (Guard) and inside
  0x1941AA (no-target skills). After a confirm the mod taps down x n, A to open
  the next member; after the last one B, B and the turn starts.
- Items: script opcode 0x280 (0x166E81) calls 0x10A324(record, amount, out) per
  member. The script is held at 0x280 while a target picker runs (input taken
  from the game); other members get amount code 0x7777 (no effect). H toggles.
- Press-turn bar: child window top-left, purple = your side, pink = enemy;
  counts from living units (+ enemy extra turns) or pending queue entries.
