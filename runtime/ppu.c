/* DMG PPU: scanline renderer with mode timing and STAT interrupts.
 * Each line is drawn when mode 3 ends, using the registers as they are then,
 * so per-line raster effects (SCX/SCY/palette changes in an HBlank/LYC
 * handler) work. */
#include "gb.h"

uint8_t vram[0x2000], oam[0xA0];
uint8_t ppu_shade[GB_H][GB_WMAX];
uint8_t ppu_layer[GB_H][GB_WMAX];
uint16_t ppu_bgtile[GB_H][GB_WMAX], ppu_sprtile[GB_H][GB_WMAX];   /* tile number 0..383, 0xFFFF = none */
uint8_t ppu_bguv[GB_H][GB_WMAX], ppu_spruv[GB_H][GB_WMAX];
int ppu_w = GB_W, ppu_xoff = 0;
static int hud_lines, hud_shift, wide_r, sprite_neg = 256, win_centre, wide_gate, wide_on = 1, cfg_hud, cfg_win;
void ppu_set_wide(int l, int r, int hud, int centre_win, int gate)
{
    if (l < 0) l = 0;
    if (r < 0) r = 0;
    if (l + r > GB_WMAX - GB_W) r = GB_WMAX - GB_W - l;
    ppu_xoff = l; wide_r = r; ppu_w = GB_W + l + r;
    hud_lines = (l + r) ? hud : 0; hud_shift = (r - l) / 2;
    win_centre = (l + r) ? centre_win : 0;
    cfg_hud = hud_lines; cfg_win = win_centre; wide_gate = gate; wide_on = 1;
    sprite_neg = l > 8 ? 256 - (l - 8) : 256;       /* OAM X bytes this high are sprites left of the screen */
}       /* (v << 3) | u inside the tile as stored (before flips) */

static uint8_t lcdc, stat_sel, scy, scx, lyc, bgp, obp0, obp1, wy, wx;
static int dot, ly, mode, lcd_on, stat_line, wlc;
static int off_cycles;
int frame_count;

void ppu_reset(void)
{
    memset(vram, 0, sizeof vram);
    memset(oam, 0, sizeof oam);
    memset(ppu_shade, 0, sizeof ppu_shade);
    memset(ppu_layer, 0, sizeof ppu_layer);
    memset(ppu_bgtile, 0xFF, sizeof ppu_bgtile); memset(ppu_sprtile, 0xFF, sizeof ppu_sprtile);
    lcdc = 0x91; stat_sel = 0; scy = scx = lyc = 0;
    bgp = 0xFC; obp0 = obp1 = 0xFF; wy = wx = 0;
    dot = 0; ly = 0; mode = 2; lcd_on = 1; stat_line = 0; wlc = 0; off_cycles = 0;
    frame_count = 0;
}

static void update_stat(void)
{
    int line = ((stat_sel & 0x08) && mode == 0) || ((stat_sel & 0x10) && mode == 1) ||
               ((stat_sel & 0x20) && mode == 2) || ((stat_sel & 0x40) && ly == lyc);
    if (line && !stat_line) io_if |= 0x02;
    stat_line = line;
}

uint8_t ppu_read(uint8_t r)
{
    switch (r) {
    case 0x40: return lcdc;
    case 0x41: return 0x80 | stat_sel | (lcd_on && ly == lyc ? 4 : 0) | (lcd_on ? mode : 0);
    case 0x42: return scy;
    case 0x43: return scx;
    case 0x44: return (uint8_t)ly;
    case 0x45: return lyc;
    case 0x46: return 0xFF;
    case 0x47: return bgp;
    case 0x48: return obp0;
    case 0x49: return obp1;
    case 0x4A: return wy;
    case 0x4B: return wx;
    }
    return 0xFF;
}

static void blank_screen(void)
{
    memset(ppu_shade, 0, sizeof ppu_shade); memset(ppu_layer, 0, sizeof ppu_layer);
    memset(ppu_bgtile, 0xFF, sizeof ppu_bgtile); memset(ppu_sprtile, 0xFF, sizeof ppu_sprtile);
}

