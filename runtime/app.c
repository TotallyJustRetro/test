/* Application loop: launcher <-> game. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "app.h"
#include "logo.h"
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

static SDL_Window *win;
static SDL_Renderer *ren;
static int win_w = 1100, win_h = 720;

static void sfx(int w) { audio_sfx(w); }

static void out_size(int *w, int *h)
{
    if (SDL_GetRendererOutputSize(ren, w, h) != 0 || *w <= 0 || *h <= 0) SDL_GetWindowSize(win, w, h);
}

static void mouse_sync(void)
{
    int mx, my, ww, wh;
    SDL_GetMouseState(&mx, &my);
    SDL_GetWindowSize(win, &ww, &wh);
    ui_set_mouse(mx, my, ww, wh);
}

static void set_game_window(int g)
{
    const GameCfg *c = &settings.g[g];
    if (c->size == SIZE_FULLSCREEN) { SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP); return; }
    SDL_SetWindowFullscreen(win, 0);
    int hh = c->size == SIZE_SMALL ? 432 : (c->size == SIZE_MEDIUM ? 576 : 720);
    float ar = c->aspect == ASPECT_4_3 ? 4.0f / 3 : (c->aspect == ASPECT_16_9 ? 16.0f / 9 : ppu_w / 144.0f);
    SDL_SetWindowSize(win, (int)(hh * ar), hh);
    SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

static void draw_bg_cover(float dim)
{
    int bw, bh, W, H;
    SDL_Texture *t = bg_texture(&bw, &bh);
    out_size(&W, &H);
    if (!t) return;
    float s = fmaxf((float)W / bw, (float)H / bh);
    SDL_Rect src = {(int)((bw - W / s) * 0.5f), (int)((bh - H / s) * 0.5f), (int)(W / s), (int)(H / s)};
    SDL_RenderCopy(ren, t, &src, NULL);
    if (dim > 0) {
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, (Uint8)(dim * 2.55f));
        SDL_RenderFillRect(ren, NULL);
    }
}

/* returns 0 = back to launcher, 1 = quit */
static int play(int g)
{
    char err[256];
    GameCfg *c = &settings.g[g];
    if (launcher_prepare(g, err, sizeof err)) { launcher_toast(err); return 0; }
    audio_menu_music(0);
    char sp[1200];
    snprintf(sp, sizeof sp, "%ssaves/", settings_dir());
    mkdir_u(sp);
    snprintf(sp, sizeof sp, "%ssaves/%s.sav", settings_dir(), games[g].id);
    emu_set_save_path(sp);
    bg_load(c->bg_path);
    texpack_load(c->tex_on ? c->tex_path : "");
    tex_collect_begin(g);
    render_reset();
    pad_set_context(g, 1);
    set_game_window(g);
    if (emu_start(0)) { launcher_toast("Couldn't start the game."); return 0; }

    Frame *f = SDL_malloc(sizeof *f);
    memset(f, 0, sizeof *f);
    int quit = 0, paused = 0, have = 0;
    Uint64 last = SDL_GetPerformanceCounter();
    int shot = 0;
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            pad_event(&e);
            if (e.type == SDL_QUIT) { quit = 2; }
            else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                switch (e.key.keysym.sym) {
                case SDLK_ESCAPE: quit = 1; break;
                case SDLK_F11: SDL_SetWindowFullscreen(win, (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP); break;
                case SDLK_p: paused = !paused; emu_set_paused(paused); break;
                case SDLK_TAB: emu_set_turbo(1); break;
                case SDLK_F12: shot = 1; break;
                }
            } else if (e.type == SDL_KEYUP && e.key.keysym.sym == SDLK_TAB) { emu_set_turbo(0); }
            else if (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) quit = 1;
        }
        uint8_t b, d;
        pad_poll(g, &b, &d);
        emu_input(b, d);
        if (emu_frame_get(f)) { have = 1; tex_collect_frame(f); }
        int ev;
        while ((ev = events_pop()) >= 0) pad_event_fx(g, ev);
        Uint64 now = SDL_GetPerformanceCounter();
        float dt = (float)(now - last) / SDL_GetPerformanceFrequency();
        last = now;

        int W, H;
        out_size(&W, &H);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        draw_bg_cover((float)c->bg_dim);
        bg_update(dt);
        if (have) {
            render_build(f, g, 1);
            SDL_Rect r;
            render_fit(W, H, c->aspect, c->scaling, &r);
            render_draw(&r, c->scaling);
            pad_set_screen_color(render_avg_color());
        }
        if (paused) {
            ui_begin(W, H, dt);
            ui_rect(ui_view_x0(), ui_view_y0(), ui_view_w(), ui_view_h(), RGBA(0, 0, 0, 120));
            ui_text_c(F_BOLD, 32, UI_W / 2, UI_H / 2 - 20, C_TEXT, "Paused");
            ui_end();
        }
        pad_frame(dt);
        if (shot) { shot = 0; SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888);
                    if (s) { SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
                             char p[1200]; snprintf(p, sizeof p, "%sscreenshot_%u.bmp", settings_dir(), SDL_GetTicks()); SDL_SaveBMP(s, p); SDL_FreeSurface(s); } }
        SDL_RenderPresent(ren);
    }
    emu_stop();
    SDL_free(f);
    tex_collect_save();
    pad_set_context(g, 0);
    SDL_SetWindowFullscreen(win, 0);
    SDL_SetWindowSize(win, win_w, win_h);
    SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    return quit == 2;
}

