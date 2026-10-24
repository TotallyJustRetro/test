#include "ui.h"
#include "font.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

UiMouse ui_mouse;
float ui_scale = 1.0f, ui_dt = 0.016f;
void (*ui_sfx_cb)(int);
uint32_t ui_accent = 0x4C8DFF;

static SDL_Renderer *ren;
static float ox, oy;
static int PW, PH;
static stbtt_fontinfo finfo[2];
static int fonts_ok;
static char hint_cur[200], hint_next[200];

/* ------------------------------------------------------------ glyph atlases */
typedef struct { int x, y, w, h; float xoff, yoff, adv; } Glyph;
typedef struct {
    int font, px;
    SDL_Texture *tex;
    Glyph g[96 + 96 + 16];
    float ascent, descent, line;
    unsigned used;
} Atlas;
#define MAX_ATLAS 24
static Atlas atlas[MAX_ATLAS];
static unsigned atlas_tick;

static const int extra_cp[16] = {0x2022, 0x2026, 0x2192, 0x2190, 0x00D7, 0x2713, 0x25B6, 0x2014, 0x2013, 0x2018, 0x2019, 0x201C, 0x201D, 0, 0, 0};

static int cp_index(int cp)
{
    if (cp >= 32 && cp < 127) return cp - 32;
    if (cp >= 160 && cp < 256) return 95 + (cp - 160);
    for (int i = 0; i < 13; i++) if (extra_cp[i] == cp) return 192 + i;
    return -1;
}
static int index_cp(int i)
{
    if (i < 95) return 32 + i;
    if (i < 192) return 160 + (i - 95);
    return extra_cp[i - 192];
}

static void atlas_free(Atlas *a) { if (a->tex) SDL_DestroyTexture(a->tex); memset(a, 0, sizeof *a); }

static Atlas *get_atlas(int font, int px)
{
    if (px < 6) px = 6;
    if (px > 160) px = 160;
    Atlas *free_a = NULL, *old = &atlas[0];
    for (int i = 0; i < MAX_ATLAS; i++) {
        if (atlas[i].tex && atlas[i].font == font && atlas[i].px == px) { atlas[i].used = ++atlas_tick; return &atlas[i]; }
        if (!atlas[i].tex && !free_a) free_a = &atlas[i];
        if (atlas[i].used < old->used) old = &atlas[i];
    }
    Atlas *a = free_a ? free_a : old;
    atlas_free(a);
    a->font = font; a->px = px; a->used = ++atlas_tick;
    stbtt_fontinfo *fi = &finfo[font];
    float sc = stbtt_ScaleForPixelHeight(fi, (float)px);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(fi, &asc, &desc, &gap);
    a->ascent = asc * sc; a->descent = desc * sc; a->line = (asc - desc + gap) * sc;
    int n = 96 + 96 + 16, AW = 512;
    int x = 1, y = 1, rowh = 0;
    for (int i = 0; i < n; i++) {
        int cp = index_cp(i);
        Glyph *g = &a->g[i];
        memset(g, 0, sizeof *g);
        if (!cp) continue;
        int gi = stbtt_FindGlyphIndex(fi, cp);
        int adv, lsb;
        stbtt_GetGlyphHMetrics(fi, gi, &adv, &lsb);
        g->adv = adv * sc;
        if (!gi) { g->adv = 0; continue; }
        int x0, y0, x1, y1;
        stbtt_GetGlyphBitmapBox(fi, gi, sc, sc, &x0, &y0, &x1, &y1);
        g->w = x1 - x0; g->h = y1 - y0; g->xoff = (float)x0; g->yoff = (float)y0;
        if (x + g->w + 1 > AW) { x = 1; y += rowh + 1; rowh = 0; }
        g->x = x; g->y = y;
        x += g->w + 1;
        if (g->h > rowh) rowh = g->h;
    }
    int AH = y + rowh + 2, H2 = 64;
    while (H2 < AH) H2 <<= 1;
    uint32_t *px32 = (uint32_t *)calloc((size_t)AW * H2, 4);
    unsigned char *tmp = (unsigned char *)malloc((size_t)(px + 8) * (px + 8) * 2);
    for (int i = 0; i < n && px32 && tmp; i++) {
        Glyph *g = &a->g[i];
        int cp = index_cp(i);
        if (!cp || g->w <= 0 || g->h <= 0) continue;
        int gi = stbtt_FindGlyphIndex(fi, cp);
        stbtt_MakeGlyphBitmap(fi, tmp, g->w, g->h, g->w, sc, sc, gi);
        for (int yy = 0; yy < g->h; yy++)
            for (int xx = 0; xx < g->w; xx++) {
                float cov = tmp[yy * g->w + xx] / 255.0f;
                cov = powf(cov, 0.82f);                    /* slightly heavier: reads better on dark backgrounds */
                px32[(size_t)(g->y + yy) * AW + g->x + xx] = ((uint32_t)(cov * 255.0f + 0.5f) << 24) | 0x00FFFFFF;
            }
    }
    a->tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, AW, H2);
    if (a->tex && px32) {
        SDL_UpdateTexture(a->tex, NULL, px32, AW * 4);
        SDL_SetTextureBlendMode(a->tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(a->tex, SDL_ScaleModeNearest);
    }
    free(px32); free(tmp);
    return a;
}

