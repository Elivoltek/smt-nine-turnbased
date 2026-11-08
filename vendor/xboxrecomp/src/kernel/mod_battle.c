/* SMT NINE turn-based mod, step 1: battle "wait" (time stop).
 *
 * The battle scene calls BattleManager::Update (guest 0x0018BA1F) once per
 * frame as Update(int, float dt); the manager object pointer is at guest
 * 0x679C7C. Rendering is a separate call (0x0018C068) with its own dt.
 * recomp_cont_enter() calls recomp_mod_battle_update() on entry to Update,
 * before the body reads its arguments, so writing 0.0f over the guest's dt
 * argument stops battle time for that frame while menus, input and drawing
 * keep running. The game itself passes dt = 0 on some frames, so zero is a
 * value it already handles.
 *
 * Auto menu: shortly after a turn starts paused, A is tapped once (the
 * player's own menu button), so the command menu is already open.
 *
 * Turn pause: at the start of each battle turn time stops until the player
 * resumes (P or L+R), so every turn begins with a chance to set commands.
 *
 * Auto wait: while the battle command menu is open, time is stopped too, so
 * choosing commands works like a turn-based game.
 *
 * While research tracing is on (F6), every 8th battle frame is also recorded
 * to the battle recorder file: the manager object plus the party records, for
 * locating action gauges and command-menu state offline. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "xbox_memory_layout.h"

extern RECOMP_TLS uint32_t g_esp;
extern ptrdiff_t g_xbox_mem_offset;

#define NINE_BATTLE_MANAGER_PTR 0x00679C7Cu
#define NINE_BATTLE_MANAGER_SIZE 0x15D00u
#define NINE_CHARACTER_TABLE 0x00589104u
#define NINE_CHARACTER_SIZE 0x264u

volatile long g_mod_battle_frames;   /* Update calls seen, for "in battle" */
volatile long g_mod_battle_frozen;   /* 1 = battle time stopped (manual toggle) */
volatile long g_mod_battle_menu_frame; /* battle frame when the command menu last ran */
volatile long g_mod_battle_auto_wait = 1; /* stop time while the command menu is open */
volatile long g_mod_battle_turn_pause = 1; /* stop time at the start of every battle turn */
volatile long g_mod_taps;                 /* requested button taps (input/xinput_device.c) */
volatile long g_mod_battle_auto_menu = 1; /* open the command menu at each turn start */
volatile long g_mod_battle_skip;          /* call-skip bits, see config/recomp_types.h */
volatile long g_mod_battle_turn = -1;     /* last seen turn (0-based), for the window notice */
#define NINE_BATTLE_TURN 0x00587446u     /* int16 turn index; on screen "TURN n / 99" = n-1 */
volatile uint32_t g_mod_pad_state;   /* buttons | LT<<16 | RT<<24, from the window thread */
FILE *volatile g_mod_battle_recorder;
FILE *volatile g_mod_event_log;          /* battle timeline (runtime/Trace/battle_events_<pid>.bin) */
extern RECOMP_TLS uint32_t g_ecx;
static long s_recorded_bytes;
#define RECORDER_LIMIT (400L * 1024 * 1024)

static int guest_range(uint32_t address, uint32_t size) {
    size_t total = g_xbox_total_ram ? g_xbox_total_ram : XBOX_TOTAL_RAM;
    return address >= 0x1000 && (size_t)address + size <= total;
}
static void *guest(uint32_t address) { return (void *)((uintptr_t)address + g_xbox_mem_offset); }

static void record_frame(long frame) {
    FILE *file = g_mod_battle_recorder;
    if (!file || (frame & 7) || s_recorded_bytes > RECORDER_LIMIT) return;
    uint32_t manager = *(uint32_t *)guest(NINE_BATTLE_MANAGER_PTR);
    if (!guest_range(manager, NINE_BATTLE_MANAGER_SIZE)) return;
    static const uint32_t party[] = {0, 1, 2, 3, 4, 5, 199};
    uint32_t header[8] = {0x4D395442u /* "BT9M" */, 2, (uint32_t)frame, manager,
                          NINE_BATTLE_MANAGER_SIZE, (uint32_t)(sizeof party / sizeof party[0]),
                          g_mod_pad_state, (uint32_t)g_mod_battle_frozen};
    fwrite(header, sizeof header, 1, file);
    fwrite(guest(manager), NINE_BATTLE_MANAGER_SIZE, 1, file);
    for (size_t i = 0; i < sizeof party / sizeof party[0]; i++)
        fwrite(guest(NINE_CHARACTER_TABLE + party[i] * NINE_CHARACTER_SIZE), NINE_CHARACTER_SIZE, 1, file);
    s_recorded_bytes += (long)(sizeof header + NINE_BATTLE_MANAGER_SIZE +
                               NINE_CHARACTER_SIZE * (sizeof party / sizeof party[0]));
}

void recomp_mod_battle_menu(void) { g_mod_battle_menu_frame = g_mod_battle_frames; }

/* ---- Turn-based mode (v2): hold the battle script ----------------------
 *
 * NINE's battle is driven by a small script interpreter (0x169C74, called
 * once per frame from Update). Its opcode is the int16 at 0x587438 and the
 * handler table is at 0x3CA288. Opcode 0x30 sets up a pairing ("PLAYER-MAIN
 * VS ENEMY-MAIN"), opcode 0x60 runs that pairing's actions one by one: on its
 * first frame 0x16C065 builds the action queue (0x572FA0, 0x18-byte entries,
 * count at 0x557668) from everyone's commands, then 0x16F6C1 selects the
 * next entry and 0x16FB21 performs it.
 *
 * The mod holds the interpreter (recomp_types.h, skip bit 2) right before an
 * action phase in which the player's own party (protagonist + demons, battle
 * units 0x5D2488 + n*0x3D4, n < 5) takes part, so commands can be set with
 * the game's own menu before the queue is built. Story allies and enemies
 * keep their AI. While the command menu is open the script is held too.
 * The units, the menus and drawing keep running, so nothing freezes mid-air. */
#define NINE_SCRIPT_OPCODE   0x00587438u
#define NINE_OP_ACTIONS      0x60
#define NINE_PLAYER_UNITS    0x005D2488u
#define NINE_UNIT_SIZE       0x3D4u
static int is_player_unit(uint32_t unit) {
    return unit >= NINE_PLAYER_UNITS && unit < NINE_PLAYER_UNITS + 5 * NINE_UNIT_SIZE &&
           (unit - NINE_PLAYER_UNITS) % NINE_UNIT_SIZE == 0;
}
static volatile long s_pair_has_player, s_queue_built, s_released, s_queue_tuned;
static void tune_queue(void);
static void note_party_member(void);
static void filter_party_entries(void);
static int drop_entry(uint8_t *entry);
static void queue_tracking_reset(void);
static void note_menu_confirm(uint32_t function);
static void item_effect(void);
static void guided_step(long frame, uint32_t manager);
static void menu_lock(long frame, uint32_t manager, int opcode);
void mod_guided_hold_started(void);
static void turn_counts_reset(void);
static volatile long s_queue_seq;
static int item_step(long frame, int opcode);
static void press_turn_counts(uint32_t manager, int opcode);

/* ---- v3: guided commands, press turns ----------------------------------
 * Party list: 0x17203A(?, &character_record->field_2, unit) runs once per
 * unit during pairing setup, in battle-menu order (Sphinx, protagonist,
 * Cait Sith in tests). Character records hold the command: +0x2 mode
 * (-1 AUTO), +0xA skill. When a member's command changes while the
 * holding menu is open, the menu moves on to the next member (D-pad down,
 * A); after the last member it closes (B, B) and the turn starts. */
#define MAX_PARTY 5
static uint32_t s_party_record[MAX_PARTY]; static long s_party_count;
static int16_t s_party_seen[MAX_PARTY][2];
static long s_touched, s_last_menu_frame, s_advance_at;
static volatile long s_hold_frame, s_menu_closed_frame, s_item_frame = -1000;
static long s_held_turn = -1, s_menu_seen_frame = -1000;
volatile long g_mod_input_block;  /* input/xinput_device.c: 1 = all buttons, 2 = B */
static long mod_strict_member(void);
static void mod_item_used_in_turn(void);
static void mod_mark_cleanup(void);
static void mod_debug_line(long frame, uint32_t manager, int opcode);
long mod_input_policy(void);
volatile long g_mod_debug;   /* D key: show the mod's menu state in the bottom line */
static void mark_party_acted(long index);
extern volatile long g_mod_input_capture;
static int mod_busy(void);
static void mod_top_back(void);
volatile long g_mod_user_buttons; /* input/xinput_device.c: 1 X, 2 Y, 4 B (player, before blocking) */
static void item_menu_watch(long frame, long screen, long depth, uint32_t manager);
static long s_item_chosen, s_in_items;
static void item_pick_start(void);
static void item_pick_step(void);  /* s_in_items: inside the Item menu (any screen) */
static int is_protagonist(long index);
volatile long g_mod_guided = 1;           /* start the turn when the battle menu closes (G toggles) */
volatile long g_mod_enemy_turns = 1;      /* extra enemy press turns (N key toggles) */
volatile long g_mod_last_extra;           /* extra enemy actions given last enemy phase */
volatile long g_mod_battle_phase;         /* 1 = waiting at a player action phase (for the notice) */

