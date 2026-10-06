/* Deleted-slot finder for ESP-IDF 4.4 NVS pages. See cairn_nvs_wipe.h. */

#include "cairn_nvs_wipe.h"

#include <stddef.h>

#define PAGE_ACTIVE   0xFFFFFFFEu
#define PAGE_FULL     0xFFFFFFFCu
#define PAGE_FREEING  0xFFFFFFF8u

#define TABLE_OFFSET   32u
#define ENTRIES_OFFSET 64u

/* Two bits per entry, little-endian 32-bit words: 0b11 empty, 0b10 written, 0b00 erased,
 * 0b01 illegal (a half-written entry; NVS erases it itself at load, so it is left alone). */
#define ENTRY_ERASED 0u

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int slot_has_bytes(const uint8_t *s)
{
    int all_zero = 1, all_ff = 1;
    for (size_t i = 0; i < CAIRN_NVS_SLOT_SIZE; i++) {
        if (s[i] != 0x00) all_zero = 0;
        if (s[i] != 0xFF) all_ff = 0;
    }
    return !(all_zero || all_ff);
}

size_t cairn_nvs_dirty_erased_slots(const uint8_t page[CAIRN_NVS_PAGE_SIZE],
                                    uint16_t *offsets, size_t cap)
{
    if (page == NULL || offsets == NULL) return 0;

    uint32_t st = le32(page);
    if (st != PAGE_ACTIVE && st != PAGE_FULL && st != PAGE_FREEING) return 0;

    size_t n = 0;
    for (size_t i = 0; i < CAIRN_NVS_ENTRY_COUNT && n < cap; i++) {
        uint32_t word = le32(page + TABLE_OFFSET + 4u * (i / 16u));
        uint32_t state = (word >> (2u * (i % 16u))) & 3u;
        if (state != ENTRY_ERASED) continue;

        size_t off = ENTRIES_OFFSET + i * CAIRN_NVS_SLOT_SIZE;
        if (slot_has_bytes(page + off)) offsets[n++] = (uint16_t)off;
    }
    return n;
}
