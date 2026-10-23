#pragma once
#include "gb.h"

typedef struct {
    uint8_t shade[GB_H][GB_WMAX], layer[GB_H][GB_WMAX];
    uint8_t bguv[GB_H][GB_WMAX], spruv[GB_H][GB_WMAX];
    uint16_t bgtile[GB_H][GB_WMAX], sprtile[GB_H][GB_WMAX];
    int w, xoff;                        /* picture width, and where the original screen starts */
    uint8_t tiles[0x1800];              /* VRAM tile data at the moment the frame completed */
    uint64_t seq;
    int lcd_on;
} Frame;

/* ---- running a game on its own thread (windowed play) ---- */
int  emu_start(int force_interp);               /* 0 ok */
void emu_stop(void);                            /* joins; writes the battery save */
int  emu_running(void);
int  emu_frame_get(Frame *f);                   /* 1 if a newer frame was copied */
void emu_input(uint8_t buttons, uint8_t dpad);
void emu_set_paused(int p);
void emu_set_turbo(int t);
void emu_set_save_path(const char *path);       /* "" = none */
uint64_t emu_frames(void);

/* ---- synchronous helpers ---- */
void emu_preview(int frames);                   /* run silently, leave the picture in ppu_* arrays */
void emu_run_blocking(int force_interp);        /* developer / headless: run on this thread until the hook stops it */

/* ---- developer options (headless runs) ---- */
typedef struct {
    int max_frames;                  /* -1 = unlimited */
    int hash, fuzz;
    uint32_t seed;
    const char *hash_log, *script;
    int region_on, region[4];        /* log frames where this screen area (x0,y0,x1,y1) changes */
} EmuDev;
extern EmuDev emu_dev;
void emu_dev_poke(int frame, uint16_t addr, uint8_t val);
int  emu_dev_init(void);            /* loads script / opens log; 0 ok */
void emu_dev_report(void);          /* prints the hash line */
void emu_dev_close(void);
