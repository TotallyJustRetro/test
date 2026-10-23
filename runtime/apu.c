/* DMG APU: 2 square channels (ch1 with sweep), wave, noise; 512 Hz frame
 * sequencer; stereo mix to 16-bit PCM at a configurable rate. */
#include "gb.h"

typedef struct {
    int enabled, dac;
    int length, length_en;
    int vol, env_init, env_dir, env_period, env_timer;
    int freq, timer, duty, duty_pos;
    /* sweep (ch1) */
    int sw_period, sw_dir, sw_shift, sw_timer, sw_shadow, sw_enabled, sw_negated;
    /* noise */
    int lfsr, nz_shift, nz_width, nz_div;
    /* wave */
    int wave_pos, wave_shift;
} Chan;

static Chan ch[4];
static uint8_t regs[0x30];        /* raw register shadow, index = addr - 0xFF10 */
static uint8_t wave_ram[16];
static int power, fs_cycles, fs_step;
static uint8_t nr50, nr51;

static const uint8_t duty_tab[4][8] = {
    {0, 0, 0, 0, 0, 0, 0, 1}, {1, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 1, 1, 1}, {0, 1, 1, 1, 1, 1, 1, 0}};
static const int noise_div[8] = {8, 16, 32, 48, 64, 80, 96, 112};

/* output ring (drained by the front end once per video frame) */
#define RING 16384
static int16_t ring[RING * 2];
static int ring_r, ring_w;
static double period_steps = (double)CPU_HZ / 4.0 / 48000.0;   /* 4-cycle steps per output sample */
static double step_t;
static float acc_l, acc_r, dc_l, dc_r, prev_l, prev_r;
static int acc_n;
static float master_vol = 1.0f;

/* second output: only the channels in tap_mask (used to play single sound effects on the controller speaker) */
#define TRING 8192
static int16_t tring[TRING * 2];
static int tring_r, tring_w, tap_mask;
static float tacc_l, tacc_r, tdc_l, tdc_r, tprev_l, tprev_r;
static void (*trigger_cb)(int ch, const uint8_t *regs);

void apu_set_tap_mask(int m) { tap_mask = m & 15; if (!tap_mask) tring_r = tring_w = 0; }
void apu_set_trigger_hook(void (*cb)(int ch, const uint8_t *regs)) { trigger_cb = cb; }
int apu_drain_tap(int16_t *dst, int max_frames)
{
    int n = 0;
    while (tring_r != tring_w && n < max_frames) {
        dst[n * 2] = tring[tring_r * 2]; dst[n * 2 + 1] = tring[tring_r * 2 + 1];
        tring_r = (tring_r + 1) % TRING; n++;
    }
    return n;
}
int apu_channel_active(int c) { return ch[c].enabled && ch[c].dac; }

void apu_set_rate(int hz) { period_steps = (double)CPU_HZ / 4.0 / hz; }
void apu_set_volume(float v) { master_vol = v < 0 ? 0 : (v > 2 ? 2 : v); }

void apu_reset(void)
{
    memset(ch, 0, sizeof ch);
    memset(regs, 0, sizeof regs);
    static const uint8_t wave_init[16] = {0x84, 0x40, 0x43, 0xAA, 0x2D, 0x78, 0x92, 0x3C,
                                          0x60, 0x59, 0x59, 0xB0, 0x34, 0xB8, 0x2E, 0xDA};
    memcpy(wave_ram, wave_init, 16);
    ch[3].lfsr = 0x7FFF;
    power = 1; fs_cycles = 0; fs_step = 0; nr50 = 0x77; nr51 = 0xF3;
    ring_r = ring_w = 0; step_t = 0; acc_l = acc_r = dc_l = dc_r = prev_l = prev_r = 0; acc_n = 0;
    tring_r = tring_w = 0; tacc_l = tacc_r = tdc_l = tdc_r = tprev_l = tprev_r = 0;
    regs[0x01] = 0x80; /* NR11 */ regs[0x02] = 0xF3; /* NR12 */
    ch[0].duty = 2; ch[0].dac = 1; ch[0].env_init = 15; ch[0].env_period = 3;
}

static int sweep_calc(Chan *c)
{
    int d = c->sw_shadow >> c->sw_shift;
    int n = c->sw_dir ? c->sw_shadow - d : c->sw_shadow + d;
    if (c->sw_dir) c->sw_negated = 1;
    if (n > 2047) c->enabled = 0;
    return n;
}

