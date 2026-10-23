/* Audio output: one SDL device mixing the game, menu music and UI sounds.
 * The emulation thread produces game audio and is paced by how fast this device consumes it,
 * so the audio clock is the master clock (no drift, no pitch changes). */
#include <SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "audio.h"
#include "decode.h"
#include "settings.h"
#include "gb.h"

#define GR 32768                       /* game ring size in frames (power of two) */

static SDL_AudioDeviceID dev;
static int rate = 48000, dev_samples = 1024;
static SDL_mutex *mx;
static SDL_cond *cv;

/* game ring */
static int16_t gring[GR * 2];
static unsigned g_r, g_w;
static int g_active, g_playing, g_paused, g_abort;
static float g_last_l, g_last_r, g_ramp;
static unsigned underruns;
static int g_target = 2048;

/* menu music + sfx */
typedef struct { float *d; long n; } Clip;
static Clip music, sfx[N_UI_SFX];
static char music_path[512], sfx_path[N_UI_SFX][512];
static long music_pos;
static float music_gain, music_target;
static int music_want_on;
typedef struct { int clip; long pos; float vol; } Voice;
static Voice voices[8];
static float sfx_vol = 0.7f;

/* controller speaker */
static SDL_AudioDeviceID pdev;
static SDL_mutex *pmx;
static int pch, prate;
static int16_t pring[8192 * 2];
static unsigned p_r, p_w;
static volatile float p_spk = 0.5f, p_hap = 0.5f;
static float p_lp_l, p_lp_r;
static Clip beep;
static long beep_pos = -1, beep_end;

/* ------------------------------------------------------------ synthesis */
static float *clip_alloc(Clip *c, long n)
{
    free(c->d);
    c->d = (float *)calloc((size_t)n * 2, sizeof(float));
    c->n = c->d ? n : 0;
    return c->d;
}

static float osc_pulse(double ph, double duty) { return (ph - floor(ph)) < duty ? 1.0f : -1.0f; }
static float osc_tri(double ph) { double f = ph - floor(ph); return (float)(f < 0.5 ? 4 * f - 1 : 3 - 4 * f); }

static unsigned rng_state = 12345;
static float noise(void) { rng_state = rng_state * 1664525u + 1013904223u; return ((rng_state >> 9) & 0xFFFF) / 32768.0f - 1.0f; }

static double midi_hz(int m) { return 440.0 * pow(2.0, (m - 69) / 12.0); }

enum { W_PULSE25, W_PULSE50, W_PULSE125, W_TRI };

static void add_note(Clip *c, long start, double dur_s, int midi, int wave, float vol, float pan, double atk, double rel_s)
{
    long n = (long)(dur_s * rate), a = (long)(atk * rate), r = (long)(rel_s * rate);
    double f = midi_hz(midi), ph = 0, inc = f / rate;
    for (long i = 0; i < n + r; i++) {
        long p = start + i;
        if (p < 0 || p >= c->n) continue;
        float env = 1.0f;
        if (i < a) env = (float)i / (float)(a ? a : 1);
        if (i >= n) env *= 1.0f - (float)(i - n) / (float)(r ? r : 1);
        else if (wave != W_TRI) env *= 0.55f + 0.45f * (1.0f - (float)i / (float)n);   /* gentle decay */
        float s;
        switch (wave) {
        case W_PULSE25: s = osc_pulse(ph, 0.25); break;
        case W_PULSE50: s = osc_pulse(ph, 0.5); break;
        case W_PULSE125: s = osc_pulse(ph, 0.125); break;
        default: s = osc_tri(ph); break;
        }
        ph += inc;
        s *= env * vol;
        c->d[p * 2] += s * (1.0f - pan) * 0.5f * 2;
        c->d[p * 2 + 1] += s * (1.0f + pan) * 0.5f * 2;
    }
}

