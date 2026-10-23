/* The launcher window: one tab per game (play, display, controls, controller light/sound, textures),
 * a filter tab and an audio tab. Everything is drawn by the immediate-mode UI at the window's real resolution. */
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "launcher.h"
#include "ui.h"
#include "settings.h"
#include "rom.h"
#include "emu.h"
#include "render.h"
#include "bgimg.h"
#include "texpack.h"
#include "pad.h"
#include "audio.h"
#include "events.h"
#include "util.h"
#include "widescreen.h"

static SDL_Renderer *g_ren; static SDL_Renderer *ren_get(void){return g_ren;}
void launcher_set_renderer(SDL_Renderer *r){g_ren=r;}
void ui_text_fit_tail(int font, float size, float x, float y, float maxw, uint32_t c, const char *s);

enum { TAB_FILTERS = N_GAMES, TAB_AUDIO, N_TABS };
enum { SUB_GAME, SUB_DISPLAY, SUB_CONTROLS, SUB_DUALSENSE, SUB_TEXTURES, N_SUB };
static const char *sub_names[N_SUB] = {"Game", "Display", "Controls", "DualSense", "Textures"};

static int tab, sub[N_GAMES];
static RomStatus rs[N_GAMES];            /* is the configured ROM usable */
static char hack_msg[N_GAMES][256];
static int hack_ok[N_GAMES];
static Frame prev[N_GAMES];
static int prev_ok[N_GAMES];
static int filter_game;                  /* which game the Filters tab previews */
static int cap_kind, cap_btn, cap_slot;  /* binding capture: 1 = key, 2 = pad */
static char toast_msg[200];
static float toast_t;
static int click_id;
static float save_timer;
static int loaded_pack_game = -1, pack_n;
static char loaded_pack_path[512];
static char export_msg[N_GAMES][160];
static int export_scale = 8;
static char bg_loaded[512], bg_msg[256];
static int bg_for_game = -1;
static float anim_clock;
static int wide_dirty, wide_note[N_GAMES];
static char sfx_test_msg[64];

static int last_game_tab;
int launcher_current_game(void) { return tab < N_GAMES ? tab : last_game_tab; }
int launcher_capturing(void) { return cap_kind != 0; }

void launcher_toast(const char *m) { snprintf(toast_msg, sizeof toast_msg, "%s", m); toast_t = 3.0f; }

/* ------------------------------------------------------------------ ROM / preview plumbing */
static void rom_check(int g)
{
    GameCfg *c = &settings.g[g];
    memset(&rs[g], 0, sizeof rs[g]);
    if (!c->rom_path[0]) { snprintf(rs[g].msg, sizeof rs[g].msg, "No ROM chosen yet."); return; }
    if (!file_exists(c->rom_path)) { snprintf(rs[g].msg, sizeof rs[g].msg, "That file is gone. Choose it again."); return; }
    rom_probe(g, c->rom_path, &rs[g]);
}

int launcher_prepare(int g, char *err, size_t n)
{
    GameCfg *c = &settings.g[g];
    RomStatus st;
    if (!c->rom_path[0] || rom_load(g, c->rom_path, &st)) {
        snprintf(err, n, "%s", c->rom_path[0] ? st.msg : "Choose your ROM on this game's tab first.");
        return 1;
    }
    hack_ok[g] = 0;
    if (c->hack_path[0]) {
        if (rom_apply_hack(c->hack_path, &st) == 0) { hack_ok[g] = 1; snprintf(hack_msg[g], sizeof hack_msg[g], "%s", st.msg); }
        else { snprintf(hack_msg[g], sizeof hack_msg[g], "%s", st.msg); c->hack_path[0] = 0; }
    }
    int wl, wr;
    wide_dims(g, c->wide, &wl, &wr);
    if (wl + wr > 0 && !wide_install(g, wl, wr)) { wl = wr = 0; wide_note[g] = 1; } else wide_note[g] = 0;
    ppu_set_wide(wl, wr, games[g].hud_lines, games[g].hud_window, games[g].wide_gate);
    return 0;
}

static void ensure_pack(int g)
{
    GameCfg *c = &settings.g[g];
    const char *want = c->tex_on ? c->tex_path : "";
    if (loaded_pack_game == g && !strcmp(loaded_pack_path, want)) return;
    pack_n = texpack_load(want);
    loaded_pack_game = g;
    snprintf(loaded_pack_path, sizeof loaded_pack_path, "%s", want);
}

static void make_preview(int g)
{
    char err[256];
    prev_ok[g] = 0;
    if (!rs[g].ok) return;
    if (launcher_prepare(g, err, sizeof err)) return;
    tex_collect_begin(g);
    emu_preview(games[g].preview_frames);
    frame_from_ppu(&prev[g]);
    prev_ok[g] = 1;
    tex_collect_frame(&prev[g]);
    tex_collect_save();
}

static void set_rom(int g, const char *path)
{
    snprintf(settings.g[g].rom_path, sizeof settings.g[g].rom_path, "%s", path);
    rom_check(g);
    if (rs[g].ok) make_preview(g);
    else prev_ok[g] = 0;
}

static void set_hack(int g, const char *path)
{
    GameCfg *c = &settings.g[g];
    if (!rs[g].ok) { launcher_toast("Choose the original ROM first."); return; }
    char old[512];
    snprintf(old, sizeof old, "%s", c->hack_path);
    snprintf(c->hack_path, sizeof c->hack_path, "%s", path);
    char err[256];
    RomStatus st;
    if (rom_load(g, c->rom_path, &st)) { launcher_toast(st.msg); return; }
    if (path[0] && rom_apply_hack(path, &st)) {
        snprintf(hack_msg[g], sizeof hack_msg[g], "%s", st.msg);
        snprintf(c->hack_path, sizeof c->hack_path, "%s", old);
        launcher_toast(st.msg);
        (void)err;
        return;
    }
    snprintf(hack_msg[g], sizeof hack_msg[g], "%s", path[0] ? st.msg : "");
    hack_ok[g] = path[0] != 0;
    make_preview(g);
}

static void ensure_background(int g)
{
    if (bg_for_game == g && !strcmp(bg_loaded, settings.g[g].bg_path)) return;
    bg_for_game = g;
    snprintf(bg_loaded, sizeof bg_loaded, "%s", settings.g[g].bg_path);
    bg_msg[0] = 0;
    if (bg_load(bg_loaded) != 0) { if (bg_loaded[0]) snprintf(bg_msg, sizeof bg_msg, "Couldn't open that picture."); settings.g[g].bg_path[0] = 0; bg_loaded[0] = 0; }
}

void launcher_init(void)
{
    for (int g = 0; g < N_GAMES; g++) rom_check(g);
    char found[N_GAMES][512];
    memset(found, 0, sizeof found);
    rom_scan(found);
    for (int g = 0; g < N_GAMES; g++)
        if (!rs[g].ok && found[g][0]) { snprintf(settings.g[g].rom_path, sizeof settings.g[g].rom_path, "%s", found[g]); rom_check(g); }
    tab = settings.last_tab >= 0 && settings.last_tab < N_TABS ? settings.last_tab : 0;
    for (int g = 0; g < N_GAMES; g++) if (rs[g].ok && !prev_ok[g]) make_preview(g);
    filter_game = 0;
    if (getenv("GBL_TAB")) { int t = 0, u = 0; sscanf(getenv("GBL_TAB"), "%d,%d", &t, &u); tab = t; if (t < N_GAMES) sub[t] = u; }
    audio_menu_apply();
}

