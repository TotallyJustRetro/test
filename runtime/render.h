#pragma once
#include <SDL.h>
#include "emu.h"

void render_init(SDL_Renderer *r);
void render_shutdown(void);
void render_reset(void);                                   /* forget ghost trails */
/* Build the picture for `game` from a frame. live != 0 applies temporal effects (ghosting). */
void render_build(const Frame *f, int game, int live);
/* Where the picture goes in a W x H area for the chosen window shape and scaling. */
void render_fit(int W, int H, int aspect, int scaling, SDL_Rect *out);
/* Draw the picture (with the screen filters) into r. */
void render_draw(const SDL_Rect *r, int scaling);
uint32_t render_avg_color(void);                           /* 0xRRGGBB of the current picture */
void frame_from_ppu(Frame *f);
int render_width(void);                                    /* width of the last built picture */                             /* snapshot of the PPU (after a preview run) */
