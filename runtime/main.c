#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdarg.h>
#include "gb.h"
#include "widescreen.h"
#include "rom.h"
#include "settings.h"
#include "rom.h"
#include "cart.h"
#include "emu.h"
#include "events.h"
#include "audio.h"
#include "app.h"

static void serial_print(uint8_t c) { putchar(c); fflush(stdout); }

static void usage(const char *a0)
{
    fprintf(stderr,
        "usage: %s [game.gb]\n"
        "  (no arguments)    open the launcher\n"
        "  game.gb           start that game right away (Dr. Mario, Super Mario Land, Super Mario Land 2)\n"
        "  --hack FILE       apply a romhack (.ips/.bps/.ups or patched .gb)\n"
        "developer options (run without a window):\n"
        "  --headless        run the game without video/audio\n"
        "  --frames N        stop after N video frames\n"
        "  --interp          run everything in the interpreter instead of the recompiled code\n"
        "  --fuzz SEED       press random buttons (deterministic)\n"
        "  --script FILE     lines of '<frame> <hexmask>': A=1 B=2 Sel=4 Start=8 R=10 L=20 U=40 D=80\n"
        "  --hash            print frame-hash chain + CPU state at exit\n"
        "  --hash-log FILE   per-frame hashes\n"
        "  --dump-ppm FILE   write final frame\n"
        "  --misses FILE     write ROM addresses that ran outside the recompiled set\n"
        "  --serial          echo serial port output (test ROMs)\n"
        "  --test-status     print the result a test ROM leaves in cart RAM\n"
        "  --watch LO-HI     log writes to a RAM range (hex)\n"
        "  --dump-ram FILE   write WRAM+HRAM at exit\n"
        "  --log-apu         print every sound-channel trigger\n"
        "  --no-crc-check    run any ROM (interpreter only)\n", a0);
}

static int scan_mode, scan_counter;
static int inc1[0x10000], other[0x10000];
static int inc_first[0x10000];
static int scan_last[0x10000], scan_cnt[0x10000];
static uint8_t scan_val[0x10000];
static uint8_t scan_seen[0x10000][32];
void dev_scan_cb(uint16_t a, uint8_t o, uint8_t n)
{
    if (o == n) return;
    if (scan_counter) { if ((uint8_t)(o + 1) == n || (o == 0x09 && n == 0x10) || ((o & 0x0F) == 9 && n == o + 7)) { if (!inc1[a]) inc_first[a] = frame_count; inc1[a]++; } else other[a]++; return; }
    if (n != 0) { scan_last[a] = frame_count; scan_val[a] = n; return; }
    if (scan_last[a] && frame_count - scan_last[a] <= 3 && o == scan_val[a]) {
        scan_cnt[a]++;
        scan_seen[a][o >> 3] |= (uint8_t)(1 << (o & 7));
    }
}
void dev_watch_cb(uint16_t a, uint8_t o, uint8_t n) { if (o != n) fprintf(stderr, "[wr] frame %d %04X: %02X -> %02X  (pc %04X scx %02X) stk %04X %04X %04X\n", frame_count, a, o, n, cpu.pc, ppu_read(0x43), rd8(cpu.sp) | rd8(cpu.sp + 1) << 8, rd8(cpu.sp + 2) | rd8(cpu.sp + 3) << 8, rd8(cpu.sp + 4) | rd8(cpu.sp + 5) << 8); }

static void dump_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    static const uint8_t pal[4][3] = {{0xE0, 0xF8, 0xD0}, {0x88, 0xC0, 0x70}, {0x34, 0x68, 0x56}, {0x08, 0x18, 0x20}};
    fprintf(f, "P6\n%d %d\n255\n", ppu_w, GB_H);
    for (int y = 0; y < GB_H; y++)
        for (int x = 0; x < ppu_w; x++) fwrite(pal[ppu_shade[y][x] & 3], 1, 3, f);
    fclose(f);
}