/* ------------------------------------------------------------ discs for rounded corners */
typedef struct { int r; SDL_Texture *disc, *ring; int ring_t; } CornerTex;
static CornerTex corners[48];
static int n_corners;

static SDL_Texture *make_disc(int r, int inner_r2x10)
{
    int d = 2 * r;
    uint32_t *px = (uint32_t *)malloc((size_t)d * d * 4);
    if (!px) return NULL;
    float ri = inner_r2x10 / 10.0f;
    for (int y = 0; y < d; y++)
        for (int x = 0; x < d; x++) {
            int hit = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float dx = x + (sx + 0.5f) / 4 - r, dy = y + (sy + 0.5f) / 4 - r, q = dx * dx + dy * dy;
                    if (q <= (float)(r * r) && q >= ri * ri) hit++;
                }
            px[y * d + x] = ((uint32_t)(hit * 255 / 16) << 24) | 0x00FFFFFF;
        }
    SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, d, d);
    if (t) {
        SDL_UpdateTexture(t, NULL, px, d * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    }
    free(px);
    return t;
}

static CornerTex *corner_for(int r, int ring_t)
{
    for (int i = 0; i < n_corners; i++)
        if (corners[i].r == r && (ring_t == 0 || corners[i].ring_t == ring_t)) {
            if (ring_t && !corners[i].ring) { corners[i].ring = make_disc(r, (r - ring_t) * 10); corners[i].ring_t = ring_t; }
            return &corners[i];
        }
    if (n_corners >= 48) {                         /* recycle slot 0 */
        if (corners[0].disc) SDL_DestroyTexture(corners[0].disc);
        if (corners[0].ring) SDL_DestroyTexture(corners[0].ring);
        memmove(&corners[0], &corners[1], sizeof(CornerTex) * 47);
        n_corners = 47;
    }
    CornerTex *c = &corners[n_corners++];
    memset(c, 0, sizeof *c);
    c->r = r;
    c->disc = make_disc(r, 0);
    if (ring_t) { c->ring = make_disc(r, (r - ring_t) * 10); c->ring_t = ring_t; }
    return c;
}

/* ------------------------------------------------------------ setup */
void ui_init(SDL_Renderer *r)
{
    ren = r;
    stbtt_InitFont(&finfo[F_REG], font_regular, stbtt_GetFontOffsetForIndex(font_regular, 0));
    stbtt_InitFont(&finfo[F_BOLD], font_bold, stbtt_GetFontOffsetForIndex(font_bold, 0));
    fonts_ok = 1;
}

void ui_shutdown(void)
{
    for (int i = 0; i < MAX_ATLAS; i++) atlas_free(&atlas[i]);
    for (int i = 0; i < n_corners; i++) {
        if (corners[i].disc) SDL_DestroyTexture(corners[i].disc);
        if (corners[i].ring) SDL_DestroyTexture(corners[i].ring);
    }
    n_corners = 0;
}

