/* Cartridge: header parsing, MBC1/MBC3/MBC5 bank switching, battery RAM. */
#include "gb.h"
#include "cart.h"
#include "patch.h"

uint8_t *rom;
uint8_t *cart_ram;
int cart_ram_on;
const uint8_t *cart_lo, *cart_hi;

static CartInfo info;
static size_t rom_alloc, ram_alloc, rom_len;
static int rom_mask;                 /* banks - 1 */
static int ram_enable, bank1, bank2, mode, rambank, mbc_ramsel;
static int ram_dirty;
static uint32_t ram_crc_saved;

const CartInfo *cart_info(void) { return &info; }

static size_t ram_size_from_code(int c)
{
    static const size_t t[6] = {0, 2048, 8192, 32768, 131072, 65536};
    return c >= 0 && c < 6 ? t[c] : 0;
}

int cart_parse(const uint8_t *img, size_t n, CartInfo *ci)
{
    memset(ci, 0, sizeof *ci);
    if (n < 0x150) return 0;
    memcpy(ci->title, img + 0x134, 16);
    ci->title[16] = 0;
    for (int i = 0; i < 16; i++) if ((unsigned char)ci->title[i] < 32 || (unsigned char)ci->title[i] > 126) { ci->title[i] = 0; break; }
    int t = img[0x147];
    switch (t) {
    case 0x00: case 0x08: case 0x09: ci->mapper = 0; break;
    case 0x01: case 0x02: case 0x03: ci->mapper = 1; break;
    case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: ci->mapper = 3; break;
    case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: ci->mapper = 5; break;
    default: ci->mapper = -1; break;
    }
    ci->battery = (t == 0x03 || t == 0x09 || t == 0x0F || t == 0x10 || t == 0x13 || t == 0x1B || t == 0x1E);
    ci->ram_bytes = ram_size_from_code(img[0x149]);
    int banks = 2;
    while ((size_t)banks * 0x4000 < n) banks <<= 1;
    ci->rom_banks = banks;
    ci->size = n;
    ci->crc = crc32_bytes(img, n);
    return 1;
}

int cart_install(const uint8_t *img, size_t n)
{
    CartInfo ci;
    if (!cart_parse(img, n, &ci) || ci.mapper < 0) return -1;
    size_t need = (size_t)ci.rom_banks * 0x4000;
    if (need > rom_alloc) {
        free(rom);
        rom = (uint8_t *)malloc(need);
        if (!rom) { rom_alloc = 0; return -1; }
        rom_alloc = need;
    }
    memset(rom, 0xFF, need);
    memcpy(rom, img, n);
    rom_len = need;
    rom_mask = ci.rom_banks - 1;
    size_t ramb = ci.ram_bytes;
    if (ramb < 0x2000 && ramb) ramb = 0x2000;       /* simple: 2 KB behaves like an 8 KB window */
    if (ramb > ram_alloc) {
        free(cart_ram);
        cart_ram = (uint8_t *)calloc(ramb, 1);
        ram_alloc = ramb;
    } else if (cart_ram) {
        memset(cart_ram, 0, ram_alloc);
    }
    info = ci;
    info.ram_bytes = ramb;
    cart_ram_on = ramb != 0;
    ram_dirty = 0; ram_crc_saved = 0;
    cart_reset();
    return 0;
}

void cart_reset(void)
{
    ram_enable = 0; bank1 = 1; bank2 = 0; mode = 0; rambank = 0; mbc_ramsel = 0;
    cart_lo = rom;
    cart_hi = rom + 0x4000;
    if (info.mapper == 0 && rom_len <= 0x4000) cart_hi = rom;
}

static void remap(void)
{
    int lo = 0, hi = 1;
    switch (info.mapper) {
    case 1: {
        int b = bank1 ? bank1 : 1;
        hi = (b | (bank2 << 5)) & rom_mask;
        lo = mode ? ((bank2 << 5) & rom_mask) : 0;
        rambank = mode ? bank2 : 0;
        break;
    }
    case 3: hi = (bank1 ? bank1 : 1) & rom_mask; break;
    case 5: hi = bank1 & rom_mask; break;
    default: break;
    }
    cart_lo = rom + (size_t)lo * 0x4000;
    cart_hi = rom + (size_t)hi * 0x4000;
}

void cart_write_ctrl(uint16_t a, uint8_t v)
{
    switch (info.mapper) {
    case 1:
        switch (a >> 13) {
        case 0: ram_enable = (v & 0x0F) == 0x0A; break;
        case 1: bank1 = v & 0x1F; break;
        case 2: bank2 = v & 3; break;
        case 3: mode = v & 1; break;
        }
        break;
    case 3:
        switch (a >> 13) {
        case 0: ram_enable = (v & 0x0F) == 0x0A; break;
        case 1: bank1 = v & 0x7F; break;
        case 2: mbc_ramsel = v; rambank = v & 3; break;
        default: break;                 /* RTC latch ignored */
        }
        break;
    case 5:
        switch (a >> 12) {
        case 0: case 1: ram_enable = (v & 0x0F) == 0x0A; break;
        case 2: bank1 = (bank1 & 0x100) | v; break;
        case 3: bank1 = (bank1 & 0xFF) | ((v & 1) << 8); break;
        case 4: case 5: rambank = v & 0x0F; break;
        }
        break;
    default: return;
    }
    remap();
}

static inline size_t ram_off(uint16_t a)
{
    size_t off = (size_t)rambank * 0x2000 + (a & 0x1FFF);
    return ram_alloc ? off % ram_alloc : 0;
}

uint8_t cart_ram_read(uint16_t a)
{
    if (!cart_ram_on) return 0xFF;
    if (info.mapper != 0 && !ram_enable) return 0xFF;
    if (info.mapper == 3 && mbc_ramsel >= 8) return 0;          /* RTC registers: not emulated */
    return cart_ram[ram_off(a)];
}

void cart_ram_write(uint16_t a, uint8_t v)
{
    if (!cart_ram_on) return;
    if (info.mapper != 0 && !ram_enable) return;
    if (info.mapper == 3 && mbc_ramsel >= 8) return;
    size_t o = ram_off(a);
    if (cart_ram[o] != v) { cart_ram[o] = v; ram_dirty = 1; }
}

int cart_load_save(const char *path)
{
    if (!info.battery || !cart_ram_on) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(cart_ram, 1, ram_alloc, f);
    fclose(f);
    ram_dirty = 0;
    return n > 0;
}

int cart_write_save(const char *path)
{
    if (!info.battery || !cart_ram_on || !ram_dirty) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(cart_ram, 1, ram_alloc, f);
    fclose(f);
    ram_dirty = 0;
    return 1;
}
