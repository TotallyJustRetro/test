#pragma once
#include <SDL.h>
unsigned char *image_load_rgba(const char *path, int *w, int *h);     /* malloc'd RGBA, first frame */
void bg_init(SDL_Renderer *r);
void bg_shutdown(void);
int  bg_load(const char *path);                 /* "" clears; 0 ok */
void bg_update(float dt);                       /* advance animation */
SDL_Texture *bg_texture(int *w, int *h);        /* NULL when no image */
int  bg_frames(void);
