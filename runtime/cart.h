#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    char title[17];
    int mapper;          /* 0 none, 1 MBC1, 3 MBC3, 5 MBC5 */
    int rom_banks;       /* 16 KB banks */
    size_t ram_bytes;
    int battery;
    size_t size;
    uint32_t crc;
} CartInfo;

/* Parse the header of an image (does not install it). Returns 0 if it doesn't look like a Game Boy ROM. */
int  cart_parse(const uint8_t *img, size_t n, CartInfo *ci);
/* Install an image (copied, padded to a power-of-two number of banks) and reset the mapper. */
int  cart_install(const uint8_t *img, size_t n);
void cart_reset(void);                       /* mapper registers back to power-on (RAM contents kept) */
const CartInfo *cart_info(void);
/* Battery saves */
int  cart_load_save(const char *path);
int  cart_write_save(const char *path);      /* only if there is battery RAM and it changed */
/* mapper-aware access used by rd8/wr8 */
extern const uint8_t *cart_lo, *cart_hi;     /* 0000-3FFF / 4000-7FFF windows */
void cart_write_ctrl(uint16_t a, uint8_t v);
uint8_t cart_ram_read(uint16_t a);
void cart_ram_write(uint16_t a, uint8_t v);
