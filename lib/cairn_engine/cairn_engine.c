#include "cairn_engine.h"

#include <string.h>

#include "config.h"

/*
 * The tables. `make firmware ENGINES=...` writes cairn_engines_sel.h into build/engines/
 * and puts that directory on the include path, so the image carries exactly the
 * selection. A plain `pio run` (and the host tests) find no such file and use the
 * committed copy for ALL engines, which `make engines-check` keeps in step with
 * the engines/ YAML files.
 */
#if defined(__has_include)
#if __has_include("cairn_engines_sel.h")
#include "cairn_engines_sel.h"
#else
#include "gen/cairn_engines_gen.h"
#endif
#else
#include "gen/cairn_engines_gen.h"
#endif

/* ── identity and registry ────────────────────────────────────────────────── */

const cairn_engine_identity_t *cairn_engine_identity(void)
{
    return &k_engine_identity;
}

size_t cairn_engine_installed_count(void)
{
    return sizeof(k_engine_profiles) / sizeof(k_engine_profiles[0]);
}

const cairn_engine_profile_t *cairn_engine_installed(size_t i)
{
    return i < cairn_engine_installed_count() ? k_engine_profiles[i] : NULL;
}

const cairn_engine_profile_t *cairn_engine_find(const char *id)
{
    size_t i;
    if (id == NULL) return NULL;
    for (i = 0; i < cairn_engine_installed_count(); i++) {
        if (strcmp(k_engine_profiles[i]->id, id) == 0) return k_engine_profiles[i];
    }
    return NULL;
}

/* ── the active profile and its resolved values ──────────────────────────── */

static const cairn_engine_profile_t *s_active;
static cairn_engine_params_t         s_params;

static void resolve(const cairn_engine_profile_t *p)
{
    cairn_engine_params_t r;
    uint32_t k = (p != NULL) ? p->known : 0;

    /* The defaults are what the firmware did before profiles existed (config.h). */
    r.obd_period_ms          = CAIRN_OBD_PERIOD_MS;
    r.obd_batch_period_ms    = CAIRN_OBD_BATCH_PERIOD_MS;
    r.cold_slots             = 1;
    r.engine_on_mv           = CAIRN_ENGINE_ON_MV;
    r.standby_idle_ms        = CAIRN_STANDBY_IDLE_MS;
    r.standby_heartbeat_ms   = CAIRN_STANDBY_HEARTBEAT_MS;
    r.drive_voltage_dwell_ms = CAIRN_DRIVE_VOLTAGE_DWELL_MS;
    r.drive_motion_dwell_ms  = CAIRN_DRIVE_MOTION_DWELL_MS;
    r.drive_both_dwell_ms    = CAIRN_DRIVE_BOTH_DWELL_MS;
    r.from_profile           = 0;

    if (k & CAIRN_K_OBD_PERIOD)          r.obd_period_ms = p->obd_period_ms;
    if (k & CAIRN_K_OBD_BATCH_PERIOD)    r.obd_batch_period_ms = p->obd_batch_period_ms;
    if ((k & CAIRN_K_COLD_SLOTS) && p->cold_slots > 0) r.cold_slots = p->cold_slots;
    if (k & CAIRN_K_ENGINE_ON_MV)        r.engine_on_mv = p->engine_on_mv;
    if (k & CAIRN_K_STANDBY_IDLE)        r.standby_idle_ms = p->standby_idle_ms;
    if (k & CAIRN_K_STANDBY_HEARTBEAT)   r.standby_heartbeat_ms = p->standby_heartbeat_ms;
    if (k & CAIRN_K_DRIVE_VOLTAGE_DWELL) r.drive_voltage_dwell_ms = p->drive_voltage_dwell_ms;
    if (k & CAIRN_K_DRIVE_MOTION_DWELL)  r.drive_motion_dwell_ms = p->drive_motion_dwell_ms;
    if (k & CAIRN_K_DRIVE_BOTH_DWELL)    r.drive_both_dwell_ms = p->drive_both_dwell_ms;

    r.from_profile = k & (CAIRN_K_OBD_PERIOD | CAIRN_K_OBD_BATCH_PERIOD |
                          CAIRN_K_COLD_SLOTS | CAIRN_K_ENGINE_ON_MV |
                          CAIRN_K_STANDBY_IDLE | CAIRN_K_STANDBY_HEARTBEAT |
                          CAIRN_K_DRIVE_VOLTAGE_DWELL | CAIRN_K_DRIVE_MOTION_DWELL |
                          CAIRN_K_DRIVE_BOTH_DWELL);
    if ((k & CAIRN_K_COLD_SLOTS) && p->cold_slots == 0) {
        r.from_profile &= ~CAIRN_K_COLD_SLOTS;
    }

    s_params = r;
    s_active = p;
}

