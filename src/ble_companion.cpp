#include "ble_companion.h"

#if CAIRN_BLE_COMPANION

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <string.h>

#include "cairn_blesec.h"
#include "cairn_engine.h"
#include "cairn_format.h"
#include "cairn_log.h"
#include "config.h"
#include "ble_offload.h"
#include "cairn_devinfo.h"
#include "device_info.h"
#include "facts.h"
#include "sensor_task.h"

#ifndef CAIRN_BLE_PASSKEY
#error "CAIRN_BLE_COMPANION requires CAIRN_BLE_PASSKEY in secrets.h"
#endif

static const char *TAG = "BLE";

static NimBLECharacteristic *s_chr_quality = nullptr;
static NimBLECharacteristic *s_chr_status  = nullptr;
static NimBLEServer         *s_server      = nullptr;

static volatile bool s_connected = false;
static volatile bool s_radio_off = false;
static uint16_t      s_conn_handle = 0xFFFF;

static uint16_t s_last_accepted_seq = 0;
static uint16_t s_accepted_count    = 0;
static uint16_t s_rejected_count    = 0;
static uint16_t s_queue_drop_count  = 0;
static uint16_t s_last_seq          = 0;
static bool     s_have_seq          = false;

static inline int32_t rd_i32(const uint8_t *p) {
    return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 |
                     (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
}

static inline uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

class GnssFixCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *chr, NimBLEConnInfo &info) override {
        NimBLEAttValue val = chr->getValue();
        if (val.size() != 28) {
            s_rejected_count++;
            return;
        }
        const uint8_t *d = val.data();

        uint16_t sample_age_ms = rd_u16(d + 22);
        uint8_t  validity      = d[21];
        uint16_t seq           = rd_u16(d + 24);

        if (sample_age_ms > CAIRN_PHONE_GNSS_STALE_MS) {
            s_rejected_count++;
            return;
        }
        if (!(validity & 0x01)) {
            s_rejected_count++;
            return;
        }
        if (s_have_seq && seq == s_last_seq) {
            s_rejected_count++;
            return;
        }

        fact_t f;
        memset(&f, 0, sizeof(f));
        f.kind = FACT_GNSS_SAMPLE;
        f.monotonic_ms = millis() - sample_age_ms;

        cairn_gnss_sample_t *g = &f.data.gnss;
        g->lat_e7       = rd_i32(d + 0);
        g->lon_e7       = rd_i32(d + 4);
        g->alt_cm       = rd_i32(d + 8);
        g->speed_cmps   = rd_u16(d + 12);
        g->heading_cdeg = rd_u16(d + 14);
        g->h_acc_cm     = rd_u16(d + 16);
        g->v_acc_cm     = rd_u16(d + 18);
        g->fix_type     = d[20];
        g->sats_used    = 0xFF;
        g->sats_visible = 0xFF;
        g->hdop_e2      = CAIRN_U16_UNKNOWN;
        g->source_flags = CAIRN_SOURCE_PHONE;
        g->utc_offset_ms = 0;
        g->utc_acc_ms    = CAIRN_U16_UNKNOWN;

        if (sensor_task_post_fact(&f)) {
            s_last_seq          = seq;
            s_have_seq          = true;
            s_last_accepted_seq = seq;
            s_accepted_count++;
        } else {
            s_queue_drop_count++;
        }
    }
};

class EngineDeclCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *chr, NimBLEConnInfo &info) override {
        NimBLEAttValue val = chr->getValue();
        if (val.size() == 0 || val.size() > 31) {
            CAIRN_LOGW(TAG, "engine declaration: bad length %d", (int)val.size());
            return;
        }

        char engine_id[32];
        memcpy(engine_id, val.data(), val.size());
        engine_id[val.size()] = '\0';

        bool known = cairn_engine_set_ble_declaration(engine_id);
        CAIRN_LOGI(TAG, "companion declared engine \"%s\" (%s)",
                   engine_id, known ? "known" : "unknown");
    }
};

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *server, NimBLEConnInfo &info) override {
        s_conn_handle       = info.getConnHandle();
        s_connected         = true;
        s_last_accepted_seq = 0;
        s_accepted_count    = 0;
        s_rejected_count    = 0;
        s_queue_drop_count  = 0;
        s_last_seq          = 0;
        s_have_seq          = false;
        ble_offload_on_connect(s_conn_handle);
        CAIRN_LOGI(TAG, "companion connected");
    }

    void onDisconnect(NimBLEServer *server, NimBLEConnInfo &info,
                      int reason) override {
        s_connected   = false;
        s_conn_handle = 0xFFFF;
        cairn_engine_set_ble_declaration(NULL); /* clear stale engine declaration */
        ble_offload_on_disconnect();
        CAIRN_LOGI(TAG, "companion disconnected (reason %d)", reason);
        if (!s_radio_off)
            NimBLEDevice::startAdvertising();
    }

    void onMTUChange(uint16_t mtu, NimBLEConnInfo &info) override {
        ble_offload_on_mtu(mtu);
    }

    void onAuthenticationComplete(NimBLEConnInfo &info) override {
        if (!info.isEncrypted()) {
            CAIRN_LOGW(TAG, "pairing failed; disconnecting");
            s_server->disconnect(info.getConnHandle());
            return;
        }
        CAIRN_LOGI(TAG, "paired: encrypted=%d bonded=%d",
                   info.isEncrypted(), info.isBonded());
    }
};