void launcher_enter(void)
{
    for (int g = 0; g < N_GAMES; g++) rom_check(g);
    for (int g = 0; g < N_GAMES; g++) if (rs[g].ok && !prev_ok[g]) make_preview(g);
    loaded_pack_game = -1;
    bg_for_game = -1;
    audio_menu_apply();
    audio_menu_music(1);
}

void launcher_shutdown(void) { tex_collect_save(); }

/* ------------------------------------------------------------------ small drawing helpers */
static SDL_Rect to_px(float x, float y, float w, float h)
{
    SDL_Rect r;
    r.x = (int)floorf((x - ui_view_x0()) * ui_scale + 0.5f);
    r.y = (int)floorf((y - ui_view_y0()) * ui_scale + 0.5f);
    r.w = (int)floorf(w * ui_scale + 0.5f);
    r.h = (int)floorf(h * ui_scale + 0.5f);
    return r;
}

static int clickable(float x, float y, float w, float h, int *over_out)
{
    int id = ui_next_id();
    int over = ui_hover(x, y, w, h) && !ui_active_any();
    if (over && ui_mouse.pressed) click_id = id;
    int clicked = over && ui_mouse.released && click_id == id;
    if (over_out) *over_out = over;
    if (clicked && ui_sfx_cb) ui_sfx_cb(SFX_CLICK);
    return clicked;
}

static uint32_t mixc(uint32_t a, uint32_t b, float t)
{
    return RGBA((int)(((a >> 24) & 255) * (1 - t) + ((b >> 24) & 255) * t), (int)(((a >> 16) & 255) * (1 - t) + ((b >> 16) & 255) * t),
                (int)(((a >> 8) & 255) * (1 - t) + ((b >> 8) & 255) * t), (int)((a & 255) * (1 - t) + (b & 255) * t));
}

static void card(float x, float y, float w, float h, const char *title)
{
    ui_shadow(x, y, w, h, 14, 8, RGBA(0, 0, 0, 50));
    ui_rrect(x, y, w, h, 14, (C_PANEL & 0xFFFFFF00u) | 232);
    if (title) ui_text(F_BOLD, 12, x + 18, y + 14, C_DIM, title);
}

static void label(float x, float y, const char *s) { ui_text(F_REG, 13, x, y, C_MUTED, s); }
static void hint_area(float x, float y, float w, float h, const char *s) { if (ui_hover(x, y, w, h)) ui_hint(s); }

static void path_box(float x, float y, float w, const char *path, const char *placeholder)
{
    ui_rrect(x, y, w, 36, 9, C_BG2);
    if (path[0]) ui_text_fit_tail(F_REG, 13, x + 12, y + 8, w - 24, C_TEXT, path);
    else ui_text_fit(F_REG, 13, x + 12, y + 8, w - 24, C_DIM, placeholder);
}

static void status_line(float x, float y, float w, int kind, const char *msg)
{
    uint32_t c = kind == 1 ? C_OK : (kind == 2 ? C_WARN : (kind == 3 ? C_ERR : C_DIM));
    ui_rrect(x, y + 4, 10, 10, 5, c);
    ui_text_fit(F_REG, 13, x + 18, y, w - 18, kind ? C_TEXT : C_MUTED, msg);
}

static void draw_cover(SDL_Texture *t, int tw, int th, float x, float y, float w, float h)
{
    SDL_Renderer *r = ren_get();
    SDL_Rect dst = to_px(x, y, w, h);
    float s = fmaxf((float)dst.w / tw, (float)dst.h / th);
    SDL_Rect src = {(int)((tw - dst.w / s) * 0.5f), (int)((th - dst.h / s) * 0.5f), (int)(dst.w / s), (int)(dst.h / s)};
    SDL_RenderCopy(r, t, &src, &dst);
}

/* ------------------------------------------------------------------ preview box */
static void draw_preview(int g, float x, float y, float w, float h, int use_cfg_shape)
{
    ui_rrect(x, y, w, h, 12, HEX(0x07080C));
    if (!prev_ok[g]) {
        ui_text_c(F_BOLD, 14, x + w * 0.5f, y + h * 0.5f - 18, C_MUTED, "No picture yet");
        ui_text_c(F_REG, 12, x + w * 0.5f, y + h * 0.5f + 4, C_DIM, rs[g].ok ? "Press Play to start" : "Add the ROM to see the game here");
        return;
    }
    ensure_pack(g);
    render_build(&prev[g], g, 0);
    const GameCfg *c = &settings.g[g];
    float ar = render_width() / 144.0f;
    if (use_cfg_shape) ar = c->aspect == ASPECT_4_3 ? 4.0f / 3.0f : (c->aspect == ASPECT_16_9 ? 16.0f / 9.0f : ar);
    float pw = w - 16, ph = pw / ar;
    if (c->scaling == SCALE_STRETCH && use_cfg_shape) { pw = w - 16; ph = h - 16; }
    if (ph > h - 16) { ph = h - 16; pw = ph * ar; }
    SDL_Rect r = to_px(x + (w - pw) * 0.5f, y + (h - ph) * 0.5f, pw, ph);
    render_draw(&r, c->scaling == SCALE_PIXEL ? SCALE_PIXEL : SCALE_SMOOTH);
}

