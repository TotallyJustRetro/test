#pragma once
#include <stdint.h>
#include <stddef.h>
/* Apply an IPS / BPS / UPS patch (auto-detected by magic) to `base`.
 * On success returns 0 and *out_len is the patched size (<= out_cap).
 * On failure returns non-zero and writes a human-readable reason into err. */
int patch_apply(const uint8_t *patch, size_t patch_len, const uint8_t *base, size_t base_len,
                uint8_t *out, size_t out_cap, size_t *out_len, char *err, size_t err_cap);
/* 1 if the file starts with a known patch signature */
int patch_is_patch(const uint8_t *p, size_t n);
uint32_t crc32_bytes(const uint8_t *p, size_t n);