static void add_drum(Clip *c, long start, int kind, float vol)
{
    long n = (long)((kind == 0 ? 0.16 : (kind == 1 ? 0.14 : 0.04)) * rate);
    double ph = 0;
    for (long i = 0; i < n; i++) {
        long p = start + i;
        if (p >= c->n) break;
        float t = (float)i / (float)n, s;
        if (kind == 0) { double f = 110 * pow(0.3, t * 1.5) + 40; ph += f / rate; s = (float)sin(ph * 6.2831853) * (1 - t) * 1.2f; }
        else if (kind == 1) s = noise() * (1 - t) * (1 - t) * 0.8f + (float)sin(i * 0.12) * (1 - t) * 0.3f;
        else s = noise() * (1 - t) * (1 - t) * 0.4f;
        c->d[p * 2] += s * vol; c->d[p * 2 + 1] += s * vol;
    }
}

static void synth_music(Clip *c)
{
    const double bpm = 112.0;
    double eighth = 60.0 / bpm / 2.0;
    int bars = 16;
    long total = (long)(bars * 8 * eighth * rate);
    if (!clip_alloc(c, total)) return;
    static const int root[4] = {48, 43, 45, 41};                    /* C G Am F */
    static const int chord[4][3] = {{0, 4, 7}, {0, 4, 7}, {0, 3, 7}, {0, 4, 7}};
    static const int mel[4][8] = {{76, 0, 79, 0, 81, 79, 76, 0}, {74, 0, 79, 0, 83, 81, 79, 0},
                                  {72, 0, 76, 0, 81, 79, 76, 0}, {77, 0, 81, 0, 84, 83, 81, 79}};
    for (int bar = 0; bar < bars; bar++) {
        int ch = bar & 3, sec = bar / 4;
        int r0 = root[ch];
        for (int e = 0; e < 8; e++) {
            long t = (long)((bar * 8 + e) * eighth * rate);
            /* bass: root on beats, fifth on offbeats */
            int bn = (e % 2 == 0) ? r0 : r0 + 7;
            add_note(c, t, eighth * 0.9, bn - (e % 2 == 0 ? 0 : 0), W_TRI, 0.55f, 0.0f, 0.004, 0.03);
            /* soft arpeggio pad */
            int ai = e % 4;
            int an = r0 + 24 + chord[ch][ai == 3 ? 1 : ai % 3] + (ai == 2 ? 12 : 0);
            add_note(c, t, eighth * 0.85, an, W_PULSE125, 0.12f, (e & 1) ? 0.35f : -0.35f, 0.003, 0.05);
            if (sec >= 1) {
                if (e % 2 == 0) add_drum(c, t, (e % 4 == 0) ? (((bar & 1) && e == 4) ? 1 : 0) : 1, 0.45f);
                add_drum(c, t, 2, 0.25f);
            }
            if (sec == 1) {            /* arpeggio lead */
                int ln = r0 + 36 + chord[ch][(e * 2) % 3] + (e >= 4 ? 12 : 0);
                add_note(c, t, eighth * 0.8, ln, W_PULSE25, 0.2f, 0.0f, 0.003, 0.05);
            } else if (sec >= 2) {
                int m = mel[ch][e];
                if (m) {
                    int dur = 1;
                    while (e + dur < 8 && mel[ch][e + dur] == 0 && dur < 2) dur++;
                    add_note(c, t, eighth * dur * 0.95, m + (sec == 3 ? 0 : 0), W_PULSE25, 0.24f, 0.0f, 0.004, 0.06);
                    if (sec == 3) add_note(c, t, eighth * dur * 0.9, m - 12, W_PULSE50, 0.08f, 0.0f, 0.004, 0.05);
                }
            }
        }
    }
    float pk = 0;
    for (long i = 0; i < total * 2; i++) if (fabsf(c->d[i]) > pk) pk = fabsf(c->d[i]);
    float g = pk > 0 ? 0.7f / pk : 1;
    for (long i = 0; i < total * 2; i++) c->d[i] *= g;
}