static int widget_counter, active_id, hot_id;
static struct { int id; float v; } anims[256];

void ui_begin(int pw, int ph, float dt)
{
    PW = pw; PH = ph; ui_dt = dt > 0.1f ? 0.1f : dt;
    float sx = (float)pw / UI_W, sy = (float)ph / UI_H;
    ui_scale = sx < sy ? sx : sy;
    if (ui_scale < 0.35f) ui_scale = 0.35f;
    ox = (pw - UI_W * ui_scale) * 0.5f;
    oy = (ph - UI_H * ui_scale) * 0.5f;
    widget_counter = 0;
    memcpy(hint_cur, hint_next, sizeof hint_cur);
    hint_next[0] = 0;
    hot_id = 0;
    SDL_RenderSetClipRect(ren, NULL);
}

void ui_end(void)
{
    ui_mouse.pressed = ui_mouse.released = 0;
    ui_mouse.wheel = 0;
    if (!ui_mouse.down) active_id = 0;
}

void ui_set_mouse(int wx, int wy, int ww, int wh)
{
    if (ww <= 0 || wh <= 0) return;
    float px = (float)wx * PW / ww, py = (float)wy * PH / wh;       /* window units -> render pixels */
    ui_mouse.x = (px - ox) / ui_scale;
    ui_mouse.y = (py - oy) / ui_scale;
}

void ui_mouse_button(int down)
{
    if (down && !ui_mouse.down) ui_mouse.pressed = 1;
    if (!down && ui_mouse.down) ui_mouse.released = 1;
    ui_mouse.down = down;
}

void ui_hint(const char *s) { snprintf(hint_next, sizeof hint_next, "%s", s ? s : ""); }
const char *ui_hint_text(void) { return hint_cur; }
float ui_view_x0(void) { return -ox / ui_scale; }
float ui_view_y0(void) { return -oy / ui_scale; }
float ui_view_w(void) { return PW / ui_scale; }
float ui_view_h(void) { return PH / ui_scale; }
int ui_next_id(void) { return ++widget_counter; }
int ui_active_any(void) { return active_id != 0; }

float ui_anim(int id, float target, float speed)
{
    int slot = id & 255;
    if (anims[slot].id != id) { anims[slot].id = id; anims[slot].v = target; }
    float *v = &anims[slot].v;
    float k = 1.0f - expf(-speed * ui_dt);
    *v += (target - *v) * k;
    if (fabsf(*v - target) < 0.002f) *v = target;
    return *v;
}

/* ------------------------------------------------------------ primitives */
static inline int PXx(float x) { return (int)floorf(ox + x * ui_scale + 0.5f); }
static inline int PXy(float y) { return (int)floorf(oy + y * ui_scale + 0.5f); }
static inline int PXs(float v) { return (int)floorf(v * ui_scale + 0.5f); }

static void set_col(uint32_t c)
{
    SDL_SetRenderDrawBlendMode(ren, (c & 255) == 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, (c >> 24) & 255, (c >> 16) & 255, (c >> 8) & 255, c & 255);
}

static void fill_px(int x, int y, int w, int h, uint32_t c)
{
    if (w <= 0 || h <= 0) return;
    set_col(c);
    SDL_Rect r = {x, y, w, h};
    SDL_RenderFillRect(ren, &r);
}

void ui_rect(float x, float y, float w, float h, uint32_t c)
{
    int x0 = PXx(x), y0 = PXy(y), x1 = PXx(x + w), y1 = PXy(y + h);
    fill_px(x0, y0, x1 - x0, y1 - y0, c);
}

static void tint(SDL_Texture *t, uint32_t c)
{
    SDL_SetTextureColorMod(t, (c >> 24) & 255, (c >> 16) & 255, (c >> 8) & 255);
    SDL_SetTextureAlphaMod(t, c & 255);
}