void cairn_engine_select_default(void)
{
    const cairn_engine_profile_t *pick = NULL;
    size_t i;

    for (i = 0; i < cairn_engine_installed_count(); i++) {
        if (k_engine_profiles[i]->status != CAIRN_ENGINE_STUB) {
            pick = k_engine_profiles[i];
            break;
        }
    }
    if (pick == NULL) pick = cairn_engine_installed(0);
    resolve(pick);
}

bool cairn_engine_select(const char *id)
{
    const cairn_engine_profile_t *p = cairn_engine_find(id);
    if (p == NULL) return false;
    resolve(p);
    return true;
}

const cairn_engine_profile_t *cairn_engine_active(void)
{
    /* Normally already chosen at boot; this covers a caller that got here first. */
    if (s_active == NULL) cairn_engine_select_default();
    return s_active;
}

const cairn_engine_params_t *cairn_engine_params(void)
{
    if (s_active == NULL) cairn_engine_select_default();
    return &s_params;
}

bool cairn_engine_has_pids(void)
{
    const cairn_engine_profile_t *p = cairn_engine_active();
    return p != NULL && (p->known & CAIRN_K_PIDS) != 0 && p->n_pids > 0;
}

/* ── PIDs ─────────────────────────────────────────────────────────────────── */

const cairn_pid_t *cairn_engine_pid_for_field(const cairn_engine_profile_t *p,
                                              cairn_field_t f)
{
    uint8_t i;
    if (p == NULL || p->pids == NULL) return NULL;
    for (i = 0; i < p->n_pids; i++) {
        if (p->pids[i].field == (uint8_t)f) return &p->pids[i];
    }
    return NULL;
}

cairn_expr_status_t cairn_engine_eval_pid(const cairn_engine_profile_t *p,
                                          const cairn_pid_t *pid,
                                          const uint8_t data[4], int32_t *out)
{
    if (p == NULL || pid == NULL || p->code == NULL) return CAIRN_EXPR_ERR_ARG;
    return cairn_expr_eval(p->code + pid->code_off, pid->code_len, data, out);
}

size_t cairn_engine_batch_request(const cairn_engine_profile_t *p, char *buf,
                                  size_t cap)
{
    static const char hexd[] = "0123456789ABCDEF";
    size_t k = 0;
    uint8_t i;

    if (p == NULL || buf == NULL || p->pids == NULL || p->n_hot == 0) return 0;
    /* "01", two digits per PID, '\r', NUL */
    if (cap < 2u + 2u * p->n_hot + 2u) return 0;

    buf[k++] = '0';
    buf[k++] = '1';
    for (i = 0; i < p->n_hot; i++) {
        uint8_t pid = (uint8_t)p->pids[i].pid;
        buf[k++] = hexd[pid >> 4];
        buf[k++] = hexd[pid & 0x0F];
    }
    buf[k++] = '\r';
    buf[k]   = '\0';
    return k;
}