static void synth_sfx(Clip *c, int which)
{
    /* each sound: list of (midi, ms, wave) steps */
    typedef struct { int midi; int ms; int wave; } Step;
    static const Step hov[] = {{96, 22, W_PULSE50}, {0, 0, 0}};
    static const Step clk[] = {{79, 35, W_PULSE50}, {86, 55, W_PULSE50}, {0, 0, 0}};
    static const Step cnf[] = {{72, 70, W_PULSE25}, {76, 70, W_PULSE25}, {79, 70, W_PULSE25}, {84, 190, W_PULSE25}, {0, 0, 0}};
    static const Step bck[] = {{81, 60, W_PULSE50}, {74, 130, W_PULSE50}, {0, 0, 0}};
    static const Step tgl[] = {{84, 40, W_PULSE50}, {91, 60, W_PULSE50}, {0, 0, 0}};
    const Step *st[N_UI_SFX] = {hov, clk, cnf, bck, tgl};
    long total = 0;
    for (const Step *s = st[which]; s->ms; s++) total += s->ms * rate / 1000;
    total += rate / 10;
    if (!clip_alloc(c, total)) return;
    long pos = 0;
    for (const Step *s = st[which]; s->ms; s++) {
        add_note(c, pos, s->ms / 1000.0 * 0.95, s->midi, s->wave, which == 0 ? 0.25f : 0.4f, 0, 0.002, 0.03);
        pos += s->ms * rate / 1000;
    }
}

static void resample_to(float **d, long *n, int from)
{
    if (from == rate || !*d) return;
    long nn = (long)((double)*n * rate / from);
    float *o = (float *)malloc((size_t)nn * 2 * sizeof(float));
    if (!o) return;
    for (long i = 0; i < nn; i++) {
        double p = (double)i * from / rate;
        long a = (long)p;
        float f = (float)(p - a);
        long b = a + 1 < *n ? a + 1 : a;
        o[i * 2] = (*d)[a * 2] * (1 - f) + (*d)[b * 2] * f;
        o[i * 2 + 1] = (*d)[a * 2 + 1] * (1 - f) + (*d)[b * 2 + 1] * f;
    }
    free(*d);
    *d = o; *n = nn;
}

static int load_clip(Clip *c, const char *path)
{
    long n; int sr = rate;
    float *d = audio_decode_file(path, &n, &sr);
    if (!d) return -1;
    resample_to(&d, &n, sr);
    free(c->d);
    c->d = d; c->n = n;
    return 0;
}

/* ------------------------------------------------------------ device */
static void audio_cb(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    float *out = (float *)stream;
    int frames = len / 8;
    SDL_LockMutex(mx);
    for (int i = 0; i < frames; i++) {
        float l = 0, r = 0;
        if (g_active && !g_paused) {
            unsigned fill = g_w - g_r;
            if (!g_playing) {
                if ((int)fill >= g_target) { g_playing = 1; g_ramp = 0; }
            }
            if (g_playing) {
                if (fill > 0) {
                    unsigned idx = g_r & (GR - 1);
                    g_last_l = gring[idx * 2] * (1.0f / 32768.0f);
                    g_last_r = gring[idx * 2 + 1] * (1.0f / 32768.0f);
                    g_r++;
                    if (g_ramp < 1.0f) { g_ramp += 1.0f / 96.0f; if (g_ramp > 1) g_ramp = 1; }
                    l = g_last_l * g_ramp; r = g_last_r * g_ramp;
                } else {                         /* ran dry: decay quietly, then rebuffer */
                    underruns++;
                    g_playing = 0;
                    g_last_l *= 0.5f; g_last_r *= 0.5f;
                    l = g_last_l; r = g_last_r;
                }
            } else {
                g_last_l *= 0.9f; g_last_r *= 0.9f;
                l = g_last_l; r = g_last_r;
            }
        }
        if (music.d && (music_gain > 0.0005f || music_target > 0)) {
            music_gain += (music_target - music_gain) * 0.0004f;
            l += music.d[music_pos * 2] * music_gain;
            r += music.d[music_pos * 2 + 1] * music_gain;
            if (++music_pos >= music.n) music_pos = 0;
        }
        for (int v = 0; v < 8; v++) {
            if (voices[v].clip < 0) continue;
            Clip *c = &sfx[voices[v].clip];
            if (voices[v].pos >= c->n) { voices[v].clip = -1; continue; }
            l += c->d[voices[v].pos * 2] * voices[v].vol;
            r += c->d[voices[v].pos * 2 + 1] * voices[v].vol;
            voices[v].pos++;
        }
        if (l > 1) l = 1; else if (l < -1) l = -1;
        if (r > 1) r = 1; else if (r < -1) r = -1;
        out[i * 2] = l; out[i * 2 + 1] = r;
    }
    SDL_UnlockMutex(mx);
    SDL_CondSignal(cv);
}