void ui_rrect(float x, float y, float w, float h, float r, uint32_t c)
{
    int x0 = PXx(x), y0 = PXy(y), x1 = PXx(x + w), y1 = PXy(y + h);
    int W = x1 - x0, H = y1 - y0, R = PXs(r);
    if (W <= 0 || H <= 0) return;
    if (R * 2 > W) R = W / 2;
    if (R * 2 > H) R = H / 2;
    if (R < 2) { fill_px(x0, y0, W, H, c); return; }
    CornerTex *ct = corner_for(R, 0);
    if (!ct->disc) { fill_px(x0, y0, W, H, c); return; }
    tint(ct->disc, c);
    SDL_Rect s, d;
    s = (SDL_Rect){0, 0, R, R};     d = (SDL_Rect){x0, y0, R, R};                 SDL_RenderCopy(ren, ct->disc, &s, &d);
    s = (SDL_Rect){R, 0, R, R};     d = (SDL_Rect){x1 - R, y0, R, R};             SDL_RenderCopy(ren, ct->disc, &s, &d);
    s = (SDL_Rect){0, R, R, R};     d = (SDL_Rect){x0, y1 - R, R, R};             SDL_RenderCopy(ren, ct->disc, &s, &d);
    s = (SDL_Rect){R, R, R, R};     d = (SDL_Rect){x1 - R, y1 - R, R, R};         SDL_RenderCopy(ren, ct->disc, &s, &d);
    fill_px(x0 + R, y0, W - 2 * R, H, c);
    fill_px(x0, y0 + R, R, H - 2 * R, c);
    fill_px(x1 - R, y0 + R, R, H - 2 * R, c);
}

void ui_stroke(float x, float y, float w, float h, float r, float t, uint32_t c)
{
    int x0 = PXx(x), y0 = PXy(y), x1 = PXx(x + w), y1 = PXy(y + h);
    int W = x1 - x0, H = y1 - y0, R = PXs(r), T = PXs(t);
    if (T < 1) T = 1;
    if (W <= 0 || H <= 0) return;
    if (R * 2 > W) R = W / 2;
    if (R * 2 > H) R = H / 2;
    if (R < 2 || T >= R) {
        fill_px(x0, y0, W, T, c); fill_px(x0, y1 - T, W, T, c);
        fill_px(x0, y0 + T, T, H - 2 * T, c); fill_px(x1 - T, y0 + T, T, H - 2 * T, c);
        return;
    }
    CornerTex *ct = corner_for(R, T);
    if (!ct->ring) return;
    tint(ct->ring, c);
    SDL_Rect s, d;
    s = (SDL_Rect){0, 0, R, R};     d = (SDL_Rect){x0, y0, R, R};                 SDL_RenderCopy(ren, ct->ring, &s, &d);
    s = (SDL_Rect){R, 0, R, R};     d = (SDL_Rect){x1 - R, y0, R, R};             SDL_RenderCopy(ren, ct->ring, &s, &d);
    s = (SDL_Rect){0, R, R, R};     d = (SDL_Rect){x0, y1 - R, R, R};             SDL_RenderCopy(ren, ct->ring, &s, &d);
    s = (SDL_Rect){R, R, R, R};     d = (SDL_Rect){x1 - R, y1 - R, R, R};         SDL_RenderCopy(ren, ct->ring, &s, &d);
    fill_px(x0 + R, y0, W - 2 * R, T, c);
    fill_px(x0 + R, y1 - T, W - 2 * R, T, c);
    fill_px(x0, y0 + R, T, H - 2 * R, c);
    fill_px(x1 - T, y0 + R, T, H - 2 * R, c);
}

static void quad_colors(int x0, int y0, int x1, int y1, uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11)
{
#define VC(c) (SDL_Color){(Uint8)((c) >> 24), (Uint8)((c) >> 16), (Uint8)((c) >> 8), (Uint8)(c)}
    SDL_Vertex v[4] = {
        {{(float)x0, (float)y0}, VC(c00), {0, 0}}, {{(float)x1, (float)y0}, VC(c10), {0, 0}},
        {{(float)x0, (float)y1}, VC(c01), {0, 0}}, {{(float)x1, (float)y1}, VC(c11), {0, 0}}};
    int idx[6] = {0, 1, 2, 1, 3, 2};
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(ren, NULL, v, 4, idx, 6);
}

