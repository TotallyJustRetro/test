/* Emulation driver: the frame hook that connects the CPU/PPU/APU core to the outside world.
 * In windowed play the core runs on its own thread, paced by the audio device (the audio clock is
 * the master clock), and hands finished frames to the main thread. */
#include <SDL.h>
#include <setjmp.h>
#include <stdlib.h>
#include "emu.h"
#include "audio.h"
#include "rom.h"
#include "cart.h"
#include "events.h"
#include "settings.h"

EmuDev emu_dev = {.max_frames = -1};

static jmp_buf stop_jmp;
static SDL_Thread *thr;
static SDL_mutex *fmx;
static Frame pub;                         /* latest published frame */
static volatile int abort_flag, paused, turbo, thread_mode, preview_target, previewing;
static volatile uint32_t input_word;     /* buttons | dpad << 8 */
static char save_path[1100];
static int save_tick;
static int force_interp_flag;
static Uint64 pace_next;

static uint64_t chain = 1469598103934665603ull;
static FILE *hash_log;
typedef struct { int frame; uint8_t mask; } ScriptEv;
static ScriptEv script[4096];
static int n_script;
static uint32_t rng = 1;
static struct { int frame; uint16_t addr; uint8_t val; } pokes[64];
static int n_pokes;
void emu_dev_poke(int frame, uint16_t addr, uint8_t val) { if (n_pokes < 64) { pokes[n_pokes].frame = frame; pokes[n_pokes].addr = addr; pokes[n_pokes].val = val; n_pokes++; } }
static int fuzz_next;
static uint8_t fz_b, fz_d;

