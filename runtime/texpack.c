/* Texture packs: replace the game's 8x8 tiles by higher resolution pictures.
 * A tile is identified by a hash of its 16 bytes of tile data (so the replacement follows the tile
 * wherever it is drawn). Packs are folders of PNGs named <16 hex digits>.png. */
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_WINDOWS_UTF8
#include "stb_image_write.h"
#include "texpack.h"
#include "bgimg.h"
#include "util.h"

uint64_t tile_hash16(const uint8_t *t)
{
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 16; i++) { h ^= t[i]; h *= 1099511628211ull; }
    return h;
}

/* ---- hash table of loaded replacement tiles ---- */
#define HT 8192
static struct { uint64_t h; TexTile t; int used; } ht[HT];
static int n_loaded, scale_max = 1;

static void ht_clear(void)
{
    for (int i = 0; i < HT; i++) if (ht[i].used) free(ht[i].t.px);
    memset(ht, 0, sizeof ht);
    n_loaded = 0; scale_max = 1;
}

static void ht_put(uint64_t h, TexTile t)
{
    unsigned i = (unsigned)(h % HT);
    while (ht[i].used) { if (ht[i].h == h) { free(ht[i].t.px); break; } i = (i + 1) % HT; }
    if (!ht[i].used) n_loaded++;
    ht[i].h = h; ht[i].t = t; ht[i].used = 1;
}

const TexTile *texpack_find(uint64_t h)
{
    if (!n_loaded) return NULL;
    unsigned i = (unsigned)(h % HT);
    for (int k = 0; k < HT && ht[i].used; k++, i = (i + 1) % HT)
        if (ht[i].h == h) return &ht[i].t;
    return NULL;
}

int texpack_count(void) { return n_loaded; }
int texpack_scale(void) { return n_loaded ? scale_max : 1; }

static int parse_hex16(const char *s, uint64_t *out)
{
    uint64_t v = 0;
    for (int i = 0; i < 16; i++) {
        char c = s[i];
        int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) return 0;
        v = (v << 4) | (uint64_t)d;
    }
    *out = v;
    return 1;
}

int texpack_load(const char *dir)
{
    ht_clear();
    if (!dir || !dir[0]) return 0;
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        uint64_t h;
        if (l < 20 || strcmp(e->d_name + l - 4, ".png") || !parse_hex16(e->d_name, &h)) continue;
        char path[1400];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        int w, hh;
        unsigned char *px = image_load_rgba(path, &w, &hh);
        if (!px) continue;
        if (w != hh || w % 8 || w < 8 || w > 128) { free(px); continue; }
        TexTile t = {w / 8, (uint32_t *)malloc((size_t)w * w * 4)};
        if (!t.px) { free(px); continue; }
        for (int i = 0; i < w * w; i++)
            t.px[i] = ((uint32_t)px[i * 4 + 3] << 24) | ((uint32_t)px[i * 4] << 16) | ((uint32_t)px[i * 4 + 1] << 8) | px[i * 4 + 2];
        free(px);
        if (t.m > scale_max) scale_max = t.m;
        ht_put(h, t);
    }
    closedir(d);
    if (scale_max > 8) scale_max = 8;
    return n_loaded;
}

/* ---- collecting ---- */
#define MAX_COLLECT 6000
static uint8_t (*coll)[16];
static uint64_t *coll_h;
static int n_coll, coll_game = -1, coll_dirty;
static int sc_cnt;

static void cache_path(int game, char *out, size_t n) { snprintf(out, n, "%stexcache_%s.bin", settings_dir(), games[game].id); }

static int coll_find(uint64_t h)
{
    for (int k = 0; k < n_coll; k++) if (coll_h[k] == h) return k;
    return -1;
}

static void coll_add(const uint8_t *t)
{
    if (n_coll >= MAX_COLLECT) return;
    uint64_t h = tile_hash16(t);
    if (coll_find(h) >= 0) return;
    memcpy(coll[n_coll], t, 16);
    coll_h[n_coll++] = h;
    coll_dirty = 1;
}

void tex_collect_begin(int game)
{
    if (!coll) { coll = calloc(MAX_COLLECT, 16); coll_h = calloc(MAX_COLLECT, sizeof(uint64_t)); }
    if (coll_game >= 0 && coll_game != game) tex_collect_save();
    if (coll_game == game) return;
    coll_game = game; n_coll = 0; coll_dirty = 0;
    char p[1300];
    cache_path(game, p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) return;
    char magic[4]; int cnt = 0;
    if (fread(magic, 1, 4, f) == 4 && !memcmp(magic, "TX01", 4) && fread(&cnt, 4, 1, f) == 1) {
        uint8_t t[16];
        for (int i = 0; i < cnt && i < MAX_COLLECT && fread(t, 1, 16, f) == 16; i++) coll_add(t);
    }
    fclose(f);
    coll_dirty = 0;
}

void tex_collect_frame(const Frame *f)
{
    if (coll_game < 0 || !f->lcd_on) return;
    if (++sc_cnt % 6) return;
    uint8_t used[384];
    memset(used, 0, sizeof used);
    for (int y = 0; y < GB_H; y++)
        for (int x = 0; x < (f->w ? f->w : GB_W); x++) {
            if (f->bgtile[y][x] < 384) used[f->bgtile[y][x]] = 1;
            if (f->sprtile[y][x] < 384) used[f->sprtile[y][x]] = 1;
        }
    for (int i = 0; i < 384; i++) if (used[i]) coll_add(&f->tiles[i * 16]);
}