/* ------------------------------------------------------------------ game tab: Game */
static void sub_game(int g, float x, float y)
{
    GameCfg *c = &settings.g[g];
    card(x, y, 360, 476, "PREVIEW");
    draw_preview(g, x + 16, y + 40, 328, 300, 0);
    ui_text_c(F_REG, 12, x + 180, y + 352, C_DIM, "A short run of the game from power-on");
    if (ui_button(x + 16, y + 386, 328, 38, "Refresh preview", B_NORMAL, rs[g].ok)) make_preview(g);
    ui_text_wrap(F_REG, 12, x + 18, y + 434, 324, C_DIM, "Tip: drag and drop a ROM, patch or picture onto this window.", 2);

    float rx = x + 372, rw = 436;
    card(rx, y, rw, 476, "FILES");
    /* ROM */
    label(rx + 18, y + 42, "Game ROM");
    path_box(rx + 18, y + 64, rw - 18 * 2 - 110, c->rom_path, "Not chosen yet");
    if (ui_button(rx + rw - 18 - 100, y + 64, 100, 36, "Browse…", B_NORMAL, 1)) {
        char p[1100];
        if (dlg_pick(DLG_ROM, "Choose your ROM", p, sizeof p)) {
            int id = rom_identify_file(p);
            if (id >= 0 && id != g) { snprintf(settings.g[id].rom_path, sizeof settings.g[id].rom_path, "%s", p); rom_check(id); make_preview(id); tab = id; launcher_toast("That ROM belongs to another game. Switched tabs."); }
            else set_rom(g, p);
        }
    }
    status_line(rx + 18, y + 108, rw - 36, rs[g].ok ? 1 : (c->rom_path[0] ? 3 : 2), rs[g].msg);
    /* hack */
    label(rx + 18, y + 150, "Romhack or patch (optional)");
    path_box(rx + 18, y + 172, rw - 18 * 2 - 110 - 66, c->hack_path, "None");
    if (ui_button(rx + rw - 18 - 100 - 60, y + 172, 100, 36, "Browse…", B_NORMAL, rs[g].ok)) {
        char p[1100];
        if (dlg_pick(DLG_PATCH, "Choose a romhack or patch", p, sizeof p)) set_hack(g, p);
    }
    if (ui_button(rx + rw - 18 - 52, y + 172, 52, 36, "Clear", B_GHOST, c->hack_path[0] != 0)) set_hack(g, "");
    if (hack_msg[g][0] && c->hack_path[0]) status_line(rx + 18, y + 216, rw - 36, hack_ok[g] ? 1 : 3, hack_msg[g]);
    else ui_text(F_REG, 12, rx + 18, y + 216, C_DIM, "Accepts .ips  .bps  .ups patches, or an already patched .gb");
    ui_text_wrap(F_REG, 12, rx + 18, y + 244, rw - 36, C_DIM,
                 games[g].lifted ? "Dr. Mario runs as native code. If a hack changes the game's program (not just its graphics or data) it automatically switches to compatibility mode."
                                  : "This game runs through the built-in CPU core with full cartridge bank switching, so romhacks work without any extra step.", 3);
    /* save data */
    label(rx + 18, y + 330, "Save data");
    char sp[1200];
    int bat = games[g].crc == 0 && g == GAME_SML2;
    snprintf(sp, sizeof sp, "%ssaves/", settings_dir());
    ui_text_wrap(F_REG, 12, rx + 18, y + 352, rw - 36, C_DIM, bat ? "Saved games are kept automatically next to the program, in the \"saves\" folder, one file per game." : "This cartridge has no save memory.", 3);
    if (bat && ui_button(rx + 18, y + 396, 180, 36, "Open saves folder", B_NORMAL, 1)) { mkdir_u(sp); open_folder(sp); }
}

/* ------------------------------------------------------------------ game tab: Display */
static void sub_display(int g, float x, float y)
{
    GameCfg *c = &settings.g[g];
    card(x, y, 536, 476, "GAME BOY COLORS");
    for (int i = 0; i < n_palettes; i++) {
        float cx = x + 16 + (i % 4) * 129, cy = y + 40 + (i / 4) * 52;
        int over;
        int clicked = clickable(cx, cy, 121, 44, &over);
        int sel = c->palette == i;
        ui_rrect(cx, cy, 121, 44, 10, sel ? mixc(C_PANEL2, HEX(ui_accent), 0.25f) : (over ? C_BTN_H : C_BTN));
        if (sel) ui_stroke(cx, cy, 121, 44, 10, 2, HEX(ui_accent));
        const Palette *p = &palettes[i];
        for (int k = 0; k < 4; k++) {
            ui_rrect(cx + 8 + k * 9, cy + 6, 9, 16, 2, HEX(p->bg[k]));
            if (p->multi) { ui_rrect(cx + 8 + k * 9, cy + 22, 9, 8, 2, HEX(p->ob0[k])); }
        }
        ui_text_fit(F_BOLD, 11, cx + 48, cy + 6, 66, sel ? C_TEXT : C_MUTED, p->name);
        if (p->multi) ui_text(F_REG, 10, cx + 48, cy + 24, C_DIM, "sprites differ");
        if (clicked) c->palette = i;
        if (over) ui_hint(p->name);
    }
    float yy = y + 40 + 4 * 52 + 14;
    label(x + 18, yy, "Window shape"); ui_seg(x + 18, yy + 22, 500, 36, aspect_names, N_ASPECT, &c->aspect);
    yy += 66;
    label(x + 18, yy, "Scaling"); ui_seg(x + 18, yy + 22, 500, 36, scale_names, N_SCALE, &c->scaling);
    hint_area(x + 18, yy, 500, 60, "Pixel-perfect keeps every pixel crisp. Smooth blends. Stretch fills the whole window.");
    yy += 66;
    label(x + 18, yy, "Starting window size"); ui_seg(x + 18, yy + 22, 500, 36, size_names, N_SIZE, &c->size);

    float rx = x + 548, rw = 260;
    card(rx, y, rw, 476, "LOOK");
    draw_preview(g, rx + 16, y + 40, rw - 32, 190, 1);
    label(rx + 18, y + 246, "Background picture");
    path_box(rx + 18, y + 268, rw - 36, c->bg_path, "None (solid colour)");
    if (ui_button(rx + 18, y + 312, 110, 34, "Choose…", B_NORMAL, 1)) {
        char p[1100];
        if (dlg_pick(DLG_IMAGE, "Choose a background picture", p, sizeof p)) { snprintf(c->bg_path, sizeof c->bg_path, "%s", p); ensure_background(g); if (bg_msg[0]) launcher_toast(bg_msg); }
    }
    if (ui_button(rx + 136, y + 312, 106, 34, "Remove", B_GHOST, c->bg_path[0] != 0)) { c->bg_path[0] = 0; ensure_background(g); }
    char t[64];
    if (bg_frames() > 1) snprintf(t, sizeof t, "Animated, %d frames", bg_frames());
    else snprintf(t, sizeof t, "PNG, JPG, BMP, TGA or animated GIF");
    ui_text_fit(F_REG, 11, rx + 18, y + 354, rw - 36, C_DIM, t);
    label(rx + 18, y + 380, "Darken background"); snprintf(t, sizeof t, "%d%%", c->bg_dim); ui_text_r(F_REG, 13, rx + rw - 18, y + 380, C_TEXT, t);
    ui_slider(rx + 22, y + 402, rw - 44, &c->bg_dim, 0, 80);
    if (games[g].wide_l + games[g].wide_r > 0) {
        label(rx + 18, y + 428, "Widescreen");
        snprintf(t, sizeof t, "%d%%", c->wide); ui_text_r(F_REG, 13, rx + rw - 18, y + 428, C_TEXT, t);
        if (ui_slider(rx + 22, y + 450, rw - 44, &c->wide, 0, 100)) wide_dirty = g + 1;
        hint_area(rx + 18, y + 424, rw - 36, 40, "Shows more of the level on the sides. 0% is the original 10:9 screen.");
        if (wide_note[g]) ui_text_fit(F_REG, 11, rx + 18, y + 468 - 6, rw - 36, C_WARN, "This ROM version isn't supported for widescreen.");
    } else {
        label(rx + 18, y + 428, "Widescreen");
        ui_text(F_REG, 12, rx + 18, y + 450, C_DIM, "Not available for this game");
    }
}