/* 0x168904: pairing setup starts. 0x1715E9 (unit, other, ...): one call per
 * unit in the pairing. 0x16C065: action queue being built. */
static void record_queue(void);
void recomp_mod_battle_hook(uint32_t function) {
    if (function == 0x0016F6C1u) { record_queue(); filter_party_entries(); tune_queue(); return; }
    if (function == 0x0017203Au) { note_party_member(); return; }
    if (function == 0x0018FB13u) { s_menu_closed_frame = g_mod_battle_frames; return; }
    if (function == 0x0018F5E1u || function == 0x00193842u || function == 0x001941AAu) { note_menu_confirm(function); return; }
    if (function == 0x0010A324u) { item_effect(); return; }
    if (function == 0x00168904u) { s_pair_has_player = 0; s_party_count = 0; }
    else if (function == 0x001715E9u) {
        uint32_t unit = guest_range(g_esp + 4, 4) ? *(uint32_t *)guest(g_esp + 4) : 0;
        if (is_player_unit(unit)) s_pair_has_player = 1;
    } else if (function == 0x0016C065u) { s_queue_built = 1; s_queue_tuned = 0; s_queue_seq++; queue_tracking_reset(); }
}

void recomp_mod_battle_update(void) {
    long frame = ++g_mod_battle_frames;
    static uint32_t last_manager;
    static long open_menu_at, was_frozen;
    uint32_t manager = *(uint32_t *)guest(NINE_BATTLE_MANAGER_PTR);
    long turn = *(int16_t *)guest(NINE_BATTLE_TURN);
    int opcode = *(uint16_t *)guest(NINE_SCRIPT_OPCODE);
    if (manager != last_manager) { s_pair_has_player = 0; s_queue_built = 0; s_released = 0; g_mod_battle_frozen = 0; s_held_turn = -1; }
    if (opcode != NINE_OP_ACTIONS) { s_queue_built = 0; s_released = 0; }
    /* The player let a held phase go (L+R / P): run it. */
    if (was_frozen && !g_mod_battle_frozen && g_mod_battle_phase) { s_released = 1; g_mod_battle_phase = 0; }
    /* Hold once per turn, at its first action phase (live test: turn 1 began
     * with "ENEMY-MAIN VS PLAYER-SUB1", which has none of the player's own
     * units, so holding only for the player's pairing came too late). The
     * commands chosen then are used when the party's own phase comes. */
    if (g_mod_battle_turn_pause && manager && opcode == NINE_OP_ACTIONS && turn != s_held_turn &&
        !s_queue_built && !s_released && !g_mod_battle_phase) {
        g_mod_battle_frozen = 1; g_mod_battle_phase = 1; s_held_turn = turn;
        open_menu_at = frame + 8;
    }
    if (!g_mod_battle_frozen) g_mod_battle_phase = 0;
    if (g_mod_battle_phase == 1) { /* hold just started: remember every command */
        for (long i = 0; i < s_party_count; i++) {
            s_party_seen[i][0] = *(int16_t *)guest(s_party_record[i] + 2);
            s_party_seen[i][1] = *(int16_t *)guest(s_party_record[i] + 0xA);
        }
        s_touched = 0; s_advance_at = 0; g_mod_battle_phase = 2; s_hold_frame = frame;
        mod_guided_hold_started(); turn_counts_reset();
    }
    int menu_now = frame - g_mod_battle_menu_frame <= 4;
    if (menu_now) s_last_menu_frame = frame;
    {
        long screen = guest_range(manager, NINE_BATTLE_MANAGER_SIZE) ? *(int16_t *)guest(manager + 0x1564Cu) : -1;
        long depth = guest_range(manager, NINE_BATTLE_MANAGER_SIZE) ? *(int16_t *)guest(manager + 0x15650u) : 0;
        long member = guest_range(manager, NINE_BATTLE_MANAGER_SIZE) ? *(int16_t *)guest(manager + 0x1565Eu) : -1;
        if (screen >= 0) s_menu_seen_frame = frame;
        /* Strict command step: the player cannot leave the member whose turn
         * it is. On the member list every button is ignored (the mod moves
         * the cursor itself). B in that member's commands leads to the top
         * menu (Item ...), where B is ignored; "Manual" there leads back to
         * the same member. */
        /* B (seen before the game gets it, see mod_input_policy) in the
         * current member's first command list or on the top menu: the mod
         * goes back one member itself, so the game never leaves that member. */
        static long held_b;
        long b_now = g_mod_user_buttons & 4;
        if (b_now && !held_b && g_mod_guided && g_mod_battle_phase == 2 && mod_strict_member() >= 0 &&
            !g_mod_taps && !mod_busy()) {
            if (screen == 0 && depth == 0 && !is_protagonist(mod_strict_member())) mod_top_back();
            else if (screen == 0 && depth == 3 && member == mod_strict_member()) g_mod_taps = 2; /* leave: handled on the list */
        }
        held_b = b_now;
        item_menu_watch(frame, screen, depth, manager);
    }
    item_pick_step();
    if (g_mod_battle_phase != 2) mod_mark_cleanup();
    if (g_mod_guided) guided_step(frame, manager);
    if (g_mod_debug && !(frame % 10)) mod_debug_line(frame, manager, opcode);
    if (g_mod_guided && g_mod_battle_turn_pause) menu_lock(frame, manager, opcode);
    if (open_menu_at && frame >= open_menu_at) {
        if (g_mod_battle_auto_menu && g_mod_battle_frozen && frame - g_mod_battle_menu_frame > 4)
            g_mod_taps = 1;  /* tap A: open the battle menu */
        open_menu_at = 0;
    }
    last_manager = manager; g_mod_battle_turn = turn; was_frozen = g_mod_battle_frozen;
    /* The command menu's per-frame functions (0x19396E / 0x191824) mark the
     * frame; they run about every 2nd battle frame. */
    int menu_open = g_mod_battle_auto_wait && frame - g_mod_battle_menu_frame <= 4;
    int item = item_step(frame, opcode); /* 1 = picking a target, 2 = item may run */
    g_mod_battle_skip = item == 1 ? 2 : item == 2 ? 0 :
                        (g_mod_battle_frozen || menu_open) ? 2 : 0; /* hold the script */
    press_turn_counts(manager, opcode);
    record_frame(frame);
}

/* Battle timeline for research: while tracing (F6) and a battle manager
 * exists, the first 32 entries of every guest function per F6 segment are
 * logged with the battle frame, wall-clock milliseconds, ecx (the "this"
 * object) and the first four stack arguments. Rare events -- turn start,
 * action start, damage, phase changes -- show up as short runs of entries
 * that can be lined up with screenshots. Record: 9 x uint32 little endian:
 * "EV9M", ms, battle frame, function, call number, ecx, arg0..arg2. */
