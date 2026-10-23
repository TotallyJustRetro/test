#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "render.h"
#include "settings.h"
#include "texpack.h"

static SDL_Renderer *ren;
static SDL_Texture *tex_game, *tex_bloom;
static int iw, ih;                       /* current picture size */
static uint32_t *img;
static float *gacc;                      /* ghost accumulator, 3 floats per pixel */
static int gacc_valid;
static uint32_t avg_rgb;
static FilterCfg cur_f;                  /* filters used for the current picture */

void render_init(SDL_Renderer *r) { ren = r; }

static void overlay_free(void);
void render_shutdown(void)
{
    if (tex_game) SDL_DestroyTexture(tex_game);
    if (tex_bloom) SDL_DestroyTexture(tex_bloom);
    tex_game = tex_bloom = NULL;
    overlay_free();
    free(img); free(gacc);
    img = NULL; gacc = NULL; iw = ih = 0;
}

void render_reset(void) { gacc_valid = 0; }
uint32_t render_avg_color(void) { return avg_rgb; }

static int gw = GB_W;
int render_width(void) { return gw; }
void frame_from_ppu(Frame *f)
{
    memcpy(f->shade, ppu_shade, sizeof f->shade);
    memcpy(f->layer, ppu_layer, sizeof f->layer);
    memcpy(f->bguv, ppu_bguv, sizeof f->bguv);
    memcpy(f->spruv, ppu_spruv, sizeof f->spruv);
    memcpy(f->bgtile, ppu_bgtile, sizeof f->bgtile);
    memcpy(f->sprtile, ppu_sprtile, sizeof f->sprtile);
    memcpy(f->tiles, vram, sizeof f->tiles);
    f->lcd_on = ppu_lcd_is_on();
    f->w = ppu_w; f->xoff = ppu_xoff;
    f->seq++;
}

/* ------------------------------------------------------------------ picture */
static int ensure(int w, int h)
{
    if (w == iw && h == ih && img) return 1;
    free(img); free(gacc);
    img = (uint32_t *)malloc((size_t)w * h * 4);
    gacc = NULL;
    gacc_valid = 0;
    if (tex_game) SDL_DestroyTexture(tex_game);
    tex_game = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!img || !tex_game) { iw = ih = 0; return 0; }
    iw = w; ih = h;
    return 1;
}

static int any_cpu_filter(const FilterCfg *f) { return f->ghost || f->hdr || f->blur || f->bloom; }

static inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

