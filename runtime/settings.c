#include "settings.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef USE_SDL
#include <SDL.h>
#else
/* same numeric values as SDL2, so settings files are interchangeable with headless builds */
#define SDLK_x 120
#define SDLK_k 107
#define SDLK_z 122
#define SDLK_j 106
#define SDLK_d 100
#define SDLK_a 97
#define SDLK_w 119
#define SDLK_s 115
#define SDLK_BACKSPACE 8
#define SDLK_RETURN 13
#define SDLK_RSHIFT 0x400000E5
#define SDLK_RIGHT 0x4000004F
#define SDLK_LEFT 0x40000050
#define SDLK_DOWN 0x40000051
#define SDLK_UP 0x40000052
enum { SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
       SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_GUIDE, SDL_CONTROLLER_BUTTON_START,
       SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK, SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
       SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
       SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT };
#endif

const char *aspect_names[] = {"Original (10:9)", "4:3", "16:9"};
const char *scale_names[] = {"Pixel-perfect", "Smooth", "Stretch"};
const char *size_names[] = {"Small", "Medium", "Large", "Fullscreen"};
const char *btn_names[] = {"A", "B", "Select", "Start", "Right", "Left", "Up", "Down"};
const char *led_names[] = {"Off", "Match colors", "Custom", "Follow screen"};
const char *lat_names[] = {"Low", "Normal", "High (safest)"};

#define MONO(n, a, b, c, d) {n, {a, b, c, d}, {a, b, c, d}, {a, b, c, d}, 0}
const Palette palettes[] = {
    MONO("Classic Green",   0xE0F8D0, 0x88C070, 0x346856, 0x081820),
    MONO("Original DMG",    0x9BBC0F, 0x8BAC0F, 0x306230, 0x0F380F),
    MONO("Pocket",          0xC4CFA1, 0x8B956D, 0x4D533C, 0x1F1F1F),
    MONO("Black & White",   0xFFFFFF, 0xAAAAAA, 0x555555, 0x000000),
    MONO("Ice Blue",        0xE8F4FF, 0x8EC5F0, 0x3A6EA5, 0x0B1F3A),
    MONO("Cherry",          0xFFE8E8, 0xF09090, 0xB03050, 0x3A0A1A),
    MONO("Gold",            0xFFF6D0, 0xF0C860, 0xA87820, 0x3A2A08),
    MONO("Grape",           0xF4E8FF, 0xB890E8, 0x6A3FA0, 0x24103F),
    MONO("Mint",            0xE8FFF4, 0x90E0B8, 0x3FA078, 0x0E3A28),
    MONO("Sepia",           0xF4E8D0, 0xC8A878, 0x7A5A3A, 0x2A1C10),
    MONO("Sunset",          0xFFF0D0, 0xFFA060, 0xC04060, 0x3A1040),
    MONO("Midnight",        0x9AB0D8, 0x5870B0, 0x28387A, 0x0A0F2A),
    /* GBC-style: background and the two sprite palettes each get their own colours */
    {"Classic (color)",
     {0xF8FFF0, 0xA8D8A0, 0x4F8F6F, 0x10281F},
     {0xFFF0E8, 0xF08878, 0xB02830, 0x3A0810},
     {0xF0F8FF, 0x88B8F0, 0x3050B8, 0x081840}, 1},
    {"Arcade (color)",
     {0xE8E8FF, 0x9090D0, 0x404090, 0x101030},
     {0xFFF8D0, 0xFFC040, 0xD06010, 0x401800},
     {0xE0FFFF, 0x60E0E0, 0x1890A0, 0x083038}, 1},
    {"Candy (color)",
     {0xFFF0F8, 0xF8B8D8, 0xB868A0, 0x502048},
     {0xFFFFF0, 0xF0E060, 0xD09020, 0x483008},
     {0xF0FFF0, 0x80E0A0, 0x30A070, 0x0C3828}, 1},
    {"Ocean (color)",
     {0xE8FAFF, 0x80D0E8, 0x2878B0, 0x08203C},
     {0xFFF4E0, 0xFFB868, 0xE06838, 0x481808},
     {0xFFFFFF, 0xD0D8F0, 0x7080C0, 0x202858}, 1},
};
const int n_palettes = (int)(sizeof palettes / sizeof palettes[0]);


Settings settings;

const char *filter_preset_names[] = {"None", "Game Boy LCD", "Pocket", "CRT TV", "Bloom", "HDR"};
const int n_filter_presets = 6;