/* ------------------------------------------------------------------ game tab: Controls */
static void bind_cell(int g, float x, float y, float w, int kind, int btn, int slot)
{
    GameCfg *c = &settings.g[g];
    int over;
    int clicked = clickable(x, y, w, 32, &over);
    int active = cap_kind == kind && cap_btn == btn && cap_slot == slot;
    ui_rrect(x, y, w, 32, 8, active ? mixc(C_BTN, HEX(ui_accent), 0.5f) : (over ? C_BTN_H : C_BTN));
    char b[48];
    if (active) ui_text_c(F_BOLD, 12, x + w / 2, y + 7, HEX(0xFFFFFF), kind == 1 ? "press a key…" : "press a button…");
    else {
        if (kind == 1) key_code_name(c->key[btn][slot], b, sizeof b); else pad_code_name(c->pad[btn][slot], b, sizeof b);
        int none = kind == 1 ? !c->key[btn][slot] : c->pad[btn][slot] < 0;
        ui_text_c(F_REG, 13, x + w / 2, y + 7, none ? C_DIM : C_TEXT, b);
    }
    if (clicked) { cap_kind = kind; cap_btn = btn; cap_slot = slot; }
    if (over && !active) ui_hint("Click, then press the key or button you want. Backspace clears, Esc cancels.");
}

static void sub_controls(int g, float x, float y)
{
    GameCfg *c = &settings.g[g];
    card(x, y, 808, 476, "BUTTON LAYOUT");
    static const float colx[5] = {18, 150, 310, 470, 630};
    ui_text(F_BOLD, 12, x + colx[0], y + 42, C_MUTED, "Game Boy button");
    ui_text(F_BOLD, 12, x + colx[1] + 10, y + 42, C_MUTED, "Keyboard");
    ui_text(F_BOLD, 12, x + colx[2] + 10, y + 42, C_MUTED, "Keyboard 2");
    ui_text(F_BOLD, 12, x + colx[3] + 10, y + 42, C_MUTED, "Controller");
    ui_text(F_BOLD, 12, x + colx[4] + 10, y + 42, C_MUTED, "Controller 2");
    for (int b = 0; b < N_BTN; b++) {
        float ry = y + 62 + b * 36;
        if (b % 2 == 0) ui_rrect(x + 10, ry - 3, 788, 36, 8, RGBA(255, 255, 255, 6));
        ui_text(F_BOLD, 14, x + colx[0], ry + 5, C_TEXT, btn_names[b]);
        bind_cell(g, x + colx[1], ry, 150, 1, b, 0);
        bind_cell(g, x + colx[2], ry, 150, 1, b, 1);
        bind_cell(g, x + colx[3], ry, 150, 2, b, 0);
        bind_cell(g, x + colx[4], ry, 150, 2, b, 1);
    }
    float by = y + 62 + N_BTN * 36 + 8;
    if (ui_button(x + 18, by, 170, 36, "Reset to defaults", B_NORMAL, 1)) { controls_defaults(c); launcher_toast("Controls reset."); }
    label(x + 220, by + 8, "Stick dead zone");
    ui_slider(x + 330, by + 7, 220, &settings.pad_deadzone, 5, 80);
    char t[16]; snprintf(t, sizeof t, "%d%%", settings.pad_deadzone); ui_text(F_REG, 13, x + 566, by + 8, C_TEXT, t);
    char st[128];
    pad_status(st, sizeof st);
    ui_text_fit(F_REG, 12, x + 18, by + 46, 770, C_DIM, st);
    ui_text_fit(F_REG, 12, x + 18, by + 64, 770, C_DIM, "The left stick always moves too. Triggers and back paddles (DualSense Edge) can be assigned like any button.");
}

/* ------------------------------------------------------------------ game tab: DualSense */
static void hue_to_rgb(int hue, uint32_t *rgb)
{
    float h = hue / 60.0f; int i = (int)h; float f = h - i;
    float r, gg, b;
    switch (i % 6) { case 0: r = 1; gg = f; b = 0; break; case 1: r = 1 - f; gg = 1; b = 0; break; case 2: r = 0; gg = 1; b = f; break;
                     case 3: r = 0; gg = 1 - f; b = 1; break; case 4: r = f; gg = 0; b = 1; break; default: r = 1; gg = 0; b = 1 - f; break; }
    *rgb = ((uint32_t)(r * 255) << 16) | ((uint32_t)(gg * 255) << 8) | (uint32_t)(b * 255);
}

static void apply_pad_gain(int g)
{
    float s = settings.g[g].ds_speaker_vol / 100.0f;
    audio_pad_set_gain(s, s * 0.7f);
}

