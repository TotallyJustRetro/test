/* Memory map, timer, serial, joypad, DMA, interrupts. */
#include "sm83.h"
#include "cart.h"

CPU cpu;
uint8_t io_if, io_ie;
uint64_t total_cycles;

static uint8_t wram[0x2000];
static uint8_t hram[0x80];
static uint8_t io_misc[0x80];

/* timer */
static uint16_t div_counter;
static uint8_t tima, tma, tac;
/* serial */
static uint8_t sb, sc;
static int serial_cycles;
static void (*serial_cb)(uint8_t);
/* joypad */
static uint8_t joy_sel = 0x30, joy_buttons, joy_dpad;

void gb_serial_hook(void (*fn)(uint8_t)) { serial_cb = fn; }

/* ------------------------------------------------------------------ */
static uint8_t joyp_read(void)
{
    uint8_t lines = 0x0F;
    if (!(joy_sel & 0x20)) lines &= (uint8_t)~joy_buttons;
    if (!(joy_sel & 0x10)) lines &= (uint8_t)~joy_dpad;
    return 0xC0 | joy_sel | (lines & 0x0F);
}

void gb_set_input(uint8_t buttons, uint8_t dpad)
{
    uint8_t before = joyp_read() & 0x0F;
    joy_buttons = buttons & 0x0F;
    joy_dpad = dpad & 0x0F;
    uint8_t after = joyp_read() & 0x0F;
    if (before & ~after) io_if |= 0x10;
}

static uint8_t io_read(uint8_t r)
{
    switch (r) {
    case 0x00: return joyp_read();
    case 0x01: return sb;
    case 0x02: return sc | 0x7E;
    case 0x04: return div_counter >> 8;
    case 0x05: return tima;
    case 0x06: return tma;
    case 0x07: return tac | 0xF8;
    case 0x0F: return io_if | 0xE0;
    default: break;
    }
    if (r >= 0x10 && r <= 0x3F) return apu_read(0xFF00 | r);
    if (r >= 0x40 && r <= 0x4B) return ppu_read(r);
    return 0xFF;
}

static void dma_start(uint8_t v)
{
    uint16_t src = (uint16_t)(v << 8);
    for (int i = 0; i < 0xA0; i++) oam[i] = rd8((uint16_t)(src + i));
}

static void timer_div_reset(void);

static void io_write(uint8_t r, uint8_t v)
{
    switch (r) {
    case 0x00: joy_sel = v & 0x30; return;
    case 0x01: sb = v; return;
    case 0x02:
        sc = v;
        if ((v & 0x81) == 0x81) {
            if (serial_cb) serial_cb(sb);
            serial_cycles = 4096;       /* 8 bits at 8192 Hz */
        }
        return;
    case 0x04: timer_div_reset(); return;
    case 0x05: tima = v; return;
    case 0x06: tma = v; return;
    case 0x07: tac = v & 7; return;
    case 0x0F: io_if = v & 0x1F; return;
    default: break;
    }
    if (r >= 0x10 && r <= 0x3F) { apu_write(0xFF00 | r, v); return; }
    if (r == 0x46) { io_misc[r] = v; dma_start(v); return; }
    if (r >= 0x40 && r <= 0x4B) { ppu_write(r, v); return; }
    io_misc[r] = v;
}

/* ------------------------------------------------------------------ */
uint8_t rd8(uint16_t a)
{
    switch (a >> 12) {
    case 0: case 1: case 2: case 3:
    case 4: case 5: case 6: case 7: return a < 0x4000 ? cart_lo[a] : cart_hi[a - 0x4000];
    case 8: case 9: return vram[a & 0x1FFF];
    case 0xA: case 0xB: return cart_ram_read(a);
    case 0xC: case 0xD: case 0xE: return wram[a & 0x1FFF];
    default:
        if (a < 0xFE00) return wram[a & 0x1FFF];
        if (a < 0xFEA0) return oam[a - 0xFE00];
        if (a < 0xFF00) return 0xFF;
        if (a < 0xFF80) return io_read((uint8_t)(a & 0x7F));
        if (a == 0xFFFF) return io_ie;
        return hram[a & 0x7F];
    }
}

/* memory-write watchers (game event detection) */
#define MAX_WATCH 16
static uint16_t watch_lo[MAX_WATCH], watch_hi[MAX_WATCH];
static int watch_n;
static void (*watch_cb)(uint16_t, uint8_t, uint8_t);

void gb_watch_set(const uint16_t *addrs, int n, void (*cb)(uint16_t addr, uint8_t old_v, uint8_t new_v))
{
    watch_n = n > MAX_WATCH ? MAX_WATCH : n;
    for (int i = 0; i < watch_n; i++) watch_lo[i] = watch_hi[i] = addrs[i];
    watch_cb = cb;
    if (!cb) watch_n = 0;
}

void gb_watch_range(uint16_t lo, uint16_t hi, void (*cb)(uint16_t addr, uint8_t old_v, uint8_t new_v))
{
    watch_lo[0] = lo; watch_hi[0] = hi; watch_n = cb ? 1 : 0; watch_cb = cb;
}