void filter_preset(FilterCfg *f, int p)
{
    FilterCfg z = {0};
    z.preset = p;
    switch (p) {
    case 1: z.lcd_grid = 55; z.ghost = 45; z.blur = 10; z.vignette = 10; break;                 /* DMG */
    case 2: z.lcd_grid = 35; z.ghost = 25; z.vignette = 0; break;                                /* Pocket */
    case 3: z.scanlines = 55; z.mask = 35; z.curve = 40; z.bloom = 25; z.vignette = 35; z.blur = 30; break;
    case 4: z.bloom = 70; z.blur = 10; break;
    case 5: z.hdr = 70; z.bloom = 45; z.vignette = 10; break;
    default: break;
    }
    *f = z;
}

void controls_defaults(GameCfg *c)
{
    static const int key[N_BTN][2] = {
        {SDLK_x, SDLK_k}, {SDLK_z, SDLK_j}, {SDLK_RSHIFT, SDLK_BACKSPACE}, {SDLK_RETURN, 0},
        {SDLK_RIGHT, SDLK_d}, {SDLK_LEFT, SDLK_a}, {SDLK_UP, SDLK_w}, {SDLK_DOWN, SDLK_s}};
    static const int pad[N_BTN][2] = {
        {SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_Y}, {SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_X},
        {SDL_CONTROLLER_BUTTON_BACK, -1}, {SDL_CONTROLLER_BUTTON_START, -1},
        {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, -1}, {SDL_CONTROLLER_BUTTON_DPAD_LEFT, -1},
        {SDL_CONTROLLER_BUTTON_DPAD_UP, -1}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, -1}};
    memcpy(c->key, key, sizeof key);
    memcpy(c->pad, pad, sizeof pad);
}

void game_cfg_defaults(GameCfg *c, int game)
{
    memset(c, 0, sizeof *c);
    c->aspect = ASPECT_ORIGINAL;
    c->scaling = SCALE_PIXEL;
    c->size = SIZE_MEDIUM;
    c->palette = game == GAME_DRMARIO ? 0 : (game == GAME_SML ? 1 : 12);
    c->bg_dim = 25;
    c->wide = 0;
    c->tex_collect = 1;
    c->ds_led_mode = LED_PALETTE; c->ds_bright = 60; c->ds_color = 0x40A0FF;
    c->ds_rumble = 70; c->ds_speaker_vol = 55;
    c->ds_ev_led = 0xFF; c->ds_ev_rumble = 0xFF; c->ds_ev_speaker = 0;
    controls_defaults(c);
}

void settings_defaults(Settings *s)
{
    memset(s, 0, sizeof *s);
    s->volume = 100;
    s->latency = LAT_NORMAL;
    s->pad_deadzone = 35;
    s->menu_music = 1; s->menu_music_vol = 55;
    s->ui_sfx = 1; s->ui_sfx_vol = 70;
    for (int i = 0; i < N_GAMES; i++) game_cfg_defaults(&s->g[i], i);
}

const char *settings_dir(void)
{
    static char dir[1024];
    if (dir[0]) return dir;
#ifdef USE_SDL
    char *b = SDL_GetBasePath();
    if (b) { snprintf(dir, sizeof dir, "%s", b); SDL_free(b); return dir; }
#endif
    snprintf(dir, sizeof dir, "./");
    return dir;
}

static void ini_path(char *out, size_t n) { snprintf(out, n, "%spipeclean.ini", settings_dir()); }
static void ini_path_legacy(char *out, size_t n) { snprintf(out, n, "%sdr_mario_launcher.ini", settings_dir()); }

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ---- key=value table ---- */
typedef struct { const char *name; int *ip; char *sp; size_t slen; int lo, hi; } Field;