/* DEVICE_INFO is built when it is read, so it reports the state at that moment (boot timing
 * that was not yet known on the last read, a card that has since mounted). */
class DeviceInfoCallbacks : public NimBLECharacteristicCallbacks {
    void onRead(NimBLECharacteristic *chr, NimBLEConnInfo &info) override {
        static uint8_t buf[CAIRN_DI_MAX_LEN];
        size_t n = device_info_build(buf, sizeof buf);
        if (n > 0) chr->setValue(buf, n);
    }
};

static ServerCallbacks    s_server_cbs;
static DeviceInfoCallbacks s_device_info_cbs;
static GnssFixCallbacks   s_gnss_fix_cbs;
static EngineDeclCallbacks s_engine_decl_cbs;

/* Bonds survive a reboot (a phone keeps its half; forgetting ours at every ignition leaves it
 * with a key the dongle no longer recognises, and an iPhone then refuses until the person
 * forgets the dongle in Settings). They are cleared only when the pairing configuration they
 * were made under has changed, which is what a stale bond after a reflash actually is. */
static void clear_bonds_if_pairing_changed(uint8_t authreq, uint8_t io_cap, uint32_t passkey)
{
    const uint32_t current = cairn_ble_security_fingerprint(authreq, io_cap, passkey);
    Preferences prefs;
    if (!prefs.begin("cairn_ble", false)) {
        /* Cannot read the record: clearing is the safe side, and it is what the dev build did. */
        ble_companion_clear_bonds();
        return;
    }
    const uint32_t stored = prefs.getUInt("pair_fp", 0);
    if (cairn_ble_bonds_stale(stored, current)) {
        ble_companion_clear_bonds();
        prefs.putUInt("pair_fp", current);
        CAIRN_LOGI(TAG, "pairing configuration changed: bonds cleared");
    } else {
        CAIRN_LOGI(TAG, "pairing configuration unchanged: %d bond(s) kept", NimBLEDevice::getNumBonds());
    }
    prefs.end();
}

void ble_companion_clear_bonds(void)
{
    int count = NimBLEDevice::getNumBonds();
    for (int i = count - 1; i >= 0; i--)
        NimBLEDevice::deleteBond(NimBLEDevice::getBondedAddress(i));
    if (count > 0)
        CAIRN_LOGI(TAG, "cleared %d bond(s)", count);
}

