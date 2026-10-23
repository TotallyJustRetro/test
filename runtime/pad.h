#pragma once
#include <SDL.h>
#include <stdint.h>

void pad_init(void);
void pad_shutdown(void);
void pad_event(const SDL_Event *e);                 /* hot-plug */
int  pad_count(void);
const char *pad_name(int i);
int  pad_is_dualsense(int i);
int  pad_is_edge(int i);
const char *pad_status(char *buf, size_t n);        /* "DualSense Edge connected (USB)" / "No controller" */

/* game input: keyboard + pads according to the game's bindings */
void pad_poll(int game, uint8_t *buttons, uint8_t *dpad);
/* binding capture: returns a binding code (button id, or PAD_AXIS_BASE+n for L2/R2) or -1 */
int  pad_capture(const SDL_Event *e);
const char *pad_code_name(int code, char *buf, size_t n);
const char *key_code_name(int key, char *buf, size_t n);

/* light + rumble */
void pad_set_context(int game, int in_game);        /* which game's light settings apply */
void pad_set_screen_color(uint32_t rgb);            /* for "follow screen" */
void pad_event_fx(int game, int ev);                /* an in-game event happened */
void pad_frame(float dt);                           /* update light */
void pad_rumble(int strength_pct, int ms);          /* test */
void pad_flash(uint32_t rgb);                       /* test */
int  pad_has_light(void);