/* FNV-1a over a tile's 16 bytes: the id used by texture packs */
static uint64_t tile_hash(const uint8_t *t)
{
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 16; i++) { h ^= t[i]; h *= 1099511628211ull; }
    return h;
}

void ppu_tile_hashes(uint64_t out[384])
{
    for (int i = 0; i < 384; i++) out[i] = tile_hash(&vram[i * 16]);
}

void ppu_write(uint8_t r, uint8_t v)
{
    switch (r) {
    case 0x40: {
        int was = lcd_on;
        lcdc = v;
        lcd_on = (v & 0x80) != 0;
        if (was && !lcd_on) {
            ly = 0; dot = 0; mode = 0; stat_line = 0; wlc = 0; off_cycles = 0;
            blank_screen();
        } else if (!was && lcd_on) {
            ly = 0; dot = 0; mode = 2; wlc = 0;
            update_stat();
        }
        break;
    }
    case 0x41: stat_sel = v & 0x78; if (lcd_on) update_stat(); break;
    case 0x42: scy = v; break;
    case 0x43: scx = v; break;
    case 0x44: break;
    case 0x45: lyc = v; if (lcd_on) update_stat(); break;
    case 0x47: bgp = v; break;
    case 0x48: obp0 = v; break;
    case 0x49: obp1 = v; break;
    case 0x4A: wy = v; break;
    case 0x4B: wx = v; break;
    }
}