/* Enabling the length counter during the first half of a length period clocks it once. */
static void len_enable(int i, int en)
{
    Chan *c = &ch[i];
    if (!c->length_en && en && c->length > 0 && (fs_step & 1)) {
        if (--c->length == 0) c->enabled = 0;
    }
    c->length_en = en;
}

static void trigger(int i)
{
    Chan *c = &ch[i];
    c->enabled = c->dac;
    if (trigger_cb) trigger_cb(i, &regs[i * 5]);
    if (c->length == 0) {
        c->length = (i == 2) ? 256 : 64;
        if (c->length_en && (fs_step & 1)) c->length--;
    }
    if (i == 0 || i == 1) c->timer = (2048 - c->freq) * 4;
    if (i == 2) { c->timer = (2048 - c->freq) * 2; c->wave_pos = 0; }
    if (i == 3) { c->timer = noise_div[c->nz_div] << c->nz_shift; c->lfsr = 0x7FFF; }
    if (i != 2) { c->vol = c->env_init; c->env_timer = c->env_period ? c->env_period : 8; }
    if (i == 0) {
        c->sw_negated = 0;
        c->sw_shadow = c->freq;
        c->sw_timer = c->sw_period ? c->sw_period : 8;
        c->sw_enabled = c->sw_period || c->sw_shift;
        if (c->sw_shift) sweep_calc(c);
    }
}

static void clock_length(void)
{
    for (int i = 0; i < 4; i++)
        if (ch[i].length_en && ch[i].length > 0 && --ch[i].length == 0) ch[i].enabled = 0;
}
static void clock_env(void)
{
    for (int i = 0; i < 4; i++) {
        if (i == 2) continue;
        Chan *c = &ch[i];
        if (c->env_period && c->enabled && --c->env_timer <= 0) {
            c->env_timer = c->env_period;
            if (c->env_dir && c->vol < 15) c->vol++;
            else if (!c->env_dir && c->vol > 0) c->vol--;
        }
    }
}
static void clock_sweep(void)
{
    Chan *c = &ch[0];
    if (--c->sw_timer <= 0) {
        c->sw_timer = c->sw_period ? c->sw_period : 8;
        if (c->sw_enabled && c->sw_period) {
            int n = sweep_calc(c);
            if (n <= 2047 && c->sw_shift) {
                c->sw_shadow = c->freq = n;
                sweep_calc(c);
            }
        }
    }
}

/* Current 4-bit DAC level of a channel (0 when silent). */
static inline int chan_out(int i)
{
    Chan *c = &ch[i];
    if (!c->enabled) return 0;
    switch (i) {
    case 0: case 1: return duty_tab[c->duty][c->duty_pos] ? c->vol : 0;
    case 2: {
        int v = wave_ram[c->wave_pos >> 1];
        v = (c->wave_pos & 1) ? (v & 0xF) : (v >> 4);
        return c->wave_shift ? v >> (c->wave_shift - 1) : 0;
    }
    default: return (c->lfsr & 1) ? 0 : c->vol;
    }
}

static void emit_sample(void)
{
    int acc_n_prev = acc_n;
    float l = acc_l / (float)acc_n, r = acc_r / (float)acc_n;
    acc_l = acc_r = 0; acc_n = 0;
    /* DC blocker (the capacitor on real hardware) removes the volume-dependent
     * baseline of each channel, so notes starting/stopping don't click. */
    float hl = l - prev_l + 0.996f * dc_l; prev_l = l; dc_l = hl;
    float hr = r - prev_r + 0.996f * dc_r; prev_r = r; dc_r = hr;
    hl *= master_vol; hr *= master_vol;
    if (hl > 1.0f) hl = 1.0f; else if (hl < -1.0f) hl = -1.0f;
    if (hr > 1.0f) hr = 1.0f; else if (hr < -1.0f) hr = -1.0f;
    int nw = (ring_w + 1) % RING;
    if (nw != ring_r) {                             /* overflow: drop */
        ring[ring_w * 2] = (int16_t)(hl * 30000);
        ring[ring_w * 2 + 1] = (int16_t)(hr * 30000);
        ring_w = nw;
    }
    if (tap_mask) {
        float tl = tacc_l / (float)acc_n_prev, tr = tacc_r / (float)acc_n_prev;
        float a = tl - tprev_l + 0.996f * tdc_l; tprev_l = tl; tdc_l = a;
        float b = tr - tprev_r + 0.996f * tdc_r; tprev_r = tr; tdc_r = b;
        a *= master_vol; b *= master_vol;
        if (a > 1.0f) a = 1.0f; else if (a < -1.0f) a = -1.0f;
        if (b > 1.0f) b = 1.0f; else if (b < -1.0f) b = -1.0f;
        int tn = (tring_w + 1) % TRING;
        if (tn != tring_r) {
            tring[tring_w * 2] = (int16_t)(a * 30000);
            tring[tring_w * 2 + 1] = (int16_t)(b * 30000);
            tring_w = tn;
        }
    }
    tacc_l = tacc_r = 0;
}