static int field_table(Field *t, int cap)
{
    int n = 0;
#define I(nm, p, l, h) do { if (n < cap) t[n++] = (Field){nm, &(p), NULL, 0, l, h}; } while (0)
#define S(nm, p)       do { if (n < cap) t[n++] = (Field){nm, NULL, p, sizeof(p), 0, 0}; } while (0)
    Settings *s = &settings;
    I("volume", s->volume, 0, 200); I("latency", s->latency, 0, N_LAT - 1); I("deadzone", s->pad_deadzone, 5, 80);
    I("menu_music", s->menu_music, 0, 1); I("menu_music_vol", s->menu_music_vol, 0, 100);
    I("ui_sfx", s->ui_sfx, 0, 1); I("ui_sfx_vol", s->ui_sfx_vol, 0, 100);
    S("menu_music_path", s->menu_music_path);
    static char names[N_UI_SFX][24];
    for (int i = 0; i < N_UI_SFX; i++) { snprintf(names[i], sizeof names[i], "ui_sfx_path%d", i); S(names[i], s->ui_sfx_path[i]); }
    I("last_tab", s->last_tab, 0, 20); I("show_stats", s->show_stats, 0, 1);
    FilterCfg *f = &s->flt;
    I("f_grid", f->lcd_grid, 0, 100); I("f_ghost", f->ghost, 0, 100); I("f_scan", f->scanlines, 0, 100);
    I("f_mask", f->mask, 0, 100); I("f_curve", f->curve, 0, 100); I("f_bloom", f->bloom, 0, 100);
    I("f_hdr", f->hdr, 0, 100); I("f_vignette", f->vignette, 0, 100); I("f_blur", f->blur, 0, 100);
    I("f_preset", f->preset, 0, n_filter_presets - 1);
    static char gn[N_GAMES][40][32];
    static int gi = 0; (void)gi;
    for (int g = 0; g < N_GAMES; g++) {
        GameCfg *c = &s->g[g];
        int k = 0;
#define GI(nm, p, l, h) do { snprintf(gn[g][k], 32, "%s.%s", games[g].id, nm); I(gn[g][k], p, l, h); k++; } while (0)
#define GS(nm, p) do { snprintf(gn[g][k], 32, "%s.%s", games[g].id, nm); S(gn[g][k], p); k++; } while (0)
        GS("rom", c->rom_path); GS("hack", c->hack_path); GS("background", c->bg_path); GS("texpack", c->tex_path);
        GI("palette", c->palette, 0, n_palettes - 1); GI("aspect", c->aspect, 0, N_ASPECT - 1);
        GI("scaling", c->scaling, 0, N_SCALE - 1); GI("size", c->size, 0, N_SIZE - 1);
        GI("bg_dim", c->bg_dim, 0, 80); GI("wide", c->wide, 0, 100); GI("tex_on", c->tex_on, 0, 1); GI("tex_collect", c->tex_collect, 0, 1);
        GI("led", c->ds_led_mode, 0, N_LED - 1); GI("led_bright", c->ds_bright, 0, 100);
        GI("rumble", c->ds_rumble, 0, 100); GI("spk_vol", c->ds_speaker_vol, 0, 100);
        GI("ev_led", c->ds_ev_led, 0, 255); GI("ev_rumble", c->ds_ev_rumble, 0, 255); GI("ev_speaker", c->ds_ev_speaker, 0, 255);
#undef GI
#undef GS
    }
#undef I
#undef S
    return n;
}

void settings_load(void)
{
    settings_defaults(&settings);
    char path[1100];
    ini_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) { ini_path_legacy(path, sizeof path); f = fopen(path, "r"); }
    if (!f) return;
    Field tab[400];
    int nf = field_table(tab, 400);
    char line[700];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1;
        size_t l = strlen(v);
        while (l && (v[l - 1] == '\n' || v[l - 1] == '\r')) v[--l] = 0;
        int done = 0;
        for (int i = 0; i < nf && !done; i++) {
            if (strcmp(line, tab[i].name)) continue;
            if (tab[i].ip) *tab[i].ip = clampi(atoi(v), tab[i].lo, tab[i].hi);
            else snprintf(tab[i].sp, tab[i].slen, "%s", v);
            done = 1;
        }
        if (done) continue;
        /* arrays: "<game>.key<btn>_<slot>", "<game>.pad<btn>_<slot>", "<game>.color" */
        for (int g = 0; g < N_GAMES; g++) {
            size_t gl = strlen(games[g].id);
            if (strncmp(line, games[g].id, gl) || line[gl] != '.') continue;
            const char *k = line + gl + 1;
            int b, s2;
            if (sscanf(k, "key%d_%d", &b, &s2) == 2 && b >= 0 && b < N_BTN && s2 >= 0 && s2 < 2) settings.g[g].key[b][s2] = atoi(v);
            else if (sscanf(k, "pad%d_%d", &b, &s2) == 2 && b >= 0 && b < N_BTN && s2 >= 0 && s2 < 2) settings.g[g].pad[b][s2] = atoi(v);
            else if (!strcmp(k, "ledcolor")) settings.g[g].ds_color = (uint32_t)strtoul(v, NULL, 16) & 0xFFFFFF;
        }
    }
    fclose(f);
    if (settings.last_tab > N_GAMES + 3) settings.last_tab = 0;
}

void settings_save(void)
{
    char path[1100];
    ini_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    Field tab[400];
    int nf = field_table(tab, 400);
    for (int i = 0; i < nf; i++) {
        if (tab[i].ip) fprintf(f, "%s=%d\n", tab[i].name, *tab[i].ip);
        else fprintf(f, "%s=%s\n", tab[i].name, tab[i].sp);
    }
    for (int g = 0; g < N_GAMES; g++) {
        for (int b = 0; b < N_BTN; b++)
            for (int s = 0; s < 2; s++) {
                fprintf(f, "%s.key%d_%d=%d\n", games[g].id, b, s, settings.g[g].key[b][s]);
                fprintf(f, "%s.pad%d_%d=%d\n", games[g].id, b, s, settings.g[g].pad[b][s]);
            }
        fprintf(f, "%s.ledcolor=%06X\n", games[g].id, settings.g[g].ds_color);
    }
    fclose(f);
}
