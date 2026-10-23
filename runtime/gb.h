#pragma once
/* Game Boy hardware model shared by the recompiled code, the fallback
 * interpreter and the front end. */
#include <stdint.h>
#include "util.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GB_W 160
#define GB_H 144
#define GB_WMAX 256            /* widest picture the PPU can produce (widescreen mode) */
#define CYCLES_PER_FRAME 70224
#define CPU_HZ 4194304

typedef struct {
    uint8_t a, f, b, c, d, e, h, l;
    uint16_t sp, pc;
    uint8_t ime;
    uint8_t ei_pending;     /* EI takes effect after the *next* instruction */
} CPU;

extern CPU cpu;
extern uint8_t io_if, io_ie;
extern uint8_t *rom;                  /* cartridge image (bank 0 first); see cart.c */
extern uint64_t total_cycles;
extern uint8_t *cart_ram;
extern int cart_ram_on;

/* ---- memory ---- */
uint8_t rd8(uint16_t a);
void wr8(uint16_t a, uint8_t v);
void gb_watch_range(uint16_t lo, uint16_t hi, void (*cb)(uint16_t addr, uint8_t old_v, uint8_t new_v));
void gb_watch_set(const uint16_t *addrs, int n, void (*cb)(uint16_t addr, uint8_t old_v, uint8_t new_v));

/* ---- core ---- */
void gb_reset(void);
void hw_tick(int tcycles);              /* advance timer/PPU/APU/serial */
void cpu_halt(void);
void cpu_service_irq(void);
void cpu_lockup(uint8_t op, uint16_t pc) __attribute__((noreturn));
void cpu_step(void);                    /* generated interpreter (interp.c) */
void cpu_step_checked(void);            /* one instruction + interrupt check */
void recomp_run(void);                  /* generated lifted code (game.c) */
void recomp_miss(uint16_t pc);
void run_interpreter(void);
void gb_set_input(uint8_t buttons, uint8_t dpad);   /* bit set = pressed */
void gb_dump_misses(const char *path);
void gb_serial_hook(void (*fn)(uint8_t));

/* ---- PPU ---- */
void ppu_reset(void);
void ppu_tick(int n);
uint8_t ppu_read(uint8_t reg);
void ppu_write(uint8_t reg, uint8_t v);
extern uint8_t ppu_shade[GB_H][GB_WMAX];   /* 0..3, already through BGP/OBPx */
extern uint8_t ppu_layer[GB_H][GB_WMAX];   /* 0 BG/window, 1 sprite (OBP0), 2 sprite (OBP1) */
extern uint8_t vram[0x2000], oam[0xA0];
extern uint16_t ppu_bgtile[GB_H][GB_WMAX], ppu_sprtile[GB_H][GB_WMAX];
extern uint8_t ppu_bguv[GB_H][GB_WMAX], ppu_spruv[GB_H][GB_WMAX];   /* sprites: bit6 = X flip, bit7 = Y flip */
void ppu_tile_hashes(uint64_t out[384]);
int  ppu_lcd_is_on(void);

/* ---- APU ---- */
void apu_reset(void);
void apu_tick(int n);
uint8_t apu_read(uint16_t addr);
void apu_write(uint16_t addr, uint8_t v);
int  apu_drain(int16_t *dst, int max_frames);   /* stereo frames */
void apu_set_rate(int hz);
void apu_set_volume(float v);          /* 0..2, 1 = normal */
void apu_set_tap_mask(int mask);       /* channels (bit0..3) mirrored into a second stream */
int  apu_drain_tap(int16_t *dst, int max_frames);
void apu_set_trigger_hook(void (*cb)(int ch, const uint8_t *regs5));
int  apu_channel_active(int c);

/* ---- front end (frontend_sdl.c or frontend_null.c) ---- */
typedef struct {
    int scale;
    int fullscreen;
    int audio;
    int headless;
    int turbo;
    const char *title;
} FrontendOpts;
int  frontend_init(const FrontendOpts *o);
int  frontend_frame(void);             /* present; pace; poll input. 0 = quit */
void frontend_shutdown(void);
void frame_hook(void);                 /* called once per video frame by PPU */
extern int frame_count;
/* Widescreen: the picture is ppu_w pixels wide; the original 160x144 screen sits ppu_xoff pixels from the left.
 * hud_lines: top lines drawn with scroll 0 (the status bar) are centred instead of left-aligned. */
extern int ppu_w, ppu_xoff;
/* gate: 0 = always wide, 1 = only while the timer interrupt is enabled (Super Mario Land levels),
 * 2 = only while the bottom status window is up (Super Mario Land 2 levels). Otherwise the normal screen with bars. */
void ppu_set_wide(int left, int right, int hud_lines, int centre_window, int gate);
