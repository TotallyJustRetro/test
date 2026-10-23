/* IPS, UPS and BPS patch appliers. */
#include "patch.h"
#include <string.h>
#include <stdio.h>

uint32_t crc32_bytes(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static int fail(char *err, size_t cap, const char *msg) { snprintf(err, cap, "%s", msg); return 1; }

int patch_is_patch(const uint8_t *p, size_t n)
{
    return (n >= 5 && !memcmp(p, "PATCH", 5)) || (n >= 4 && (!memcmp(p, "UPS1", 4) || !memcmp(p, "BPS1", 4)));
}

/* ---------------------------------------------------------------- IPS */
static int apply_ips(const uint8_t *pt, size_t n, const uint8_t *base, size_t bl,
                     uint8_t *out, size_t cap, size_t *ol, char *err, size_t ec)
{
    if (bl > cap) return fail(err, ec, "ROM too large");
    memcpy(out, base, bl);
    size_t len = bl, i = 5;
    for (;;) {
        if (i + 3 > n) return fail(err, ec, "IPS patch is truncated");
        if (!memcmp(pt + i, "EOF", 3)) {
            i += 3;
            if (i + 3 <= n) {                       /* optional truncate length */
                size_t t = ((size_t)pt[i] << 16) | ((size_t)pt[i + 1] << 8) | pt[i + 2];
                if (t < len) len = t;
            }
            break;
        }
        size_t off = ((size_t)pt[i] << 16) | ((size_t)pt[i + 1] << 8) | pt[i + 2];
        i += 3;
        if (i + 2 > n) return fail(err, ec, "IPS patch is truncated");
        size_t sz = ((size_t)pt[i] << 8) | pt[i + 1];
        i += 2;
        if (sz == 0) {                              /* RLE */
            if (i + 3 > n) return fail(err, ec, "IPS patch is truncated");
            size_t run = ((size_t)pt[i] << 8) | pt[i + 1];
            uint8_t v = pt[i + 2];
            i += 3;
            if (off + run > cap) return fail(err, ec, "This hack makes the ROM bigger than the cartridge allows");
            if (off > len) memset(out + len, 0, off - len);
            memset(out + off, v, run);
            if (off + run > len) len = off + run;
        } else {
            if (i + sz > n) return fail(err, ec, "IPS patch is truncated");
            if (off + sz > cap) return fail(err, ec, "This hack makes the ROM bigger than the cartridge allows");
            if (off > len) memset(out + len, 0, off - len);
            memcpy(out + off, pt + i, sz);
            i += sz;
            if (off + sz > len) len = off + sz;
        }
    }
    *ol = len;
    return 0;
}

/* ---------------------------------------------------------------- varints (UPS/BPS) */
static int varint(const uint8_t *p, size_t n, size_t *i, uint64_t *v)
{
    uint64_t data = 0, shift = 1;
    for (;;) {
        if (*i >= n) return 0;
        uint8_t x = p[(*i)++];
        data += (uint64_t)(x & 0x7F) * shift;
        if (x & 0x80) break;
        shift <<= 7;
        data += shift;
    }
    *v = data;
    return 1;
}

static uint32_t rd32le(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

/* ---------------------------------------------------------------- UPS */
static int apply_ups(const uint8_t *pt, size_t n, const uint8_t *base, size_t bl,
                     uint8_t *out, size_t cap, size_t *ol, char *err, size_t ec)
{
    if (n < 4 + 12) return fail(err, ec, "UPS patch is truncated");
    size_t i = 4, end = n - 12;
    uint64_t in_sz, out_sz;
    if (!varint(pt, end, &i, &in_sz) || !varint(pt, end, &i, &out_sz)) return fail(err, ec, "UPS patch is damaged");
    if (out_sz > cap) return fail(err, ec, "This hack makes the ROM bigger than the cartridge allows");
    if (rd32le(pt + n - 12) != crc32_bytes(base, bl) && rd32le(pt + n - 8) != crc32_bytes(base, bl))
        return fail(err, ec, "This patch was made for a different ROM than yours");
    memset(out, 0, (size_t)out_sz);
    memcpy(out, base, bl < out_sz ? bl : (size_t)out_sz);
    size_t pos = 0;
    while (i < end) {
        uint64_t skip;
        if (!varint(pt, end, &i, &skip)) return fail(err, ec, "UPS patch is damaged");
        pos += (size_t)skip;
        while (i < end) {
            uint8_t x = pt[i++];
            if (x == 0) { pos++; break; }
            if (pos < out_sz) out[pos] ^= x;
            pos++;
        }
    }
    *ol = (size_t)out_sz;
    return 0;
}

/* ---------------------------------------------------------------- BPS */
static int apply_bps(const uint8_t *pt, size_t n, const uint8_t *base, size_t bl,
                     uint8_t *out, size_t cap, size_t *ol, char *err, size_t ec)
{
    if (n < 4 + 12) return fail(err, ec, "BPS patch is truncated");
    size_t i = 4, end = n - 12;
    uint64_t src_sz, tgt_sz, meta;
    if (!varint(pt, end, &i, &src_sz) || !varint(pt, end, &i, &tgt_sz) || !varint(pt, end, &i, &meta))
        return fail(err, ec, "BPS patch is damaged");
    if (tgt_sz > cap) return fail(err, ec, "This hack makes the ROM bigger than the cartridge allows");
    if (src_sz != bl || rd32le(pt + n - 12) != crc32_bytes(base, bl))
        return fail(err, ec, "This patch was made for a different ROM than yours");
    i += (size_t)meta;
    size_t o = 0;
    int64_t src_rel = 0, tgt_rel = 0;
    while (i < end) {
        uint64_t cmd;
        if (!varint(pt, end, &i, &cmd)) return fail(err, ec, "BPS patch is damaged");
        size_t len = (size_t)(cmd >> 2) + 1;
        if (o + len > tgt_sz) return fail(err, ec, "BPS patch is damaged");
        switch (cmd & 3) {
        case 0:                                     /* SourceRead */
            for (size_t k = 0; k < len; k++, o++) out[o] = o < bl ? base[o] : 0;
            break;
        case 1:                                     /* TargetRead */
            if (i + len > end) return fail(err, ec, "BPS patch is damaged");
            memcpy(out + o, pt + i, len); i += len; o += len;
            break;
        case 2: case 3: {                           /* SourceCopy / TargetCopy */
            uint64_t d;
            if (!varint(pt, end, &i, &d)) return fail(err, ec, "BPS patch is damaged");
            int64_t delta = (d & 1) ? -(int64_t)(d >> 1) : (int64_t)(d >> 1);
            if ((cmd & 3) == 2) {
                src_rel += delta;
                for (size_t k = 0; k < len; k++, o++, src_rel++)
                    out[o] = (src_rel >= 0 && (size_t)src_rel < bl) ? base[src_rel] : 0;
            } else {
                tgt_rel += delta;
                for (size_t k = 0; k < len; k++, o++, tgt_rel++) {
                    if (tgt_rel < 0 || (size_t)tgt_rel >= o) return fail(err, ec, "BPS patch is damaged");
                    out[o] = out[tgt_rel];
                }
            }
            break;
        }
        }
    }
    if (o != tgt_sz) return fail(err, ec, "BPS patch is damaged");
    if (rd32le(pt + n - 8) != crc32_bytes(out, o)) return fail(err, ec, "BPS patch didn't produce the expected result (wrong ROM version?)");
    *ol = o;
    return 0;
}

int patch_apply(const uint8_t *pt, size_t n, const uint8_t *base, size_t bl,
                uint8_t *out, size_t cap, size_t *ol, char *err, size_t ec)
{
    if (n >= 5 && !memcmp(pt, "PATCH", 5)) return apply_ips(pt, n, base, bl, out, cap, ol, err, ec);
    if (n >= 4 && !memcmp(pt, "UPS1", 4)) return apply_ups(pt, n, base, bl, out, cap, ol, err, ec);
    if (n >= 4 && !memcmp(pt, "BPS1", 4)) return apply_bps(pt, n, base, bl, out, cap, ol, err, ec);
    return fail(err, ec, "Not a recognised patch file (expected .ips, .bps or .ups)");
}