int audio_ok(void) { return dev != 0; }
int audio_rate(void) { return rate; }

int audio_init(int latency)
{
    static const int samples[N_LAT] = {512, 1024, 2048};
    if (dev) return 0;
    SDL_SetHint(SDL_HINT_AUDIO_RESAMPLING_MODE, "3");
    mx = SDL_CreateMutex(); cv = SDL_CreateCond(); pmx = SDL_CreateMutex();
    for (int i = 0; i < 8; i++) voices[i].clip = -1;
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = 48000; want.format = AUDIO_F32SYS; want.channels = 2;
    want.samples = (Uint16)samples[latency < 0 || latency >= N_LAT ? 1 : latency];
    want.callback = audio_cb;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
    if (!dev) { fprintf(stderr, "audio disabled: %s\n", SDL_GetError()); return -1; }
    rate = have.freq; dev_samples = have.samples;
    apu_set_rate(rate);
    static const int tgt_ms[N_LAT] = {45, 70, 120};
    g_target = rate * tgt_ms[latency < 0 || latency >= N_LAT ? 1 : latency] / 1000;
    synth_music(&music);
    for (int i = 0; i < N_UI_SFX; i++) synth_sfx(&sfx[i], i);
    SDL_PauseAudioDevice(dev, 0);
    return 0;
}

void audio_shutdown(void)
{
    audio_pad_close();
    if (dev) { SDL_CloseAudioDevice(dev); dev = 0; }
}

int audio_game_target(void) { return g_target; }

void audio_game_begin(void)
{
    if (!dev) return;
    SDL_LockMutex(mx);
    g_r = g_w = 0; g_active = 1; g_playing = 0; g_abort = 0; g_paused = 0; underruns = 0;
    g_last_l = g_last_r = 0;
    music_target = 0;
    SDL_UnlockMutex(mx);
}

void audio_game_end(void)
{
    if (!dev) return;
    SDL_LockMutex(mx);
    g_active = 0; g_playing = 0; g_r = g_w = 0; g_abort = 1;
    SDL_UnlockMutex(mx);
    SDL_CondBroadcast(cv);
}

void audio_game_set_paused(int p) { if (dev) { SDL_LockMutex(mx); g_paused = p; SDL_UnlockMutex(mx); } }

void audio_game_push(const int16_t *st, int frames)
{
    if (!dev) return;
    SDL_LockMutex(mx);
    for (int i = 0; i < frames; i++) {
        if (g_w - g_r >= GR) break;          /* full: drop (can only happen when not paced) */
        unsigned idx = g_w & (GR - 1);
        gring[idx * 2] = st[i * 2]; gring[idx * 2 + 1] = st[i * 2 + 1];
        g_w++;
    }
    SDL_UnlockMutex(mx);
}

void audio_game_wait(int target)
{
    if (!dev) return;
    SDL_LockMutex(mx);
    while (!g_abort && g_active && (int)(g_w - g_r) > target) {
        if (SDL_CondWaitTimeout(cv, mx, 20) == SDL_MUTEX_TIMEDOUT && g_paused) break;
    }
    SDL_UnlockMutex(mx);
}

void audio_game_abort(void)
{
    if (!dev) return;
    SDL_LockMutex(mx); g_abort = 1; SDL_UnlockMutex(mx);
    SDL_CondBroadcast(cv);
}