/* ------------------------------------------------------------------ */
static void render_line(int y)
{
    const int W = ppu_w, L = ppu_xoff;
    hud_lines = wide_on ? cfg_hud : 0; win_centre = wide_on ? cfg_win : 0;
    uint8_t *out = ppu_shade[y];
    memset(ppu_layer[y], 0, W);
    memset(ppu_bgtile[y], 0xFF, W * 2);
    memset(ppu_sprtile[y], 0xFF, W * 2);
    uint8_t bgci[GB_WMAX];                      /* BG/window colour index, for sprite priority */
    memset(bgci, 0, sizeof bgci);

    if (lcdc & 0x01) {
        const uint8_t *map = &vram[(lcdc & 0x08) ? 0x1C00 : 0x1800];
        int sy = (y + scy) & 0xFF;
        int off = L + (y < hud_lines ? hud_shift : 0);
        for (int X = 0; X < W; X++) {
            int px = (X - off + scx) & 0xFF;
            uint8_t t = map[(sy >> 3) * 32 + (px >> 3)];
            int addr = (lcdc & 0x10) ? t * 16 : 0x1000 + (int8_t)t * 16;
            addr += (sy & 7) * 2;
            int bit = 7 - (px & 7);
            int ci = (((vram[addr + 1] >> bit) & 1) << 1) | ((vram[addr] >> bit) & 1);
            bgci[X] = (uint8_t)ci;
            out[X] = (bgp >> (ci * 2)) & 3;
            ppu_bgtile[y][X] = (uint16_t)(addr >> 4);
            ppu_bguv[y][X] = (uint8_t)(((sy & 7) << 3) | (px & 7));
        }
    } else {
        memset(out, 0, W);
    }

    if ((lcdc & 0x20) && (lcdc & 0x01) && y >= wy && wx <= 166) {
        const uint8_t *map = &vram[(lcdc & 0x40) ? 0x1C00 : 0x1800];
        int x0 = wx - 7 + L + (win_centre ? hud_shift : 0), drew = 0;
        for (int X = (win_centre ? 0 : (x0 < 0 ? 0 : x0)); X < W; X++) {
            int px = (X - x0) & 0xFF;
            uint8_t t = map[(wlc >> 3) * 32 + (px >> 3)];
            int addr = (lcdc & 0x10) ? t * 16 : 0x1000 + (int8_t)t * 16;
            addr += (wlc & 7) * 2;
            int bit = 7 - (px & 7);
            int ci = (((vram[addr + 1] >> bit) & 1) << 1) | ((vram[addr] >> bit) & 1);
            bgci[X] = (uint8_t)ci;
            out[X] = (bgp >> (ci * 2)) & 3;
            ppu_bgtile[y][X] = (uint16_t)(addr >> 4);
            ppu_bguv[y][X] = (uint8_t)(((wlc & 7) << 3) | (px & 7));
            drew = 1;
        }
        if (drew) wlc++;
    }

    if (lcdc & 0x02) {
        int h = (lcdc & 0x04) ? 16 : 8;
        int idx[10], n = 0;
        for (int i = 0; i < 40 && n < 10; i++) {
            int sy = oam[i * 4] - 16;
            if (y >= sy && y < sy + h) idx[n++] = i;
        }
        /* lower X wins, ties go to lower OAM index: draw losers first */
        for (int a = 0; a < n; a++)
            for (int b = a + 1; b < n; b++) {
                int xa = oam[idx[a] * 4 + 1], xb = oam[idx[b] * 4 + 1];
                if (xb > xa || (xb == xa && idx[b] > idx[a])) { int t = idx[a]; idx[a] = idx[b]; idx[b] = t; }
            }
        for (int k = 0; k < n; k++) {
            int i = idx[k];
            int ox = oam[i * 4 + 1];
            if (ox >= sprite_neg) ox -= 256;
            int sy = oam[i * 4] - 16, sx = ox - 8 + L;
            uint8_t tile = oam[i * 4 + 2], fl = oam[i * 4 + 3];
            int row = y - sy;
            if (fl & 0x40) row = h - 1 - row;
            if (h == 16) tile &= 0xFE;
            int addr = (tile + (row >> 3)) * 16 + (row & 7) * 2;
            uint8_t pal = (fl & 0x10) ? obp1 : obp0;
            for (int px = 0; px < 8; px++) {
                int x = sx + px;
                if (x < 0 || x >= W) continue;
                int bit = (fl & 0x20) ? px : 7 - px;
                int ci = (((vram[addr + 1] >> bit) & 1) << 1) | ((vram[addr] >> bit) & 1);
                if (ci == 0) continue;
                if ((fl & 0x80) && bgci[x] != 0) continue;
                out[x] = (pal >> (ci * 2)) & 3;
                ppu_layer[y][x] = (fl & 0x10) ? 2 : 1;
                ppu_sprtile[y][x] = (uint16_t)(addr >> 4);
                ppu_spruv[y][x] = (uint8_t)(((row & 7) << 3) | (7 - bit) | ((fl & 0x20) ? 0x40 : 0) | ((fl & 0x40) ? 0x80 : 0));
            }
        }
    }    if (!wide_on) {                              /* not a level: normal screen with bars at the sides */
        for (int X = 0; X < W; X++) {
            if (X >= L && X < L + GB_W) continue;
            out[X] = 3; ppu_layer[y][X] = 0; ppu_bgtile[y][X] = 0xFFFF; ppu_sprtile[y][X] = 0xFFFF;
        }
    }
}

void ppu_tick(int n)
{
    if (!lcd_on) {
        off_cycles += n;
        if (off_cycles >= CYCLES_PER_FRAME) { off_cycles -= CYCLES_PER_FRAME; frame_count++; frame_hook(); }
        return;
    }
    dot += n;
    for (;;) {
        if (ly < 144) {
            if (mode == 2) { if (dot < 80) break; mode = 3; update_stat(); continue; }
            if (mode == 3) { if (dot < 252) break; mode = 0; render_line(ly); update_stat(); continue; }
            if (dot < 456) break;
            dot -= 456; ly++;
            if (ly == 144) {
                mode = 1; io_if |= 0x01; wlc = 0;
                update_stat();
                frame_count++;
                frame_hook();
            } else { mode = 2; update_stat(); }
        } else {
            if (dot < 456) break;
            dot -= 456; ly++;
            if (ly == 154) {
                ly = 0; mode = 2;
                wide_on = wide_gate == 0 ? 1 : (wide_gate == 1 ? (io_ie & 4) != 0 : ((lcdc & 0x20) && wx == 7 && wy == 0x88));
            }
            update_stat();
        }
    }
}

int ppu_lcd_is_on(void) { return lcd_on; }
