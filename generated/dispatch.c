/* GENERATED — do not edit.
 * Dispatch shim: routes the generic recomp_run() / cpu_step() /
 * recomp_code_mask[] to the currently-loaded game's implementation.
 * Updated by tools/recomp.py --prefix.
 */
#include "gb.h"
#include "games.h"

/* ---- forward declarations for every game ---- */
void recomp_run_drmario(void);
void recomp_run_sml(void);
void recomp_run_sml2(void);

void cpu_step_drmario(void);
void cpu_step_sml(void);
void cpu_step_sml2(void);

extern const uint8_t recomp_code_mask_drmario[0x1000];
extern const uint8_t recomp_code_mask_sml[0x1000];
extern const uint8_t recomp_code_mask_sml2[0x1000];

/* ---- the public symbols the runtime uses ---- */

/* Points at the active game's mask; updated by dispatch_set_game(). */
const uint8_t *recomp_code_mask_ptr = NULL;

/* Dummy 4096-byte zero mask used before any game is loaded. */
static const uint8_t mask_zero[0x1000];

/* Called from rom.c after a game is loaded. */
void dispatch_set_game(int game)
{
    switch (game) {
    case GAME_DRMARIO: recomp_code_mask_ptr = recomp_code_mask_drmario; break;
    case GAME_SML:     recomp_code_mask_ptr = recomp_code_mask_sml;     break;
    case GAME_SML2:    recomp_code_mask_ptr = recomp_code_mask_sml2;    break;
    default:           recomp_code_mask_ptr = mask_zero;                break;
    }
}

void recomp_run(void)
{
    /* rom.c calls gb_reset() before recomp_run(); game is already loaded. */
    extern int rom_loaded_game(void);
    switch (rom_loaded_game()) {
    case GAME_DRMARIO: recomp_run_drmario(); break;
    case GAME_SML:     recomp_run_sml();     break;
    case GAME_SML2:    recomp_run_sml2();    break;
    default: break;
    }
}

void cpu_step(void)
{
    extern int rom_loaded_game(void);
    switch (rom_loaded_game()) {
    case GAME_DRMARIO: cpu_step_drmario(); break;
    case GAME_SML:     cpu_step_sml();     break;
    case GAME_SML2:    cpu_step_sml2();    break;
    default: break;
    }
}
