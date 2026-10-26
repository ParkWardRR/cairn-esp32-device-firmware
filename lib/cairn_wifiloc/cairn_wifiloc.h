/*
 * Where the dongle is, from one Wi-Fi scan and the access-point positions the server
 * has learned: the position hint for a cold GNSS receiver.
 *
 * Why. A cold receiver in the M240i drew nothing for the first eight minutes of the
 * drive of 2026-10-25 (home to the first stop), and its only position hint is the last
 * saved fix at +/- 10 km. The dongle already scans Wi-Fi when it has no fix (WIFI_SCAN,
 * format v3 §4.13), and the store learns where each access point is from trusted fixes
 * beside the device's learning scans (v_wifi_ap_learned, store/v1.7). The server hands
 * that table down (GET /api/v2/wifi-anchors, format/v3 §6.4); this finds the scan's
 * access points in it and says where the car is, or says nothing.
 *
 * Saying nothing is the important half. Nothing the receiver is told can be checked:
 * the co-processor passes no UBX reply back (bench, 2026-10-26), and a hint that is
 * wrong by more than its stated accuracy is what u-blox warns degrades a start. So an
 * estimate is given only when the scan is unambiguous:
 *
 *   - two or more known access points, and a strict majority of the known ones agree:
 *     each within CAIRN_WIFILOC_AGREE_M of their signal-weighted centre (weight
 *     10^(dBm/20), as the server learns them), after the ones that do not are dropped;
 *   - or exactly one known access point, heard at CAIRN_WIFILOC_SINGLE_MIN_DBM or
 *     stronger (close enough that its own learned position is the car's);
 *   - nothing else: no known access point, one heard weakly, or a scan whose access
 *     points point at two places.
 *
 * The accuracy given is the largest of CAIRN_WIFILOC_MIN_ACC_M, the worst learned
 * accuracy among the access points used, and their RMS distance from the centre. It is
 * horizontal; the altitude the hint needs is judged separately (cairn_wifiloc_hint).
 *
 * The table on the card ("CWA1", format/v3 §6.4), little-endian:
 *
 *   header 16 B: magic "CWA1", version u16 = 1, record_size u16 (>= 16), count u32,
 *                generated_at u32 (Unix seconds of the newest sighting behind it)
 *   record:      bssid[6], lat_e7 i32, lon_e7 i32, acc_m u16, then record_size - 16
 *                bytes a newer server may add and this reader skips
 *
 * Records are strictly ascending by BSSID, so a lookup is a binary search over the
 * bytes as they lie in PSRAM: nothing is copied or decoded up front.
 *
 * Portable C, host-tested (test/host/wifiloc_test.c).
 */
#ifndef CAIRN_WIFILOC_H
#define CAIRN_WIFILOC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAIRN_WIFILOC_MAGIC       "CWA1"
#define CAIRN_WIFILOC_VERSION     1u
#define CAIRN_WIFILOC_HEADER      16u
#define CAIRN_WIFILOC_RECORD_MIN  16u

/* The server's cap (internal/wifianchors MaxRecords). A longer table is refused, so the
 * download buffer and the PSRAM copy have a bound. */
#define CAIRN_WIFILOC_MAX_RECORDS 4096u
#define CAIRN_WIFILOC_MAX_BYTES   (CAIRN_WIFILOC_HEADER + CAIRN_WIFILOC_RECORD_MIN * CAIRN_WIFILOC_MAX_RECORDS)

/* The acceptance rules above. 300 m is the spread past which the server already refuses
 * to call an access point a place (v_wifi_ap_learned). -60 dBm is roughly ten metres
 * through a wall from a home router: close enough that its position is the car's. */
#define CAIRN_WIFILOC_AGREE_M        300u
#define CAIRN_WIFILOC_SINGLE_MIN_DBM (-60)
#define CAIRN_WIFILOC_MIN_ACC_M      100u

/* The most access points one estimate considers: what a scan record keeps
 * (CAIRN_WIFI_SCAN_MAX_APS). The working set is on the controller's stack, ~500 bytes. */
#define CAIRN_WIFILOC_MAX_OBS 16u

typedef enum {
    CAIRN_WIFILOC_OK = 0,
    CAIRN_WIFILOC_ERR_SHORT,      /* under a header, or not the length the header says */
    CAIRN_WIFILOC_ERR_MAGIC,
    CAIRN_WIFILOC_ERR_VERSION,
    CAIRN_WIFILOC_ERR_RECORD,     /* record_size under 16 */
    CAIRN_WIFILOC_ERR_COUNT,      /* over CAIRN_WIFILOC_MAX_RECORDS */
    CAIRN_WIFILOC_ERR_ORDER,      /* BSSIDs not strictly ascending */
    CAIRN_WIFILOC_ERR_RANGE,      /* a latitude, longitude or accuracy that cannot be */
} cairn_wifiloc_err_t;

const char *cairn_wifiloc_err_name(cairn_wifiloc_err_t e);

typedef struct {
    const uint8_t *records;
    uint32_t       count;
    uint16_t       record_size;
    uint32_t       generated_at;
} cairn_wifiloc_table_t;

/* Check a whole table and point t at it (no copy: buf must outlive t). Any defect
 * refuses the table outright, since one bad record means the bytes are not what the
 * server sent. */
cairn_wifiloc_err_t cairn_wifiloc_parse(const uint8_t *buf, size_t len, cairn_wifiloc_table_t *t);

typedef struct {
    int32_t  lat_e7, lon_e7;
    uint16_t acc_m;
} cairn_wifiloc_anchor_t;

bool cairn_wifiloc_lookup(const cairn_wifiloc_table_t *t, const uint8_t bssid[6],
                          cairn_wifiloc_anchor_t *out);

