/*
 * The capture lifecycle, as four independent regions.
 *
 * Regions are independent on purpose. A single flat state machine forces false
 * couplings — losing the network should not end a trip, and a degraded sensor
 * should not stop capture — and the v1 firmware's single enum is what made
 * those couplings hard to see. Each region here transitions on its own
 * evidence, and every transition is recorded as a STATE_TRANSITION frame
 * carrying the policy version in force, so a decision in the data can always be
 * explained later.
 *
 * The journal chain carries transitions and health; the capture chain carries
 * sensor data. They are separate chains (spec §3.2.1), so a health record
 * written while a trip is idle does not look like a gap in the capture
 * sequence.
 */

#ifndef CAIRN_LIFECYCLE_H
#define CAIRN_LIFECYCLE_H

#include <stdbool.h>
#include <stdint.h>

#include "cairn_store.h"
#include "preroll.h"
#include "sensors.h"

/* Region 1: capture. What the device believes the vehicle is doing. */
enum class CaptureState : uint8_t {
    Idle      = 0,  /* stationary, nothing being recorded to the capture chain */
    Pretrip   = 1,  /* motion suspected; samples kept and flagged PRETRIP */
    Active    = 2,  /* a trip is underway */
    Trailing  = 3,  /* motion stopped; waiting out the dwell before sealing */
};

/* Region 2: bundle. The state of the open capture's storage. */
enum class BundleState : uint8_t {
    Open      = 0,
    Sealing   = 1,
    Sealed    = 2,
};

/* Region 3: connectivity. Independent of whether a trip is in progress. */
enum class LinkState : uint8_t {
    Offline     = 0,
    Associating = 1,
    Online      = 2,
    Syncing     = 3,
};

/*
 * Region 4: health. Degradation is recorded, never a reason to stop capturing.
 *
 * Carried as the specification's bitmap (§4.10) rather than a severity, because
 * degradation is not ordered: a low battery and a missing fix and a full card
 * are different problems with different fixes, and a scalar would force a
 * priority between them and discard the rest. See CAIRN_HEALTH_* in
 * cairn_format.h.
 */

struct Lifecycle {
    CaptureState capture = CaptureState::Idle;
    BundleState  bundle  = BundleState::Open;
    LinkState    link    = LinkState::Offline;

    /* Bitmap of active degraded conditions; CAIRN_HEALTH_OK when none are. */
    uint8_t      health_state = CAIRN_HEALTH_OK;

    cairn_capture_t cap;
    SensorStatus    sensors;

    /*
     * Records captured before a trip is confirmed. Flushed with
     * CAIRN_FLAG_PRETRIP on confirmation, dropped if the motion does not
     * persist — so a parked car accumulates nothing while the first seconds of
     * a real drive are still recorded.
     */
    cairn_preroll_t preroll;

    uint8_t device_id[16];
    uint8_t boot_id[16];
    uint8_t key_seed[32];
    uint8_t key_public[32];
    uint32_t boot_count;

    /*
     * Last-known sensor values, as reported by facts. The controller keeps its
     * own copies rather than reading driver state the sensing task owns, and
     * the have_* flags matter: a stale fix or a silent ECU must stop
     * contributing to the motion score rather than vouching for it forever.
     */
    cairn_gnss_sample_t  last_gnss = {};
    cairn_obd_snapshot_t last_obd = {};
    bool     have_recent_gnss = false;
    bool     have_recent_obd = false;
    uint16_t last_accel_rms_mg = 0;

    /* Dropped facts already mentioned in the log, so the warning is not
     * repeated every health interval. */
    uint32_t reported_drops = 0;

    /* Motion scoring. Hundredths, matching the STATE_TRANSITION payload. */
    uint16_t start_score_e2 = 0;
    uint16_t stop_score_e2  = 0;
    uint32_t motion_since_ms = 0;
    uint32_t still_since_ms  = 0;

    /* GNSS gap accounting, so a gap is recorded as a gap. */
    uint32_t gnss_gap_started_ms = 0;
    uint32_t gnss_expected_in_gap = 0;
    bool     in_gnss_gap = false;

    /* The UTC basis, once the receiver provides one. */
    bool     have_utc_basis = false;

    uint32_t idle_since_ms = 0;

    /* Sealed bundles awaiting a receipt, refreshed before each sync attempt.
     * Being offline matters only when something is waiting to go. */
    uint32_t pending_bundles = 0;
};

bool lifecycle_begin(Lifecycle *lc);

/* One pass. Non-blocking; call continuously from loop(). */
void lifecycle_tick(Lifecycle *lc);

const char *capture_state_name(CaptureState s);
const char *link_state_name(LinkState s);

#endif /* CAIRN_LIFECYCLE_H */