void recomp_mod_event(uint32_t function, uint32_t count) {
    FILE *file = g_mod_event_log;
    if (!file || !*(uint32_t *)guest(NINE_BATTLE_MANAGER_PTR)) return;
    struct timespec now; timespec_get(&now, TIME_UTC);
    uint32_t sp = g_esp, record[9] = {0x4D395645u /* "EV9M" */,
        (uint32_t)((uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u),
        (uint32_t)g_mod_battle_frames, function, count, g_ecx, 0, 0, 0};
    for (int i = 0; i < 3; i++)
        if (guest_range(sp + 4 + 4 * i, 4)) record[6 + i] = *(uint32_t *)guest(sp + 4 + 4 * i);
    fwrite(record, sizeof record, 1, file);
}

/* Research for press turns: each time the next action is selected (0x16F6C1)
 * while tracing, append the whole action queue and the battle unit area to
 * the event log as one "QU9M" block: u32 tag, u32 battle frame, u32 queue
 * count, 32 x 0x18 bytes of queue (0x572FA0), then 0x7000 bytes of battle
 * units from 0x5CE000 (enemy main 0x5CEB18, player 0x5D2488, allies 0x5D37AC). */
static void record_queue(void) {
    FILE *file = g_mod_event_log;
    if (!file) return;
    uint32_t header[3] = {0x4D395551u /* "QU9M" */, (uint32_t)g_mod_battle_frames,
                          *(uint32_t *)guest(0x00557668u)};
    fwrite(header, sizeof header, 1, file);
    fwrite(guest(0x00572FA0u), 32 * 0x18, 1, file);
    fwrite(guest(0x005CE000u), 0x7000, 1, file);
}

static void note_party_member(void) {
    if (!guest_range(g_esp + 4, 12)) return;
    uint32_t record = *(uint32_t *)guest(g_esp + 8) - 2, unit = *(uint32_t *)guest(g_esp + 12);
    if (!is_player_unit(unit) || s_party_count >= MAX_PARTY || !guest_range(record, NINE_CHARACTER_SIZE)) return;
    for (long i = 0; i < s_party_count; i++) if (s_party_record[i] == record) return;
    s_party_record[s_party_count++] = record;
}

/* Press turns for enemies: when an enemy side's action queue has just been
 * built (first 0x16F6C1 of the phase), every enemy action is repeated for
 * each extra press turn the enemy earns, while the queue (10 slots) has room.
 * Extra turns grow with the enemy's might against the party: its HP against
 * the party's average HP (unit +0x19A). Speed is not mapped yet. */
#define NINE_QUEUE 0x00572FA0u
#define NINE_QUEUE_COUNT 0x00557668u
#define NINE_QUEUE_SLOTS 10
#define NINE_ENEMY_UNITS 0x005CEB18u
static int is_enemy_unit(uint32_t unit) {
    return unit >= NINE_ENEMY_UNITS && unit < NINE_ENEMY_UNITS + 5 * NINE_UNIT_SIZE &&
           (unit - NINE_ENEMY_UNITS) % NINE_UNIT_SIZE == 0;
}
static void tune_queue(void) {
    if (s_queue_tuned) return;
    s_queue_tuned = 1;
    if (!g_mod_enemy_turns || !g_mod_battle_turn_pause) return;
    uint32_t count = *(uint32_t *)guest(NINE_QUEUE_COUNT);
    if (!count || count > NINE_QUEUE_SLOTS) return;
    uint8_t *queue = (uint8_t *)guest(NINE_QUEUE);
    if (!is_enemy_unit(*(uint32_t *)queue)) return;
    long party_hp = 0, party_n = 0;
    for (uint32_t i = 0; i < 5; i++) {
        int16_t hp = *(int16_t *)guest(NINE_PLAYER_UNITS + i * NINE_UNIT_SIZE + 0x19A);
        if (hp > 0) { party_hp += hp; party_n++; }
    }
    long average = party_n ? party_hp / party_n : 1, added = 0;
    uint32_t original = count;
    for (uint32_t i = 0; i < original; i++) {
        uint8_t *entry = queue + i * 0x18;
        uint32_t unit = *(uint32_t *)entry;
        if (!is_enemy_unit(unit)) continue;
        long hp = *(int16_t *)guest(unit + 0x19A);
        long extra = hp >= 3 * average ? 2 : hp * 2 >= 3 * average ? 1 : 0;
        while (extra-- > 0 && count < NINE_QUEUE_SLOTS) {
            memcpy(queue + count * 0x18, entry, 0x18);
            count++; added++;
        }
    }
    *(uint32_t *)guest(NINE_QUEUE_COUNT) = count;
    g_mod_last_extra = added;
}

/* ---- v4: guided command input ------------------------------------------
 * Battle manager menu state (manager = *0x679C7C), from battle recordings:
 *   +0x1564C int16 menu screen: -1 closed, 0 top menu / member submenus,
 *            2 party member list, 3 target list
 *   +0x15650 int16 submenu depth (0 = none, 3 command list, 5 skill list)
 *   +0x1565C int16 cursor, +0x1565E int16 member being commanded
 * A command is written to the member's character record (unit +4) by
 * 0x18F5E1 (target confirmed), 0x193842 (Guard) or inside 0x1941AA (skills
 * that need no target); each is entered once per A press. After a confirm
 * the menu returns to the member list (+0x1564C = 2, depth 0) with the
 * cursor on the same member. Then the mod moves the cursor down to the next
 * member and opens their commands (A). After the last member it closes the
 * menu (B, B) and starts the turn. */
#define MENU_SCREEN 0x1564Cu
#define MENU_DEPTH  0x15650u
#define MENU_MEMBER 0x1565Eu
#define NINE_UNIT_GROUP 0x00587474u
static long s_confirm_frame, s_confirm_sure, s_confirm_member;
static int16_t s_confirm_before[3];
static long s_advance_frame, s_advance_taps;

static uint32_t party_unit(long index) {
    long group = *(int16_t *)guest(NINE_UNIT_GROUP);
    if (group < 0 || group > 1) group = 0;
    return NINE_PLAYER_UNITS + (uint32_t)(group * 5 + index) * NINE_UNIT_SIZE;
}
static uint32_t unit_record(uint32_t unit) {
    uint32_t record = *(uint32_t *)guest(unit + 4);
    return guest_range(record, NINE_CHARACTER_SIZE) ? record : 0;
}
static int unit_alive(uint32_t unit) {
    int16_t hp = *(int16_t *)guest(unit + 0x19A);
    return unit_record(unit) && hp > 0 && hp < 10000;
}
static void read_command(uint32_t record, int16_t out[3]) {
    out[0] = *(int16_t *)guest(record + 2);
    out[1] = *(int16_t *)guest(record + 0xA);
    out[2] = *(int16_t *)guest(record + 0xC);
}

static void note_menu_confirm(uint32_t function) {
    uint32_t manager = *(uint32_t *)guest(NINE_BATTLE_MANAGER_PTR);
    if (!guest_range(manager, NINE_BATTLE_MANAGER_SIZE) || !g_mod_guided) return;
    long member = *(int16_t *)guest(manager + MENU_MEMBER);
    if (member < 0 || member >= 5) return;
    /* A on the top menu (Manual / Auto / Item ...) is not a member's command;
     * "Auto" rewrites every record and must not count as a choice. */
    long screen = *(int16_t *)guest(manager + MENU_SCREEN), depth = *(int16_t *)guest(manager + MENU_DEPTH);
    if (screen == 0 && depth == 0) return;
    uint32_t record = unit_record(party_unit(member));
    if (!record) return;
    s_confirm_member = member;
    s_confirm_sure = function != 0x001941AAu;
    read_command(record, s_confirm_before);
    s_confirm_frame = g_mod_battle_frames;
}

/* Strict order (traditional SMT): while the turn is held, commands are given
 * member by member in menu order (Sphinx, protagonist, Cait Sith ...).
 *  - entering the member list opens the member whose turn it is;
 *  - opening any other member is undone (B) and the right one is opened;
 *  - backing out of a member's commands opens the same member again;
 *  - a confirmed command moves on; after the last member the turn starts. */
#define MOD_MARK 0x7A5A
static long s_mark_member = -1;
static int16_t s_mark_saved;
static void mark_restore(void) {
    uint32_t record = s_mark_member >= 0 ? unit_record(party_unit(s_mark_member)) : 0;
    if (record && *(int16_t *)guest(record + 0xE) == MOD_MARK) *(int16_t *)guest(record + 0xE) = s_mark_saved;
    s_mark_member = -1;
}
/* Live test: Dia aimed at Cait Sith (third row) healed the protagonist,
 * twice. The commit stores the ally target (+0xE) as the row in the menu
 * (Sphinx, protagonist, Cait Sith = battle unit order), but it is carried
 * out as an index into the party ordered by character record address
 * (Cait Sith entry 0, Sphinx entry 1, protagonist entry 199). For a single
 * ally target (+0xC == 5) the row is translated to that order. */
static long s_dbg_row = -99, s_dbg_val = -99, s_dbg_type = -99, s_dbg_cur = -99;
static uint32_t s_dbg_q[4]; static long s_dbg_qn;
static void remap_ally_target(uint32_t record) {
    uint32_t manager = *(uint32_t *)guest(NINE_BATTLE_MANAGER_PTR);
    s_dbg_type = *(int16_t *)guest(record + 0xC); s_dbg_row = *(int16_t *)guest(record + 0xE); s_dbg_val = -99;
    s_dbg_cur = guest_range(manager, NINE_BATTLE_MANAGER_SIZE) ? *(int16_t *)guest(manager + 0x156B0u) : -99;
    if (*(int16_t *)guest(record + 0xC) != 5) return;
    long row = *(int16_t *)guest(record + 0xE);
    uint32_t rows[5]; long n = 0;
    for (long i = 0; i < 5; i++) { uint32_t r = unit_record(party_unit(i)); if (r) rows[n++] = r; }
    if (row < 0 || row >= n) return;
    uint32_t chosen = rows[row];
    long rank = 0;
    for (long i = 0; i < n; i++) if (rows[i] < chosen) rank++;
    *(int16_t *)guest(record + 0xE) = (int16_t)rank;
    s_dbg_val = rank;
}
static long s_top_seen, s_unknown_since, s_top_reset, s_back_from_top = -1, s_list_idle, s_top_done, s_expected, s_wrong, s_cooldown, s_was_list, s_from, s_from_member, s_from_list, s_arrival, s_arrival_member;
enum { FROM_NONE, FROM_TOP, FROM_MEMBER };

static long first_member(long from) {
    for (long i = from; i < 5; i++) if (unit_alive(party_unit(i))) return i;
    return -1;
}
static long cursor_taps(long cursor, long target, long last) {
    long taps = 0, shift = 0;
    for (long i = cursor; i < target && shift < 24; i++, shift += 4) taps |= 3L << shift; /* down */
    for (long i = cursor; i > target && shift < 24; i--, shift += 4) taps |= 4L << shift; /* up */
    return taps | (last << shift);
}
static long mod_strict_member(void) { return s_expected; }
static int mod_busy(void) { return s_advance_frame != 0; }
/* The protagonist's turn uses the top menu (Manual = his commands, Item =
 * items): items only on his turn, as he is the one carrying them. */
static int is_protagonist(long index) {
    uint32_t record = index >= 0 ? unit_record(party_unit(index)) : 0;
    return record && *(uint16_t *)guest(record) == 400;
}
static long previous_alive(long before) {
    for (long i = before - 1; i >= 0; i--) if (unit_alive(party_unit(i))) return i;
    return -1;
}
/* Open a member from the member list: demons get their commands (cursor,
 * A); the protagonist gets the top menu (B), unless he was just chosen there
 * with "Manual". */
static void open_member(long target, long cursor, long frame, int via_top) {
    s_expected = target;
    if (is_protagonist(target) && !via_top) { s_top_reset = 1; s_advance_taps = 2; }
    else s_advance_taps = cursor_taps(cursor, target, 1);
    s_advance_frame = frame + 1; s_list_idle = 0;
}
static void mod_top_back(void) {
    long back = previous_alive(s_expected);
    if (back < 0 || g_mod_taps || s_advance_frame) return;
    s_back_from_top = back;   /* "Manual" leads to the list, which then opens it */
}
/* An item used during the command step is that member's action (as in
 * traditional SMT): the member passes (Guard, mode 9) and the next member
 * is up; after the last one the turn starts. */
static void mod_item_used_in_turn(void) {
    static long last_frame = -1000;
    if (g_mod_battle_frames - last_frame < 120) return;   /* one item, counted once */
    if (g_mod_battle_phase != 2 || s_expected < 0) return;
    last_frame = g_mod_battle_frames;
    /* No command change (that could cancel the queued item); marking the
     * member as having acted makes the queue filter drop any other action
     * of theirs this turn. */
    mark_party_acted(s_expected);   /* the item used up this member's press turn */
    long next = first_member(s_expected + 1);
    s_expected = next; s_top_done = 0; s_confirm_frame = 0;
    if (next < 0) g_mod_battle_frozen = 0;
}
static long s_pre_state;            /* item target picked during the command step: 1 picking, 2 picked */
static uint32_t s_pre_target;        /* 0 = whole party */
static char s_pre_name[24];
static long s_pre_buttons;
void mod_guided_hold_started(void) {
    s_pre_state = 0; s_item_chosen = 0;
    s_expected = first_member(0); s_wrong = 0; s_top_done = 0; s_confirm_frame = 0; s_was_list = 0; s_from = FROM_NONE; s_from_list = 0; s_arrival = FROM_NONE;
}

static void guided_step(long frame, uint32_t manager) {
    if (s_advance_frame) {
        if (frame >= s_advance_frame) { g_mod_taps = s_advance_taps; s_advance_frame = 0; s_cooldown = frame + 12; }
        return;
    }
    if (!guest_range(manager, NINE_BATTLE_MANAGER_SIZE)) return;
    long screen = *(int16_t *)guest(manager + MENU_SCREEN), depth = *(int16_t *)guest(manager + MENU_DEPTH);
    long member = *(int16_t *)guest(manager + MENU_MEMBER), cursor = *(int16_t *)guest(manager + 0x1565Cu);
    /* The member list is screen 2 whatever the depth: after backing out of a
     * member's commands it shows with depth 3 (that let the cursor move to
     * another member in live tests). */
    int at_list = screen == 2, arrived = at_list && !s_was_list;
    if (!at_list) s_list_idle = 0;
    if (arrived) { s_arrival = s_from; s_arrival_member = s_from_member; s_from = FROM_NONE; }
    /* Where the menu is coming from: the top menu, or a member's commands
     * (opened from the member list). */
    if (at_list) s_from_list = 1;
    else if (screen == 0 && depth == 0) { s_from = FROM_TOP; s_from_list = 0; }
    else if (screen >= 0 && s_from_list) { s_from = FROM_MEMBER; s_from_member = member; }
    s_was_list = at_list;
    int strict = g_mod_battle_phase == 2 && s_expected >= 0;
    if (g_mod_taps || frame < s_cooldown) return;
    if (s_in_items) { s_arrival = FROM_NONE; s_list_idle = 0; s_unknown_since = 0; return; } /* Item menu: the game's own */
    if (strict) {
        /* A screen the command step does not use (e.g. the "Auto / Tactics /
         * AI" submenu, reached by a stray A): the player's buttons are off
         * there, so the mod backs out itself (B) instead of leaving it stuck. */
        /* Every screen of the member whose turn it is counts (skill and magic
         * lists, all kinds of target selection -- Agilao's was cut off). */
        int known = screen < 0 || (screen == 0 && depth == 0) || screen == 2 ||
                    (member == s_expected && !(screen == 0 && depth > 0 && depth < 3));
        /* Left the top menu for anything but the member list on a demon's
         * turn (Auto, Item, Summon ...): not allowed -- straight back. */
        if (s_from == FROM_TOP && !(screen == 0 && depth == 0) && !at_list && screen >= 0 && !is_protagonist(s_expected)) known = 0;
        /* Protagonist's turn, coming from the top menu: Item (and its lists)
         * are his -- tested live: the Item list was backed out of at once. */
        if (s_from == FROM_TOP && !at_list && is_protagonist(s_expected)) known = 1;
        if (known) s_unknown_since = 0;
        else if (!s_unknown_since) s_unknown_since = frame;
        else if (frame - s_unknown_since > 2) { g_mod_taps = 2; s_cooldown = frame + 10; s_unknown_since = 0; return; }
    }

    /* Arm the commit marker when the current member's commands are open. */
    if (strict && screen == 0 && depth >= 3 && member == s_expected && s_mark_member != member) {
        uint32_t record = unit_record(party_unit(member));
        if (record) {
            if (s_mark_member >= 0) mark_restore();
            s_mark_saved = *(int16_t *)guest(record + 0xE);
            *(int16_t *)guest(record + 0xE) = MOD_MARK;
            s_mark_member = member;
        }
    }
    if (at_list && s_mark_member >= 0 && !s_confirm_frame) mark_restore();   /* left without a commit */
    if (s_confirm_frame) {
        if (frame - s_confirm_frame > 90) s_confirm_frame = 0;
        else if (at_list) {
            long done = s_confirm_member;
            int16_t now[3];
            uint32_t record = unit_record(party_unit(done));
            s_confirm_frame = 0;
            if (record) {
                read_command(record, now);
                /* Commit check: every command write sets +0xE to 0 (0x18F5E1,
                 * 0x193842, 0x1941AA all do), so a marker put there when the
                 * member's commands opened shows a commit even when the same
                 * command is chosen again (live test: Guard twice). */
                int marked = s_mark_member == done && *(int16_t *)guest(record + 0xE) != MOD_MARK;
                if (s_confirm_sure || marked || memcmp(now, s_confirm_before, sizeof now)) {
                    s_mark_member = -1;
                    remap_ally_target(record);
                    long next = first_member(done + 1);
                    if (next >= 0) {
                        open_member(next, cursor, frame, 0);
                    } else {
                        s_expected = -1;
                        s_advance_taps = 0x22;   /* B, B: close the menu */
                        if (g_mod_battle_phase == 2) g_mod_battle_frozen = 0; /* start the turn */
                    }
                    s_advance_frame = frame + 1; s_arrival = FROM_NONE;
                    return;
                }
            }
            /* no change ("Return"): handled as backing out below */
        } else return;
    }
    if (!strict) return;
    /* Someone else's commands were opened: close them again. */
    if (screen == 0 && depth > 0 && s_from_list && member != s_expected) {
        g_mod_taps = 2; s_wrong = 1; s_cooldown = frame + 20;
        return;
    }
    if (screen == 0 && depth == 0) {
        /* The top menu's cursor is +0x1565C, but only after the menu has been
         * up a few frames (right after opening it still holds an old value,
         * and writing it does not move the real selection: live tests landed
         * on "Summon" and "Auto"). So: wait, read it, press Up that many
         * times (Manual is the first entry), then A. */
        /* Live test with the debug line: the top cursor (+0x1565C) counts
         * from 1 (Manual 1, Auto 2, Item 3, Summon 4) -- the old 0-based
         * guess is what landed on Summon. On a demon's turn the mod picks
         * Manual itself: Up (cursor - 1) times, then A. */
        if (s_back_from_top >= 0) { s_expected = s_back_from_top; s_back_from_top = -1; }
        s_top_done = 1; s_top_reset = 0;
        if (!s_top_seen) s_top_seen = frame;
        if (!is_protagonist(s_expected) && frame - s_top_seen >= 8 && cursor >= 1 && cursor <= 4) {
            s_advance_taps = cursor_taps(cursor - 1, 0, 1); s_advance_frame = frame + 1; s_top_seen = 0;
            return;
        }
    } else s_top_seen = 0;
    if (!at_list) { s_list_idle = 0; return; }
    long target = -1;
    /* Safety net: if the mod has nothing to do on this list (it was reached
     * some way the mod does not know, e.g. from Summon, where the list may be
     * a different selection), the player's buttons come back after a short
     * wait instead of leaving the game stuck. Opening the wrong member is
     * still undone above. */
    if (!s_list_idle) s_list_idle = frame;
    if (frame - s_list_idle > 20) s_wrong = 1;   /* nothing else to do here: open the right member */
    int via_top = 0;
    if (s_wrong) { s_wrong = 0; target = s_expected; }
    else if (s_arrival == FROM_MEMBER) {
        if (is_protagonist(s_expected)) target = s_expected;   /* B in his commands: his top menu */
        else {
            long back = previous_alive(s_expected);             /* B: back to the previous member */
            target = back >= 0 ? back : s_expected;            /* (first member: stays) */
        }
    } else if (s_arrival == FROM_TOP) { target = s_expected; via_top = 1; } /* "Manual" */
    s_arrival = FROM_NONE;
    if (target >= 0) open_member(target, cursor, frame, via_top);
}

/* ---- v4: single-target items --------------------------------------------
 * Using an item runs script opcode 0x280 (handler 0x166E81), which calls
 * 0x10A324(character record, amount code, out) once per party member: the
 * whole party is healed. The mod holds the script at opcode 0x280 and lets
 * the player pick one member (D-pad, A; B = whole party as in the original).
 * Then 0x10A324 gets an amount code it does not know (= no effect) for
 * everyone else. Input is taken away from the game while picking. */
#define NINE_OP_ITEM 0x280
#define NINE_DUMMY_RECORD (NINE_CHARACTER_TABLE + 198u * NINE_CHARACTER_SIZE)
volatile long g_mod_input_capture;        /* input/xinput_device.c: game sees no input */
volatile long g_mod_captured_buttons;     /* 1 up, 2 down, 4 left, 8 right, 16 A, 32 B */
volatile long g_mod_single_items = 1;     /* H key toggles */
volatile long g_mod_message_seq;          /* video/fb_present.c shows g_mod_message */
char g_mod_message[160];
static long s_item_state, s_item_choice, s_item_buttons, s_item_count;
static uint32_t s_item_target, s_item_units[5];

/* Target panel (video/fb_present.c): names from the demon table (0x52-byte
 * records, English name at 0x3CD3C8 + id * 0x52; id 400 = the protagonist). */
#define NINE_DEMON_NAMES 0x003CD3C8u
volatile long g_mod_pick_active, g_mod_pick_count, g_mod_pick_choice, g_mod_pick_seq;
volatile long g_mod_pick_hp[5], g_mod_pick_mp[5];
char g_mod_pick_name[5][24];
static void member_name(uint32_t record, char *out, size_t size) {
    long id = *(uint16_t *)guest(record);
    if (id == 400 || id >= 1000) { snprintf(out, size, "You"); return; }
    const char *name = (const char *)guest(NINE_DEMON_NAMES + (uint32_t)id * 0x52);
    size_t n = 0;
    while (n + 1 < size && n < 0x20 && name[n] >= 0x20 && name[n] < 0x7F) { out[n] = name[n]; n++; }
    while (n && out[n - 1] == ' ') n--;
    out[n] = 0;
    if (!n) snprintf(out, size, "Member");
}
static void show_message(void) {
    for (long i = 0; i < s_item_count; i++) {
        uint32_t record = unit_record(s_item_units[i]);
        if (!record) continue;
        member_name(record, g_mod_pick_name[i], sizeof g_mod_pick_name[i]);
        g_mod_pick_hp[i] = *(int16_t *)guest(record + 0x14C);
        g_mod_pick_mp[i] = *(int16_t *)guest(record + 0x14E);
    }
    g_mod_pick_count = s_item_count; g_mod_pick_choice = s_item_choice;
    g_mod_pick_active = 1; g_mod_pick_seq++;
    snprintf(g_mod_message, sizeof g_mod_message, s_pre_state == 1 ? "ITEM: choose who gets it - Up/Down, A = use, B = back"
                                                                  : "ITEM: choose who gets it - Up/Down, A = use, B = whole party");
    g_mod_message_seq++;
}

static int item_step(long frame, int opcode) {
    if (opcode != NINE_OP_ITEM) {
        if (s_item_state) {
            s_item_state = 0; s_item_target = 0; g_mod_input_capture = 0; g_mod_pick_active = 0;
            *(int16_t *)guest(NINE_DUMMY_RECORD + 0x14C) = 0;
        }
        return 0;
    }
    s_item_frame = frame;
    if (s_item_state == 0 && s_pre_state == 2) {
        s_item_target = s_pre_target; s_pre_state = 0; s_item_chosen = 0; s_item_state = 2;
        if (s_item_target) snprintf(g_mod_message, sizeof g_mod_message, "Item used on %s", s_pre_name);
        else snprintf(g_mod_message, sizeof g_mod_message, "Item used on the whole party");
        g_mod_message_seq++;
        return 2;
    }
    if (s_item_state == 0) {
        s_item_count = 0;
        for (long i = 0; i < 5; i++) {
            uint32_t unit = party_unit(i);
            if (unit_record(unit)) s_item_units[s_item_count++] = unit;
        }
        /* Only items the player chose in the battle menu just now; items used
         * by AI (allies, AUTO) keep the original whole-party effect. */
        /* The target is now picked when the item is chosen (user: the panel
         * came up after his attack). An item step without that pick is not
         * the player's: it keeps the game's own effect. */
        int from_menu = s_item_chosen;
        if (from_menu && !s_item_chosen && (!g_mod_single_items || s_item_count < 2)) mod_item_used_in_turn();
        if (!g_mod_single_items || s_item_count < 2 || !from_menu) s_item_chosen = 0;
        if (!g_mod_single_items || s_item_count < 2 || !from_menu) { s_item_state = 2; s_item_target = 0; return 2; }
        s_item_state = 1; s_item_choice = 0; s_item_buttons = 0x3F; /* wait for release */
        g_mod_input_capture = 1;
        show_message();
    }
    if (s_item_state == 1) {
        long buttons = g_mod_captured_buttons, pressed = buttons & ~s_item_buttons;
        s_item_buttons = buttons;
        if (pressed & 5) { s_item_choice = (s_item_choice + s_item_count - 1) % s_item_count; show_message(); }
        if (pressed & 10) { s_item_choice = (s_item_choice + 1) % s_item_count; show_message(); }
        if (pressed & 16) { s_item_target = unit_record(s_item_units[s_item_choice]); s_item_state = 2; }
        else if (pressed & 32) { s_item_target = 0; s_item_state = 2; }
        if (s_item_state == 2) {
            g_mod_input_capture = 0; g_mod_pick_active = 0;
            if (!s_item_chosen) mod_item_used_in_turn();
            s_item_chosen = 0;
            if (s_item_target) snprintf(g_mod_message, sizeof g_mod_message, "Item used on %s", g_mod_pick_name[s_item_choice]);
            else snprintf(g_mod_message, sizeof g_mod_message, "Item used on the whole party");
            g_mod_message_seq++;
        }
        return s_item_state;
    }
    return 2;
}

static void item_effect(void) {
    if (s_item_state != 2 || !s_item_target || !guest_range(g_esp + 4, 8)) return;
    uint32_t record = *(uint32_t *)guest(g_esp + 4);
    /* Live test: an unknown amount code still healed 9 HP. So the heal is
     * sent to an empty character slot instead (entry 198, all zero in
     * memory dumps; its HP is cleared again when the item step ends). */
    if (record != s_item_target) *(uint32_t *)guest(g_esp + 4) = NINE_DUMMY_RECORD;
}

/* Target picker right when the protagonist chooses an item (menu closed from
 * the Item list): A picks the member and uses up his press turn, B goes back
 * to his menu (the next command he gives replaces the item). */
static void item_pick_start(void) {
    s_item_count = 0;
    for (long i = 0; i < 5; i++) { uint32_t unit = party_unit(i); if (unit_record(unit)) s_item_units[s_item_count++] = unit; }
    if (!g_mod_single_items || s_item_count < 2) {
        s_pre_state = 0; s_item_chosen = 1; mod_item_used_in_turn();
        snprintf(g_mod_message, sizeof g_mod_message, "Item chosen");
        g_mod_message_seq++;
        return;
    }
    s_pre_state = 1; s_item_choice = 0; s_pre_buttons = 0x3F;   /* wait for A to be released */
    g_mod_input_capture = 1;
    show_message();
}
static void item_pick_step(void) {
    if (s_pre_state != 1) return;
    if (g_mod_battle_phase != 2) { s_pre_state = 0; g_mod_input_capture = 0; g_mod_pick_active = 0; return; }
    long buttons = g_mod_captured_buttons, pressed = buttons & ~s_pre_buttons;
    s_pre_buttons = buttons;
    if (pressed & 5) { s_item_choice = (s_item_choice + s_item_count - 1) % s_item_count; show_message(); }
    if (pressed & 10) { s_item_choice = (s_item_choice + 1) % s_item_count; show_message(); }
    if (pressed & 16) {
        s_pre_target = unit_record(s_item_units[s_item_choice]);
        snprintf(s_pre_name, sizeof s_pre_name, "%s", g_mod_pick_name[s_item_choice]);
        s_pre_state = 2; s_item_chosen = 1;
        g_mod_input_capture = 0; g_mod_pick_active = 0;
        snprintf(g_mod_message, sizeof g_mod_message, "Item will be used on %s", s_pre_name);
        g_mod_message_seq++;
        mod_item_used_in_turn();
    } else if (pressed & 32) {
        s_pre_state = 0; g_mod_input_capture = 0; g_mod_pick_active = 0;
        snprintf(g_mod_message, sizeof g_mod_message, "Item cancelled - choose your action again");
        g_mod_message_seq++;
    }
}

/* ---- v4: press-turn icons -----------------------------------------------
 * Counts for the icon bar (video/fb_present.c). While commands are chosen:
 * one icon per living party member and story ally, and per living enemy
 * plus the extra turns it will earn. While actions run: the actions still
 * waiting in the queue for each side. */
volatile long g_mod_pt_player, g_mod_pt_enemy, g_mod_pt_show;
static long enemy_extra(uint32_t unit, long average) {
    long hp = *(int16_t *)guest(unit + 0x19A);
    if (!g_mod_enemy_turns) return 0;
    return hp >= 3 * average ? 2 : hp * 2 >= 3 * average ? 1 : 0;
}
/* Press turns per turn (traditional SMT): your side has one per living
 * party member (protagonist + own demons), the enemy one per living enemy
 * plus earned extras. An icon goes away when one of that side's actions has
 * been carried out. Action queues are rebuilt for every phase, so finished
 * entries are counted per queue (s_queue_seq changes with each new queue). */
static long s_turn_enemy_budget, s_enemy_used, s_counted_seq = -1, s_counted_done;
static unsigned s_party_acted;
static long s_drops[5];   /* actions dropped per member this turn */
/* Misses (traditional SMT): an attack that leaves the target's HP unchanged
 * costs that side one extra press turn -- the side's next waiting action in
 * this turn is dropped (queue entry +6 set to -1, "done"). Heals and buffs on
 * the own side, and the party's support magic (command mode 1), never count. */
#define MAX_QUEUE 32
typedef struct { uint32_t unit, target; int16_t ref, low; long until; } MissCheck;
static MissCheck s_checks[8];
static long s_penalty_party, s_penalty_enemy;
static int16_t s_entry_state[MAX_QUEUE], s_entry_hp[MAX_QUEUE];
static unsigned char s_entry_skipped[MAX_QUEUE];
static void turn_counts_reset(void) {
    s_party_acted = 0; s_enemy_used = 0; s_turn_enemy_budget = -1; s_counted_done = 0; s_counted_seq = s_queue_seq;
    s_penalty_party = 0; s_penalty_enemy = 0;
    for (int k = 0; k < 5; k++) s_drops[k] = 0;
    for (int k = 0; k < 8; k++) s_checks[k].until = 0;
}
static int is_side_unit(uint32_t unit, int party) {
    if (party) return unit >= NINE_PLAYER_UNITS && unit < NINE_PLAYER_UNITS + 10 * NINE_UNIT_SIZE;
    return unit >= NINE_ENEMY_UNITS && unit < NINE_PLAYER_UNITS;
}
static void drop_next_action(uint32_t count, int party) {
    for (long i = (long)count - 1; i >= 0; i--) {    /* the last waiting one of that side */
        uint8_t *entry = (uint8_t *)guest(NINE_QUEUE + (uint32_t)i * 0x18);
        uint32_t unit = *(uint32_t *)entry;
        if (*(int16_t *)(entry + 6) != 0 || !is_side_unit(unit, party)) continue;
        if (party && !is_player_unit(unit)) continue;  /* story allies keep their turns */
        drop_entry(entry); s_entry_skipped[i] = 1;
        if (party) s_penalty_party--; else s_penalty_enemy--;
        return;
    }
}
/* hp_ref: the target's HP when the previous action finished (or when the
 * queue was first seen) -- i.e. just before this action. While the action
 * runs and for 45 frames after it is marked done, the lowest HP is kept;
 * if it never went below hp_ref (and the target is alive), it was a miss. */
static int16_t s_entry_min[MAX_QUEUE];
static void check_misses(long frame, uint32_t count) {
    int finished = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t *entry = (uint8_t *)guest(NINE_QUEUE + i * 0x18);
        uint32_t unit = *(uint32_t *)entry, target = *(uint32_t *)(entry + 0xC);
        int16_t state = *(int16_t *)(entry + 6);
        int valid = (is_side_unit(target, 1) || is_side_unit(target, 0)) && (is_side_unit(unit, 1) || is_side_unit(unit, 0));
        if (!valid) { s_entry_state[i] = state; continue; }
        int16_t hp = *(int16_t *)guest(target + 0x19A);
        if (s_entry_hp[i] == -32768) { s_entry_hp[i] = hp; s_entry_min[i] = hp; }
        if (hp < s_entry_min[i]) s_entry_min[i] = hp;
        if (state == -1 && s_entry_state[i] != -1 && !s_entry_skipped[i]) {
            finished = 1;
            for (int k = 0; k < 8; k++) if (!s_checks[k].until) {
                s_checks[k] = (MissCheck){unit, target, s_entry_hp[i], s_entry_min[i], frame + 45};
                break;
            }
        }
        s_entry_state[i] = state;
    }
    if (finished)   /* everyone still waiting: their reference HP is "now" */
        for (uint32_t i = 0; i < count; i++) {
            uint8_t *entry = (uint8_t *)guest(NINE_QUEUE + i * 0x18);
            uint32_t target = *(uint32_t *)(entry + 0xC);
            if (*(int16_t *)(entry + 6) == -1 || !(is_side_unit(target, 1) || is_side_unit(target, 0))) continue;
            s_entry_hp[i] = s_entry_min[i] = *(int16_t *)guest(target + 0x19A);
        }
    if (s_penalty_party > 0) drop_next_action(count, 1);
    if (s_penalty_enemy > 0) drop_next_action(count, 0);
}
static void run_miss_checks(long frame) {
    for (int k = 0; k < 8; k++) {
        MissCheck *c = &s_checks[k];
        if (!c->until) continue;
        int16_t hp = *(int16_t *)guest(c->target + 0x19A);
        if (hp < c->low) c->low = hp;
        if (frame < c->until) continue;
        c->until = 0;
        int party = is_side_unit(c->unit, 1);
        if (party == is_side_unit(c->target, 1)) continue;                 /* own side: heal/buff */
        if (party) {
            uint32_t record = unit_record(c->unit);
            if (!is_player_unit(c->unit) || (record && *(int16_t *)guest(record + 2) == 1)) continue;
        }
        if (hp <= 0 || c->low < c->ref) continue;                           /* hit (or target down) */
        if (party) s_penalty_party++; else s_penalty_enemy++;
        snprintf(g_mod_message, sizeof g_mod_message, party ? "MISS! Your party loses 1 extra press turn" : "Enemy MISSED! The enemy loses 1 extra press turn");
        g_mod_message_seq++;
    }
}
static void press_turn_counts(uint32_t manager, int opcode) {
    static long last_turn = -1000;
    if (!manager || !g_mod_battle_turn_pause) { g_mod_pt_show = 0; last_turn = -1000; return; }
    long turn = *(int16_t *)guest(NINE_BATTLE_TURN);
    if (turn != last_turn) { last_turn = turn; turn_counts_reset(); }
    long alive = 0, party_hp = 0;
    for (uint32_t i = 0; i < 5; i++) {
        uint32_t unit = party_unit(i);
        if (unit_alive(unit)) { alive++; party_hp += *(int16_t *)guest(unit + 0x19A); }
    }
    long average = alive ? party_hp / alive : 1, enemies = 0;
    for (uint32_t i = 0; i < 5; i++) {
        uint32_t unit = NINE_ENEMY_UNITS + i * NINE_UNIT_SIZE;
        if (unit_alive(unit)) enemies += 1 + enemy_extra(unit, average);
    }
    if (s_turn_enemy_budget < 0) s_turn_enemy_budget = enemies;
    if (opcode == NINE_OP_ACTIONS && s_queue_built) {
        uint32_t count = *(uint32_t *)guest(NINE_QUEUE_COUNT);
        if (count > 32) count = 32;
        if (s_counted_seq != s_queue_seq) { s_counted_seq = s_queue_seq; s_counted_done = 0; }
        check_misses(g_mod_battle_frames, count);
        long done_enemy = 0;
        for (uint32_t i = 0; i < count; i++) {
            uint8_t *entry = (uint8_t *)guest(NINE_QUEUE + i * 0x18);
            uint32_t unit = *(uint32_t *)entry;
            if (*(int16_t *)(entry + 6) != -1 || (i < MAX_QUEUE && s_entry_skipped[i])) continue;
            for (uint32_t k = 0; k < 5; k++) if (unit == party_unit(k)) s_party_acted |= 1u << k;
            if (is_enemy_unit(unit)) done_enemy++;
        }
        if (done_enemy > s_counted_done) { s_enemy_used += done_enemy - s_counted_done; s_counted_done = done_enemy; }
    }
    run_miss_checks(g_mod_battle_frames);
    long player = 0;
    for (uint32_t k = 0; k < 5; k++) if (unit_alive(party_unit(k)) && !(s_party_acted & (1u << k))) player++;
    long enemy = s_turn_enemy_budget - s_enemy_used - (s_penalty_enemy > 0 ? s_penalty_enemy : 0);
    player -= s_penalty_party > 0 ? s_penalty_party : 0;
    g_mod_pt_player = player < 0 ? 0 : player;
    g_mod_pt_enemy = enemy < 0 ? 0 : enemy > 10 ? 10 : enemy;
    g_mod_pt_show = 1;
}

