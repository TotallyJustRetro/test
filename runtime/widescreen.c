#include <string.h>
#include "widescreen.h"
#include "gb.h"

/* ---- Super Mario Land ----
 * Level columns are streamed 7 columns ahead of the screen and 4 behind, so the background is already
 * correct up to 56 px right / 32 px left of the screen. Enemies are the limit: they live in an 8 bit X
 * range, spawn at X = C0 (just off the right edge) and are removed once X wraps past E0. We
 *   - spawn them k columns (16 px) earlier and 16k px further right,
 *   - remove only entities in [T, U) so a few px on the left stay alive too. */
static const int sml_gap = 8;

static int sml_k(int r) { return r <= 24 ? 0 : (r - 24 + 15) / 16; }

static int sml_lmax(int k)
{
    if (k == 0) return 32;
    if (k == 1) return 32;
    return 19;              /* k == 2: T = ED leaves 256 - ED - gap = 11 px of negative X */
}

void wide_dims(int game, int pct, int *l, int *r)
{
    *l = *r = 0;
    if (pct <= 0) return;
    if (game == GAME_SML2) { *l = (49 * pct + 50) / 100; *r = (33 * pct + 50) / 100; return; }
    if (game == GAME_SML) {
        int rr = (56 * pct + 50) / 100, ll = (32 * pct + 50) / 100;
        int k = sml_k(rr), lm = sml_lmax(k);
        *l = ll < lm ? ll : lm; *r = rr;
    }
}

static int expect(int addr, const uint8_t *b, int n) { return memcmp(&rom[addr], b, (size_t)n) == 0; }

int wide_install(int game, int l, int r)
{
    if (l + r == 0) return 1;
    if (game == GAME_SML2) {
        /* Enemies wake up when their world X is within +-0x60 of the camera (bank 2, 0x4000).
         * Widen only the right edge by r px: hi bound = cam + 0x60 + r, via a stub in bank 0 padding. */
        static const uint8_t o[] = {0x19, 0x7C, 0xEA, 0x0A, 0xAF};
        static const uint8_t zero[16] = {0};
        const int act = 0x8000 + 0x1C, stub = 0x1B63;
        if (r <= 0) return 1;
        if (!expect(act, o, 5) || !expect(stub, zero, 16)) return 0;
        const uint8_t t[] = {0x19, 0x11, (uint8_t)r, 0x00, 0x19, 0x7C, 0xEA, 0x0A, 0xAF, 0xC9};   /* ADD HL,DE; LD DE,r; ADD HL,DE; LD A,H; LD (AF0A),A; RET */
        memcpy(&rom[stub], t, sizeof t);
        rom[act] = 0xCD; rom[act + 1] = 0x63; rom[act + 2] = 0x1B; rom[act + 3] = 0x00; rom[act + 4] = 0x00;
        return 1;
    }
    if (game == GAME_SML) {
        static const uint8_t o1[] = {0xFA, 0xAB, 0xC0}, o2[] = {0xC6, 0xD0}, o3[] = {0xF0, 0xC3, 0xFE, 0xE0, 0x38, 0x0A};
        static const uint8_t zero[26] = {0};
        if (!expect(0x24A5, o1, 3) || !expect(0x24BF, o2, 2) || !expect(0x2584, o3, 6) || !expect(0x3FE4, zero, 26)) return 0;
        int k = sml_k(r);
        int T = k >= 2 ? 0xC0 + 16 * k + 12 + 1 : 0xE0;
        int U = l > 8 ? 256 - (l - 8) : 0x100;
        if (U - T < sml_gap && l > 8) return 0;
        if (k > 0) {
            const uint8_t t1[] = {0xFA, 0xAB, 0xC0, 0xC6, (uint8_t)k, 0xC9};      /* LD A,(C0AB); ADD A,k; RET */
            memcpy(&rom[0x3FE4], t1, sizeof t1);
            rom[0x24A5] = 0xCD; rom[0x24A6] = 0xE4; rom[0x24A7] = 0x3F;           /* CALL 3FE4 */
            rom[0x24C0] = (uint8_t)(0xD0 + 16 * k);                               /* spawn X + 16k */
        }
        if (l > 8 || k >= 2) {
            const uint8_t t2[] = {0xF0, 0xC3, 0xFE, (uint8_t)T, 0xD8, 0xFE, (uint8_t)(U & 0xFF), 0x3F, 0xC9};
            if (U >= 0x100) { /* no left extension: plain threshold */
                const uint8_t t2b[] = {0xF0, 0xC3, 0xFE, (uint8_t)T, 0xC9};
                memcpy(&rom[0x3FEA], t2b, sizeof t2b);
            } else memcpy(&rom[0x3FEA], t2, sizeof t2);
            rom[0x2584] = 0xCD; rom[0x2585] = 0xEA; rom[0x2586] = 0x3F; rom[0x2587] = 0x00;   /* CALL 3FEA; NOP */
        }
        return 1;
    }
    return 0;
}