static int widepct;
int main(int argc, char **argv)
{
    const char *path = NULL, *hack = NULL, *ppm = NULL, *misses = NULL, *ramdump = NULL, *tiles_arg = NULL;
    int wlo = -1, whi = 0;
    int headless = 0, interp = 0, ser = 0, crc_check = 1, status = 0, dev = 0;
    settings_load();
    emu_dev.max_frames = -1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--headless")) headless = 1;
        else if (!strcmp(a, "--frames") && i + 1 < argc) { emu_dev.max_frames = atoi(argv[++i]); headless = 1; }
        else if (!strcmp(a, "--interp")) { interp = 1; dev = 1; }
        else if (!strcmp(a, "--hack") && i + 1 < argc) hack = argv[++i];
        else if (!strcmp(a, "--fuzz") && i + 1 < argc) { emu_dev.fuzz = 1; emu_dev.seed = (uint32_t)atoi(argv[++i]); headless = 1; }
        else if (!strcmp(a, "--script") && i + 1 < argc) { emu_dev.script = argv[++i]; headless = 1; }
        else if (!strcmp(a, "--hash")) { emu_dev.hash = 1; headless = 1; }
        else if (!strcmp(a, "--hash-log") && i + 1 < argc) { emu_dev.hash_log = argv[++i]; headless = 1; }
        else if (!strcmp(a, "--dump-ppm") && i + 1 < argc) { ppm = argv[++i]; headless = 1; }
        else if (!strcmp(a, "--misses") && i + 1 < argc) { misses = argv[++i]; headless = 1; }
        else if (!strcmp(a, "--dump-ram") && i + 1 < argc) { ramdump = argv[++i]; headless = 1; }
        else if (!strcmp(a, "--wide") && i + 1 < argc) { widepct = atoi(argv[++i]); }
        else if (!strcmp(a, "--watch") && i + 1 < argc) { sscanf(argv[++i], "%x-%x", &wlo, &whi); headless = 1; }
        else if (!strcmp(a, "--poke") && i + 1 < argc) { unsigned ad, v; int fr; if (sscanf(argv[++i], "%x=%x@%d", &ad, &v, &fr) == 3) emu_dev_poke(fr, (uint16_t)ad, (uint8_t)v); headless = 1; }
        else if (!strcmp(a, "--mailbox-scan")) { scan_mode = 1; headless = 1; }
        else if (!strcmp(a, "--region") && i + 1 < argc) { emu_dev.region_on = sscanf(argv[++i], "%d,%d,%d,%d", &emu_dev.region[0], &emu_dev.region[1], &emu_dev.region[2], &emu_dev.region[3]) == 4; headless = 1; }
        else if (!strcmp(a, "--counter-scan")) { scan_mode = 1; scan_counter = 1; headless = 1; }
        else if (!strcmp(a, "--tiles") && i + 1 < argc) { tiles_arg = argv[++i]; headless = 1; }
        else if (!strcmp(a, "--serial")) { ser = 1; headless = 1; }
        else if (!strcmp(a, "--test-status")) { status = 1; headless = 1; }
        else if (!strcmp(a, "--log-apu")) { events_log = 1; headless = 1; }
        else if (!strcmp(a, "--no-crc-check")) { crc_check = 0; dev = 1; }
        else if (a[0] == '-') { usage(argv[0]); return 1; }
        else path = a;
    }
    (void)dev;

    if (!headless) return app_run(path, hack);      /* windowed: launcher or direct start */

    if (!path) { usage(argv[0]); return 1; }
    if (ser) gb_serial_hook(serial_print);
    RomStatus st;
    memset(&st, 0, sizeof st);
    if (!crc_check || rom_identify_file(path) < 0) {
        if (rom_load_raw(path)) { fprintf(stderr, "Cannot load %s\n", path); return 1; }
        if (widepct > 0) {
            int g = rom_identify_file(path), l, r;
            wide_dims(g, widepct, &l, &r);
            if (g >= 0 && wide_install(g, l, r)) ppu_set_wide(l, r, games[g].hud_lines, games[g].hud_window, games[g].wide_gate); else fprintf(stderr, "widescreen not available\n");
        }
        interp = 1;
    } else {
        int g = rom_identify_file(path);
        if (rom_load(g, path, &st)) { fprintf(stderr, "%s: %s\n", path, st.msg); return 1; }
        if (hack) {
            if (rom_apply_hack(hack, &st)) { fprintf(stderr, "%s: %s\n", hack, st.msg); return 1; }
            fprintf(stderr, "%s\n", st.msg);
        }
    }
    if (emu_dev_init()) return 1;
    if (scan_mode) gb_watch_range(0xC000, 0xFFFE, dev_scan_cb);
    if (wlo >= 0) { extern void dev_watch_cb(uint16_t, uint8_t, uint8_t); gb_watch_range((uint16_t)wlo, (uint16_t)whi, dev_watch_cb); }
    gb_reset();
    emu_run_blocking(interp);
    emu_dev_report();
    if (tiles_arg) { int ty, x0, x1; if (sscanf(tiles_arg, "%d,%d,%d", &ty, &x0, &x1) == 3) { for (int x = x0; x < x1; x += 8) printf("%d ", ppu_bgtile[ty][x]); printf("\n"); } }
    if (scan_counter) { for (int a = 0xC000; a < 0xFFFF; a++) if (inc1[a] >= 2 && inc1[a] <= 80 && other[a] <= inc1[a] / 2 + 3) printf("%04X inc=%d other=%d first=%d\n", a, inc1[a], other[a], inc_first[a]); } else if (scan_mode) for (int a = 0xC000; a < 0xFFFF; a++) if (scan_cnt[a] >= 3) { int nd = 0; for (int v = 0; v < 256; v++) nd += (scan_seen[a][v >> 3] >> (v & 7)) & 1; printf("%04X n=%d distinct=%d:", a, scan_cnt[a], nd); for (int v = 0; v < 256 && nd <= 24; v++) if ((scan_seen[a][v >> 3] >> (v & 7)) & 1) printf(" %02X", v); printf("\n"); }
    if (ppm) dump_ppm(ppm);
    if (status && cart_ram_on) {
        printf("test-status=%02X sig=%s text=", cart_ram[0], (cart_ram[1] == 0xDE && cart_ram[2] == 0xB0 && cart_ram[3] == 0x61) ? "ok" : "none");
        for (int i = 4; i < 0x400 && cart_ram[i]; i++) putchar(cart_ram[i] == '\n' ? '|' : cart_ram[i]);
        putchar('\n');
    }
    if (ramdump) {
        FILE *f = fopen(ramdump, "wb");
        if (f) { for (int a = 0xC000; a < 0xE000; a++) fputc(rd8((uint16_t)a), f); for (int a = 0xFF80; a < 0xFFFF; a++) fputc(rd8((uint16_t)a), f); for (int a = 0xA000; a < 0xC000; a++) fputc(rd8((uint16_t)a), f); fclose(f); }
    }
    if (misses) gb_dump_misses(misses);
    emu_dev_close();
    fflush(stdout);
    return 0;
}