/* Menu lock: outside the command step (while a turn's actions play) the
 * battle menu is closed again as soon as it opens (B). During the command
 * step the menu cannot be closed early: it is opened again (A), so the turn
 * only starts once every member has an action. Item use (script opcode
 * 0x280) is left alone. */
static void menu_lock(long frame, uint32_t manager, int opcode) {
    static long next, closed_since;
    if (!guest_range(manager, NINE_BATTLE_MANAGER_SIZE) || opcode == NINE_OP_ITEM || g_mod_input_capture) return;
    if (frame < next || g_mod_taps || s_advance_frame) return;
    if (frame - s_item_frame < 60) return;
    long screen = *(int16_t *)guest(manager + MENU_SCREEN);
    if (g_mod_battle_phase == 0 && screen >= 0) {
        g_mod_taps = 2; next = frame + 8;           /* not your turn: close it */
    } else if (g_mod_battle_phase == 2 && s_expected >= 0 && screen < 0) {
        if (!closed_since) closed_since = frame;
        if (frame - closed_since > 60 && frame - s_hold_frame > 60) {
            g_mod_taps = 1; next = frame + 30; closed_since = 0;   /* your turn: keep it open */
        }
        return;
    }
    closed_since = 0;
}

/* Runs on every "next action" (0x16F6C1): later physical phases (opcode 0x70)
 * fill the queue without 0x16C065, so this cannot wait for a new queue.
 * The game runs a demon's default physical skill in the physical phases even
 * when its command is magic (Dia, Tarukaja, Agi ...); the magic itself comes
 * in the magical phase. Traditional SMT: one chosen action. So in a new queue
 * every own-party entry whose skill is not the member's chosen skill (record
 * +0xA, command mode not AUTO / Guard) is dropped, and so is every entry of a
 * member who already acted this turn (NINE has up to two physical phases). */
