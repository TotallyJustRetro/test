#include "decode.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#include "dr_wav.h"
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#include "dr_flac.h"
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#include "stb_vorbis.inc"

#define MAX_SECONDS 900

static unsigned char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (200L << 20)) { fclose(f); return NULL; }
    unsigned char *b = (unsigned char *)malloc((size_t)sz);
    if (!b) { fclose(f); return NULL; }
    *n = fread(b, 1, (size_t)sz, f);
    fclose(f);
    return b;
}

static float *to_stereo(const float *src, long frames, int ch)
{
    float *o = (float *)malloc((size_t)frames * 2 * sizeof(float));
    if (!o) return NULL;
    for (long i = 0; i < frames; i++) {
        if (ch == 1) { o[i * 2] = o[i * 2 + 1] = src[i]; }
        else { o[i * 2] = src[i * ch]; o[i * 2 + 1] = src[i * ch + 1]; }
    }
    return o;
}

static int ext_is(const char *p, const char *e)
{
    size_t lp = strlen(p), le = strlen(e);
    if (lp < le) return 0;
    for (size_t i = 0; i < le; i++) {
        char a = p[lp - le + i], b = e[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b) return 0;
    }
    return 1;
}

float *audio_decode_file(const char *path, long *frames, int *rate)
{
    size_t n = 0;
    unsigned char *buf = slurp(path, &n);
    if (!buf) return NULL;
    float *out = NULL;
    *frames = 0;
    if (ext_is(path, ".mp3")) {
        drmp3_config cfg; drmp3_uint64 fr = 0;
        float *d = drmp3_open_memory_and_read_pcm_frames_f32(buf, n, &cfg, &fr, NULL);
        if (d) { if (fr && (long)fr < (long)MAX_SECONDS * (long)cfg.sampleRate) { out = to_stereo(d, (long)fr, (int)cfg.channels); *frames = (long)fr; *rate = (int)cfg.sampleRate; } drmp3_free(d, NULL); }
    } else if (ext_is(path, ".flac")) {
        unsigned ch, sr; drflac_uint64 fr = 0;
        float *d = drflac_open_memory_and_read_pcm_frames_f32(buf, n, &ch, &sr, &fr, NULL);
        if (d) { if (fr && (long)fr < (long)MAX_SECONDS * (long)sr) { out = to_stereo(d, (long)fr, (int)ch); *frames = (long)fr; *rate = (int)sr; } drflac_free(d, NULL); }
    } else if (ext_is(path, ".ogg") || ext_is(path, ".oga")) {
        int ch, sr; short *d = NULL;
        int fr = stb_vorbis_decode_memory(buf, (int)n, &ch, &sr, &d);
        if (fr > 0 && d) {
            if ((long)fr < (long)MAX_SECONDS * sr) {
                float *tmp = (float *)malloc((size_t)fr * ch * sizeof(float));
                if (tmp) { for (long i = 0; i < (long)fr * ch; i++) tmp[i] = d[i] / 32768.0f; out = to_stereo(tmp, fr, ch); free(tmp); *frames = fr; *rate = sr; }
            }
            free(d);
        }
    } else {
        unsigned ch, sr; drwav_uint64 fr = 0;
        float *d = drwav_open_memory_and_read_pcm_frames_f32(buf, n, &ch, &sr, &fr, NULL);
        if (d) { if (fr && (long)fr < (long)MAX_SECONDS * (long)sr) { out = to_stereo(d, (long)fr, (int)ch); *frames = (long)fr; *rate = (int)sr; } drwav_free(d, NULL); }
    }
    free(buf);
    return out;
}