void audio_get_stats(AudioStats *s)
{
    memset(s, 0, sizeof *s);
    if (!dev) return;
    SDL_LockMutex(mx);
    s->underruns = underruns; s->fill_frames = (int)(g_w - g_r); s->target_frames = g_target;
    s->playing = g_playing; s->device_rate = rate; s->device_samples = dev_samples;
    SDL_UnlockMutex(mx);
}

/* ---- menu ---- */
void audio_menu_apply(void)
{
    if (!dev) return;
    SDL_LockMutex(mx);
    sfx_vol = settings.ui_sfx_vol / 100.0f;
    SDL_UnlockMutex(mx);
    /* music file */
    if (strcmp(music_path, settings.menu_music_path)) {
        Clip tmp = {0};
        if (settings.menu_music_path[0]) {
            if (load_clip(&tmp, settings.menu_music_path) != 0) { settings.menu_music_path[0] = 0; }
        }
        if (!tmp.d) {
            synth_music(&tmp);
            snprintf(music_path, sizeof music_path, "%s", "");
            settings.menu_music_path[0] = 0;
        } else snprintf(music_path, sizeof music_path, "%s", settings.menu_music_path);
        SDL_LockMutex(mx);
        Clip old = music; music = tmp; music_pos = 0;
        SDL_UnlockMutex(mx);
        free(old.d);
    }
    for (int i = 0; i < N_UI_SFX; i++) {
        if (strcmp(sfx_path[i], settings.ui_sfx_path[i])) {
            Clip tmp = {0};
            if (settings.ui_sfx_path[i][0] && load_clip(&tmp, settings.ui_sfx_path[i]) != 0) settings.ui_sfx_path[i][0] = 0;
            if (!tmp.d) { synth_sfx(&tmp, i); sfx_path[i][0] = 0; settings.ui_sfx_path[i][0] = 0; }
            else snprintf(sfx_path[i], sizeof sfx_path[i], "%s", settings.ui_sfx_path[i]);
            SDL_LockMutex(mx);
            Clip old = sfx[i]; sfx[i] = tmp;
            for (int v = 0; v < 8; v++) if (voices[v].clip == i) voices[v].clip = -1;
            SDL_UnlockMutex(mx);
            free(old.d);
        }
    }
    audio_menu_music(music_want_on);
}

void audio_menu_music(int on)
{
    if (!dev) return;
    music_want_on = on;
    SDL_LockMutex(mx);
    music_target = (on && settings.menu_music && !g_active) ? settings.menu_music_vol / 100.0f * 0.6f : 0.0f;
    SDL_UnlockMutex(mx);
}

static void play_sfx(int which)
{
    SDL_LockMutex(mx);
    for (int v = 0; v < 8; v++)
        if (voices[v].clip < 0) { voices[v].clip = which; voices[v].pos = 0; voices[v].vol = sfx_vol; break; }
    SDL_UnlockMutex(mx);
}

void audio_sfx(int which)
{
    if (!dev || !settings.ui_sfx || which < 0 || which >= N_UI_SFX) return;
    play_sfx(which);
}
void audio_sfx_preview(int which) { if (dev && which >= 0 && which < N_UI_SFX) play_sfx(which); }

void audio_music_preview(void)
{
    if (!dev) return;
    SDL_LockMutex(mx);
    music_pos = 0;
    music_target = settings.menu_music_vol / 100.0f * 0.6f;
    SDL_UnlockMutex(mx);
}