static inline void apu_step4(void)
{
    /* frame sequencer */
    fs_cycles += 4;
    if (fs_cycles >= 8192) {
        fs_cycles -= 8192;
        if ((fs_step & 1) == 0) clock_length();
        if (fs_step == 2 || fs_step == 6) clock_sweep();
        if (fs_step == 7) clock_env();
        fs_step = (fs_step + 1) & 7;
    }
    float l = 0, r = 0, tl = 0, tr = 0;
    for (int i = 0; i < 4; i++) {
        Chan *c = &ch[i];
        if (c->enabled) {
            c->timer -= 4;
            while (c->timer <= 0) {
                if (i < 2) { c->timer += (2048 - c->freq) * 4; c->duty_pos = (c->duty_pos + 1) & 7; }
                else if (i == 2) { c->timer += (2048 - c->freq) * 2; c->wave_pos = (c->wave_pos + 1) & 31; }
                else {
                    c->timer += noise_div[c->nz_div] << c->nz_shift;
                    int x = (c->lfsr ^ (c->lfsr >> 1)) & 1;
                    c->lfsr = (c->lfsr >> 1) | (x << 14);
                    if (c->nz_width) c->lfsr = (c->lfsr & ~0x40) | (x << 6);
                }
            }
        }
        float o = chan_out(i) * (1.0f / 15.0f);
        if (nr51 & (0x10 << i)) l += o;
        if (nr51 & (1 << i)) r += o;
        if (tap_mask & (1 << i)) {
            if (nr51 & (0x10 << i)) tl += o;
            if (nr51 & (1 << i)) tr += o;
        }
    }
    float gl = (((nr50 >> 4) & 7) + 1) * (1.0f / 8.0f) * 0.33f, gr = ((nr50 & 7) + 1) * (1.0f / 8.0f) * 0.33f;
    acc_l += l * gl;
    acc_r += r * gr;
    if (tap_mask) { tacc_l += tl * gl; tacc_r += tr * gr; }
    acc_n++;
    step_t += 1.0;
    if (step_t >= period_steps) { step_t -= period_steps; emit_sample(); }
}

static int sub_cycles;

void apu_tick(int n)
{
    sub_cycles += n;
    while (sub_cycles >= 4) {
        sub_cycles -= 4;
        if (power) apu_step4();
        else {          /* powered off: silence, but keep the sample clock running */
            acc_n++;
            step_t += 1.0;
            if (step_t >= period_steps) { step_t -= period_steps; emit_sample(); }
        }
    }
}