static void watch_check(uint16_t a, uint8_t v)
{
    for (int i = 0; i < watch_n; i++)
        if (a >= watch_lo[i] && a <= watch_hi[i]) { watch_cb(a, rd8(a), v); return; }
}

void wr8(uint16_t a, uint8_t v)
{
    if (UNLIKELY(watch_n)) watch_check(a, v);
    switch (a >> 12) {
    case 0: case 1: case 2: case 3:
    case 4: case 5: case 6: case 7: cart_write_ctrl(a, v); return;     /* mapper registers */
    case 8: case 9: vram[a & 0x1FFF] = v; return;
    case 0xA: case 0xB: cart_ram_write(a, v); return;
    case 0xC: case 0xD: case 0xE: wram[a & 0x1FFF] = v; return;
    default:
        if (a < 0xFE00) { wram[a & 0x1FFF] = v; return; }
        if (a < 0xFEA0) { oam[a - 0xFE00] = v; return; }
        if (a < 0xFF00) return;
        if (a < 0xFF80) { io_write((uint8_t)(a & 0x7F), v); return; }
        if (a == 0xFFFF) { io_ie = v; return; }
        hram[a & 0x7F] = v;
    }
}

/* ------------------------------------------------------------------ */
static inline unsigned timer_mask(void)
{
    static const unsigned m[4] = {1u << 9, 1u << 3, 1u << 5, 1u << 7};
    return m[tac & 3];
}

static void tima_inc(void)
{
    if (++tima == 0) { tima = tma; io_if |= 0x04; }
}

static void timer_div_reset(void)
{
    if ((tac & 4) && (div_counter & timer_mask())) tima_inc();
    div_counter = 0;
}

static void timer_tick(int n)
{
    for (int i = 0; i < n; i += 4) {
        uint16_t old = div_counter;
        div_counter += 4;
        if ((tac & 4) && (old & timer_mask()) && !(div_counter & timer_mask())) tima_inc();
    }
}

void hw_tick(int n)
{
    total_cycles += (uint64_t)n;
    timer_tick(n);
    ppu_tick(n);
    apu_tick(n);
    if (serial_cycles > 0 && (serial_cycles -= n) <= 0) {
        serial_cycles = 0;
        sc &= 0x7F;
        sb = 0xFF;
        io_if |= 0x08;
    }
}

/* ------------------------------------------------------------------ */
void cpu_halt(void)
{
    while (!(io_if & io_ie & 0x1F)) hw_tick(4);
}

void cpu_service_irq(void)
{
    uint8_t pend = io_if & io_ie & 0x1F;
    cpu.ime = 0;
    cpu.ei_pending = 0;
    cpu.sp--; wr8(cpu.sp, cpu.pc >> 8);
    cpu.sp--; wr8(cpu.sp, (uint8_t)cpu.pc);
    for (int i = 0; i < 5; i++) {
        if (pend & (1 << i)) {
            io_if &= (uint8_t)~(1 << i);
            cpu.pc = (uint16_t)(0x40 + i * 8);
            break;
        }
    }
    hw_tick(20);
}

void cpu_lockup(uint8_t op, uint16_t pc)
{
    fprintf(stderr, "CPU locked up: illegal opcode %02X at %04X\n", op, pc);
    fflush(stdout);
    exit(2);
}

void cpu_step_checked(void)
{
    cpu_step();
    if (cpu_irq_check()) cpu_service_irq();
}

void run_interpreter(void)
{
    for (;;) cpu_step_checked();
}

/* ---- coverage of ROM addresses that were executed but not recompiled ---- */
static uint8_t miss_seen[0x8000];
static unsigned long miss_ram_steps;

void recomp_miss(uint16_t pc)
{
    if (pc < 0x8000) miss_seen[pc] = 1;
    else miss_ram_steps++;
}

void gb_dump_misses(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    int n = 0;
    for (int i = 0; i < 0x8000; i++)
        if (miss_seen[i]) { fprintf(f, "%04X\n", i); n++; }
    fclose(f);
    fprintf(stderr, "[recomp] %d ROM addresses ran in the interpreter (wrote %s); %lu RAM/HRAM steps\n",
            n, path, miss_ram_steps);
}

/* ------------------------------------------------------------------ */
void gb_reset(void)
{
    memset(&cpu, 0, sizeof cpu);
    cart_reset();
    /* state after the DMG boot ROM hands over */
    cpu.a = 0x01; cpu.f = 0xB0; cpu.b = 0x00; cpu.c = 0x13;
    cpu.d = 0x00; cpu.e = 0xD8; cpu.h = 0x01; cpu.l = 0x4D;
    cpu.sp = 0xFFFE; cpu.pc = 0x0100;
    io_if = 0x01; io_ie = 0;
    div_counter = 0xABCC; tima = tma = tac = 0;
    sb = 0; sc = 0x7E; serial_cycles = 0;
    joy_sel = 0x30; joy_buttons = joy_dpad = 0;
    memset(wram, 0, sizeof wram);
    memset(hram, 0, sizeof hram);
    memset(io_misc, 0xFF, sizeof io_misc);
    total_cycles = 0;
    ppu_reset();
    apu_reset();
}