static void cpu_filters(const FilterCfg *f, int live, int N)
{
    size_t np = (size_t)iw * ih;
    float *c = (float *)malloc(np * 3 * sizeof(float));
    if (!c) return;
    for (size_t i = 0; i < np; i++) {
        uint32_t p = img[i];
        c[i * 3] = ((p >> 16) & 255) * (1.0f / 255); c[i * 3 + 1] = ((p >> 8) & 255) * (1.0f / 255); c[i * 3 + 2] = (p & 255) * (1.0f / 255);
    }
    /* LCD response: pixels fade towards their new value instead of switching instantly */
    if (f->ghost && live) {
        if (!gacc) gacc = (float *)malloc(np * 3 * sizeof(float));
        if (gacc) {
            if (!gacc_valid) { memcpy(gacc, c, np * 3 * sizeof(float)); gacc_valid = 1; }
            float k = 1.0f - 0.82f * (f->ghost / 100.0f);
            for (size_t i = 0; i < np * 3; i++) { gacc[i] += (c[i] - gacc[i]) * k; c[i] = gacc[i]; }
        }
    } else gacc_valid = 0;
    /* HDR look: bright areas get brighter and a touch more saturated, highlights roll off softly */
    if (f->hdr) {
        float h = f->hdr / 100.0f;
        for (size_t i = 0; i < np; i++) {
            float r = c[i * 3], g = c[i * 3 + 1], b = c[i * 3 + 2];
            float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            float boost = 1.0f + h * 1.1f * y * y;
            float sat = 1.0f + 0.35f * h;
            r = (y + (r - y) * sat) * boost; g = (y + (g - y) * sat) * boost; b = (y + (b - y) * sat) * boost;
            float con = 1.0f + 0.25f * h;
            r = (r - 0.45f) * con + 0.45f; g = (g - 0.45f) * con + 0.45f; b = (b - 0.45f) * con + 0.45f;
            c[i * 3] = r < 0 ? 0 : r; c[i * 3 + 1] = g < 0 ? 0 : g; c[i * 3 + 2] = b < 0 ? 0 : b;
        }
    }
    /* bloom source (before clipping so HDR highlights glow) */
    if (f->bloom || f->hdr) {
        const int bw = 80, bh = 72;
        static float bl[80 * 72 * 3], tmp[80 * 72 * 3];
        float thr = 0.55f - 0.2f * (f->hdr / 100.0f);
        int sx = iw / bw, sy = ih / bh;
        for (int y = 0; y < bh; y++)
            for (int x = 0; x < bw; x++) {
                float s[3] = {0, 0, 0};
                for (int j = 0; j < sy; j++)
                    for (int i = 0; i < sx; i++) {
                        size_t q = ((size_t)(y * sy + j) * iw + (x * sx + i)) * 3;
                        for (int k = 0; k < 3; k++) { float v = c[q + k] - thr; s[k] += v > 0 ? v : 0; }
                    }
                for (int k = 0; k < 3; k++) bl[(y * bw + x) * 3 + k] = s[k] / (float)(sx * sy);
            }
        for (int pass = 0; pass < 3; pass++) {            /* separable box blur, radius 2 */
            for (int y = 0; y < bh; y++)
                for (int x = 0; x < bw; x++)
                    for (int k = 0; k < 3; k++) {
                        float a = 0;
                        for (int d = -2; d <= 2; d++) { int xx = x + d < 0 ? 0 : (x + d >= bw ? bw - 1 : x + d); a += bl[(y * bw + xx) * 3 + k]; }
                        tmp[(y * bw + x) * 3 + k] = a * 0.2f;
                    }
            for (int y = 0; y < bh; y++)
                for (int x = 0; x < bw; x++)
                    for (int k = 0; k < 3; k++) {
                        float a = 0;
                        for (int d = -2; d <= 2; d++) { int yy = y + d < 0 ? 0 : (y + d >= bh ? bh - 1 : y + d); a += tmp[(yy * bw + x) * 3 + k]; }
                        bl[(y * bw + x) * 3 + k] = a * 0.2f;
                    }
        }
        static uint32_t bpx[80 * 72];
        float gain = 2.2f * ((f->bloom + f->hdr * 0.6f) / 100.0f);
        for (int i = 0; i < bw * bh; i++) {
            int r = (int)(clamp01(bl[i * 3] * gain) * 255), g = (int)(clamp01(bl[i * 3 + 1] * gain) * 255), b = (int)(clamp01(bl[i * 3 + 2] * gain) * 255);
            bpx[i] = 0xFF000000u | (uint32_t)(r << 16) | (uint32_t)(g << 8) | (uint32_t)b;
        }
        if (!tex_bloom) {
            tex_bloom = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, bw, bh);
            if (tex_bloom) { SDL_SetTextureBlendMode(tex_bloom, SDL_BLENDMODE_ADD); SDL_SetTextureScaleMode(tex_bloom, SDL_ScaleModeLinear); }
        }
        if (tex_bloom) SDL_UpdateTexture(tex_bloom, NULL, bpx, bw * 4);
    }
    /* softness: horizontal blur, like a slightly smeared analogue signal */
    if (f->blur) {
        float a = f->blur / 100.0f * 0.30f;
        int step = N;
        float *row = (float *)malloc((size_t)iw * 3 * sizeof(float));
        if (row) {
            for (int y = 0; y < ih; y++) {
                float *p = &c[(size_t)y * iw * 3];
                memcpy(row, p, (size_t)iw * 3 * sizeof(float));
                for (int x = 0; x < iw; x++) {
                    int l = x - step < 0 ? 0 : x - step, r = x + step >= iw ? iw - 1 : x + step;
                    for (int k = 0; k < 3; k++) p[x * 3 + k] = row[x * 3 + k] * (1 - 2 * a) + (row[l * 3 + k] + row[r * 3 + k]) * a;
                }
            }
            free(row);
        }
    }
    for (size_t i = 0; i < np; i++) {
        float v[3];
        for (int k = 0; k < 3; k++) {
            float x = c[i * 3 + k];
            if (x > 0.85f) x = 0.85f + 0.15f * tanhf((x - 0.85f) / 0.15f);      /* soft shoulder */
            v[k] = clamp01(x);
        }
        img[i] = 0xFF000000u | ((uint32_t)(v[0] * 255 + 0.5f) << 16) | ((uint32_t)(v[1] * 255 + 0.5f) << 8) | (uint32_t)(v[2] * 255 + 0.5f);
    }
    free(c);
}