static void filter_party_entries(void) {
    uint32_t count = *(uint32_t *)guest(NINE_QUEUE_COUNT);
    if (!g_mod_battle_turn_pause || count > NINE_QUEUE_SLOTS) return;
    /* A queue filled some other way than 0x16C065: start its tracking over. */
    static uint32_t last_sig;
    uint32_t sig = count * 2654435761u;
    for (uint32_t i = 0; i < count; i++) sig = sig * 31u + *(uint32_t *)guest(NINE_QUEUE + i * 0x18) + *(uint16_t *)guest(NINE_QUEUE + i * 0x18 + 8);
    if (sig != last_sig) { last_sig = sig; queue_tracking_reset(); s_queue_seq++; }
    for (uint32_t i = 0; i < count; i++) {
        uint8_t *entry = (uint8_t *)guest(NINE_QUEUE + i * 0x18);
        uint32_t unit = *(uint32_t *)entry, record;
        int16_t state = *(int16_t *)(entry + 6);
        if (!is_player_unit(unit) || !(record = unit_record(unit))) continue;
        int16_t mode = *(int16_t *)guest(record + 2), skill = *(int16_t *)guest(record + 0xA);
        if (*(int16_t *)guest(record + 0xC) == 5 && mode >= 0 && mode != 9) {   /* debug: ally-target entry */
            s_dbg_q[0] = *(uint32_t *)(entry + 0xC); s_dbg_q[1] = *(uint32_t *)(entry + 0x10);
            s_dbg_q[2] = *(uint16_t *)guest(record + 0xE); s_dbg_q[3] = *(uint32_t *)(entry + 4); s_dbg_qn++;
        }
        int mismatch = mode >= 0 && mode != 9 && skill && *(int16_t *)(entry + 8) != skill;
        if (state == -1) {          /* dropped earlier: keep it marked after a tracking reset */
            if (mismatch && i < MAX_QUEUE) s_entry_skipped[i] = 1;
            continue;
        }
        if (state != 0) continue;
        int acted = 0;   /* one action per member per turn: later phases drop it */
        for (uint32_t k = 0; k < 5; k++) if (unit == party_unit(k) && (s_party_acted & (1u << k))) acted = 1;
        long k_index = -1;
        for (uint32_t k = 0; k < 5; k++) if (unit == party_unit(k)) k_index = k;
        /* Safety: if the game keeps bringing the same member back (the slot
         * could not be marked), let the action run rather than loop forever. */
        if ((acted || mismatch) && k_index >= 0 && s_drops[k_index] >= 3) continue;
        if (acted || mismatch) {
            /* Only drops whose source slot could not be marked count toward
             * the loop safety (live test: a magic user's physical slot comes
             * back in many phases and the old limit of 3 let it attack). */
            if (!drop_entry(entry) && k_index >= 0) s_drops[k_index]++;
            if (i < MAX_QUEUE) s_entry_skipped[i] = 1;
        }
    }
}

