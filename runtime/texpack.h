#pragma once
#include <stdint.h>
#include "emu.h"
#include "settings.h"

typedef struct { int m; uint32_t *px; } TexTile;     /* tile image is (8*m) x (8*m) ARGB */

uint64_t tile_hash16(const uint8_t *t);
/* ---- packs ---- */
int  texpack_load(const char *dir);                  /* "" clears. Returns number of tiles loaded (-1 = folder problem) */
int  texpack_count(void);
int  texpack_scale(void);                            /* biggest tile multiplier in the pack (1 = none) */
const TexTile *texpack_find(uint64_t hash);
/* ---- collecting what the game draws ---- */
void tex_collect_begin(int game);                    /* loads the cache of earlier sessions */
void tex_collect_frame(const Frame *f);
void tex_collect_save(void);
int  tex_collected(void);
void tex_collect_clear(void);
/* Writes one PNG per collected tile (named by its id) + an overview sheet + a readme. Returns tiles written or -1. */
int  tex_export(const char *dir, const Palette *pal, int scale, char *msg, size_t n);