void render_build(const Frame *f, int game, int live)
{
    const GameCfg *cfg = &settings.g[game];
    const Palette *p = &palettes[cfg->palette];
    const FilterCfg *flt = &settings.flt;
    cur_f = *flt;
    int use_pack = cfg->tex_on && texpack_count() > 0;
    int N = use_pack ? texpack_scale() : 1;
    gw = f->w > 0 ? f->w : GB_W;
    if (!ensure(gw * N, GB_H * N)) return;

    uint32_t t[3][4];
    for (int i = 0; i < 4; i++) {
        t[0][i] = 0xFF000000u | p->bg[i];
        t[1][i] = 0xFF000000u | (p->multi ? p->ob0[i] : p->bg[i]);
        t[2][i] = 0xFF000000u | (p->multi ? p->ob1[i] : p->bg[i]);
    }
    const TexTile *rep[384];
    if (use_pack) for (int i = 0; i < 384; i++) rep[i] = texpack_find(tile_hash16(&f->tiles[i * 16]));

    unsigned long long sr = 0, sg = 0, sb = 0;
    for (int y = 0; y < GB_H; y++)
        for (int x = 0; x < gw; x++) {
            int layer = f->layer[y][x] % 3;
            uint32_t col = t[layer][f->shade[y][x] & 3];
            sr += (col >> 16) & 255; sg += (col >> 8) & 255; sb += col & 255;
            if (N == 1) { img[y * gw + x] = col; continue; }
            const TexTile *tt = NULL;
            int uv = 0, fx = 0, fy = 0;
            if (f->lcd_on) {
                if (layer) { int id = f->sprtile[y][x]; if (id < 384) tt = rep[id]; uv = f->spruv[y][x]; fx = (uv >> 6) & 1; fy = (uv >> 7) & 1; }
                else { int id = f->bgtile[y][x]; if (id < 384) tt = rep[id]; uv = f->bguv[y][x]; }
            }
            int u = uv & 7, v = (uv >> 3) & 7;
            for (int j = 0; j < N; j++) {
                uint32_t *dst = &img[(size_t)(y * N + j) * iw + (size_t)x * N];
                for (int i = 0; i < N; i++) {
                    uint32_t px = col;
                    if (tt) {
                        int ii = fx ? N - 1 - i : i, jj = fy ? N - 1 - j : j;
                        int sx = u * tt->m + ii * tt->m / N, sy = v * tt->m + jj * tt->m / N;
                        uint32_t q = tt->px[sy * 8 * tt->m + sx];
                        if ((q >> 24) >= 128) px = 0xFF000000u | (q & 0xFFFFFF);
                    }
                    dst[i] = px;
                }
            }
        }
    avg_rgb = (uint32_t)((sr / (gw * GB_H)) << 16 | (sg / (gw * GB_H)) << 8 | (sb / (gw * GB_H)));
    if (any_cpu_filter(flt)) cpu_filters(flt, live, N);
    else gacc_valid = 0;
    SDL_UpdateTexture(tex_game, NULL, img, iw * 4);
}