bool ble_companion_begin(void)
{
    NimBLEDevice::init(CAIRN_BLE_NAME);
    NimBLEDevice::setMTU(185);

    /* Dev build: Just Works (SC without MITM). Encryption still negotiates, so the
     * READ_ENC/WRITE_ENC characteristics still work, but macOS does not need to accept a
     * passkey dialog. The receipt gate (Ed25519 at the application layer, verified in
     * ble_offload against CAIRN_SERVER_RECEIPT_KEY_HEX) is unaffected. Flip back to MITM +
     * DISPLAY_ONLY before shipping. */
    const uint8_t authreq = BLE_SM_PAIR_AUTHREQ_BOND | BLE_SM_PAIR_AUTHREQ_SC;
    const uint8_t io_cap  = BLE_HS_IO_NO_INPUT_OUTPUT;
    NimBLEDevice::setSecurityAuth(authreq);
    NimBLEDevice::setSecurityIOCap(io_cap);
    NimBLEDevice::setSecurityPasskey(CAIRN_BLE_PASSKEY);

    /* NimBLE persists bonds in NVS (CONFIG_BT_NIMBLE_MAX_BONDS=1). A reflash that changed the
     * passkey or the auth mode leaves a bond keyed to the old secret, which macOS tries to
     * re-encrypt with before any dialog, so the pair fails before the user is prompted: clear
     * bonds when (and only when) the pairing configuration changed. */
    clear_bonds_if_pairing_changed(authreq, io_cap, (uint32_t)CAIRN_BLE_PASSKEY);

    s_server = NimBLEDevice::createServer();
    s_server->setCallbacks(&s_server_cbs);

    NimBLEService *svc = s_server->createService(
        "A8E30000-4F5B-11EF-A017-325096B39F47");

    NimBLECharacteristic *chr_version = svc->createCharacteristic(
        "A8E300F0-4F5B-11EF-A017-325096B39F47",
        NIMBLE_PROPERTY::READ |
        NIMBLE_PROPERTY::READ_ENC);
    uint8_t ver[2] = {1, 0};   /* capabilities: bit 2 = bundle offload, set below */

    NimBLECharacteristic *chr_fix = svc->createCharacteristic(
        "A8E30001-4F5B-11EF-A017-325096B39F47",
        NIMBLE_PROPERTY::WRITE_NR |
        NIMBLE_PROPERTY::WRITE_ENC);
    chr_fix->setCallbacks(&s_gnss_fix_cbs);

    NimBLECharacteristic *chr_engine = svc->createCharacteristic(
        "A8E30004-4F5B-11EF-A017-325096B39F47",
        NIMBLE_PROPERTY::WRITE_NR |
        NIMBLE_PROPERTY::WRITE_ENC |
        NIMBLE_PROPERTY::WRITE_AUTHEN);
    chr_engine->setCallbacks(&s_engine_decl_cbs);

    s_chr_quality = svc->createCharacteristic(
        "A8E30010-4F5B-11EF-A017-325096B39F47",
        NIMBLE_PROPERTY::NOTIFY |
        NIMBLE_PROPERTY::READ_ENC);

    s_chr_status = svc->createCharacteristic(
        "A8E30011-4F5B-11EF-A017-325096B39F47",
        NIMBLE_PROPERTY::NOTIFY |
        NIMBLE_PROPERTY::READ_ENC);

    /* Capabilities: bit 2 = bundle offload (contracts/ble/v1/offload.md). Advertised only
     * if the offload task actually started. */
    uint32_t caps = CAIRN_CAP_PHONE_GNSS;
    if (ble_offload_register(svc)) {
        ver[1] |= 0x04;
        caps |= CAIRN_CAP_OFFLOAD;
    } else {
        CAIRN_LOGE(TAG, "bundle offload unavailable");
    }

    /* Device information (contracts/ble/v1/device-info.md): capability bit 3, and the
     * characteristic itself. The uplink event, instruction and home-trigger characteristics
     * (0041 to 0044) are not created: nothing sends or accepts them yet, and a capability bit
     * for something absent would be a lie the app is told to trust. */
    NimBLECharacteristic *chr_info = svc->createCharacteristic(
        "A8E30040-4F5B-11EF-A017-325096B39F47",
        NIMBLE_PROPERTY::READ |
        NIMBLE_PROPERTY::READ_ENC);
    chr_info->setCallbacks(&s_device_info_cbs);
    ver[1] |= 0x08;
    device_info_set_capabilities(caps);

    chr_version->setValue(ver, sizeof(ver));

    s_server->start();

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID("A8E30000-4F5B-11EF-A017-325096B39F47");
    adv->setName(CAIRN_BLE_NAME);
    adv->enableScanResponse(true);
    adv->start();

    CAIRN_LOGI(TAG, "BLE companion started, advertising as '%s'",
               CAIRN_BLE_NAME);
    return true;
}

bool ble_companion_connected(void) { return s_connected; }

void ble_companion_notify_quality(uint8_t fix_type, uint8_t sats_used,
                                  uint16_t hdop_e2, uint32_t fix_age_ms)
{
    if (!s_connected || !s_chr_quality) return;

    uint8_t buf[8];
    buf[0] = fix_type;
    buf[1] = sats_used;
    buf[2] = (uint8_t)(hdop_e2);
    buf[3] = (uint8_t)(hdop_e2 >> 8);
    buf[4] = (uint8_t)(fix_age_ms);
    buf[5] = (uint8_t)(fix_age_ms >> 8);
    buf[6] = (uint8_t)(fix_age_ms >> 16);
    buf[7] = (uint8_t)(fix_age_ms >> 24);

    s_chr_quality->setValue(buf, sizeof(buf));
    s_chr_quality->notify();
}

void ble_companion_notify_status(void)
{
    if (!s_connected || !s_chr_status) return;

    uint8_t buf[8];
    buf[0] = (uint8_t)(s_last_accepted_seq);
    buf[1] = (uint8_t)(s_last_accepted_seq >> 8);
    buf[2] = (uint8_t)(s_accepted_count);
    buf[3] = (uint8_t)(s_accepted_count >> 8);
    buf[4] = (uint8_t)(s_rejected_count);
    buf[5] = (uint8_t)(s_rejected_count >> 8);
    buf[6] = (uint8_t)(s_queue_drop_count);
    buf[7] = (uint8_t)(s_queue_drop_count >> 8);

    s_chr_status->setValue(buf, sizeof(buf));
    s_chr_status->notify();
}

void ble_companion_radio_off(void)
{
    if (s_radio_off) return;
    s_radio_off = true;

    if (s_connected && s_conn_handle != 0xFFFF) {
        s_server->disconnect(s_conn_handle);
    }
    NimBLEDevice::getAdvertising()->stop();
    CAIRN_LOGI(TAG, "BLE radio released");
}

void ble_companion_radio_on(void)
{
    if (!s_radio_off) return;
    s_radio_off = false;

    NimBLEDevice::getAdvertising()->start();
    CAIRN_LOGI(TAG, "BLE advertising resumed");
}

#endif /* CAIRN_BLE_COMPANION */