void ui_vgrad(float x, float y, float w, float h, uint32_t a, uint32_t b)
{
    quad_colors(PXx(x), PXy(y), PXx(x + w), PXy(y + h), a, a, b, b);
}
void ui_hgrad(float x, float y, float w, float h, uint32_t a, uint32_t b)
{
    quad_colors(PXx(x), PXy(y), PXx(x + w), PXy(y + h), a, b, a, b);
}

void ui_line(float x1, float y1, float x2, float y2, float t, uint32_t c)
{
    float dx = x2 - x1, dy = y2 - y1, l = sqrtf(dx * dx + dy * dy);
    if (l < 0.001f) return;
    float nx = -dy / l * t * 0.5f, ny = dx / l * t * 0.5f;
    SDL_Color col = {(Uint8)(c >> 24), (Uint8)(c >> 16), (Uint8)(c >> 8), (Uint8)c};
    float s = ui_scale;
    SDL_Vertex v[4] = {
        {{ox + (x1 + nx) * s, oy + (y1 + ny) * s}, col, {0, 0}}, {{ox + (x1 - nx) * s, oy + (y1 - ny) * s}, col, {0, 0}},
        {{ox + (x2 + nx) * s, oy + (y2 + ny) * s}, col, {0, 0}}, {{ox + (x2 - nx) * s, oy + (y2 - ny) * s}, col, {0, 0}}};
    int idx[6] = {0, 1, 2, 1, 3, 2};
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(ren, NULL, v, 4, idx, 6);
}

void ui_tri(float x1, float y1, float x2, float y2, float x3, float y3, uint32_t c)
{
    SDL_Color col = {(Uint8)(c >> 24), (Uint8)(c >> 16), (Uint8)(c >> 8), (Uint8)c};
    float s = ui_scale;
    SDL_Vertex v[3] = {{{ox + x1 * s, oy + y1 * s}, col, {0, 0}}, {{ox + x2 * s, oy + y2 * s}, col, {0, 0}},
                       {{ox + x3 * s, oy + y3 * s}, col, {0, 0}}};
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(ren, NULL, v, 3, NULL, 0);
}

void ui_shadow(float x, float y, float w, float h, float r, float spread, uint32_t c)
{
    int a = c & 255;
    for (int i = 6; i >= 1; i--) {
        float g = spread * i / 6.0f;
        ui_rrect(x - g, y - g + spread * 0.35f, w + 2 * g, h + 2 * g, r + g, (c & 0xFFFFFF00u) | (uint32_t)(a * 0.045f * (7 - i) / 3.0f > 255 ? 255 : (int)(a * 0.045f * (7 - i) / 3.0f)));
    }
}

void ui_clip(float x, float y, float w, float h)
{
    SDL_Rect r = {PXx(x), PXy(y), PXs(w), PXs(h)};
    SDL_RenderSetClipRect(ren, &r);
}
void ui_unclip(void) { SDL_RenderSetClipRect(ren, NULL); }

void ui_image(SDL_Texture *t, const SDL_Rect *src, float x, float y, float w, float h)
{
    SDL_Rect d = {PXx(x), PXy(y), PXx(x + w) - PXx(x), PXy(y + h) - PXy(y)};
    SDL_SetTextureColorMod(t, 255, 255, 255);
    SDL_SetTextureAlphaMod(t, 255);
    SDL_RenderCopy(ren, t, src, &d);
}

/* ------------------------------------------------------------ text */
static int next_cp(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    int c = *p;
    if (c < 0x80) { (*s)++; return c; }
    if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) { *s += 2; return ((c & 0x1F) << 6) | (p[1] & 0x3F); }
    if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) { *s += 3; return ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); }
    (*s)++;
    return '?';
}

static const Glyph *glyph_of(Atlas *a, int cp)
{
    int i = cp_index(cp);
    if (i < 0 || a->g[i].adv == 0) i = cp_index('?');
    return &a->g[i];
}

