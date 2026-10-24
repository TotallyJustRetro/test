#pragma once
/* Widescreen: how much extra picture a game can show, and the small in-memory ROM patches
 * that keep enemies and level data correct in the extra space. The ROM file is never changed. */
#include "games.h"

/* pixels to add left and right for a slider value (0..100) */
void wide_dims(int game, int pct, int *left, int *right);
/* patch the loaded ROM for these dimensions. 1 = ok / nothing needed, 0 = this ROM doesn't match (use the normal screen) */
int  wide_install(int game, int left, int right);