int apu_drain(int16_t *dst, int max_frames)
{
    int n = 0;
    while (ring_r != ring_w && n < max_frames) {
        dst[n * 2] = ring[ring_r * 2];
        dst[n * 2 + 1] = ring[ring_r * 2 + 1];
        ring_r = (ring_r + 1) % RING;
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
uint8_t apu_read(uint16_t a)
{
    if (a >= 0xFF30) return wave_ram[a - 0xFF30];
    static const uint8_t mask[0x17] = {0x80, 0x3F, 0x00, 0xFF, 0xBF, 0xFF, 0x3F, 0x00, 0xFF, 0xBF,
                                       0x7F, 0xFF, 0x9F, 0xFF, 0xBF, 0xFF, 0xFF, 0x00, 0x00, 0xBF,
                                       0x00, 0x00, 0x70};
    int i = a - 0xFF10;
    if (i >= 0x17) return 0xFF;
    if (a == 0xFF26)
        return 0x70 | (power ? 0x80 : 0) | ch[0].enabled | (ch[1].enabled << 1) | (ch[2].enabled << 2) | (ch[3].enabled << 3);
    return regs[i] | mask[i];
}

void apu_write(uint16_t a, uint8_t v)
{
    if (a >= 0xFF30) { wave_ram[a - 0xFF30] = v; return; }
    if (a == 0xFF26) {
        int on = (v & 0x80) != 0;
        if (power && !on) {
            /* DMG: everything clears except the length counters (and NRx1 length writes keep working) */
            uint8_t keep[4] = {regs[0x01] & 0x3F, regs[0x06] & 0x3F, regs[0x0B], regs[0x10]};
            int len[4] = {ch[0].length, ch[1].length, ch[2].length, ch[3].length};
            memset(regs, 0, sizeof regs);
            for (int i = 0; i < 4; i++) { memset(&ch[i], 0, sizeof ch[i]); ch[i].length = len[i]; }
            ch[3].lfsr = 0x7FFF;
            regs[0x01] = keep[0]; regs[0x06] = keep[1]; regs[0x0B] = keep[2]; regs[0x10] = keep[3];
            nr50 = nr51 = 0;
        } else if (!power && on) { fs_step = 0; fs_cycles = 0; }
        power = on;
        return;
    }
    if (!power) {
        /* DMG: length registers stay writable while powered off */
        switch (a) {
        case 0xFF11: ch[0].length = 64 - (v & 63); regs[0x01] = v & 0x3F; break;
        case 0xFF16: ch[1].length = 64 - (v & 63); regs[0x06] = v & 0x3F; break;
        case 0xFF1B: ch[2].length = 256 - v; regs[0x0B] = v; break;
        case 0xFF20: ch[3].length = 64 - (v & 63); regs[0x10] = v; break;
        }
        return;
    }
    int i = a - 0xFF10;
    if (i < 0 || i >= 0x17) return;
    regs[i] = v;
    switch (a) {
    case 0xFF10: if (ch[0].sw_dir && !((v >> 3) & 1) && ch[0].sw_negated) ch[0].enabled = 0;
        ch[0].sw_period = (v >> 4) & 7; ch[0].sw_dir = (v >> 3) & 1; ch[0].sw_shift = v & 7; break;
    case 0xFF11: ch[0].duty = v >> 6; ch[0].length = 64 - (v & 63); break;
    case 0xFF12: ch[0].env_init = v >> 4; ch[0].env_dir = (v >> 3) & 1; ch[0].env_period = v & 7; ch[0].dac = (v & 0xF8) != 0; if (!ch[0].dac) ch[0].enabled = 0; break;
    case 0xFF13: ch[0].freq = (ch[0].freq & 0x700) | v; break;
    case 0xFF14: ch[0].freq = (ch[0].freq & 0xFF) | ((v & 7) << 8); len_enable(0, (v >> 6) & 1); if (v & 0x80) trigger(0); break;
    case 0xFF16: ch[1].duty = v >> 6; ch[1].length = 64 - (v & 63); break;
    case 0xFF17: ch[1].env_init = v >> 4; ch[1].env_dir = (v >> 3) & 1; ch[1].env_period = v & 7; ch[1].dac = (v & 0xF8) != 0; if (!ch[1].dac) ch[1].enabled = 0; break;
    case 0xFF18: ch[1].freq = (ch[1].freq & 0x700) | v; break;
    case 0xFF19: ch[1].freq = (ch[1].freq & 0xFF) | ((v & 7) << 8); len_enable(1, (v >> 6) & 1); if (v & 0x80) trigger(1); break;
    case 0xFF1A: ch[2].dac = (v & 0x80) != 0; if (!ch[2].dac) ch[2].enabled = 0; break;
    case 0xFF1B: ch[2].length = 256 - v; break;
    case 0xFF1C: ch[2].wave_shift = (v >> 5) & 3; break;
    case 0xFF1D: ch[2].freq = (ch[2].freq & 0x700) | v; break;
    case 0xFF1E: ch[2].freq = (ch[2].freq & 0xFF) | ((v & 7) << 8); len_enable(2, (v >> 6) & 1); if (v & 0x80) trigger(2); break;
    case 0xFF20: ch[3].length = 64 - (v & 63); break;
    case 0xFF21: ch[3].env_init = v >> 4; ch[3].env_dir = (v >> 3) & 1; ch[3].env_period = v & 7; ch[3].dac = (v & 0xF8) != 0; if (!ch[3].dac) ch[3].enabled = 0; break;
    case 0xFF22: ch[3].nz_shift = v >> 4; ch[3].nz_width = (v >> 3) & 1; ch[3].nz_div = v & 7; break;
    case 0xFF23: len_enable(3, (v >> 6) & 1); if (v & 0x80) trigger(3); break;
    case 0xFF24: nr50 = v; break;
    case 0xFF25: nr51 = v; break;
    }
}