typedef struct {
    uint8_t bssid[6];
    int8_t  rssi_dbm;
} cairn_wifiloc_obs_t;

typedef enum {
    CAIRN_WIFILOC_LOCATED = 0,
    CAIRN_WIFILOC_NONE_KNOWN,    /* no access point in the scan is in the table */
    CAIRN_WIFILOC_ONE_WEAK,      /* one known, heard below CAIRN_WIFILOC_SINGLE_MIN_DBM */
    CAIRN_WIFILOC_DISAGREE,      /* known ones point at more than one place */
} cairn_wifiloc_verdict_t;

const char *cairn_wifiloc_verdict_name(cairn_wifiloc_verdict_t v);

typedef struct {
    cairn_wifiloc_verdict_t verdict;
    int32_t  lat_e7, lon_e7;     /* set when LOCATED */
    uint32_t acc_m;              /* horizontal, set when LOCATED */
    uint32_t rms_m;              /* of the access points used, from the centre */
    uint8_t  heard;              /* access points in the scan */
    uint8_t  known;              /* of those, in the table */
    uint8_t  used;               /* of those, in the estimate */
} cairn_wifiloc_estimate_t;

/* One scan's access points (any order; a repeated BSSID counts once, at its strongest)
 * against the table. Returns true when LOCATED. */
bool cairn_wifiloc_locate(const cairn_wifiloc_table_t *t, const cairn_wifiloc_obs_t *obs,
                          size_t n, cairn_wifiloc_estimate_t *out);

/* Metres between two points, equirectangular (as the server's views compute spread):
 * within a fraction of a percent at the distances these rules compare. */
double cairn_wifiloc_distance_m(int32_t lat1_e7, int32_t lon1_e7, int32_t lat2_e7, int32_t lon2_e7);

/*
 * The altitude and the 3-D accuracy for the hint. The table has no altitudes, and
 * MGA-INI-POS_LLH wants one with a single accuracy figure. The last saved fix is where
 * the car last was, so its altitude stands in when it is near the estimate, with an
 * allowance for the terrain between them; with none near, sea level and a wide
 * allowance. The allowances are judgement, not measurement, and err loose: a hint some
 * hundreds of metres loose still tells the receiver which satellites are up, while one
 * that overclaims can stall it. acc_cm = sqrt(horizontal^2 + vertical^2).
 */
#define CAIRN_WIFILOC_ALT_NEAR_M    10000u   /* last fix within 10 km: */
#define CAIRN_WIFILOC_ALT_NEAR_ACC_M  200u   /*   its altitude, +/- 200 m */
#define CAIRN_WIFILOC_ALT_FAR_M    100000u   /* within 100 km: */
#define CAIRN_WIFILOC_ALT_FAR_ACC_M  1000u   /*   its altitude, +/- 1 km */
#define CAIRN_WIFILOC_ALT_NONE_ACC_M 3000u   /* none near: sea level, +/- 3 km */

void cairn_wifiloc_hint(const cairn_wifiloc_estimate_t *e, bool have_last_fix,
                        int32_t last_lat_e7, int32_t last_lon_e7, int32_t last_alt_cm,
                        int32_t *alt_cm, uint32_t *acc_cm);

/*
 * When to ask the server for a fresh table: when there is none on the card, or the last
 * successful check (200 or 304) is a day old by the clock, or -- with no clock -- once a
 * boot; never twice within an hour of an attempt this boot, whatever its outcome, so a
 * failing server costs one request an hour at most. Times are Unix seconds, 0 for
 * unknown.
 */
#define CAIRN_WIFILOC_RECHECK_S         (24u * 3600u)
#define CAIRN_WIFILOC_MIN_RETRY_MS      (3600u * 1000u)

bool cairn_wifiloc_fetch_due(bool have_copy, uint32_t last_check_s, uint32_t now_s,
                             bool attempted, uint32_t since_attempt_ms);


/*
 * The card copy is sealed. The table is where the car's owner lives and works, keyed by
 * their routers' BSSIDs, and a card is the part of the dongle easiest to walk away
 * with: the bundles on it are sealed and the card log carries no BSSID
 * (wifiscan_runner.cpp), so this must not be the one plaintext map on it. XChaCha20-
 * Poly1305 (lib/cairn_format) under a key derived from the storage root with HKDF
 * (salt "cairn-wifi-anchors/v1", info "card copy"); AAD the 4-byte magic.
 *
 *   "CWS1" | nonce[24] | tag[16] | ciphertext (the table, as the server sent it)
 *
 * A copy that does not open (another root after re-enrolment, a torn write) is simply
 * not a copy: it is removed and the next session downloads afresh.
 */
#define CAIRN_WIFILOC_CARD_MAGIC    "CWS1"
#define CAIRN_WIFILOC_CARD_OVERHEAD (4u + 24u + 16u)

void   cairn_wifiloc_card_key(const uint8_t root[32], uint8_t key[32]);
/* out must not overlap pt; returns the sealed length, 0 if cap is short. */
size_t cairn_wifiloc_card_seal(const uint8_t key[32], const uint8_t nonce[24], const uint8_t *pt,
                               size_t len, uint8_t *out, size_t cap);
/* The plaintext into pt (cap >= len - overhead); false if it is not a sealed copy under
 * this key. Nothing unauthenticated is written. */
bool   cairn_wifiloc_card_open(const uint8_t key[32], const uint8_t *in, size_t len, uint8_t *pt,
                               size_t cap, size_t *pt_len);

#ifdef __cplusplus
}
#endif

#endif /* CAIRN_WIFILOC_H */