/* ------------------------------------------------------------ controller speaker */
static void pad_cb(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    int16_t *o = (int16_t *)stream;
    int frames = len / (2 * pch);
    float spk = p_spk, hap = p_hap;
    SDL_LockMutex(pmx);
    for (int i = 0; i < frames; i++) {
        float l = 0, r = 0;
        if (p_w != p_r) {
            unsigned idx = p_r & 8191;
            l = pring[idx * 2] / 32768.0f; r = pring[idx * 2 + 1] / 32768.0f;
            p_r++;
        }
        if (beep_pos >= 0 && beep_pos < beep.n && beep_pos < beep_end) {
            l += beep.d[beep_pos * 2]; r += beep.d[beep_pos * 2 + 1];
            beep_pos++;
        } else beep_pos = -1;
        p_lp_l += (l - p_lp_l) * 0.04f; p_lp_r += (r - p_lp_r) * 0.04f;      /* ~300 Hz low-pass for the actuators */
        float v[8] = {l * spk, r * spk, p_lp_l * hap * 2.0f, p_lp_r * hap * 2.0f, 0, 0, 0, 0};
        for (int c = 0; c < pch && c < 8; c++) {
            float x = v[c];
            if (x > 1) x = 1; else if (x < -1) x = -1;
            o[i * pch + c] = (int16_t)(x * 32767);
        }
    }
    SDL_UnlockMutex(pmx);
}

static int has_ci(const char *h, const char *n)
{
    size_t ln = strlen(n);
    for (; *h; h++) {
        size_t i = 0;
        while (i < ln && h[i] && (h[i] | 32) == (n[i] | 32)) i++;
        if (i == ln) return 1;
    }
    return 0;
}

int audio_pad_available(void) { return pdev != 0; }

int audio_pad_open(void)
{
    if (pdev) return pch;
    int n = SDL_GetNumAudioDevices(0);
    for (int i = 0; i < n; i++) {
        const char *nm = SDL_GetAudioDeviceName(i, 0);
        if (!nm || !(has_ci(nm, "DualSense") || has_ci(nm, "Wireless Controller"))) continue;
        SDL_AudioSpec want, have;
        SDL_zero(want);
        want.freq = 48000; want.format = AUDIO_S16SYS; want.channels = 4; want.samples = 1024; want.callback = pad_cb;
        pdev = SDL_OpenAudioDevice(nm, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
        if (!pdev) continue;
        pch = have.channels; prate = have.freq;
        if (!beep.d) {
            /* a little bank of synthesized sounds, one per kind, played back to back in one clip */
            clip_alloc(&beep, prate * 5 / 2);
            static const struct { float f1, f2; float t1; float len; float vol; } k[5] = {
                {880, 1320, 0.08f, 0.35f, 0.5f}, {988, 1319, 0.07f, 0.40f, 0.5f}, {330, 220, 0.05f, 0.30f, 0.6f},
                {392, 262, 0.15f, 0.55f, 0.6f}, {523, 784, 0.10f, 0.50f, 0.5f}};
            for (int b = 0; b < 5; b++)
                for (long j = 0; j < (long)(k[b].len * prate); j++) {
                    long o = b * (prate / 2) + j;
                    if (o >= beep.n) break;
                    float t = (float)j / prate, env = expf(-t * 7.0f);
                    float s2 = osc_pulse(t * (t < k[b].t1 ? k[b].f1 : k[b].f2), 0.5) * env * k[b].vol;
                    beep.d[o * 2] = beep.d[o * 2 + 1] = s2;
                }
        }
        SDL_PauseAudioDevice(pdev, 0);
        return pch;
    }
    return 0;
}

void audio_pad_close(void)
{
    if (pdev) { SDL_CloseAudioDevice(pdev); pdev = 0; }
    free(beep.d); beep.d = NULL; beep.n = 0;
}

void audio_pad_set_gain(float spk, float hap) { p_spk = spk; p_hap = hap; }

void audio_pad_push(const int16_t *st, int frames)
{
    if (!pdev) return;
    SDL_LockMutex(pmx);
    for (int i = 0; i < frames; i++) {
        if (p_w - p_r >= 8192) break;
        unsigned idx = p_w & 8191;
        pring[idx * 2] = st[i * 2]; pring[idx * 2 + 1] = st[i * 2 + 1];
        p_w++;
    }
    SDL_UnlockMutex(pmx);
}

void audio_pad_play_beep(int kind)
{
    if (!pdev) return;
    if (kind < 0 || kind > 4) kind = 0;
    SDL_LockMutex(pmx);
    beep_pos = (long)kind * (prate / 2);
    beep_end = beep_pos + (long)(prate * 0.45);
    SDL_UnlockMutex(pmx);
}
