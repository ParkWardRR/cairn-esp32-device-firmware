/*
 * Host tests for lib/cairn_fs/cairn_nvs_wipe: which deleted NVS slots still hold bytes.
 *
 * The pages are built by hand to the ESP-IDF 4.4 layout (header, state table, 32-byte
 * entries). A canary "secret" sits in the deleted slots; the properties are that every one
 * of them is found, that nothing live, empty or structural is ever offered for writing, and
 * that a second pass finds nothing. Zeroing is applied here the way the device does it: only
 * at the returned offsets, 32 bytes each.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../lib/cairn_fs/cairn_nvs_wipe.h"

#define PAGE_ACTIVE  0xFFFFFFFEu
#define PAGE_FULL    0xFFFFFFFCu
#define PAGE_FREEING 0xFFFFFFF8u

enum { S_ERASED = 0, S_ILLEGAL = 1, S_WRITTEN = 2, S_EMPTY = 3 };

static int s_pass, s_fail;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("  FAIL  %s:%d: ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
            return 0;                                                          \
        }                                                                      \
    } while (0)

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void new_page(uint8_t *pg, uint32_t state)
{
    memset(pg, 0xFF, CAIRN_NVS_PAGE_SIZE);
    put_le32(pg, state);
    put_le32(pg + 4, 7);   /* sequence number */
    pg[8] = 0xFE;          /* version */
}

static void set_state(uint8_t *pg, unsigned i, unsigned s)
{
    uint8_t *w = pg + 32 + 4 * (i / 16);
    uint32_t v = (uint32_t)w[0] | ((uint32_t)w[1] << 8) | ((uint32_t)w[2] << 16) | ((uint32_t)w[3] << 24);
    v &= ~(3u << (2 * (i % 16)));
    v |= (uint32_t)s << (2 * (i % 16));
    put_le32(w, v);
}

static uint8_t *slot(uint8_t *pg, unsigned i) { return pg + 64 + 32u * i; }

/* An entry with a recognisable body: key-ish prefix then the canary. */
static void fill(uint8_t *pg, unsigned i, unsigned st, const char *canary)
{
    uint8_t *s = slot(pg, i);
    memset(s, 0, 32);
    memcpy(s + 8, "sta.pswd", 8);
    memcpy(s + 16, canary, strlen(canary) < 16 ? strlen(canary) : 16);
    set_state(pg, i, st);
}

static int contains(const uint8_t *buf, size_t n, const char *needle)
{
    size_t m = strlen(needle);
    for (size_t i = 0; i + m <= n; i++)
        if (memcmp(buf + i, needle, m) == 0) return 1;
    return 0;
}

/* The device's write step: zero 32 bytes at each returned offset, nothing else. */
static void zero_at(uint8_t *pg, const uint16_t *off, size_t n)
{
    for (size_t i = 0; i < n; i++) memset(pg + off[i], 0, CAIRN_NVS_SLOT_SIZE);
}

static int t_finds_erased_leaves_the_rest(void)
{
    uint8_t pg[CAIRN_NVS_PAGE_SIZE], ref[CAIRN_NVS_PAGE_SIZE];
    uint16_t off[CAIRN_NVS_ENTRY_COUNT];

    new_page(pg, PAGE_FULL);
    fill(pg, 0, S_WRITTEN, "LIVE-ONE");
    fill(pg, 1, S_ERASED, "SECRET-PASSWORD");
    fill(pg, 2, S_WRITTEN, "LIVE-TWO");
    fill(pg, 3, S_ERASED, "SECRET-KEY-DATA");
    fill(pg, 4, S_ILLEGAL, "HALF-WRITTEN");
    /* slots 5.. are empty and 0xFF */
    memcpy(ref, pg, sizeof pg);

    size_t n = cairn_nvs_dirty_erased_slots(pg, off, CAIRN_NVS_ENTRY_COUNT);
    CHECK(n == 2, "found %zu deleted slots, want 2", n);
    CHECK(off[0] == 64 + 32 * 1 && off[1] == 64 + 32 * 3, "offsets %u %u", off[0], off[1]);

    zero_at(pg, off, n);
    CHECK(!contains(pg, sizeof pg, "SECRET"), "a deleted secret is still in the page");
    /* Everything outside the two slots is byte-identical: live entries, the illegal one,
     * the empty slots, the header and the state table. */
    for (size_t i = 0; i < sizeof pg; i++) {
        int in_slot = (i >= off[0] && i < off[0] + 32u) || (i >= off[1] && i < off[1] + 32u);
        if (!in_slot) CHECK(pg[i] == ref[i], "byte %zu outside the deleted slots changed", i);
    }
    CHECK(contains(pg, sizeof pg, "LIVE-ONE") && contains(pg, sizeof pg, "LIVE-TWO"), "live entry lost");
    return 1;
}

static int t_second_pass_finds_nothing(void)
{
    uint8_t pg[CAIRN_NVS_PAGE_SIZE];
    uint16_t off[CAIRN_NVS_ENTRY_COUNT];
    new_page(pg, PAGE_ACTIVE);
    for (unsigned i = 0; i < 40; i++) fill(pg, i, i % 3 ? S_ERASED : S_WRITTEN, "SECRET-XYZ");
    size_t n = cairn_nvs_dirty_erased_slots(pg, off, CAIRN_NVS_ENTRY_COUNT);
    CHECK(n > 0, "nothing found on a page full of deleted entries");
    zero_at(pg, off, n);
    CHECK(cairn_nvs_dirty_erased_slots(pg, off, CAIRN_NVS_ENTRY_COUNT) == 0, "a second pass found work to do");
    return 1;
}

