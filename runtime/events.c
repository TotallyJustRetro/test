#include <SDL.h>
#include <stdio.h>
#include "events.h"
#include "gb.h"
#include "rom.h"
#include "audio.h"
#include "settings.h"

int events_log;

/* ---- what can happen in each game --------------------------------------------------------- */
static const EventDef dr_defs[] = {
    {"clear",  "Pills cleared",    "Four in a row disappear",         1, 800, 0, 0xFFD23C, 220, 80},
    {"virus",  "Virus eliminated", "A virus is wiped out",            1, 700, 0, 0xFF4A4A, 320, 100},
    {"land",   "Pill lands",       "The pill settles in the bottle",  8, 250, 0, 0x4C8DFF, 90, 40},
    {"rotate", "Rotate",           "Flip the pill",                   1, 200, 0, 0x8CE0A0, 40, 20},
    {"move",   "Move",             "Slide the pill sideways",         1, 200, 0, 0x8CE0A0, 30, 15},
    {"over",   "Game over",        "The bottle fills up",             15, 2500, 0, 0xFF3030, 700, 100},
};
static const EventDef sml_defs[] = {
    {"coin",   "Coin",             "Pick up a coin",                  1, 350, 0, 0xFFD23C, 90, 40},
    {"stomp",  "Stomp an enemy",   "Jump on an enemy",                1, 300, 0, 0x8CE0A0, 140, 70},
    {"power",  "Power-up",         "Grab a mushroom or flower",       1, 1000, 0, 0xFF8CFF, 350, 80},
    {"bump",   "Bump a block",     "Hit a block from below",          1, 250, 0, 0xB0B0B0, 110, 60},
    {"jump",   "Jump",             "Mario jumps",                     1, 300, 0, 0x4C8DFF, 35, 15},
    {"life",   "Lose a life",      "Mario is hurt",                   15, 2500, 0, 0xFF3030, 600, 100},
    {"over",   "Game over",        "No lives left",                   15, 3000, 0, 0xFF3030, 800, 100},
};
static const EventDef sml2_defs[] = {
    {"coin",   "Coin",             "The coin counter goes up",        0, 0, 1, 0xFFD23C, 90, 40},
    {"enemy",  "Enemy defeated",   "The defeated counter goes up",    0, 0, 2, 0x8CE0A0, 140, 70},
    {"life",   "Lose a life",      "Lives go down",                   0, 0, 3, 0xFF3030, 600, 100},
    {"oneup",  "Extra life",       "Lives go up",                     0, 0, 4, 0x6CFF8C, 400, 60},
};
typedef struct { const EventDef *defs; int n; } GameEvents;
#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))
static const GameEvents ge[N_GAMES] = {{dr_defs, N(dr_defs)}, {sml_defs, N(sml_defs)}, {sml2_defs, N(sml2_defs)}};

int events_count(int g) { return g >= 0 && g < N_GAMES ? ge[g].n : 0; }
const EventDef *events_def(int g, int i) { return (g >= 0 && g < N_GAMES && i >= 0 && i < ge[g].n) ? &ge[g].defs[i] : NULL; }

/* sound-request rules: the game writes an effect number into a mailbox byte */
typedef struct { uint16_t addr; uint8_t val; int ev; } Rule;
static const Rule dr_rules[] = {
    {0xDFE0, 0x06, 0}, {0xDFE0, 0x0C, 1}, {0xDFF8, 0x01, 2}, {0xDFE0, 0x02, 3}, {0xDFE0, 0x03, 4}, {0xDFE8, 0x08, 5},
};
static const Rule sml_rules[] = {
    {0xDFE0, 0x05, 0}, {0xDFE0, 0x03, 1}, {0xDFE0, 0x04, 2}, {0xDFE0, 0x07, 3}, {0xDFE0, 0x01, 4}, {0xDFE8, 0x02, 5}, {0xDFE8, 0x10, 6},
};
static const Rule *rules;
static int n_rules;

/* ---- queue (emulation thread -> main thread) ----------------------------------------------- */
#define QN 64
static volatile int q[QN];
static volatile unsigned qr, qw;
static int cur_game = -1, tap_frames;