float ui_text_w(int font, float size, const char *s)
{
    if (!fonts_ok) return 0;
    Atlas *a = get_atlas(font, PXs(size));
    float w = 0;
    while (*s) w += glyph_of(a, next_cp(&s))->adv;
    return w / ui_scale;
}

float ui_line_h(float size) { return size * 1.5f; }

void ui_text(int font, float size, float x, float y, uint32_t c, const char *s)
{
    if (!fonts_ok || !s) return;
    Atlas *a = get_atlas(font, PXs(size));
    if (!a->tex) return;
    tint(a->tex, c);
    float px = (float)PXx(x);
    int base = PXy(y) + (int)floorf((ui_line_h(size) * ui_scale - (a->ascent - a->descent)) * 0.5f + a->ascent + 0.5f);
    while (*s) {
        const Glyph *g = glyph_of(a, next_cp(&s));
        if (g->w > 0 && g->h > 0) {
            SDL_Rect sr = {g->x, g->y, g->w, g->h};
            SDL_Rect dr = {(int)floorf(px + g->xoff + 0.5f), base + (int)g->yoff, g->w, g->h};
            SDL_RenderCopy(ren, a->tex, &sr, &dr);
        }
        px += g->adv;
    }
}

void ui_text_c(int font, float size, float cx, float y, uint32_t c, const char *s) { ui_text(font, size, cx - ui_text_w(font, size, s) * 0.5f, y, c, s); }
void ui_text_r(int font, float size, float rx, float y, uint32_t c, const char *s) { ui_text(font, size, rx - ui_text_w(font, size, s), y, c, s); }

void ui_text_fit(int font, float size, float x, float y, float maxw, uint32_t c, const char *s)
{
    if (ui_text_w(font, size, s) <= maxw) { ui_text(font, size, x, y, c, s); return; }
    char buf[600];
    snprintf(buf, sizeof buf, "%s", s);
    size_t l = strlen(buf);
    while (l > 1) {
        l--;
        while (l > 1 && ((unsigned char)buf[l] & 0xC0) == 0x80) l--;      /* keep UTF-8 intact */
        buf[l] = 0;
        char t[620];
        snprintf(t, sizeof t, "%s\xE2\x80\xA6", buf);
        if (ui_text_w(font, size, t) <= maxw) { ui_text(font, size, x, y, c, t); return; }
    }
}

/* path-style fit: keeps the end ("...\\folder\\file.gb") */
void ui_text_fit_tail(int font, float size, float x, float y, float maxw, uint32_t c, const char *s)
{
    if (ui_text_w(font, size, s) <= maxw) { ui_text(font, size, x, y, c, s); return; }
    const char *p = s;
    char t[620];
    while (*p) {
        const char *q = p;
        next_cp(&q);
        p = q;
        snprintf(t, sizeof t, "\xE2\x80\xA6%s", p);
        if (ui_text_w(font, size, t) <= maxw) { ui_text(font, size, x, y, c, t); return; }
    }
}

static int wrap_core(int font, float size, float w, const char *s, float x, float y, uint32_t c, float gap, int draw)
{
    char line[400];
    int lines = 0;
    const char *p = s;
    while (*p) {
        size_t ll = 0, last_space = 0;
        line[0] = 0;
        const char *start = p;
        while (*p && *p != '\n') {
            const char *q = p;
            next_cp(&q);
            size_t cl = (size_t)(q - p);
            if (ll + cl >= sizeof line - 1) break;
            memcpy(line + ll, p, cl); line[ll + cl] = 0;
            if (ui_text_w(font, size, line) > w && ll > 0) { line[ll] = 0; break; }
            if (*p == ' ') last_space = ll;
            ll += cl; p = q;
        }
        if (*p && *p != '\n' && last_space > 0) {          /* back up to the last space */
            p = start + last_space + 1;
            line[last_space] = 0;
        } else line[ll] = 0;
        if (draw) ui_text(font, size, x, y + lines * (ui_line_h(size) + gap), c, line);
        lines++;
        if (*p == '\n') p++;
        if (p == start) break;
    }
    return lines;
}

