#include <string.h>
#include <math.h>
#include "pad.h"
#include "settings.h"
#include "events.h"
#include "audio.h"

#define MAXPADS 4
static SDL_GameController *pads[MAXPADS];
static int n_pads;

static const char *name_of(SDL_GameController *c) { const char *n = SDL_GameControllerName(c); return n ? n : "Controller"; }

static void rescan(void)
{
    for (int i = 0; i < n_pads; i++) if (pads[i]) SDL_GameControllerClose(pads[i]);
    n_pads = 0;
    int nj = SDL_NumJoysticks();
    for (int i = 0; i < nj && n_pads < MAXPADS; i++) {
        if (!SDL_IsGameController(i)) continue;
        SDL_GameController *c = SDL_GameControllerOpen(i);
        if (c) pads[n_pads++] = c;
    }
}

void pad_init(void)
{
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);
    rescan();
}

void pad_shutdown(void)
{
    for (int i = 0; i < n_pads; i++) if (pads[i]) SDL_GameControllerClose(pads[i]);
    n_pads = 0;
    audio_pad_close();
}

void pad_event(const SDL_Event *e)
{
    if (e->type == SDL_CONTROLLERDEVICEADDED || e->type == SDL_CONTROLLERDEVICEREMOVED) {
        rescan();
        if (e->type == SDL_CONTROLLERDEVICEADDED && pad_is_dualsense(0) + pad_is_dualsense(1) > 0) audio_pad_open();
    }
}

int pad_count(void) { return n_pads; }
const char *pad_name(int i) { return i >= 0 && i < n_pads ? name_of(pads[i]) : ""; }
int pad_is_dualsense(int i)
{
    if (i < 0 || i >= n_pads) return 0;
    return SDL_GameControllerGetType(pads[i]) == SDL_CONTROLLER_TYPE_PS5;
}
int pad_is_edge(int i)
{
    if (!pad_is_dualsense(i)) return 0;
    return SDL_GameControllerGetVendor(pads[i]) == 0x054C && SDL_GameControllerGetProduct(pads[i]) == 0x0DF2;
}

const char *pad_status(char *buf, size_t n)
{
    if (!n_pads) { snprintf(buf, n, "No controller connected"); return buf; }
    int i = 0;
    for (int k = 0; k < n_pads; k++) if (pad_is_dualsense(k)) { i = k; break; }
    const char *kind = pad_is_edge(i) ? "DualSense Edge" : (pad_is_dualsense(i) ? "DualSense" : pad_name(i));
    const char *conn = "";
    SDL_JoystickPowerLevel pl = SDL_JoystickCurrentPowerLevel(SDL_GameControllerGetJoystick(pads[i]));
    conn = pl == SDL_JOYSTICK_POWER_WIRED ? " (USB)" : (pl == SDL_JOYSTICK_POWER_UNKNOWN ? "" : " (wireless)");
    snprintf(buf, n, "%s connected%s%s", kind, conn, n_pads > 1 ? " + more" : "");
    return buf;
}