/* ------------------------------------------------------------------ placement */
void render_fit(int W, int H, int aspect, int scaling, SDL_Rect *r)
{
    if (scaling == SCALE_STRETCH) { r->x = 0; r->y = 0; r->w = W; r->h = H; return; }
    double ar = aspect == ASPECT_4_3 ? 4.0 / 3.0 : (aspect == ASPECT_16_9 ? 16.0 / 9.0 : (double)gw / 144.0);
    double scale = H / 144.0;
    if (144.0 * scale * ar > W) scale = W / (144.0 * ar);
    if (scaling == SCALE_PIXEL) {
        double k = floor(scale);
        if (k < 1) k = scale;                      /* window smaller than 1x: just fit */
        scale = k;
    }
    r->h = (int)(144.0 * scale + 0.5);
    r->w = (int)(144.0 * scale * ar + 0.5);
    r->x = (W - r->w) / 2;
    r->y = (H - r->h) / 2;
}

/* ------------------------------------------------------------------ overlays (multiplied onto the picture) */
typedef struct { SDL_Texture *t; long key; } Ov;
static Ov ov_grid, ov_scan, ov_mask, ov_vig;

static void overlay_free(void)
{
    Ov *all[4] = {&ov_grid, &ov_scan, &ov_mask, &ov_vig};
    for (int i = 0; i < 4; i++) { if (all[i]->t) SDL_DestroyTexture(all[i]->t); all[i]->t = NULL; all[i]->key = 0; }
}

static SDL_Texture *make_ov(Ov *o, long key, int w, int h, int linear, uint32_t (*fn)(int x, int y, int w, int h, int a), int a)
{
    if (o->t && o->key == key) return o->t;
    if (o->t) SDL_DestroyTexture(o->t);
    o->t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
    o->key = key;
    if (!o->t) return NULL;
    uint32_t *px = (uint32_t *)malloc((size_t)w * h * 4);
    if (!px) return o->t;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) px[y * w + x] = fn(x, y, w, h, a);
    SDL_UpdateTexture(o->t, NULL, px, w * 4);
    free(px);
    SDL_SetTextureBlendMode(o->t, SDL_BLENDMODE_MOD);
    SDL_SetTextureScaleMode(o->t, linear ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
    return o->t;
}

static inline uint32_t grey(float v) { int g = (int)(clamp01(v) * 255 + 0.5f); return 0xFF000000u | (uint32_t)(g << 16 | g << 8 | g); }

static int grid_c;
static uint32_t fn_grid(int x, int y, int w, int h, int a)
{
    (void)w; (void)h;
    int c = grid_c, t = c >= 7 ? 2 : 1;
    int lx = x % c, ly = y % c;
    float s = a / 100.0f;
    float v = 1.0f;
    if (lx >= c - t || ly >= c - t) v = 1.0f - 0.80f * s;                       /* gap between the LCD dots */
    else {
        /* the dot itself is a little brighter in the middle */
        float dx = (lx + 0.5f) / (c - t) - 0.5f, dy = (ly + 0.5f) / (c - t) - 0.5f;
        v = 1.0f - 0.18f * s * (dx * dx + dy * dy) * 4.0f;
    }
    return grey(v);
}
static uint32_t fn_scan(int x, int y, int w, int h, int a)
{
    (void)x; (void)w; (void)h;
    int c = grid_c;
    float t = ((y % c) + 0.5f) / c;
    float bright = 0.5f + 0.5f * cosf((t - 0.5f) * 6.2831853f);               /* 1 in the middle of the line */
    bright = powf(bright, 0.8f);
    return grey(1.0f - (a / 100.0f) * 0.85f * (1.0f - bright));
}
static uint32_t fn_mask(int x, int y, int w, int h, int a)
{
    (void)y; (void)w; (void)h;
    float m = a / 100.0f * 0.55f;
    float r = 1, g = 1, b = 1;
    switch (x % 3) { case 0: g = b = 1 - m; break; case 1: r = b = 1 - m; break; default: r = g = 1 - m; break; }
    r = r * (1.0f + m * 0.3f); g = g * (1.0f + m * 0.3f); b = b * (1.0f + m * 0.3f);
    return 0xFF000000u | ((uint32_t)(clamp01(r) * 255) << 16) | ((uint32_t)(clamp01(g) * 255) << 8) | (uint32_t)(clamp01(b) * 255);
}
static uint32_t fn_vig(int x, int y, int w, int h, int a)
{
    float dx = ((x + 0.5f) / w - 0.5f) * 2, dy = ((y + 0.5f) / h - 0.5f) * 2;
    float r = sqrtf(dx * dx + dy * dy) / 1.4142f;
    float s = clamp01((r - 0.35f) / 0.65f);
    s = s * s * (3 - 2 * s);
    return grey(1.0f - (a / 100.0f) * 0.85f * s);
}