static void sub_dualsense(int g, float x, float y)
{
    GameCfg *c = &settings.g[g];
    int ds = 0, edge = 0;
    for (int i = 0; i < pad_count(); i++) { if (pad_is_dualsense(i)) ds = 1; if (pad_is_edge(i)) edge = 1; }
    char st[160];
    float lw = 392;
    card(x, y, lw, 476, "CONTROLLER");
    pad_status(st, sizeof st);
    status_line(x + 18, y + 38, lw - 36, ds ? 1 : (pad_count() ? 2 : 3), st);
    if (ds) ui_text_fit(F_REG, 12, x + 18, y + 60, lw - 36, C_DIM, audio_pad_available() ? (edge ? "DualSense Edge: light, rumble and speaker ready" : "DualSense: light, rumble and speaker ready") : "Light and rumble ready. Speaker needs the USB cable.");
    else ui_text_fit(F_REG, 12, x + 18, y + 60, lw - 36, C_DIM, "Plug in a DualSense or DualSense Edge (USB for the speaker).");

    label(x + 18, y + 90, "Light");
    ui_seg(x + 18, y + 112, lw - 36, 34, led_names, N_LED, &c->ds_led_mode);
    label(x + 18, y + 158, "Brightness"); ui_slider(x + 110, y + 156, lw - 110 - 60, &c->ds_bright, 0, 100);
    char t[24]; snprintf(t, sizeof t, "%d%%", c->ds_bright); ui_text_r(F_REG, 13, x + lw - 18, y + 158, C_TEXT, t);
    if (c->ds_led_mode == LED_CUSTOM) {
        static int hue_for[N_GAMES] = {-1, -1, -1};
        static int hue[N_GAMES];
        if (hue_for[g] < 0) { hue_for[g] = 1; hue[g] = 210; }
        label(x + 18, y + 192, "Colour");
        if (ui_slider(x + 110, y + 190, lw - 110 - 60, &hue[g], 0, 359)) hue_to_rgb(hue[g], &c->ds_color);
        ui_rrect(x + lw - 18 - 36, y + 190, 36, 22, 6, HEX(c->ds_color));
    }
    ui_text(F_BOLD, 12, x + 18, y + 232, C_DIM, "RUMBLE AND SPEAKER");
    label(x + 18, y + 256, "Rumble strength"); ui_slider(x + 160, y + 254, lw - 160 - 60, &c->ds_rumble, 0, 100);
    snprintf(t, sizeof t, "%d%%", c->ds_rumble); ui_text_r(F_REG, 13, x + lw - 18, y + 256, C_TEXT, t);
    label(x + 18, y + 290, "Speaker volume");
    if (ui_slider(x + 160, y + 288, lw - 160 - 60, &c->ds_speaker_vol, 0, 100)) apply_pad_gain(g);
    snprintf(t, sizeof t, "%d%%", c->ds_speaker_vol); ui_text_r(F_REG, 13, x + lw - 18, y + 290, C_TEXT, t);
    float bw = (lw - 36 - 16) / 3;
    if (ui_button(x + 18, y + 334, bw, 36, "Test light", B_NORMAL, 1)) { pad_set_context(g, 0); pad_flash(0xFFFFFF); }
    if (ui_button(x + 18 + bw + 8, y + 334, bw, 36, "Test rumble", B_NORMAL, 1)) pad_rumble(c->ds_rumble, 350);
    if (ui_button(x + 18 + (bw + 8) * 2, y + 334, bw, 36, "Test speaker", B_NORMAL, 1)) {
        if (audio_pad_open()) { apply_pad_gain(g); audio_pad_play_beep(0); } else launcher_toast("Speaker not found. Connect the controller with the USB cable.");
    }
    ui_text_wrap(F_REG, 12, x + 18, y + 386, lw - 36, C_DIM,
                 "The speaker plays the game's own sound effect while the event happens, so you hear a coin or a clear from the controller. Rumble and light follow the choices on the right.", 3);

    float rx = x + lw + 12, rw = 808 - lw - 12;
    card(rx, y, rw, 476, "WHEN THIS HAPPENS");
    int n = events_count(g);
    float tx[3] = {rw - 18 - 44 - 64 - 64 - 44 + 0, 0, 0};
    (void)tx;
    float c1 = rx + rw - 18 - 44 * 3 - 20 * 2 - 0, c2 = c1 + 64, c3 = c2 + 64;
    c1 = rx + 196; c2 = c1 + 50; c3 = c2 + 50;
    ui_text_c(F_BOLD, 11, c1 + 22, y + 40, C_MUTED, "LIGHT"); ui_text_c(F_BOLD, 11, c2 + 22, y + 40, C_MUTED, "RUMBLE"); ui_text_c(F_BOLD, 11, c3 + 22, y + 40, C_MUTED, "SPEAKER");
    for (int i = 0; i < n; i++) {
        const EventDef *d = events_def(g, i);
        float ry = y + 62 + i * 46;
        ui_rrect(rx + 10, ry - 3, rw - 20, 44, 8, RGBA(255, 255, 255, i % 2 ? 0 : 6));
        ui_rrect(rx + 18, ry + 8, 8, 24, 3, HEX(d->color));
        ui_text_fit(F_BOLD, 13, rx + 34, ry + 2, 150, C_TEXT, d->name);
        ui_text_fit(F_REG, 11, rx + 34, ry + 20, 150, C_DIM, d->desc);
        int v[3] = {(c->ds_ev_led >> i) & 1, (c->ds_ev_rumble >> i) & 1, (c->ds_ev_speaker >> i) & 1};
        float cx[3] = {c1, c2, c3};
        for (int k = 0; k < 3; k++) {
            if (ui_toggle(cx[k], ry + 9, &v[k])) {
                int *m = k == 0 ? &c->ds_ev_led : (k == 1 ? &c->ds_ev_rumble : &c->ds_ev_speaker);
                *m = (*m & ~(1 << i)) | (v[k] << i);
            }
        }
        if (ui_button(c3 + 52, ry + 6, 46, 30, "Try", B_GHOST, 1)) {
            pad_set_context(g, 0);
            pad_flash(d->color);
            pad_rumble(c->ds_rumble * d->rumble_str / 100, d->rumble_ms);
            if ((c->ds_ev_speaker >> i) & 1 || 1) { if (audio_pad_open()) { apply_pad_gain(g); audio_pad_play_beep(d->beep ? d->beep : 0); } }
        }
    }
    ui_text_wrap(F_REG, 11, rx + 18, y + 62 + n * 46 + 8, rw - 36, C_DIM, "The Try button tries the light, rumble and speaker together.", 2);
}

/* ------------------------------------------------------------------ game tab: Textures */
static void sub_textures(int g, float x, float y)
{
    GameCfg *c = &settings.g[g];
    card(x, y, 808, 476, "TEXTURE PACK");
    label(x + 18, y + 42, "Pack folder");
    path_box(x + 18, y + 64, 808 - 36 - 330, c->tex_path, "Choose a folder of tile pictures");
    float bx = x + 808 - 18 - 312;
    if (ui_button(bx, y + 64, 116, 36, "Choose…", B_NORMAL, 1)) {
        char p[1100];
        if (dlg_pick(DLG_FOLDER, "Choose a texture pack folder", p, sizeof p)) { snprintf(c->tex_path, sizeof c->tex_path, "%s", p); c->tex_on = 1; loaded_pack_game = -1; }
    }
    if (ui_button(bx + 124, y + 64, 90, 36, "Clear", B_GHOST, c->tex_path[0] != 0)) { c->tex_path[0] = 0; c->tex_on = 0; loaded_pack_game = -1; }
    if (ui_button(bx + 222, y + 64, 90, 36, "Open", B_GHOST, c->tex_path[0] != 0)) open_folder(c->tex_path);
    label(x + 18, y + 118, "Use this pack");
    if (ui_toggle(x + 130, y + 114, &c->tex_on)) loaded_pack_game = -1;
    ensure_pack(g);
    char t[160];
    if (c->tex_on && c->tex_path[0]) {
        if (pack_n < 0) snprintf(t, sizeof t, "Can't read that folder.");
        else if (pack_n == 0) snprintf(t, sizeof t, "No usable tiles found. Files must be named like 0123456789abcdef.png");
        else snprintf(t, sizeof t, "%d tiles loaded, up to %dx sharper", pack_n, texpack_scale());
        status_line(x + 200, y + 116, 590, pack_n > 0 ? 1 : 3, t);
    } else ui_text(F_REG, 12, x + 200, y + 118, C_DIM, "Replaces the game's 8x8 tiles with your own pictures (any size that is a multiple of 8).");
    ui_rect(x + 18, y + 156, 772, 1, C_LINE);

    ui_text(F_BOLD, 12, x + 18, y + 172, C_DIM, "MAKE YOUR OWN PACK");
    ui_text_wrap(F_REG, 13, x + 18, y + 196, 772, C_MUTED,
                 "Play (or just open the game) and the launcher remembers every tile the game draws. Export them to a folder, redraw the ones you like, then choose that folder above. Tiles you don't change keep their original look.", 3);
    label(x + 18, y + 262, "Remember tiles while I play");
    ui_toggle(x + 220, y + 258, &c->tex_collect);
    tex_collect_begin(g);
    snprintf(t, sizeof t, "%d tiles collected so far", tex_collected());
    ui_text(F_BOLD, 13, x + 290, y + 262, C_TEXT, t);
    label(x + 18, y + 304, "Export size");
    static const char *sz[3] = {"4x", "8x", "16x"};
    int si = export_scale == 4 ? 0 : (export_scale == 8 ? 1 : 2);
    if (ui_seg(x + 120, y + 296, 210, 36, sz, 3, &si)) export_scale = si == 0 ? 4 : (si == 1 ? 8 : 16);
    ui_text(F_REG, 12, x + 346, y + 304, C_DIM, "pixels per original pixel (8x = 64x64 per tile)");
    if (ui_button(x + 18, y + 350, 250, 42, "Export tiles to a folder…", B_PRIMARY, tex_collected() > 0)) {
        char p[1100];
        if (dlg_pick(DLG_FOLDER, "Choose where to save the tiles", p, sizeof p)) {
            int n = tex_export(p, &palettes[c->palette], export_scale, export_msg[g], sizeof export_msg[g]);
            if (n > 0) { snprintf(c->tex_path, sizeof c->tex_path, "%s", p); loaded_pack_game = -1; }
            launcher_toast(export_msg[g]);
        }
    }
    if (ui_button(x + 280, y + 350, 160, 42, "Forget collected", B_GHOST, tex_collected() > 0)) { tex_collect_clear(); export_msg[g][0] = 0; }
    if (export_msg[g][0]) ui_text_fit(F_REG, 13, x + 18, y + 404, 772, C_OK, export_msg[g]);
    ui_text_wrap(F_REG, 12, x + 18, y + 428, 772, C_DIM, "Each file is named after its tile. _overview.png shows them all on one sheet. Transparent pixels in a replacement let the original show through.", 3);
}

