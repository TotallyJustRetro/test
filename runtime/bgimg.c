/* Custom background: PNG / JPEG / BMP / TGA / PSD-less, and animated GIFs (every frame is kept and played with its own delay). */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_ONLY_TGA
#include <stdlib.h>
#include <string.h>
#include "util.h"
#include "stb_image.h"
#include "bgimg.h"

static SDL_Renderer *ren;
static SDL_Texture *tex;
static unsigned char *stack;     /* frames * w * h * 4 */
static int *delays, nframes, cur, bw, bh;
static float acc;

static unsigned char *read_all(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (*n <= 0 || *n > (200L << 20)) { fclose(f); return NULL; }
    unsigned char *b = (unsigned char *)malloc((size_t)*n);
    if (b && fread(b, 1, (size_t)*n, f) != (size_t)*n) { free(b); b = NULL; }
    fclose(f);
    return b;
}

unsigned char *image_load_rgba(const char *path, int *w, int *h)
{
    long n;
    unsigned char *buf = read_all(path, &n);
    if (!buf) return NULL;
    int c;
    unsigned char *px = stbi_load_from_memory(buf, (int)n, w, h, &c, 4);
    free(buf);
    if (!px) return NULL;
    unsigned char *copy = (unsigned char *)malloc((size_t)*w * *h * 4);
    if (copy) memcpy(copy, px, (size_t)*w * *h * 4);
    stbi_image_free(px);
    return copy;
}

void bg_init(SDL_Renderer *r) { ren = r; }

static void clear(void)
{
    if (tex) SDL_DestroyTexture(tex);
    tex = NULL;
    free(stack); stack = NULL;
    free(delays); delays = NULL;
    nframes = 0; cur = 0; acc = 0; bw = bh = 0;
}

void bg_shutdown(void) { clear(); }

static void shrink(unsigned char **px, int *w, int *h, int frames)
{
    /* box-filter every frame down so the largest side is <= 2048 */
    int big = *w > *h ? *w : *h;
    if (big <= 2048) return;
    int k = (big + 2047) / 2048, nw = *w / k, nh = *h / k;
    unsigned char *o = (unsigned char *)malloc((size_t)nw * nh * 4 * frames);
    if (!o) return;
    for (int f = 0; f < frames; f++)
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++) {
                unsigned sum[4] = {0, 0, 0, 0};
                for (int j = 0; j < k; j++)
                    for (int i = 0; i < k; i++)
                        for (int c = 0; c < 4; c++) sum[c] += (*px)[(((size_t)f * *h + (size_t)(y * k + j)) * *w + (x * k + i)) * 4 + c];
                for (int c = 0; c < 4; c++) o[(((size_t)f * nh + y) * nw + x) * 4 + c] = (unsigned char)(sum[c] / (k * k));
            }
    free(*px); *px = o; *w = nw; *h = nh;
}

int bg_load(const char *path)
{
    clear();
    if (!path || !path[0]) return 0;
    long n;
    unsigned char *buf = read_all(path, &n);
    if (!buf) return -1;
    int w = 0, h = 0, z = 1, c;
    unsigned char *px = NULL;
    int *dl = NULL;
    if (n > 6 && !memcmp(buf, "GIF8", 4)) {
        px = stbi_load_gif_from_memory(buf, (int)n, &dl, &w, &h, &z, &c, 4);
        if (px && (double)w * h * z * 4 > 400e6) {            /* too much memory: keep every other frame */
            int keep = (int)(400e6 / ((double)w * h * 4));
            if (keep < 1) keep = 1;
            if (keep < z) {
                int step = (z + keep - 1) / keep, nk = 0;
                for (int f = 0; f < z; f += step) {
                    if (nk != f) memmove(px + (size_t)nk * w * h * 4, px + (size_t)f * w * h * 4, (size_t)w * h * 4);
                    int d = 0;
                    for (int j = f; j < f + step && j < z; j++) d += dl[j];
                    dl[nk++] = d;
                }
                z = nk;
            }
        }
    } else {
        px = stbi_load_from_memory(buf, (int)n, &w, &h, &c, 4);
        z = 1;
    }
    free(buf);
    if (!px) return -1;
    if (z < 1) z = 1;
    stack = (unsigned char *)malloc((size_t)w * h * 4 * z);
    if (!stack) { stbi_image_free(px); if (dl) stbi_image_free(dl); return -1; }
    memcpy(stack, px, (size_t)w * h * 4 * z);
    stbi_image_free(px);
    delays = (int *)calloc((size_t)z, sizeof(int));
    for (int i = 0; i < z; i++) delays[i] = (dl && dl[i] > 0) ? dl[i] : 100;
    if (dl) stbi_image_free(dl);
    bw = w; bh = h; nframes = z;
    shrink(&stack, &bw, &bh, z);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, bw, bh);
    if (!tex) { clear(); return -1; }
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
    SDL_UpdateTexture(tex, NULL, stack, bw * 4);
    return 0;
}

void bg_update(float dt)
{
    if (!tex || nframes < 2) return;
    acc += dt * 1000.0f;
    int moved = 0;
    while (acc >= delays[cur]) {
        acc -= (float)delays[cur];
        cur = (cur + 1) % nframes;
        moved = 1;
        if (acc > 5000) acc = 0;
    }
    if (moved) SDL_UpdateTexture(tex, NULL, stack + (size_t)cur * bw * bh * 4, bw * 4);
}

SDL_Texture *bg_texture(int *w, int *h)
{
    if (!tex) return NULL;
    if (w) *w = bw;
    if (h) *h = bh;
    return tex;
}

int bg_frames(void) { return nframes; }
