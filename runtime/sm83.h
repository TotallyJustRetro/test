#pragma once
/* Helpers used by generated code (game.c / interp.c). */
#include "gb.h"

#define UNLIKELY(x) __builtin_expect(!!(x), 0)

#define FZ 0x80
#define FN 0x40
#define FH 0x20
#define FC 0x10

static inline uint16_t BC(void) { return (uint16_t)((cpu.b << 8) | cpu.c); }
static inline uint16_t DE(void) { return (uint16_t)((cpu.d << 8) | cpu.e); }
static inline uint16_t HL(void) { return (uint16_t)((cpu.h << 8) | cpu.l); }
static inline uint16_t AF(void) { return (uint16_t)((cpu.a << 8) | cpu.f); }
static inline void SET_BC(uint16_t v) { cpu.b = v >> 8; cpu.c = (uint8_t)v; }
static inline void SET_DE(uint16_t v) { cpu.d = v >> 8; cpu.e = (uint8_t)v; }
static inline void SET_HL(uint16_t v) { cpu.h = v >> 8; cpu.l = (uint8_t)v; }
static inline void SET_AF(uint16_t v) { cpu.a = v >> 8; cpu.f = (uint8_t)v & 0xF0; }

static inline void push16(uint16_t v)
{
    cpu.sp--; wr8(cpu.sp, v >> 8);
    cpu.sp--; wr8(cpu.sp, (uint8_t)v);
}
static inline uint16_t pop16(void)
{
    uint8_t lo = rd8(cpu.sp++);
    uint8_t hi = rd8(cpu.sp++);
    return (uint16_t)(lo | (hi << 8));
}

/* ---- 8-bit ALU ---- */
static inline void alu_add(uint8_t v)
{
    unsigned r = cpu.a + v;
    cpu.f = ((r & 0xFF) == 0 ? FZ : 0) | (((cpu.a & 0xF) + (v & 0xF)) > 0xF ? FH : 0) | (r > 0xFF ? FC : 0);
    cpu.a = (uint8_t)r;
}
static inline void alu_adc(uint8_t v)
{
    unsigned c = (cpu.f & FC) ? 1 : 0, r = cpu.a + v + c;
    cpu.f = ((r & 0xFF) == 0 ? FZ : 0) | (((cpu.a & 0xF) + (v & 0xF) + c) > 0xF ? FH : 0) | (r > 0xFF ? FC : 0);
    cpu.a = (uint8_t)r;
}
static inline void alu_sub(uint8_t v)
{
    unsigned r = (unsigned)cpu.a - v;
    cpu.f = FN | ((r & 0xFF) == 0 ? FZ : 0) | ((cpu.a & 0xF) < (v & 0xF) ? FH : 0) | (cpu.a < v ? FC : 0);
    cpu.a = (uint8_t)r;
}
static inline void alu_sbc(uint8_t v)
{
    int c = (cpu.f & FC) ? 1 : 0, r = (int)cpu.a - v - c;
    cpu.f = FN | ((r & 0xFF) == 0 ? FZ : 0) | (((int)(cpu.a & 0xF) - (v & 0xF) - c) < 0 ? FH : 0) | (r < 0 ? FC : 0);
    cpu.a = (uint8_t)r;
}
static inline void alu_and(uint8_t v) { cpu.a &= v; cpu.f = (cpu.a == 0 ? FZ : 0) | FH; }
static inline void alu_xor(uint8_t v) { cpu.a ^= v; cpu.f = (cpu.a == 0 ? FZ : 0); }
static inline void alu_or(uint8_t v)  { cpu.a |= v; cpu.f = (cpu.a == 0 ? FZ : 0); }
static inline void alu_cp(uint8_t v)
{
    unsigned r = (unsigned)cpu.a - v;
    cpu.f = FN | ((r & 0xFF) == 0 ? FZ : 0) | ((cpu.a & 0xF) < (v & 0xF) ? FH : 0) | (cpu.a < v ? FC : 0);
}
static inline uint8_t alu_inc(uint8_t v)
{
    uint8_t r = v + 1;
    cpu.f = (cpu.f & FC) | (r == 0 ? FZ : 0) | ((v & 0xF) == 0xF ? FH : 0);
    return r;
}
static inline uint8_t alu_dec(uint8_t v)
{
    uint8_t r = v - 1;
    cpu.f = (cpu.f & FC) | FN | (r == 0 ? FZ : 0) | ((v & 0xF) == 0 ? FH : 0);
    return r;
}
static inline void alu_daa(void)
{
    unsigned a = cpu.a, f = cpu.f;
    if (!(f & FN)) {
        if ((f & FC) || a > 0x99) { a += 0x60; f |= FC; }
        if ((f & FH) || (a & 0xF) > 9) a += 6;
    } else {
        if (f & FC) a -= 0x60;
        if (f & FH) a -= 6;
    }
    a &= 0xFF;
    cpu.f = (uint8_t)((f & (FN | FC)) | (a == 0 ? FZ : 0));
    cpu.a = (uint8_t)a;
}

