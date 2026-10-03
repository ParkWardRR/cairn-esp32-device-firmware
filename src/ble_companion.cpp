#include "ble_companion.h"

#if CAIRN_BLE_COMPANION

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <string.h>

#include "cairn_format.h"
#include "cairn_log.h"
#include "config.h"
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

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *server, NimBLEConnInfo &info) override {
        s_connected         = true;
        s_last_accepted_seq = 0;
        s_accepted_count    = 0;
        s_rejected_count    = 0;
        s_queue_drop_count  = 0;
        s_last_seq          = 0;
        s_have_seq          = false;
        CAIRN_LOGI(TAG, "companion connected");
    }

    void onDisconnect(NimBLEServer *server, NimBLEConnInfo &info,
                      int reason) override {
        s_connected = false;
        CAIRN_LOGI(TAG, "companion disconnected (reason %d)", reason);
        NimBLEDevice::startAdvertising();
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

static ServerCallbacks    s_server_cbs;
static GnssFixCallbacks   s_gnss_fix_cbs;

bool ble_companion_begin(void)
{
    NimBLEDevice::init(CAIRN_BLE_NAME);
    NimBLEDevice::setMTU(185);

    NimBLEDevice::setSecurityAuth(BLE_SM_PAIR_AUTHREQ_BOND |
                                  BLE_SM_PAIR_AUTHREQ_MITM |
                                  BLE_SM_PAIR_AUTHREQ_SC);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
    NimBLEDevice::setSecurityPasskey(CAIRN_BLE_PASSKEY);

    s_server = NimBLEDevice::createServer();
    s_server->setCallbacks(&s_server_cbs);

    NimBLEService *svc = s_server->createService(
        "A8E30000-4F5B-11EF-A017-325096B39F47");

    NimBLECharacteristic *chr_version = svc->createCharacteristic(
        "A8E300F0-4F5B-11EF-A017-325096B39F47",
        NIMBLE_PROPERTY::READ |
        NIMBLE_PROPERTY::READ_ENC |
        NIMBLE_PROPERTY::READ_AUTHEN);
    uint8_t ver[2] = {1, 0};
    chr_version->setValue(ver, sizeof(ver));

    NimBLECharacteristic *chr_fix = svc->createCharacteristic(
        "A8E30001-4F5B-11EF-A017-325096B39F47",
        NIMBLE_PROPERTY::WRITE_NR |
        NIMBLE_PROPERTY::WRITE_ENC |
        NIMBLE_PROPERTY::WRITE_AUTHEN);
    chr_fix->setCallbacks(&s_gnss_fix_cbs);

    s_chr_quality = svc->createCharacteristic(
        "A8E30010-4F5B-11EF-A017-325096B39F47",
        NIMBLE_PROPERTY::NOTIFY |
        NIMBLE_PROPERTY::READ_ENC |
        NIMBLE_PROPERTY::READ_AUTHEN);

    s_chr_status = svc->createCharacteristic(
        "A8E30011-4F5B-11EF-A017-325096B39F47",
        NIMBLE_PROPERTY::NOTIFY |
        NIMBLE_PROPERTY::READ_ENC |
        NIMBLE_PROPERTY::READ_AUTHEN);

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

#endif /* CAIRN_BLE_COMPANION */