static void push(int ev)
{
    if (qw - qr >= QN) return;
    q[qw % QN] = ev;
    qw++;
}

int events_pop(void)
{
    if (qr == qw) return -1;
    int v = q[qr % QN];
    qr++;
    return v;
}

static void fire(int ev)
{
    const EventDef *d = events_def(cur_game, ev);
    if (!d) return;
    if (events_log) fprintf(stderr, "[event] frame %d %s\n", frame_count, d->name);
    push(ev);
    const GameCfg *c = &settings.g[cur_game];
    if ((c->ds_ev_speaker >> ev) & 1) {
        if (d->sfx_mask && audio_pad_available()) { apu_set_tap_mask(d->sfx_mask); tap_frames = d->sfx_ms * 60 / 1000 + 1; }
        else audio_pad_play_beep(d->beep);
    }
}

static void on_write(uint16_t a, uint8_t old_v, uint8_t v)
{
    (void)old_v;
    if (!v) return;
    for (int i = 0; i < n_rules; i++)
        if (rules[i].addr == a && rules[i].val == v) { fire(rules[i].ev); return; }
}

static void on_trigger(int ch, const uint8_t *r)
{
    if (events_log) fprintf(stderr, "[apu] frame %d ch%d  %02X %02X %02X %02X %02X\n", frame_count, ch + 1, r[0], r[1], r[2], r[3], r[4]);
}

/* ---- Super Mario Land 2: read the status bar -------------------------------------------------
 * The bar is drawn with digit tiles numbered 128 + digit, so the counters can be read from the
 * tile map (immune to palette fades). A change only counts after it has been stable for 3 frames. */
typedef struct { int lives, coins, kills; } Hud;
static Hud hud_committed, hud_cand;
static int hud_have, hud_cnt;

static int digits(int y, int x0, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        int t = ppu_bgtile[y][x0 + i * 8];
        if (t < 128 || t > 137) return -1;
        v = v * 10 + (t - 128);
    }
    return v;
}

static void hud_frame(void)
{
    const int y = 138;
    if (ppu_bgtile[y][8] != 172 || ppu_bgtile[y][48] != 172) { hud_cnt = 0; return; }   /* status bar not on screen */
    Hud h = {digits(y, 16, 2), digits(y, 56, 3), digits(y, 104, 2)};
    if (h.lives < 0 || h.coins < 0 || h.kills < 0) { hud_cnt = 0; return; }
    if (hud_cnt && h.lives == hud_cand.lives && h.coins == hud_cand.coins && h.kills == hud_cand.kills) hud_cnt++;
    else { hud_cand = h; hud_cnt = 1; }
    if (hud_cnt != 3) return;
    if (hud_have) {
        if (hud_cand.coins > hud_committed.coins) fire(0);
        if (hud_cand.kills > hud_committed.kills) fire(1);
        if (hud_cand.lives < hud_committed.lives) fire(2);
        if (hud_cand.lives > hud_committed.lives) fire(3);
    }
    hud_committed = hud_cand; hud_have = 1;
}

void events_begin(void)
{
    cur_game = rom_loaded_game();
    qr = qw = 0; tap_frames = 0; hud_have = 0; hud_cnt = 0;
    apu_set_tap_mask(0);
    apu_set_trigger_hook(on_trigger);
    rules = NULL; n_rules = 0;
    if (cur_game == GAME_DRMARIO) { rules = dr_rules; n_rules = N(dr_rules); }
    else if (cur_game == GAME_SML) { rules = sml_rules; n_rules = N(sml_rules); }
    static const uint16_t mailbox[] = {0xDFE0, 0xDFE8, 0xDFF8};
    if (n_rules) gb_watch_set(mailbox, 3, on_write);
}

void events_end(void)
{
    apu_set_trigger_hook(NULL);
    apu_set_tap_mask(0);
    gb_watch_set(NULL, 0, NULL);
}

void events_frame(void)
{
    static int16_t tb[2048 * 2];
    if (cur_game == GAME_SML2) hud_frame();
    if (tap_frames > 0) {
        int n = apu_drain_tap(tb, 2048);
        if (n > 0) audio_pad_push(tb, n);
        if (--tap_frames == 0) apu_set_tap_mask(0);
    }
}
