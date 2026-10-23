#pragma once
#include <stdint.h>

enum { GAME_DRMARIO, GAME_SML, GAME_SML2, N_GAMES };

typedef struct {
    const char *id;           /* file-name safe */
    const char *name;
    const char *sub;          /* one-line description for the tab */
    const char *title;        /* cartridge header title used to recognise the game */
    uint32_t crc;             /* checksum of the known-good dump (0 = any revision) */
    int lifted;               /* statically recompiled code is built in (otherwise: interpreter core) */
    uint32_t accent;          /* 0xRRGGBB highlight colour of the tab */
    int preview_frames;       /* how far to run for the launcher preview picture */
    int wide_l, wide_r;       /* widescreen: most extra pixels that can be shown left / right (0 = unsupported) */
    int hud_lines;            /* top lines drawn as a status bar (centred in widescreen) */
    int hud_window;           /* the status bar is the window layer (centred in widescreen) */
    int wide_gate;            /* how to tell a level from a menu (see ppu_set_wide) */
} GameDef;

extern const GameDef games[N_GAMES];