/* ------------------------------------------------------------------ drawing */
static void draw_quad(SDL_Texture *t, const SDL_Rect *r, int curve)
{
    if (!t) return;
    if (!curve) { SDL_RenderCopy(ren, t, NULL, r); return; }
    enum { GX = 24, GY = 20 };
    SDL_Vertex v[(GX + 1) * (GY + 1)];
    int idx[GX * GY * 6], ni = 0;
    float k = curve / 100.0f * 2.2f;
    float norm = 1.0f / (1.0f - 0.08f * k);
    for (int j = 0; j <= GY; j++)
        for (int i = 0; i <= GX; i++) {
            float u = (float)i / GX, w = (float)j / GY;
            float px = (u - 0.5f) * 2, py = (w - 0.5f) * 2;
            float r2 = px * px + py * py;
            float s = (1.0f - 0.08f * k * r2) * norm;
            SDL_Vertex *p = &v[j * (GX + 1) + i];
            p->position.x = r->x + (0.5f + px * s * 0.5f) * r->w;
            p->position.y = r->y + (0.5f + py * s * 0.5f) * r->h;
            p->color = (SDL_Color){255, 255, 255, 255};
            p->tex_coord.x = u; p->tex_coord.y = w;
        }
    for (int j = 0; j < GY; j++)
        for (int i = 0; i < GX; i++) {
            int a = j * (GX + 1) + i, b = a + 1, c = a + GX + 1, d = c + 1;
            idx[ni++] = a; idx[ni++] = b; idx[ni++] = c; idx[ni++] = b; idx[ni++] = d; idx[ni++] = c;
        }
    SDL_RenderGeometry(ren, t, v, (GX + 1) * (GY + 1), idx, ni);
}

void render_draw(const SDL_Rect *r, int scaling)
{
    if (!tex_game || r->w < 4 || r->h < 4) return;
    const FilterCfg *f = &cur_f;
    if (f->curve) { SDL_SetRenderDrawColor(ren, 0, 0, 0, 255); SDL_RenderFillRect(ren, r); }
    SDL_SetTextureScaleMode(tex_game, scaling == SCALE_PIXEL ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
    SDL_SetTextureBlendMode(tex_game, SDL_BLENDMODE_NONE);
    draw_quad(tex_game, r, f->curve);

    int c = (int)floorf(r->h / 144.0f + 0.5f);
    if (c < 3) c = 3;
    if (c > 8) c = 8;
    int exact = (r->h % 144 == 0);
    if (f->lcd_grid) {
        grid_c = c;
        SDL_Texture *t = make_ov(&ov_grid, (long)c * 1000 + f->lcd_grid + (long)gw * 100000, gw * c, GB_H * c, !exact, fn_grid, f->lcd_grid);
        draw_quad(t, r, f->curve);
    }
    if (f->scanlines) {
        grid_c = 6;
        SDL_Texture *t = make_ov(&ov_scan, 6000 + f->scanlines, 4, GB_H * 6, 1, fn_scan, f->scanlines);
        draw_quad(t, r, f->curve);
    }
    if (f->mask) {
        SDL_Texture *t = make_ov(&ov_mask, 7000 + f->mask + (long)gw * 100000, gw * 3, 2, 1, fn_mask, f->mask);
        draw_quad(t, r, f->curve);
    }
    if ((f->bloom || f->hdr) && tex_bloom) draw_quad(tex_bloom, r, f->curve);
    if (f->vignette) {
        SDL_Texture *t = make_ov(&ov_vig, 8000 + f->vignette, 160, 144, 1, fn_vig, f->vignette);
        draw_quad(t, r, f->curve);
    }
}