bool cairn_engine_batch_parse(const cairn_engine_profile_t *p, const uint8_t *bytes,
                              size_t n, int32_t values[CAIRN_FIELD_COUNT],
                              uint32_t *present)
{
    size_t  expected = 0;
    size_t  pos = 0;
    uint8_t i;
    int     f;

    if (p == NULL || bytes == NULL || values == NULL || present == NULL ||
        p->pids == NULL || p->n_hot == 0) {
        return false;
    }

    for (f = 0; f < CAIRN_FIELD_COUNT; f++) values[f] = 0;
    *present = 0;

    for (i = 0; i < p->n_hot; i++) expected += 1u + p->pids[i].nbytes;
    if (n < expected) return false;

    for (i = 0; i < p->n_hot; i++) {
        const cairn_pid_t *pid = &p->pids[i];
        uint8_t data[4] = { 0, 0, 0, 0 };
        int32_t v;
        uint8_t j;

        if (bytes[pos] != (uint8_t)pid->pid) return false;
        pos++;
        for (j = 0; j < pid->nbytes && j < 4; j++) data[j] = bytes[pos++];

        if (cairn_engine_eval_pid(p, pid, data, &v) != CAIRN_EXPR_OK) return false;
        if (pid->field != CAIRN_FIELD_NONE && pid->field < CAIRN_FIELD_COUNT) {
            values[pid->field] = v;
            *present |= 1u << pid->field;
        }
    }
    return true;
}

/* ── vehicle gate ─────────────────────────────────────────────────────────── */

const char *cairn_vehicle_verdict_name(cairn_vehicle_verdict_t v)
{
    switch (v) {
    case CAIRN_VEHICLE_UNIDENTIFIED:            return "UNIDENTIFIED";
    case CAIRN_VEHICLE_SERVED:                  return "SERVED";
    case CAIRN_VEHICLE_REFUSED_NOT_INSTALLED:   return "REFUSED_NOT_INSTALLED";
    case CAIRN_VEHICLE_REFUSED_UNKNOWN_ENGINE:  return "REFUSED_UNKNOWN_ENGINE";
    default:                                    return "?";
    }
}

bool cairn_vin_matches(const char *pattern, const char *vin)
{
    size_t i;
    if (pattern == NULL || vin == NULL) return false;
    if (strlen(pattern) != 17 || strlen(vin) != 17) return false;
    for (i = 0; i < 17; i++) {
        if (pattern[i] != '?' && pattern[i] != vin[i]) return false;
    }
    return true;
}

static const cairn_engine_catalogue_entry_t *cat_find(
    const cairn_engine_catalogue_entry_t *cat, size_t n, const char *id)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (strcmp(cat[i].id, id) == 0) return &cat[i];
    }
    return NULL;
}

cairn_vehicle_verdict_t cairn_engine_check_vehicle_in(
    const cairn_engine_catalogue_entry_t *cat, size_t n, const char *declared_id,
    const char *vin, const char **engine_id)
{
    const cairn_engine_catalogue_entry_t *hit = NULL;
    size_t i, j;

    if (engine_id != NULL) *engine_id = NULL;

    if (declared_id != NULL && declared_id[0] != '\0') {
        hit = cat_find(cat, n, declared_id);
        if (hit == NULL) return CAIRN_VEHICLE_REFUSED_UNKNOWN_ENGINE;
        if (engine_id != NULL) *engine_id = hit->id;
        return hit->installed ? CAIRN_VEHICLE_SERVED
                              : CAIRN_VEHICLE_REFUSED_NOT_INSTALLED;
    }

    if (vin == NULL || strlen(vin) != 17) return CAIRN_VEHICLE_UNIDENTIFIED;

    for (i = 0; i < n; i++) {
        for (j = 0; j < cat[i].n_vin_patterns; j++) {
            if (cairn_vin_matches(cat[i].vin_patterns[j], vin)) {
                if (hit != NULL && hit != &cat[i]) {
                    /* Two engines claim this VIN. A contradiction is no evidence. */
                    return CAIRN_VEHICLE_UNIDENTIFIED;
                }
                hit = &cat[i];
            }
        }
    }
    if (hit == NULL) return CAIRN_VEHICLE_UNIDENTIFIED;

    if (engine_id != NULL) *engine_id = hit->id;
    return hit->installed ? CAIRN_VEHICLE_SERVED
                          : CAIRN_VEHICLE_REFUSED_NOT_INSTALLED;
}

cairn_vehicle_verdict_t cairn_engine_check_vehicle(const char *declared_id,
                                                   const char *vin,
                                                   const char **engine_id)
{
    return cairn_engine_check_vehicle_in(
        k_engine_catalogue,
        sizeof(k_engine_catalogue) / sizeof(k_engine_catalogue[0]), declared_id, vin,
        engine_id);
}