int app_run(const char *autorun, const char *hack)
{
    (void)hack;
#ifdef _WIN32
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    settings_load();
    win = SDL_CreateWindow("PipeClean", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    SDL_SetWindowMinimumSize(win, 640, 420);
    {   /* window / taskbar icon */
        SDL_Surface *ic = SDL_CreateRGBSurfaceWithFormatFrom((void *)logo_rgba, LOGO_W, LOGO_H, 32, LOGO_W * 4, SDL_PIXELFORMAT_ABGR8888);
        if (ic) { SDL_SetWindowIcon(win, ic); SDL_FreeSurface(ic); }
    }
    if (getenv("GBL_SIZE")) { sscanf(getenv("GBL_SIZE"), "%dx%d", &win_w, &win_h); SDL_SetWindowSize(win, win_w, win_h); }
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) { fprintf(stderr, "renderer: %s\n", SDL_GetError()); return 1; }
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    ui_init(ren);
    bg_init(ren);
    render_init(ren);
    launcher_set_renderer(ren);
    audio_init(settings.latency);
    pad_init();
    ui_sfx_cb = sfx;
    launcher_init();
    launcher_enter();
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    int quit = 0;
    if (autorun) {
        int id = rom_identify_file(autorun);
        if (id >= 0) { snprintf(settings.g[id].rom_path, sizeof settings.g[id].rom_path, "%s", autorun); quit = play(id); launcher_enter(); }
    }
    Uint64 last = SDL_GetPerformanceCounter();
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            pad_event(&e);
            launcher_event(&e);
            switch (e.type) {
            case SDL_QUIT: quit = 1; break;
            case SDL_MOUSEBUTTONDOWN: if (e.button.button == SDL_BUTTON_LEFT) { mouse_sync(); ui_mouse_button(1); } break;
            case SDL_MOUSEBUTTONUP: if (e.button.button == SDL_BUTTON_LEFT) { mouse_sync(); ui_mouse_button(0); } break;
            case SDL_MOUSEWHEEL: ui_mouse.wheel += e.wheel.y; break;
            case SDL_DROPFILE: launcher_drop(e.drop.file); SDL_free(e.drop.file); break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_LEAVE) ui_mouse.inside = 0;
                break;
            }
        }
        mouse_sync();
        Uint64 now = SDL_GetPerformanceCounter();
        float dt = (float)(now - last) / SDL_GetPerformanceFrequency();
        last = now;
        int W, H;
        out_size(&W, &H);
        SDL_SetRenderDrawColor(ren, 14, 16, 22, 255);
        SDL_RenderClear(ren);
        ui_begin(W, H, dt);
        LauncherResult r = launcher_frame(dt);
        ui_end();
        if (getenv("GBL_SHOT")) {
            static int fr;
            if (fr == 3 && getenv("GBL_MOUSE")) { int mx, my; sscanf(getenv("GBL_MOUSE"), "%d,%d", &mx, &my); SDL_WarpMouseInWindow(win, mx, my); }
            if (++fr == 8) {
                SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888);
                SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
                SDL_SaveBMP(s, getenv("GBL_SHOT"));
                printf("out %dx%d hint='%s' mouse=%.0f,%.0f\n", W, H, ui_hint_text(), ui_mouse.x, ui_mouse.y);
                quit = 1;
            }
        }
        SDL_RenderPresent(ren);
        if (r.play >= 0) {
            audio_sfx(SFX_CONFIRM);
            settings_save();
            quit = play(r.play);
            launcher_enter();
            memset(&ui_mouse, 0, sizeof ui_mouse);
        }
        if (!(SDL_GetWindowFlags(win) & SDL_WINDOW_SHOWN)) SDL_Delay(30);
    }
    launcher_shutdown();
    settings_save();
    pad_shutdown();
    audio_shutdown();
    render_shutdown();
    bg_shutdown();
    ui_shutdown();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