/* A new action queue is being built: per-entry tracking starts over (before
 * filter_party_entries marks entries it drops). */
static void queue_tracking_reset(void) {
    for (int i = 0; i < MAX_QUEUE; i++) { s_entry_state[i] = 0; s_entry_skipped[i] = 0; s_entry_hp[i] = -32768; }
}

/* Input policy, asked by the input layer on every controller poll (so no
 * button can slip through between two battle frames):
 *   1 = the game gets no buttons, 2 = no B, 4 = no A and no B, 0 = everything.
 * Command step: the member list and anything the mod is doing get nothing;
 * the current member's first command list and the top menu get no B (the
 * mod handles B); deeper menus (skills, targets, items) work normally.
 * Outside the command step, during the turn's actions: nothing. */
long mod_input_policy(void) {
    if (g_mod_input_capture || !g_mod_guided || !g_mod_battle_turn_pause) return 0;
    uint32_t manager = *(uint32_t *)guest(NINE_BATTLE_MANAGER_PTR);
    if (!guest_range(manager, NINE_BATTLE_MANAGER_SIZE)) return 0;
    /* Only while a battle is really running: the battle update must have run
     * within the last ~40 controller polls. Outside battles (or if the battle
     * is stuck) the player always has every button. */
    static long last_frames, quiet_polls;
    long frames = g_mod_battle_frames;
    if (frames != last_frames) { last_frames = frames; quiet_polls = 0; }
    else if (++quiet_polls > 40) return 0;
    /* A mod button sequence that never finished must not lock the pad. */
    static long busy_polls;
    if (g_mod_taps || s_advance_frame) { if (++busy_polls > 240) { g_mod_taps = 0; s_advance_frame = 0; busy_polls = 0; } }
    else busy_polls = 0;
    if (g_mod_battle_phase == 2 && s_expected >= 0) {
        long screen = *(int16_t *)guest(manager + MENU_SCREEN), depth = *(int16_t *)guest(manager + MENU_DEPTH);
        long member = *(int16_t *)guest(manager + MENU_MEMBER);
        if (g_mod_taps || s_advance_frame) return 1;
        /* User: spamming A right as the turn starts opened the menu for a
         * split second. Until the mod has opened the menu itself, A/B do
         * nothing. */
        if (screen < 0 && g_mod_battle_frames - s_hold_frame < 24) return 4;
        if (screen == 2) return 1;     /* member list (any depth): the mod moves the cursor */
        if (s_in_items) return 0;      /* the protagonist's Item menu works as in the game */
        if (screen == 0 && depth == 0) {
            /* Top level (top menu and the Item list): on the protagonist's turn
             * everything works -- B leaves the Item list, and B closing the
             * menu means "back a member" (item_menu_watch). Demon's turn: no B. */
            return is_protagonist(s_expected) ? 0 : 2;
        }
        if (s_from == FROM_TOP && screen != 2 && screen >= 0 && !is_protagonist(s_expected))
            return 1;
        if (s_from == FROM_TOP && screen != 2 && screen >= 0 && is_protagonist(s_expected))
            return 0;   /* the protagonist's Item menu */   /* a demon's turn: from the top menu only "Manual" counts, the mod backs out */
        if (screen == 0 && depth == 3 && member == s_expected) return 2;
        /* Deeper menus of the member whose turn it is (skills, magic, any
         * target selection) work; any other screen does not. */
        if (member == s_expected && screen != 2 && !(screen == 0 && depth > 0 && depth < 3)) return 0;
        return 1;
    }
    int opcode = *(uint16_t *)guest(NINE_SCRIPT_OPCODE);
    if (g_mod_battle_phase == 0 && opcode >= 0x20 && opcode <= 0x90) return 1;
    /* Between turns (turn start, chants, end-of-turn steps) A must not open
     * the menu early either -- but only while both sides still stand, so the
     * battle result screens keep every button. */
    if (g_mod_battle_phase == 0 && opcode != 0x140) {
        int party = 0, enemies = 0;
        for (uint32_t i = 0; i < 5; i++) {
            if (unit_alive(party_unit(i))) party = 1;
            if (unit_alive(NINE_ENEMY_UNITS + i * NINE_UNIT_SIZE)) enemies = 1;
        }
        if (party && enemies) return 4;
    }
    return 0;
}

