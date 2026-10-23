#pragma once
#include <stdint.h>
#include <stddef.h>
#include "games.h"

typedef struct {
    int ok;                 /* 1 = usable */
    int hack_loaded;        /* a romhack is applied on top of the base ROM */
    int changed_bytes;      /* bytes that differ from the base ROM */
    int code_changed;       /* a changed byte lies inside recompiled code -> interpreter mode */
    int exact;              /* matches the known-good dump exactly */
    char msg[256];          /* human readable status / error */
} RomStatus;

/* Which game is this image? -1 if none of ours. Uses the header title. */
int  rom_identify(const uint8_t *img, size_t n);
int  rom_identify_file(const char *path);       /* game index or -1 */
/* Read `path` and check that it is `game`; does not change the running cartridge. */
int  rom_probe(int game, const char *path, RomStatus *st);
/* Make `game` (from path) the base ROM and install it. */
int  rom_load(int game, const char *path, RomStatus *st);
/* Apply a romhack (IPS/BPS/UPS patch or an already-patched ROM) on top of the loaded base ROM. */
int  rom_apply_hack(const char *path, RomStatus *st);
void rom_clear_hack(RomStatus *st);
/* Run everything in the interpreter instead of the recompiled code? */
int  rom_needs_interpreter(void);
int  rom_loaded_game(void);
int  rom_hack_active(void);
/* Search the usual folders for ROMs; fills paths[game] (empty string if none). */
void rom_scan(char paths[N_GAMES][512]);
int  rom_load_raw(const char *path);          /* any ROM, interpreter only (testing) */
/* Points to the active game's recompiled-instruction bitmask (set by dispatch_set_game). */
extern const uint8_t *recomp_code_mask_ptr;
void dispatch_set_game(int game);