/* ------------------------------------------------------------------ filters tab */
static void slider_row(float x, float y, float w, const char *name, int *v, const char *hint)
{
    char t[16];
    label(x, y + 2, name);
    if (ui_slider(x + 130, y, w - 130 - 54, v, 0, 100)) settings.flt.preset = 0;
    snprintf(t, sizeof t, "%d", *v);
    ui_text_r(F_REG, 13, x + w, y + 2, C_TEXT, t);
    hint_area(x, y - 4, w, 30, hint);
}

static void tab_filters(float x, float y)
{
    FilterCfg *f = &settings.flt;
    card(x, y, 428, 504, "SCREEN LOOK");
    ui_text(F_BOLD, 12, x + 18, y + 38, C_MUTED, "Presets");
    for (int i = 0; i < n_filter_presets; i++) {
        float cx = x + 18 + (i % 3) * 133, cy = y + 58 + (i / 3) * 38;
        if (ui_chip(cx, cy, 125, 30, filter_preset_names[i], f->preset == i && i != 0 ? 1 : (i == 0 && !f->lcd_grid && !f->ghost && !f->scanlines && !f->mask && !f->curve && !f->bloom && !f->hdr && !f->vignette && !f->blur)))
            filter_preset(f, i);
    }
    float sy = y + 144;
    ui_text(F_BOLD, 12, x + 18, sy, C_MUTED, "Game Boy screen");
    slider_row(x + 18, sy + 24, 392, "Dot grid", &f->lcd_grid, "Dark gaps between the screen's dots, like the real LCD");
    slider_row(x + 18, sy + 52, 392, "Ghosting", &f->ghost, "Pixels fade slowly like the old LCD (visible while playing)");
    sy += 88;
    ui_text(F_BOLD, 12, x + 18, sy, C_MUTED, "CRT TV");
    slider_row(x + 18, sy + 24, 392, "Scanlines", &f->scanlines, "Dark lines between rows");
    slider_row(x + 18, sy + 52, 392, "Shadow mask", &f->mask, "Red, green and blue stripes");
    slider_row(x + 18, sy + 80, 392, "Curvature", &f->curve, "Bulges the picture like a curved tube");
    sy += 116;
    ui_text(F_BOLD, 12, x + 18, sy, C_MUTED, "Light");
    slider_row(x + 18, sy + 24, 392, "Bloom", &f->bloom, "Bright areas glow");
    slider_row(x + 18, sy + 52, 392, "HDR", &f->hdr, "Brighter highlights, richer colour and a stronger glow");
    slider_row(x + 18, sy + 80, 392, "Vignette", &f->vignette, "Darker corners");
    slider_row(x + 18, sy + 108, 392, "Softness", &f->blur, "Slight horizontal blur");

    float rx = x + 440, rw = 808 - 440;
    card(rx, y, rw, 504, "PREVIEW");
    for (int i = 0; i < N_GAMES; i++) {
        static const char *short_names[N_GAMES] = {"Dr. Mario", "Mario Land", "Mario Land 2"};
        float cw = (rw - 36 - 16) / 3;
        if (ui_chip(rx + 18 + i * (cw + 8), y + 38, cw, 30, short_names[i], filter_game == i)) filter_game = i;
    }
    draw_preview(filter_game, rx + 16, y + 80, rw - 32, 300, 1);
    ui_text_wrap(F_REG, 12, rx + 18, y + 394, rw - 36, C_DIM,
                 "These filters apply to every game, in the launcher preview and while you play. Try the presets first, then adjust.", 3);
    if (ui_button(rx + 18, y + 452, 140, 36, "Turn all off", B_GHOST, 1)) filter_preset(f, 0);
}

/* ------------------------------------------------------------------ audio tab */
static const char *sfx_names[N_UI_SFX] = {"Hover", "Click", "Confirm", "Back", "Toggle"};