/* ---- 16-bit ---- */
static inline void alu_add_hl(uint16_t v)
{
    unsigned hl = HL(), r = hl + v;
    cpu.f = (cpu.f & FZ) | (((hl & 0xFFF) + (v & 0xFFF)) > 0xFFF ? FH : 0) | (r > 0xFFFF ? FC : 0);
    SET_HL((uint16_t)r);
}
static inline uint16_t alu_ld_hl_sp(int8_t e)
{
    unsigned sp = cpu.sp, u = (uint8_t)e;
    cpu.f = (((sp & 0xF) + (u & 0xF)) > 0xF ? FH : 0) | (((sp & 0xFF) + u) > 0xFF ? FC : 0);
    return (uint16_t)(sp + e);
}
static inline uint16_t alu_add_sp(int8_t e) { return alu_ld_hl_sp(e); }

/* ---- accumulator rotates (Z cleared) ---- */
static inline void rlca(void) { uint8_t c = cpu.a >> 7; cpu.a = (cpu.a << 1) | c; cpu.f = c ? FC : 0; }
static inline void rrca(void) { uint8_t c = cpu.a & 1; cpu.a = (cpu.a >> 1) | (c << 7); cpu.f = c ? FC : 0; }
static inline void rla(void)  { uint8_t c = cpu.a >> 7; cpu.a = (cpu.a << 1) | ((cpu.f & FC) ? 1 : 0); cpu.f = c ? FC : 0; }
static inline void rra(void)  { uint8_t c = cpu.a & 1; cpu.a = (cpu.a >> 1) | ((cpu.f & FC) ? 0x80 : 0); cpu.f = c ? FC : 0; }

/* ---- CB-prefixed ---- */
static inline uint8_t cb_flags(uint8_t r, int carry) { cpu.f = (r == 0 ? FZ : 0) | (carry ? FC : 0); return r; }
static inline uint8_t cb_rlc(uint8_t v) { return cb_flags((v << 1) | (v >> 7), v >> 7); }
static inline uint8_t cb_rrc(uint8_t v) { return cb_flags((v >> 1) | (v << 7), v & 1); }
static inline uint8_t cb_rl(uint8_t v)  { return cb_flags((v << 1) | ((cpu.f & FC) ? 1 : 0), v >> 7); }
static inline uint8_t cb_rr(uint8_t v)  { return cb_flags((v >> 1) | ((cpu.f & FC) ? 0x80 : 0), v & 1); }
static inline uint8_t cb_sla(uint8_t v) { return cb_flags(v << 1, v >> 7); }
static inline uint8_t cb_sra(uint8_t v) { return cb_flags((v >> 1) | (v & 0x80), v & 1); }
static inline uint8_t cb_swap(uint8_t v) { return cb_flags((v << 4) | (v >> 4), 0); }
static inline uint8_t cb_srl(uint8_t v) { return cb_flags(v >> 1, v & 1); }
static inline void cb_bit(int n, uint8_t v) { cpu.f = (cpu.f & FC) | FH | ((v & (1 << n)) ? 0 : FZ); }

/* ---- timing / interrupts, used by lifted code ---- */
#define tick(n) hw_tick(n)

static inline int cpu_irq_check(void)
{
    if (UNLIKELY(cpu.ei_pending)) {
        if (--cpu.ei_pending == 0) cpu.ime = 1;
    }
    return cpu.ime && (io_if & io_ie & 0x1F);
}

/* After every instruction: take a pending interrupt, with PC = address of
 * the instruction that would have run next. */
#define NEXT(pcv) do { if (UNLIKELY(cpu_irq_check())) { cpu.pc = (pcv); goto irq; } } while (0)