static uint32_t xr(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

uint64_t emu_frames(void) { return pub.seq; }
void emu_input(uint8_t b, uint8_t d) { input_word = (uint32_t)b | ((uint32_t)d << 8); }
void emu_set_paused(int p) { paused = p; audio_game_set_paused(p); }
void emu_set_turbo(int t) { turbo = t; }
void emu_set_save_path(const char *p) { snprintf(save_path, sizeof save_path, "%s", p ? p : ""); }
int emu_running(void) { return thr != NULL; }

static void run_core(int force_interp)
{
    if (force_interp || rom_needs_interpreter()) run_interpreter();
    else recomp_run();
}

static void publish(void)
{
    SDL_LockMutex(fmx);
    memcpy(pub.shade, ppu_shade, sizeof pub.shade);
    memcpy(pub.layer, ppu_layer, sizeof pub.layer);
    memcpy(pub.bguv, ppu_bguv, sizeof pub.bguv);
    memcpy(pub.spruv, ppu_spruv, sizeof pub.spruv);
    memcpy(pub.bgtile, ppu_bgtile, sizeof pub.bgtile);
    memcpy(pub.sprtile, ppu_sprtile, sizeof pub.sprtile);
    memcpy(pub.tiles, vram, sizeof pub.tiles);
    pub.w = ppu_w; pub.xoff = ppu_xoff;
    pub.lcd_on = ppu_lcd_is_on();
    pub.seq++;
    SDL_UnlockMutex(fmx);
}

int emu_frame_get(Frame *f)
{
    int got = 0;
    SDL_LockMutex(fmx);
    if (pub.seq != f->seq) { memcpy(f, &pub, sizeof *f); got = 1; }
    SDL_UnlockMutex(fmx);
    return got;
}

static void dev_hook(uint8_t *b, uint8_t *d)
{
    if (emu_dev.hash || hash_log) {
        uint64_t h = 1469598103934665603ull;
        for (int y = 0; y < GB_H; y++)
            for (int x = 0; x < GB_W; x++) { h ^= ppu_shade[y][x + ppu_xoff]; h *= 1099511628211ull; }
        chain = (chain ^ h) * 1099511628211ull;
        if (hash_log) fprintf(hash_log, "%d %016llx %04X\n", frame_count, (unsigned long long)h, cpu.pc);
    }
    if (emu_dev.region_on) {
        static uint64_t last; uint64_t h = 1469598103934665603ull;
        for (int y = emu_dev.region[1]; y < emu_dev.region[3]; y++)
            for (int x = emu_dev.region[0]; x < emu_dev.region[2]; x++) { h ^= ppu_shade[y][x + ppu_xoff]; h *= 1099511628211ull; }
        if (h != last) { fprintf(stderr, "[region] frame %d changed\n", frame_count); last = h; }
    }
    if (getenv("GBL_POPIN")) {                 /* dev: sprites that appear out of nowhere inside the picture */
        static uint8_t prevl[GB_H][GB_WMAX];
        static int tot, shown;
        int W = ppu_w;
        for (int y = 0; y < GB_H; y++)
            for (int x = 12; x < W - 12; x++) {
                if (!ppu_layer[y][x]) continue;
                int near = 0;
                for (int dy = -6; dy <= 6 && !near; dy++)
                    for (int dx = -6; dx <= 6; dx++) {
                        int yy = y + dy, xx = x + dx;
                        if (yy >= 0 && yy < GB_H && xx >= 0 && xx < W && prevl[yy][xx]) { near = 1; break; }
                    }
                if (!near && x >= atoi(getenv("GBL_POPIN"))) { tot++; if (shown < 3000) { shown++; fprintf(stderr, "[popin] frame %d x %d y %d (W %d)\n", frame_count, x, y, W); } }
            }
        memcpy(prevl, ppu_layer, sizeof prevl);
        if (emu_dev.max_frames >= 0 && frame_count + 1 >= emu_dev.max_frames) fprintf(stderr, "[popin] total %d\n", tot);
    }
    for (int i = 0; i < n_pokes; i++) if (pokes[i].frame == frame_count) wr8(pokes[i].addr, pokes[i].val);
    if (emu_dev.max_frames >= 0 && frame_count >= emu_dev.max_frames) longjmp(stop_jmp, 2);
    if (emu_dev.fuzz) {
        if (frame_count >= fuzz_next) {
            fuzz_next = frame_count + 3 + (int)(xr() % 25);
            uint32_t q = xr() % 16;
            fz_b = fz_d = 0;
            if (q < 5) fz_b = (uint8_t)(1u << (xr() & 3));
            else if (q < 11) fz_d = (uint8_t)(1u << (xr() & 3));
            else if (q < 13) fz_b = 8;
        }
        *b |= fz_b; *d |= fz_d;
    }
    if (n_script) {
        uint8_t m = 0;
        for (int i = 0; i < n_script; i++) if (script[i].frame <= frame_count) m = script[i].mask;
        *b |= m & 0x0F; *d |= m >> 4;
    }
}

static void pace_without_audio(void)
{
    Uint64 f = SDL_GetPerformanceFrequency();
    Uint64 per = (Uint64)((double)f * CYCLES_PER_FRAME / CPU_HZ);
    Uint64 now = SDL_GetPerformanceCounter();
    if (!pace_next || now > pace_next + per * 4) pace_next = now;
    pace_next += per;
    while ((now = SDL_GetPerformanceCounter()) < pace_next) {
        Uint64 left = pace_next - now;
        if (left * 1000 / f > 2) SDL_Delay(1);
    }
}

void frame_hook(void)
{
    static int16_t abuf[4096 * 2];
    if (previewing) {
        if (frame_count >= preview_target) longjmp(stop_jmp, 1);
        apu_drain(abuf, 4096);
        return;
    }
    uint8_t b = (uint8_t)(input_word & 0xFF), d = (uint8_t)(input_word >> 8);
    if (!thread_mode) {                      /* headless / developer run */
        apu_drain(abuf, 4096);
        events_frame();
        dev_hook(&b, &d);
        gb_set_input(b, d);
        return;
    }
    if (abort_flag) longjmp(stop_jmp, 1);
    publish();
    events_frame();
    int n = apu_drain(abuf, 4096);
    if (!turbo) audio_game_push(abuf, n);
    gb_set_input(b, d);
    while (paused && !abort_flag) SDL_Delay(8);
    if (abort_flag) longjmp(stop_jmp, 1);
    if (!turbo) {
        if (audio_ok()) audio_game_wait(audio_game_target());
        else pace_without_audio();
    } else pace_next = 0;
    if (++save_tick >= 600 && save_path[0]) { save_tick = 0; cart_write_save(save_path); }
}

static int thread_main(void *u)
{
    (void)u;
    thread_mode = 1;
    if (setjmp(stop_jmp) == 0) run_core(force_interp_flag);
    thread_mode = 0;
    if (save_path[0]) cart_write_save(save_path);
    return 0;
}

int emu_start(int force_interp)
{
    if (thr) return 0;
    if (!fmx) fmx = SDL_CreateMutex();
    force_interp_flag = force_interp;
    abort_flag = 0; paused = 0; turbo = 0; save_tick = 0; pace_next = 0;
    gb_reset();
    if (save_path[0]) cart_load_save(save_path);
    memset(&pub, 0, sizeof pub);
    apu_set_volume(settings.volume / 100.0f);
    audio_game_begin();
    events_begin();
    thr = SDL_CreateThread(thread_main, "emulation", NULL);
    if (!thr) { audio_game_end(); events_end(); return -1; }
    return 0;
}

void emu_stop(void)
{
    if (!thr) return;
    abort_flag = 1;
    audio_game_abort();
    SDL_WaitThread(thr, NULL);
    thr = NULL;
    audio_game_end();
    events_end();
}

void emu_preview(int frames)
{
    if (!fmx) fmx = SDL_CreateMutex();
    previewing = 1; preview_target = frames;
    gb_reset();
    if (setjmp(stop_jmp) == 0) run_core(0);
    previewing = 0;
}

void emu_run_blocking(int force_interp)
{
    thread_mode = 0;
    events_begin();
    if (setjmp(stop_jmp) == 0) run_core(force_interp);
    events_end();
}

int emu_dev_init(void)
{
    if (!fmx) fmx = SDL_CreateMutex();
    rng = emu_dev.seed * 2654435761u + 1;
    if (emu_dev.hash_log) hash_log = fopen(emu_dev.hash_log, "w");
    if (emu_dev.script) {
        FILE *f = fopen(emu_dev.script, "r");
        if (!f) { perror("script"); return 1; }
        int fr; unsigned m;
        while (n_script < 4096 && fscanf(f, "%d %x", &fr, &m) == 2) { script[n_script].frame = fr; script[n_script++].mask = (uint8_t)m; }
        fclose(f);
    }
    return 0;
}

void emu_dev_report(void)
{
    if (emu_dev.hash)
        printf("frames=%d cycles=%llu hash=%016llx pc=%04X sp=%04X af=%02X%02X bc=%02X%02X de=%02X%02X hl=%02X%02X\n",
               frame_count, (unsigned long long)total_cycles, (unsigned long long)chain, cpu.pc, cpu.sp,
               cpu.a, cpu.f, cpu.b, cpu.c, cpu.d, cpu.e, cpu.h, cpu.l);
}

void emu_dev_close(void) { if (hash_log) { fclose(hash_log); hash_log = NULL; } }