static void tab_audio(float x, float y)
{
    Settings *s = &settings;
    char t[96];
    float lw = 396;
    card(x, y, lw, 200, "GAME SOUND");
    label(x + 18, y + 44, "Volume");
    ui_slider(x + 100, y + 42, lw - 100 - 70, &s->volume, 0, 200);
    snprintf(t, sizeof t, "%d%%", s->volume); ui_text_r(F_REG, 13, x + lw - 18, y + 44, C_TEXT, t);
    label(x + 18, y + 84, "Sound delay");
    ui_seg(x + 18, y + 108, lw - 36, 34, lat_names, N_LAT, &s->latency);
    hint_area(x + 18, y + 84, lw - 36, 60, "Higher is safest. Choose High if you ever hear crackling or choppy sound.");
    AudioStats st;
    audio_get_stats(&st);
    if (audio_ok()) snprintf(t, sizeof t, "Output: %d Hz, %d sample buffer", st.device_rate, st.device_samples);
    else snprintf(t, sizeof t, "No audio device found");
    ui_text_fit(F_REG, 12, x + 18, y + 154, lw - 36, C_DIM, t);
    ui_text(F_REG, 12, x + 18, y + 174, C_DIM, "Changes to the delay apply the next time you start the program.");

    card(x, y + 212, lw, 292, "MENU MUSIC");
    label(x + 18, y + 256, "Play music in the launcher");
    if (ui_toggle(x + lw - 18 - 44, y + 252, &s->menu_music)) audio_menu_apply();
    label(x + 18, y + 296, "Volume");
    if (ui_slider(x + 100, y + 294, lw - 100 - 70, &s->menu_music_vol, 0, 100)) audio_menu_apply();
    snprintf(t, sizeof t, "%d%%", s->menu_music_vol); ui_text_r(F_REG, 13, x + lw - 18, y + 296, C_TEXT, t);
    label(x + 18, y + 334, "Track");
    path_box(x + 18, y + 356, lw - 36, s->menu_music_path, "Built-in chiptune");
    float bw = (lw - 36 - 16) / 3;
    if (ui_button(x + 18, y + 404, bw, 36, "Import…", B_NORMAL, 1)) {
        char p[1100];
        if (dlg_pick(DLG_AUDIO, "Choose a music file", p, sizeof p)) {
            char old[512]; snprintf(old, sizeof old, "%s", s->menu_music_path);
            snprintf(s->menu_music_path, sizeof s->menu_music_path, "%s", p);
            audio_menu_apply();
            if (!s->menu_music_path[0]) { launcher_toast("Couldn't read that audio file."); snprintf(s->menu_music_path, sizeof s->menu_music_path, "%s", old); audio_menu_apply(); }
            else { audio_music_preview(); launcher_toast("Menu music changed."); }
        }
    }
    if (ui_button(x + 18 + bw + 8, y + 404, bw, 36, "Built-in", B_GHOST, s->menu_music_path[0] != 0)) { s->menu_music_path[0] = 0; audio_menu_apply(); }
    if (ui_button(x + 18 + (bw + 8) * 2, y + 404, bw, 36, "Restart", B_GHOST, 1)) audio_music_preview();
    ui_text_wrap(F_REG, 12, x + 18, y + 456, lw - 36, C_DIM, "WAV, MP3, OGG or FLAC. It loops while you are in the launcher and stops when a game starts. You can also drop a file onto this tab.", 3);

    float rx = x + lw + 12, rw = 808 - lw - 12;
    card(rx, y, rw, 504, "INTERFACE SOUNDS");
    label(rx + 18, y + 44, "Click and hover sounds");
    ui_toggle(rx + rw - 18 - 44, y + 40, &s->ui_sfx);
    label(rx + 18, y + 84, "Volume");
    if (ui_slider(rx + 100, y + 82, rw - 100 - 70, &s->ui_sfx_vol, 0, 100)) audio_menu_apply();
    snprintf(t, sizeof t, "%d%%", s->ui_sfx_vol); ui_text_r(F_REG, 13, rx + rw - 18, y + 84, C_TEXT, t);
    for (int i = 0; i < N_UI_SFX; i++) {
        float ry = y + 126 + i * 70;
        ui_rrect(rx + 10, ry - 4, rw - 20, 64, 10, RGBA(255, 255, 255, i % 2 ? 0 : 6));
        ui_text(F_BOLD, 14, rx + 18, ry + 2, C_TEXT, sfx_names[i]);
        ui_text_fit_tail(F_REG, 12, rx + 18, ry + 26, rw - 36 - 0, C_DIM, s->ui_sfx_path[i][0] ? path_base(s->ui_sfx_path[i]) : "Built-in");
        float bx = rx + rw - 18 - 3 * 64 - 2 * 6;
        if (ui_button(bx, ry + 2, 64, 30, "Play", B_NORMAL, 1)) audio_sfx_preview(i);
        if (ui_button(bx + 70, ry + 2, 64, 30, "Import", B_NORMAL, 1)) {
            char p[1100];
            if (dlg_pick(DLG_AUDIO, "Choose a sound file", p, sizeof p)) {
                snprintf(s->ui_sfx_path[i], sizeof s->ui_sfx_path[i], "%s", p);
                audio_menu_apply();
                if (!s->ui_sfx_path[i][0]) launcher_toast("Couldn't read that audio file."); else audio_sfx_preview(i);
            }
        }
        if (ui_button(bx + 140, ry + 2, 64, 30, "Reset", B_GHOST, s->ui_sfx_path[i][0] != 0)) { s->ui_sfx_path[i][0] = 0; audio_menu_apply(); }
    }
    ui_text_wrap(F_REG, 12, rx + 18, y + 126 + N_UI_SFX * 70 + 6, rw - 36, C_DIM, "Short sounds work best. Any WAV, MP3, OGG or FLAC.", 3);
    (void)sfx_test_msg;
}

/* ------------------------------------------------------------------ frame */
static const char *tab_labels[N_TABS] = {"", "", "", "Filters", "Audio & menu"};

static int tab_button(float x, float y, float w, float h, int selected, uint32_t accent)
{
    int over;
    int clicked = clickable(x, y, w, h, &over);
    float a = ui_anim(ui_next_id() * 4 + 3, selected ? 1.0f : (over ? 0.5f : 0.0f), 16.0f);
    if (a > 0.01f) ui_rrect(x, y, w, h, 12, RGBA(255, 255, 255, (int)(a * (selected ? 24 : 14))));
    if (selected) ui_rrect(x, y + 10, 4, h - 20, 2, HEX(accent));
    return clicked;
}