static int t_clean_slots_are_not_rewritten(void)
{
    uint8_t pg[CAIRN_NVS_PAGE_SIZE];
    uint16_t off[CAIRN_NVS_ENTRY_COUNT];
    new_page(pg, PAGE_FULL);
    set_state(pg, 0, S_ERASED);                 /* deleted, already zero */
    memset(slot(pg, 0), 0x00, 32);
    set_state(pg, 1, S_ERASED);                 /* deleted, never programmed (all 0xFF) */
    CHECK(cairn_nvs_dirty_erased_slots(pg, off, CAIRN_NVS_ENTRY_COUNT) == 0,
          "already-clean deleted slots were offered for a write (flash wear for nothing)");
    return 1;
}

static int t_unused_or_unknown_pages_are_never_written(void)
{
    uint8_t pg[CAIRN_NVS_PAGE_SIZE];
    uint16_t off[CAIRN_NVS_ENTRY_COUNT];
    const uint32_t states[] = { 0xFFFFFFFFu /* uninitialised */, 0xFFFFFFFAu /* corrupt */, 0u, 0x12345678u };
    for (size_t k = 0; k < sizeof states / sizeof states[0]; k++) {
        new_page(pg, states[k]);
        fill(pg, 0, S_ERASED, "SECRET");
        CHECK(cairn_nvs_dirty_erased_slots(pg, off, CAIRN_NVS_ENTRY_COUNT) == 0,
              "page state 0x%08x was treated as a page in use", states[k]);
    }
    new_page(pg, PAGE_FREEING);                 /* mid garbage-collection: still a page in use */
    fill(pg, 0, S_ERASED, "SECRET");
    CHECK(cairn_nvs_dirty_erased_slots(pg, off, CAIRN_NVS_ENTRY_COUNT) == 1, "a freeing page was skipped");
    return 1;
}

static int t_cap_and_bounds(void)
{
    uint8_t pg[CAIRN_NVS_PAGE_SIZE];
    uint16_t off[CAIRN_NVS_ENTRY_COUNT];
    new_page(pg, PAGE_FULL);
    for (unsigned i = 0; i < CAIRN_NVS_ENTRY_COUNT; i++) fill(pg, i, S_ERASED, "SECRET");
    CHECK(cairn_nvs_dirty_erased_slots(pg, off, 3) == 3, "cap not respected");
    CHECK(cairn_nvs_dirty_erased_slots(pg, off, CAIRN_NVS_ENTRY_COUNT) == CAIRN_NVS_ENTRY_COUNT,
          "a page of 126 deleted entries did not report 126");
    for (unsigned i = 0; i < CAIRN_NVS_ENTRY_COUNT; i++) {
        CHECK(off[i] >= 64 && off[i] + 32u <= CAIRN_NVS_PAGE_SIZE && (off[i] - 64) % 32 == 0,
              "offset %u is outside the entry area or misaligned", off[i]);
    }
    CHECK(cairn_nvs_dirty_erased_slots(NULL, off, 1) == 0 && cairn_nvs_dirty_erased_slots(pg, NULL, 1) == 0,
          "NULL arguments were not refused");
    CHECK(cairn_nvs_dirty_erased_slots(pg, off, 0) == 0, "cap 0 returned slots");
    return 1;
}

/* The 126th entry (index 125) is in the last 16-entry word of the table: the one place a
 * word-index slip would show. */
static int t_last_entry_and_word_boundaries(void)
{
    uint8_t pg[CAIRN_NVS_PAGE_SIZE];
    uint16_t off[CAIRN_NVS_ENTRY_COUNT];
    const unsigned idx[] = { 0, 15, 16, 17, 31, 32, 111, 112, 125 };
    new_page(pg, PAGE_FULL);
    for (size_t k = 0; k < sizeof idx / sizeof idx[0]; k++) fill(pg, idx[k], S_ERASED, "SECRET");
    size_t n = cairn_nvs_dirty_erased_slots(pg, off, CAIRN_NVS_ENTRY_COUNT);
    CHECK(n == sizeof idx / sizeof idx[0], "found %zu of %zu", n, sizeof idx / sizeof idx[0]);
    for (size_t k = 0; k < n; k++) CHECK(off[k] == 64 + 32 * idx[k], "slot %u reported at %u", idx[k], off[k]);
    return 1;
}

int main(void)
{
    static const struct { const char *name; int (*fn)(void); } T[] = {
        { "finds deleted slots, leaves everything else byte-identical", t_finds_erased_leaves_the_rest },
        { "a second pass finds nothing", t_second_pass_finds_nothing },
        { "already-clean deleted slots are not rewritten", t_clean_slots_are_not_rewritten },
        { "unused and unknown pages are never written", t_unused_or_unknown_pages_are_never_written },
        { "cap, bounds, alignment, NULL", t_cap_and_bounds },
        { "state-table word boundaries and the last entry", t_last_entry_and_word_boundaries },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        int ok = T[i].fn();
        printf("  %s  %s\n", ok ? "pass" : "FAIL", T[i].name);
        if (ok) s_pass++; else s_fail++;
    }
    printf("nvs wipe: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
