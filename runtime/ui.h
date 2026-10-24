#pragma once
/* Immediate-mode UI that draws at the real window resolution. All coordinates are in "dp":
 * a 1100 x 720 design space that is scaled to fit the window, so nothing is ever blurry or mis-hit. */
#include <SDL.h>
#include <stdint.h>

#define RGBA(r, g, b, a) ((uint32_t)(((uint32_t)(r) << 24) | ((uint32_t)(g) << 16) | ((uint32_t)(b) << 8) | (uint32_t)(a)))
#define RGB(r, g, b) RGBA(r, g, b, 255)
#define HEX(h) RGBA(((h) >> 16) & 255, ((h) >> 8) & 255, (h) & 255, 255)
#define HEXA(h, a) RGBA(((h) >> 16) & 255, ((h) >> 8) & 255, (h) & 255, (a))

#define UI_W 1100
#define UI_H 720

enum { F_REG, F_BOLD };
enum { B_NORMAL, B_PRIMARY, B_GHOST, B_DANGER };

typedef struct {
    float x, y;                 /* dp */
    int down, pressed, released;
    float wheel;
    int inside;
} UiMouse;
extern UiMouse ui_mouse;
extern float ui_scale;          /* pixels per dp */
extern float ui_dt;             /* seconds since last frame */
extern void (*ui_sfx_cb)(int);  /* hover / click sounds (SFX_*) */

void ui_init(SDL_Renderer *r);
void ui_shutdown(void);
/* call at the start of every frame with the renderer output size (pixels) */
void ui_begin(int pw, int ph, float dt);
void ui_end(void);
/* mouse in window units -> dp; ww/wh = window size in window units */
void ui_set_mouse(int wx, int wy, int ww, int wh);
void ui_mouse_button(int down);                 /* left button */
void ui_hint(const char *s);                    /* shown in the footer while hovering */
const char *ui_hint_text(void);
float ui_view_x0(void), ui_view_y0(void), ui_view_w(void), ui_view_h(void);   /* visible area in dp (can exceed UI_W/UI_H) */

/* primitives (dp) */
void ui_rect(float x, float y, float w, float h, uint32_t c);
void ui_rrect(float x, float y, float w, float h, float r, uint32_t c);
void ui_stroke(float x, float y, float w, float h, float r, float t, uint32_t c);
void ui_vgrad(float x, float y, float w, float h, uint32_t top, uint32_t bottom);
void ui_hgrad(float x, float y, float w, float h, uint32_t left, uint32_t right);
void ui_line(float x1, float y1, float x2, float y2, float t, uint32_t c);
void ui_tri(float x1, float y1, float x2, float y2, float x3, float y3, uint32_t c);
void ui_shadow(float x, float y, float w, float h, float r, float spread, uint32_t c);
void ui_clip(float x, float y, float w, float h);
void ui_unclip(void);
void ui_image(SDL_Texture *t, const SDL_Rect *src, float x, float y, float w, float h);

void  ui_text(int font, float size, float x, float y, uint32_t c, const char *s);        /* y = top of the line box */
float ui_text_w(int font, float size, const char *s);
void  ui_text_c(int font, float size, float cx, float y, uint32_t c, const char *s);
void  ui_text_r(int font, float size, float rx, float y, uint32_t c, const char *s);
float ui_line_h(float size);
void  ui_text_fit(int font, float size, float x, float y, float maxw, uint32_t c, const char *s);   /* "…" if too long */
void  ui_text_wrap(int font, float size, float x, float y, float w, uint32_t c, const char *s, float line_gap);   /* returns nothing; wraps on spaces */
int   ui_wrap_lines(int font, float size, float w, const char *s);

/* widgets */
int   ui_hover(float x, float y, float w, float h);
int   ui_button(float x, float y, float w, float h, const char *label, int kind, int enabled);
int   ui_seg(float x, float y, float w, float h, const char **names, int n, int *sel);
int   ui_toggle(float x, float y, int *v);                       /* 44 x 24 */
int   ui_slider(float x, float y, float w, int *v, int lo, int hi);
int   ui_chip(float x, float y, float w, float h, const char *label, int selected);
int   ui_next_id(void);
int   ui_active_any(void);                                      /* a drag is in progress */
float ui_anim(int id, float target, float speed);               /* smoothed 0..1 */

/* theme */
#define C_BG       HEX(0x0E1016)
#define C_BG2      HEX(0x151823)
#define C_PANEL    HEX(0x1A1E2B)
#define C_PANEL2   HEX(0x222737)
#define C_BTN      HEX(0x2B3247)
#define C_BTN_H    HEX(0x3A4361)
#define C_LINE     HEX(0x2C3347)
#define C_TEXT     HEX(0xEDEFF6)
#define C_MUTED    HEX(0x959CB2)
#define C_DIM      HEX(0x626A82)
#define C_ACCENT   HEX(0x4C8DFF)
#define C_OK       HEX(0x4CC38A)
#define C_WARN     HEX(0xEDB451)
#define C_ERR      HEX(0xF2685C)
extern uint32_t ui_accent;