LauncherResult launcher_frame(float dt)
{
    LauncherResult res = {-1, 0};
    anim_clock += dt;
    int g = launcher_current_game();
    if (tab < N_GAMES) last_game_tab = tab;
    ui_accent = tab < N_GAMES ? games[tab].accent : 0x4C8DFF;
    ensure_background(g);
    bg_update(dt);

    /* backdrop */
    float vx = ui_view_x0(), vy = ui_view_y0(), vw = ui_view_w(), vh = ui_view_h();
    ui_vgrad(vx, vy, vw, vh, C_BG2, C_BG);
    int bw, bh;
    SDL_Texture *bt = bg_texture(&bw, &bh);
    if (bt) {
        draw_cover(bt, bw, bh, vx, vy, vw, vh);
        ui_rect(vx, vy, vw, vh, RGBA(8, 9, 14, (int)(60 + settings.g[g].bg_dim * 2.4f)));
    }

    /* top bar */
    ui_text(F_BOLD, 20, 28, 22, C_TEXT, "Dr Mario Launcher");
    ui_text(F_REG, 12, 28, 50, C_DIM, "Game Boy classics on your PC");
    ui_rect(24, 80, UI_W - 48, 1, C_LINE);

    /* sidebar */
    float sx = 24, sy = 98;
    for (int i = 0; i < N_GAMES; i++) {
        float y = sy + i * 76;
        if (tab_button(sx, y, 224, 68, tab == i, games[i].accent)) { tab = i; }
        ui_text_fit(F_BOLD, 15, sx + 18, y + 11, 196, tab == i ? C_TEXT : C_MUTED, games[i].name);
        status_line(sx + 18, y + 38, 196, rs[i].ok ? 1 : 2, rs[i].ok ? (settings.g[i].hack_path[0] && hack_ok[i] ? "Ready with romhack" : "Ready to play") : "ROM needed");
    }
    float y2 = sy + N_GAMES * 76 + 8;
    ui_rect(sx + 8, y2, 208, 1, C_LINE);
    for (int i = TAB_FILTERS; i < N_TABS; i++) {
        float y = y2 + 12 + (i - TAB_FILTERS) * 52;
        if (tab_button(sx, y, 224, 44, tab == i, 0x4C8DFF)) tab = i;
        ui_text(F_BOLD, 15, sx + 18, y + 11, tab == i ? C_TEXT : C_MUTED, tab_labels[i]);
    }

    float cx = 268, cy = 98;
    if (tab < N_GAMES) {
        ui_accent = games[tab].accent;
        ui_text(F_BOLD, 28, cx, cy - 4, C_TEXT, games[tab].name);
        ui_text(F_REG, 13, cx, cy + 34, C_MUTED, games[tab].sub);
        int can = rs[tab].ok;
        if (ui_button(cx + 808 - 176, cy + 2, 176, 52, can ? "Play" : "Needs ROM", B_PRIMARY, can)) res.play = tab;
        if (!can) hint_area(cx + 808 - 176, cy + 2, 176, 52, "Choose your ROM on the Game tab (or drop it on this window)");
        ui_seg(cx, cy + 66, 808, 38, sub_names, N_SUB, &sub[tab]);
        float y = cy + 118;
        pad_set_context(tab, 0);
        switch (sub[tab]) {
        case SUB_GAME: sub_game(tab, cx, y); break;
        case SUB_DISPLAY: sub_display(tab, cx, y); break;
        case SUB_CONTROLS: sub_controls(tab, cx, y); break;
        case SUB_DUALSENSE: sub_dualsense(tab, cx, y); break;
        default: sub_textures(tab, cx, y); break;
        }
    } else if (tab == TAB_FILTERS) {
        ui_text(F_BOLD, 28, cx, cy - 4, C_TEXT, "Filters");
        ui_text(F_REG, 13, cx, cy + 34, C_MUTED, "Make the screen look like a real Game Boy, a CRT TV, or something glowing.");
        tab_filters(cx, cy + 58);
    } else {
        ui_text(F_BOLD, 28, cx, cy - 4, C_TEXT, "Audio & menu");
        ui_text(F_REG, 13, cx, cy + 34, C_MUTED, "Game volume, the launcher's own music and the little click sounds.");
        tab_audio(cx, cy + 58);
    }

    /* footer */
    const char *h = ui_hint_text();
    ui_text_fit(F_REG, 12, 28, UI_H - 26, 900, C_DIM, h && h[0] ? h : "Drag and drop a ROM, patch, picture, sound or texture folder anywhere on this window.");

    /* toast */
    if (toast_t > 0) {
        toast_t -= dt;
        float a = toast_t < 0.4f ? toast_t / 0.4f : 1.0f;
        float tw = ui_text_w(F_BOLD, 14, toast_msg) + 40;
        float tx = UI_W * 0.5f - tw * 0.5f, ty = UI_H - 74;
        ui_shadow(tx, ty, tw, 40, 12, 10, RGBA(0, 0, 0, (int)(90 * a)));
        ui_rrect(tx, ty, tw, 40, 12, RGBA(40, 46, 66, (int)(250 * a)));
        ui_text_c(F_BOLD, 14, UI_W * 0.5f, ty + 10, RGBA(237, 239, 246, (int)(255 * a)), toast_msg);
    }

    /* binding capture hint */
    if (cap_kind) ui_hint(cap_kind == 1 ? "Press a key. Backspace clears the slot, Esc cancels." : "Press a controller button or trigger. Esc (keyboard) cancels.");

    if (wide_dirty && !ui_mouse.down) { make_preview(wide_dirty - 1); wide_dirty = 0; }
    if (ui_mouse.released) click_id = 0;
    settings.last_tab = tab;
    save_timer += dt;
    if (save_timer > 2.0f && !ui_mouse.down) { save_timer = 0; settings_save(); }

    /* pad light follows the tab while in the launcher */
    pad_frame(dt);
    return res;
}

/* ------------------------------------------------------------------ events */
void launcher_event(const SDL_Event *e)
{
    if (cap_kind) {
        GameCfg *c = &settings.g[launcher_current_game()];
        if (e->type == SDL_KEYDOWN && !e->key.repeat) {
            SDL_Keycode k = e->key.keysym.sym;
            if (k == SDLK_ESCAPE) { cap_kind = 0; return; }
            if (cap_kind == 1) {
                c->key[cap_btn][cap_slot] = (k == SDLK_BACKSPACE || k == SDLK_DELETE) ? 0 : k;
                cap_kind = 0;
            } else if (k == SDLK_BACKSPACE || k == SDLK_DELETE) { c->pad[cap_btn][cap_slot] = -1; cap_kind = 0; }
        } else if (cap_kind == 2) {
            int code = pad_capture(e);
            if (code >= 0) { c->pad[cap_btn][cap_slot] = code; cap_kind = 0; }
        }
        return;
    }
    if (e->type == SDL_KEYDOWN && !e->key.repeat) {
        if (e->key.keysym.sym == SDLK_TAB && (e->key.keysym.mod & KMOD_CTRL)) tab = (tab + 1) % N_TABS;
    }
}

static int ends_with(const char *s, const char *ext)
{
    size_t a = strlen(s), b = strlen(ext);
    if (a < b) return 0;
    for (size_t i = 0; i < b; i++) if ((s[a - b + i] | 32) != (ext[i] | 32)) return 0;
    return 1;
}

void launcher_drop(const char *path)
{
    int g = launcher_current_game();
    int id = rom_identify_file(path);
    if (id >= 0) {
        snprintf(settings.g[id].rom_path, sizeof settings.g[id].rom_path, "%s", path);
        rom_check(id);
        if (rs[id].ok) make_preview(id);
        tab = id;
        launcher_toast(rs[id].ok ? "ROM added." : rs[id].msg);
        return;
    }
    if (ends_with(path, ".ips") || ends_with(path, ".bps") || ends_with(path, ".ups")) { if (tab >= N_GAMES) tab = g; set_hack(tab, path); launcher_toast(hack_msg[tab]); return; }
    if (ends_with(path, ".png") || ends_with(path, ".jpg") || ends_with(path, ".jpeg") || ends_with(path, ".bmp") || ends_with(path, ".gif") || ends_with(path, ".tga")) {
        if (tab >= N_GAMES) tab = g;
        snprintf(settings.g[tab].bg_path, sizeof settings.g[tab].bg_path, "%s", path);
        ensure_background(tab);
        launcher_toast(bg_msg[0] ? bg_msg : "Background changed.");
        return;
    }
    if (ends_with(path, ".wav") || ends_with(path, ".mp3") || ends_with(path, ".ogg") || ends_with(path, ".flac")) {
        snprintf(settings.menu_music_path, sizeof settings.menu_music_path, "%s", path);
        audio_menu_apply();
        tab = TAB_AUDIO;
        launcher_toast(settings.menu_music_path[0] ? "Menu music changed." : "Couldn't read that audio file.");
        return;
    }
    if (ends_with(path, ".gb") || ends_with(path, ".gbc")) { launcher_toast("That ROM isn't one of the supported games."); return; }
    /* a folder: texture pack */
    {
        SDL_RWops *r = SDL_RWFromFile(path, "rb");
        if (r) { SDL_RWclose(r); launcher_toast("Not a file type I know."); return; }
        if (tab >= N_GAMES) tab = g;
        snprintf(settings.g[tab].tex_path, sizeof settings.g[tab].tex_path, "%s", path);
        settings.g[tab].tex_on = 1;
        loaded_pack_game = -1;
        sub[tab] = SUB_TEXTURES;
        launcher_toast("Texture pack folder set.");
    }
}