/* ---------------------------------------------------------------- input */
static int pad_down(int code)
{
    for (int i = 0; i < n_pads; i++) {
        if (code >= PAD_AXIS_BASE) {
            int v = SDL_GameControllerGetAxis(pads[i], code == PAD_AXIS_BASE ? SDL_CONTROLLER_AXIS_TRIGGERLEFT : SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
            if (v > 12000) return 1;
        } else if (code >= 0 && SDL_GameControllerGetButton(pads[i], (SDL_GameControllerButton)code)) return 1;
    }
    return 0;
}

void pad_poll(int game, uint8_t *b, uint8_t *d)
{
    const GameCfg *c = &settings.g[game];
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    uint8_t bits[N_BTN] = {0};
    for (int i = 0; i < N_BTN; i++) {
        for (int s = 0; s < 2; s++) {
            if (c->key[i][s]) { SDL_Scancode sc = SDL_GetScancodeFromKey(c->key[i][s]); if (sc != SDL_SCANCODE_UNKNOWN && ks[sc]) bits[i] = 1; }
            if (c->pad[i][s] >= 0 && pad_down(c->pad[i][s])) bits[i] = 1;
        }
    }
    /* left stick = d-pad */
    float dz = settings.pad_deadzone / 100.0f * 32767.0f;
    for (int i = 0; i < n_pads; i++) {
        int ax = SDL_GameControllerGetAxis(pads[i], SDL_CONTROLLER_AXIS_LEFTX), ay = SDL_GameControllerGetAxis(pads[i], SDL_CONTROLLER_AXIS_LEFTY);
        if (ax > dz) bits[BTN_RIGHT] = 1; else if (ax < -dz) bits[BTN_LEFT] = 1;
        if (ay > dz) bits[BTN_DOWN] = 1; else if (ay < -dz) bits[BTN_UP] = 1;
    }
    *b = (uint8_t)(bits[BTN_A] | bits[BTN_B] << 1 | bits[BTN_SELECT] << 2 | bits[BTN_START] << 3);
    *d = (uint8_t)(bits[BTN_RIGHT] | bits[BTN_LEFT] << 1 | bits[BTN_UP] << 2 | bits[BTN_DOWN] << 3);
    if ((*d & 3) == 3) *d &= (uint8_t)~3;       /* no left+right at once */
    if ((*d & 12) == 12) *d &= (uint8_t)~12;
}

int pad_capture(const SDL_Event *e)
{
    if (e->type == SDL_CONTROLLERBUTTONDOWN) return e->cbutton.button;
    if (e->type == SDL_CONTROLLERAXISMOTION && e->caxis.value > 20000) {
        if (e->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) return PAD_AXIS_BASE;
        if (e->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) return PAD_AXIS_BASE + 1;
    }
    return -1;
}

const char *pad_code_name(int code, char *buf, size_t n)
{
    if (code < 0) { snprintf(buf, n, "-"); return buf; }
    int ps = 0;
    for (int i = 0; i < n_pads; i++) if (pad_is_dualsense(i)) ps = 1;
    if (code == PAD_AXIS_BASE) { snprintf(buf, n, ps ? "L2" : "LT"); return buf; }
    if (code == PAD_AXIS_BASE + 1) { snprintf(buf, n, ps ? "R2" : "RT"); return buf; }
    static const char *xb[] = {"A", "B", "X", "Y", "Back", "Guide", "Start", "L3", "R3", "LB", "RB", "Up", "Down", "Left", "Right", "Share", "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4", "Touchpad"};
    static const char *dsn[] = {"Cross", "Circle", "Square", "Triangle", "Create", "PS", "Options", "L3", "R3", "L1", "R1", "Up", "Down", "Left", "Right", "Mute", "Right paddle", "Left paddle", "Right Fn", "Left Fn", "Touchpad"};
    if (code < (int)(sizeof xb / sizeof xb[0])) snprintf(buf, n, "%s", ps ? dsn[code] : xb[code]);
    else snprintf(buf, n, "Button %d", code);
    return buf;
}

const char *key_code_name(int key, char *buf, size_t n)
{
    if (!key) { snprintf(buf, n, "-"); return buf; }
    const char *k = SDL_GetKeyName(key);
    snprintf(buf, n, "%s", k && k[0] ? k : "?");
    return buf;
}

/* ---------------------------------------------------------------- light + rumble */
static int ctx_game = -1, ctx_in_game;
static uint32_t screen_rgb = 0x808080;
static uint32_t flash_rgb;
static float flash_t;
static uint32_t last_sent = 0xFFFFFFFFu;
static float send_acc;

void pad_set_context(int game, int in_game) { ctx_game = game; ctx_in_game = in_game; last_sent = 0xFFFFFFFFu; }
void pad_set_screen_color(uint32_t rgb) { screen_rgb = rgb; }
void pad_flash(uint32_t rgb) { flash_rgb = rgb; flash_t = 1.0f; }

int pad_has_light(void)
{
    for (int i = 0; i < n_pads; i++) if (SDL_GameControllerHasLED(pads[i])) return 1;
    return 0;
}

void pad_rumble(int pct, int ms)
{
    if (pct <= 0) return;
    if (pct > 100) pct = 100;
    Uint16 lo = (Uint16)(pct * 655), hi = (Uint16)(pct * 655 * 0.8);
    for (int i = 0; i < n_pads; i++) if (SDL_GameControllerHasRumble(pads[i])) SDL_GameControllerRumble(pads[i], lo, hi, (Uint32)ms);
}

void pad_event_fx(int game, int ev)
{
    const EventDef *d = events_def(game, ev);
    if (!d) return;
    const GameCfg *c = &settings.g[game];
    if ((c->ds_ev_rumble >> ev) & 1) pad_rumble(c->ds_rumble * d->rumble_str / 100, d->rumble_ms);
    if ((c->ds_ev_led >> ev) & 1) pad_flash(d->color);
}

static uint32_t base_color(const GameCfg *c)
{
    switch (c->ds_led_mode) {
    case LED_PALETTE: return palettes[c->palette].bg[1];
    case LED_CUSTOM: return c->ds_color;
    case LED_SCREEN: return screen_rgb;
    default: return 0;
    }
}

void pad_frame(float dt)
{
    if (ctx_game < 0 || !n_pads) return;
    const GameCfg *c = &settings.g[ctx_game];
    uint32_t base = base_color(c);
    float k = c->ds_bright / 100.0f;
    float r = ((base >> 16) & 255) * k, g = ((base >> 8) & 255) * k, b = (base & 255) * k;
    if (flash_t > 0) {
        float f = flash_t * flash_t;
        r += (((flash_rgb >> 16) & 255) - r) * f; g += (((flash_rgb >> 8) & 255) - g) * f; b += ((flash_rgb & 255) - b) * f;
        flash_t -= dt / 0.40f;
        if (flash_t < 0) flash_t = 0;
    }
    uint32_t out = ((uint32_t)(r + 0.5f) << 16) | ((uint32_t)(g + 0.5f) << 8) | (uint32_t)(b + 0.5f);
    send_acc += dt;
    if (out == last_sent || send_acc < 0.033f) return;
    send_acc = 0;
    last_sent = out;
    for (int i = 0; i < n_pads; i++) if (SDL_GameControllerHasLED(pads[i])) SDL_GameControllerSetLED(pads[i], (Uint8)(out >> 16), (Uint8)(out >> 8), (Uint8)out);
}