/* Items on the protagonist's turn: the top menu's "Item" (cursor 2) was
 * entered and then the menu closed = an item was chosen. The game may use it
 * only when the turn's actions run, so it is remembered: it is the
 * protagonist's action now, and the target panel still opens when the item
 * is used, however late. */
static void item_menu_watch(long frame, long screen, long depth, uint32_t manager) {
    /* Battle recording (user's item test): the Item list is screen 0,
     * depth 4; choosing an item closes the menu (screen -1) straight from
     * there, and the item is only used when the turn's actions run (13528
     * vs. 12744). So: menu closing from (0,4) = an item was chosen. */
    static long last_screen = -1, last_depth = -1, b_frame = -1000;
    (void)manager;
    if (g_mod_user_buttons & 4) b_frame = frame;
    if (g_mod_battle_phase == 2 && s_expected >= 0 && is_protagonist(s_expected) &&
        last_screen == 0 && last_depth == 0 && screen < 0 && !g_mod_taps && !s_advance_frame &&
        frame - b_frame < 15) {
        /* closed with B: back to the previous member (the menu reopens) */
        mod_top_back();
    } else if (g_mod_battle_phase == 2 && s_expected >= 0 && is_protagonist(s_expected) &&
        last_screen == 0 && last_depth == 4 && screen < 0 && !g_mod_taps && !s_advance_frame) {
        /* User's test: B in the Item list also closes the menu from (0,4);
         * that was taken for an item, so he was asked for a target after his
         * attack. Only a close without B is an item; the target is chosen
         * right away and the item is his action for this turn. */
        if (frame - b_frame < 15) {
            snprintf(g_mod_message, sizeof g_mod_message, "Item menu closed - no item used");
            g_mod_message_seq++;
        } else item_pick_start();
    }
    last_screen = screen; last_depth = depth; (void)frame;
}