void tex_collect_save(void)
{
    if (coll_game < 0 || !coll_dirty) return;
    char p[1300];
    cache_path(coll_game, p, sizeof p);
    FILE *f = fopen(p, "wb");
    if (!f) return;
    fwrite("TX01", 1, 4, f);
    fwrite(&n_coll, 4, 1, f);
    fwrite(coll, 16, (size_t)n_coll, f);
    fclose(f);
    coll_dirty = 0;
}

int tex_collected(void) { return n_coll; }
void tex_collect_clear(void) { n_coll = 0; coll_dirty = 1; tex_collect_save(); }

int tex_export(const char *dir, const Palette *pal, int scale, char *msg, size_t mn)
{
    if (!dir || !dir[0]) { snprintf(msg, mn, "Pick a folder first."); return -1; }
    if (n_coll == 0) { snprintf(msg, mn, "Nothing collected yet. Play (or preview) the game first."); return -1; }
    if (scale < 1) scale = 1;
    if (scale > 16) scale = 16;
    mkdir_u(dir);
    int S = 8 * scale, written = 0;
    uint32_t col[4];
    for (int i = 0; i < 4; i++) col[i] = 0xFF000000u | pal->bg[i];
    uint32_t *img = (uint32_t *)malloc((size_t)S * S * 4);
    if (!img) return -1;
    int cols = 16, rows = (n_coll + cols - 1) / cols, SW = cols * 17 + 1, SH = rows * 17 + 1;
    uint32_t *sheet = (uint32_t *)malloc((size_t)SW * SH * 4);
    if (sheet) for (int i = 0; i < SW * SH; i++) sheet[i] = 0xFF303030u;
    for (int t = 0; t < n_coll; t++) {
        const uint8_t *d = coll[t];
        int ci[8][8];
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int bit = 7 - x;
                ci[y][x] = (((d[y * 2 + 1] >> bit) & 1) << 1) | ((d[y * 2] >> bit) & 1);
            }
        for (int y = 0; y < S; y++)
            for (int x = 0; x < S; x++) img[y * S + x] = col[ci[y / scale][x / scale]];
        /* stb wants RGBA bytes */
        uint8_t *rgba = (uint8_t *)malloc((size_t)S * S * 4);
        if (!rgba) break;
        for (int i = 0; i < S * S; i++) {
            rgba[i * 4] = (uint8_t)(img[i] >> 16); rgba[i * 4 + 1] = (uint8_t)(img[i] >> 8); rgba[i * 4 + 2] = (uint8_t)img[i]; rgba[i * 4 + 3] = 255;
        }
        char path[1400];
        snprintf(path, sizeof path, "%s/%016llx.png", dir, (unsigned long long)coll_h[t]);
        if (stbi_write_png(path, S, S, 4, rgba, S * 4)) written++;
        free(rgba);
        if (sheet) {
            int cx = 1 + (t % cols) * 17, cy = 1 + (t / cols) * 17;
            for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) { sheet[(cy + y * 2) * SW + cx + x * 2] = sheet[(cy + y * 2) * SW + cx + x * 2 + 1] = sheet[(cy + y * 2 + 1) * SW + cx + x * 2] = sheet[(cy + y * 2 + 1) * SW + cx + x * 2 + 1] = col[ci[y][x]]; }
        }
    }
    if (sheet) {
        uint8_t *rgba = (uint8_t *)malloc((size_t)SW * SH * 4);
        if (rgba) {
            for (int i = 0; i < SW * SH; i++) { rgba[i * 4] = (uint8_t)(sheet[i] >> 16); rgba[i * 4 + 1] = (uint8_t)(sheet[i] >> 8); rgba[i * 4 + 2] = (uint8_t)sheet[i]; rgba[i * 4 + 3] = 255; }
            char path[1400];
            snprintf(path, sizeof path, "%s/_overview.png", dir);
            stbi_write_png(path, SW, SH, 4, rgba, SW * 4);
            free(rgba);
        }
        free(sheet);
    }
    char rp[1400];
    snprintf(rp, sizeof rp, "%s/README.txt", dir);
    FILE *f = fopen(rp, "w");
    if (f) {
        fprintf(f, "Texture pack for the launcher\n\n"
                   "Every PNG here is one 8x8 tile of the game, saved at %dx%d pixels in the current Game Boy colors.\n"
                   "The file name is the tile's id. To make a pack:\n"
                   "  1. Redraw the tiles you like. Keep the file names.\n"
                   "  2. Any size that is a multiple of 8 works (64x64, 128x128 ...). Transparent pixels show the original tile.\n"
                   "  3. Delete the tiles you did not change if you like (_overview.png is only for looking at).\n"
                   "  4. In the launcher choose this folder under Textures and switch the pack on.\n", S, S);
        fclose(f);
    }
    free(img);
    snprintf(msg, mn, "Saved %d tiles.", written);
    return written;
}
