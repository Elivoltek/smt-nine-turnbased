/* Mod research: the game's own debug printf, made visible.
 *
 * SMT NINE's retail build keeps its debug-print calls (for example the
 * "BattleLog -Field %d Turn %d ..." line) but the print routine itself was
 * compiled down to "xor eax,eax; ret". recomp_cont_enter calls
 * recomp_mod_guest_printf() when that routine is entered; it formats the
 * guest's cdecl arguments and appends the line to the trace log that F6 opens.
 * The original routine still runs unchanged, so game behavior is identical. */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "xbox_memory_layout.h"

extern RECOMP_TLS uint32_t g_esp;
extern ptrdiff_t g_xbox_mem_offset;

FILE *volatile g_recomp_mod_log;
static volatile long s_logged_bytes;
#define MOD_LOG_LIMIT (64L * 1024 * 1024)

static int guest_ok(uint32_t address, uint32_t size) {
    size_t total = g_xbox_total_ram ? g_xbox_total_ram : XBOX_TOTAL_RAM;
    return address >= 0x1000 && (size_t)address + size <= total;
}
static uint32_t guest32(uint32_t address) {
    return guest_ok(address, 4) ? *(const uint32_t *)((uintptr_t)address + g_xbox_mem_offset) : 0;
}
static void guest_string(uint32_t address, char *out, size_t capacity) {
    size_t n = 0;
    while (n + 1 < capacity && guest_ok(address + (uint32_t)n, 1)) {
        char c = *(const char *)((uintptr_t)(address + (uint32_t)n) + g_xbox_mem_offset);
        if (!c) break;
        out[n++] = c;
    }
    out[n] = 0;
}

void recomp_mod_guest_printf(void) {
    FILE *log = g_recomp_mod_log;
    if (!log || s_logged_bytes > MOD_LOG_LIMIT) return;
    uint32_t sp = g_esp, caller = guest32(sp), arg = sp + 8;
    char format[512], line[2048], spec[32], text[512];
    guest_string(guest32(sp + 4), format, sizeof format);
    int used = snprintf(line, sizeof line, "%08X: ", caller - 5);
    for (const char *p = format; *p && used < (int)sizeof line - 1; ) {
        if (*p != '%') { line[used++] = *p++; continue; }
        if (p[1] == '%') { line[used++] = '%'; p += 2; continue; }
        size_t n = 0; spec[n++] = *p++;
        while (*p && strchr("-+ #0123456789.", *p) && n < sizeof spec - 4) spec[n++] = *p++;
        int wide = 0;
        while (*p && strchr("hlLqIjzt", *p)) { if (*p == 'l' && p[1] == 'l') wide = 1; p++; }
        char conversion = *p ? *p++ : 0;
        int wrote = 0, room = (int)sizeof line - used;
        if (conversion == 'd' || conversion == 'i' || conversion == 'u' || conversion == 'x' ||
            conversion == 'X' || conversion == 'o' || conversion == 'c') {
            spec[n++] = conversion; spec[n] = 0;
            uint32_t value = guest32(arg); arg += wide ? 8 : 4;
            if (conversion == 'd' || conversion == 'i') wrote = snprintf(line + used, room, spec, (int)value);
            else wrote = snprintf(line + used, room, spec, (unsigned)value);
        } else if (conversion == 'f' || conversion == 'g' || conversion == 'e' || conversion == 'G' || conversion == 'E') {
            spec[n++] = conversion; spec[n] = 0;
            uint64_t bits = (uint64_t)guest32(arg) | ((uint64_t)guest32(arg + 4) << 32); arg += 8;
            double value; memcpy(&value, &bits, sizeof value);
            wrote = snprintf(line + used, room, spec, value);
        } else if (conversion == 's') {
            spec[n++] = 's'; spec[n] = 0;
            guest_string(guest32(arg), text, sizeof text); arg += 4;
            wrote = snprintf(line + used, room, spec, text);
        } else if (conversion == 'p') {
            wrote = snprintf(line + used, room, "%08X", guest32(arg)); arg += 4;
        } else if (conversion) {
            wrote = snprintf(line + used, room, "%%%c", conversion);
        }
        if (wrote > 0) used += wrote < room ? wrote : room - 1;
    }
    while (used > 0 && (line[used - 1] == '\n' || line[used - 1] == '\r')) used--;
    if (used > (int)sizeof line - 2) used = (int)sizeof line - 2;
    line[used++] = '\n';
    fwrite(line, 1, (size_t)used, log);
    s_logged_bytes += used;
}