void ui_text_wrap(int font, float size, float x, float y, float w, uint32_t c, const char *s, float gap) { wrap_core(font, size, w, s, x, y, c, gap, 1); }
int ui_wrap_lines(int font, float size, float w, const char *s) { return wrap_core(font, size, w, s, 0, 0, 0, 0, 0); }

/* ------------------------------------------------------------ widgets */
int ui_hover(float x, float y, float w, float h)
{
    return ui_mouse.x >= x && ui_mouse.x < x + w && ui_mouse.y >= y && ui_mouse.y < y + h;
}

static void sfx(int s) { if (ui_sfx_cb) ui_sfx_cb(s); }

/* common press logic; returns 1 when released over the widget after pressing on it */
static int press(int id, int over, int enabled)
{
    int clicked = 0;
    if (!enabled) return 0;
    if (over) {
        if (hot_id == 0) hot_id = id;
        if (ui_mouse.pressed) { active_id = id; }
    }
    if (ui_mouse.released && active_id == id) {
        if (over) clicked = 1;
        active_id = 0;
    }
    return clicked;
}

static uint32_t mix(uint32_t a, uint32_t b, float t)
{
    int r = (int)(((a >> 24) & 255) * (1 - t) + ((b >> 24) & 255) * t);
    int g = (int)(((a >> 16) & 255) * (1 - t) + ((b >> 16) & 255) * t);
    int bl = (int)(((a >> 8) & 255) * (1 - t) + ((b >> 8) & 255) * t);
    int al = (int)((a & 255) * (1 - t) + (b & 255) * t);
    return RGBA(r, g, bl, al);
}

static void hover_sound(int id, int over)
{
    static int last_over;
    if (over && last_over != id) { sfx(0); last_over = id; }
    if (!over && last_over == id) last_over = 0;
}

int ui_button(float x, float y, float w, float h, const char *label, int kind, int enabled)
{
    int id = ui_next_id();
    int over = enabled && ui_hover(x, y, w, h) && (!active_id || active_id == id);
    int clicked = press(id, over, enabled);
    float hv = ui_anim(id * 4 + 1, over ? 1.0f : 0.0f, 16.0f);
    float pv = ui_anim(id * 4 + 2, (active_id == id && over) ? 1.0f : 0.0f, 30.0f);
    uint32_t base, hi, txt = C_TEXT;
    switch (kind) {
    case B_PRIMARY: base = HEX(ui_accent); hi = mix(HEX(ui_accent), HEX(0xFFFFFF), 0.18f); txt = HEX(0xFFFFFF); break;
    case B_GHOST: base = RGBA(255, 255, 255, 0); hi = RGBA(255, 255, 255, 22); break;
    case B_DANGER: base = HEX(0x6A2B30); hi = HEX(0x8A343B); break;
    default: base = C_BTN; hi = C_BTN_H; break;
    }
    if (!enabled) { base = kind == B_GHOST ? base : HEX(0x1E2230); hi = base; txt = C_DIM; }
    uint32_t col = mix(base, hi, hv);
    if (pv > 0.01f) col = mix(col, RGBA(0, 0, 0, 255), pv * 0.2f);
    float yo = pv * 1.0f;
    if (kind == B_PRIMARY && enabled) ui_shadow(x, y, w, h, 10, 7, HEXA(ui_accent, 70));
    ui_rrect(x, y + yo, w, h, 10, col);
    if (kind == B_GHOST && enabled) ui_stroke(x, y + yo, w, h, 10, 1, mix(C_LINE, C_MUTED, hv));
    ui_text_c(F_BOLD, 14, x + w * 0.5f, y + yo + (h - ui_line_h(14)) * 0.5f, txt, label);
    hover_sound(id, over);
    if (clicked) sfx(1);
    return clicked;
}