static void mark_party_acted(long index) { if (index >= 0 && index < 5) s_party_acted |= 1u << index; }

/* Drop a queued action as if it had been carried out. The queue holds copies
 * of the per-unit action slots (0x16C065 copies them from the two slot tables
 * 0x1671B2 returns into, at 0x56E3D0 and 0x5755E0). A carried-out action has
 * +6 = -1, +0x12 = its skill and +0x14 = 0. Only marking the copy left the
 * slot pending, so the game kept starting new physical phases for it (live
 * test: "PHYSICAL PHASE 33"); the matching slot is marked the same way. */
static int drop_entry(uint8_t *entry) {
    uint8_t original[0x18];
    memcpy(original, entry, sizeof original);
    int16_t skill = *(int16_t *)(entry + 8);
    *(int16_t *)(entry + 6) = -1; *(int16_t *)(entry + 0x12) = skill; *(int16_t *)(entry + 0x14) = 0;
    static const uint32_t tables[2] = {0x0056E3D0u, 0x005755E0u};
    for (int t = 0; t < 2; t++)
        for (uint32_t off = 0; off < 0x6000; off += 0x18) {
            uint32_t address = tables[t] + off;
            if (address >= NINE_QUEUE && address < NINE_QUEUE + 32 * 0x18) continue; /* the queue itself */
            uint8_t *slot = (uint8_t *)guest(address);
            if (memcmp(slot, original, 0x10)) continue;    /* unit, state, skill, target */
            *(int16_t *)(slot + 6) = -1; *(int16_t *)(slot + 0x12) = skill; *(int16_t *)(slot + 0x14) = 0;
            return 1;
        }
    return 0;
}

/* D key: the mod's menu state, for screenshots of problems. */
static void mod_debug_line(long frame, uint32_t manager, int opcode) {
    long screen = -9, depth = -9, member = -9, cursor = -9;
    if (guest_range(manager, NINE_BATTLE_MANAGER_SIZE)) {
        screen = *(int16_t *)guest(manager + MENU_SCREEN); depth = *(int16_t *)guest(manager + MENU_DEPTH);
        member = *(int16_t *)guest(manager + MENU_MEMBER); cursor = *(int16_t *)guest(manager + 0x1565Cu);
    }
    (void)frame; (void)screen; (void)depth; (void)member; (void)cursor;
    snprintf(g_mod_message, sizeof g_mod_message,
             "op%X t%ld row%ld val%ld tc%ld q%ld %lX %lX e%lX s%lX u%lX %lX %lX r%lX %lX %lX",
             opcode, s_dbg_type, s_dbg_row, s_dbg_val, s_dbg_cur, s_dbg_qn, (unsigned long)s_dbg_q[0], (unsigned long)s_dbg_q[1], (unsigned long)s_dbg_q[2], (unsigned long)s_dbg_q[3],
             (unsigned long)party_unit(0), (unsigned long)party_unit(1), (unsigned long)party_unit(2),
             (unsigned long)unit_record(party_unit(0)), (unsigned long)unit_record(party_unit(1)), (unsigned long)unit_record(party_unit(2)));
    g_mod_message_seq++;
}

static void mod_mark_cleanup(void) { if (s_mark_member >= 0) mark_restore(); }