int ui_chip(float x, float y, float w, float h, const char *label, int selected)
{
    int id = ui_next_id();
    int over = ui_hover(x, y, w, h) && (!active_id || active_id == id);
    int clicked = press(id, over, 1);
    float hv = ui_anim(id * 4 + 1, over ? 1.0f : 0.0f, 16.0f);
    float sv = ui_anim(id * 4 + 3, selected ? 1.0f : 0.0f, 18.0f);
    uint32_t col = mix(mix(C_BTN, C_BTN_H, hv), HEX(ui_accent), sv);
    ui_rrect(x, y, w, h, h * 0.5f, col);
    ui_text_c(F_BOLD, 13, x + w * 0.5f, y + (h - ui_line_h(13)) * 0.5f, selected ? HEX(0xFFFFFF) : C_TEXT, label);
    hover_sound(id, over);
    if (clicked) sfx(1);
    return clicked;
}

int ui_seg(float x, float y, float w, float h, const char **names, int n, int *sel)
{
    int changed = 0;
    float gap = 4, pad = 4;
    ui_rrect(x, y, w, h, 11, C_PANEL2);
    float each = (w - pad * 2 - gap * (n - 1)) / n;
    for (int i = 0; i < n; i++) {
        float bx = x + pad + i * (each + gap), by = y + pad, bh = h - pad * 2;
        int id = ui_next_id();
        int over = ui_hover(bx, by, each, bh) && (!active_id || active_id == id);
        int clicked = press(id, over, 1);
        float hv = ui_anim(id * 4 + 1, over ? 1.0f : 0.0f, 16.0f);
        float sv = ui_anim(id * 4 + 3, *sel == i ? 1.0f : 0.0f, 20.0f);
        uint32_t col = mix(RGBA(255, 255, 255, (int)(hv * 18)), HEX(ui_accent), sv);
        ui_rrect(bx, by, each, bh, 8, col);
        ui_text_c(F_BOLD, 13, bx + each * 0.5f, by + (bh - ui_line_h(13)) * 0.5f, *sel == i ? HEX(0xFFFFFF) : mix(C_MUTED, C_TEXT, hv), names[i]);
        hover_sound(id, over);
        if (clicked) { if (*sel != i) { *sel = i; changed = 1; } sfx(1); }
    }
    return changed;
}

int ui_toggle(float x, float y, int *v)
{
    int id = ui_next_id();
    float w = 44, h = 24;
    int over = ui_hover(x, y, w, h) && (!active_id || active_id == id);
    int clicked = press(id, over, 1);
    float t = ui_anim(id * 4 + 3, *v ? 1.0f : 0.0f, 18.0f);
    ui_rrect(x, y, w, h, 12, mix(C_BTN, HEX(ui_accent), t));
    float kx = x + 3 + t * (w - 24 + 0);
    ui_rrect(kx, y + 3, 18, 18, 9, mix(C_MUTED, HEX(0xFFFFFF), t));
    hover_sound(id, over);
    if (clicked) { *v = !*v; sfx(4); return 1; }
    return 0;
}

int ui_slider(float x, float y, float w, int *v, int lo, int hi)
{
    int id = ui_next_id();
    float h = 22;
    int over = ui_hover(x - 8, y, w + 16, h) && (!active_id || active_id == id);
    if (over && ui_mouse.pressed) active_id = id;
    int changed = 0;
    if (active_id == id) {
        float t = (ui_mouse.x - x) / w;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        int nv = lo + (int)floorf(t * (hi - lo) + 0.5f);
        if (nv != *v) { *v = nv; changed = 1; }
    }
    float t = hi > lo ? (float)(*v - lo) / (hi - lo) : 0;
    float hv = ui_anim(id * 4 + 1, (over || active_id == id) ? 1.0f : 0.0f, 16.0f);
    ui_rrect(x, y + h * 0.5f - 3, w, 6, 3, C_BTN);
    ui_rrect(x, y + h * 0.5f - 3, w * t < 6 ? 6 : w * t, 6, 3, HEX(ui_accent));
    ui_shadow(x + w * t - 9, y + h * 0.5f - 9, 18, 18, 9, 4, RGBA(0, 0, 0, 80));
    ui_rrect(x + w * t - 9, y + h * 0.5f - 9, 18, 18, 9, mix(HEX(0xDDE3F5), HEX(0xFFFFFF), hv));
    hover_sound(id, over);
    return changed;
}
