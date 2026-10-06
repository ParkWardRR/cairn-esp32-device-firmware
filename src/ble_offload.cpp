#include "ble_offload.h"

#if CAIRN_BLE_COMPANION

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <string.h>

#include "cairn_log.h"
#include "cairn_offload.h"

static const char *TAG = "OFFLOAD";

/*
 * Why a queue and a task rather than doing the work in the write callback.
 *
 * The callbacks run on NimBLE's host task, whose stack is a few kilobytes. A
 * LIST reads manifests off the card, and a receipt is verified with Ed25519; both
 * need far more than that, and a long card read there would stall the radio. So a
 * callback copies the bytes into a queue and returns, and one task with its own
 * stack runs the module. That also means the module is only ever touched from one
 * task, so it needs no locking.
 */

enum : uint8_t { EV_CONTROL = 1, EV_DATA, EV_CONNECT, EV_DISCONNECT, EV_MTU };

struct Event {
    uint8_t  kind;
    uint8_t  len;
    uint16_t value;                                /* conn handle or MTU */
    uint8_t  data[CAIRN_OFFLOAD_MAX_PAYLOAD + 4];
};

static QueueHandle_t         s_queue = nullptr;
static NimBLECharacteristic *s_chr_control = nullptr;
static NimBLECharacteristic *s_chr_data = nullptr;
static bool                (*s_trip_active)(void) = nullptr;
static cairn_offload_t       s_offload;
static uint8_t               s_pinned[32];
static bool                  s_have_pinned = false;
static uint16_t              s_conn = BLE_HS_CONN_HANDLE_NONE;

/* Read by the lifecycle from another task; written here. */
static volatile bool     s_connected = false;
static volatile bool     s_busy = false;
static volatile uint32_t s_last_activity_ms = 0;
static volatile uint32_t s_session_start_ms = 0;

/* A phone must show activity at least this often, and may hold standby off for
 * at most this long in total, per connection. */
static const uint32_t ACTIVITY_WINDOW_MS = 120000;
static const uint32_t SESSION_CAP_MS     = 15UL * 60UL * 1000UL;

/* ── io ───────────────────────────────────────────────────────────────────── */

static bool io_indicate(void *, const uint8_t *buf, size_t len)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || s_chr_control == nullptr) return false;
    return s_chr_control->indicate(buf, len, s_conn);
}

static bool io_notify(void *, const uint8_t *buf, size_t len)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || s_chr_data == nullptr) return false;
    return s_chr_data->notify(buf, len, s_conn);
}

static uint32_t io_now(void *) { return millis(); }

static bool io_trip(void *)
{
    /* Unknown means active: refusing a transfer is the safe direction. */
    return s_trip_active == nullptr ? true : s_trip_active();
}

static bool unhex32(const char *hex, uint8_t out[32])
{
    if (strlen(hex) != 64) return false;
    for (int i = 0; i < 32; i++) {
        char b[3] = { hex[2 * i], hex[2 * i + 1], 0 };
        char *end;
        unsigned v = (unsigned)strtoul(b, &end, 16);
        if (*end != '\0') return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

/* ── the task ─────────────────────────────────────────────────────────────── */

static void handle(const Event &ev)
{
    switch (ev.kind) {
    case EV_CONNECT:
        s_conn = ev.value;
        cairn_offload_on_disconnect(&s_offload);
        cairn_offload_set_mtu(&s_offload, 23);
        break;
    case EV_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        cairn_offload_on_disconnect(&s_offload);
        break;
    case EV_MTU:
        cairn_offload_set_mtu(&s_offload, ev.value);
        CAIRN_LOGI(TAG, "ATT MTU %u", (unsigned)ev.value);
        break;
    case EV_CONTROL:
        cairn_offload_on_control_write(&s_offload, ev.data, ev.len);
        break;
    case EV_DATA:
        cairn_offload_on_data_write(&s_offload, ev.data, ev.len);
        break;
    }
}

static void offload_task(void *)
{
    for (;;) {
        Event ev;
        /* Poll quickly while there is work to push, lazily otherwise. */
        TickType_t wait = cairn_offload_busy(&s_offload) ? 1 : pdMS_TO_TICKS(25);
        if (xQueueReceive(s_queue, &ev, wait) == pdTRUE) {
            handle(ev);
            for (int more = 0; more < 8 && xQueueReceive(s_queue, &ev, 0) == pdTRUE; more++) handle(ev);
        }
        cairn_offload_pump(&s_offload);
        s_busy = cairn_offload_busy(&s_offload);
    }
}

/* ── callbacks (host task: enqueue and return) ────────────────────────────── */

static void post(uint8_t kind, uint16_t value, const uint8_t *data, size_t len)
{
    Event ev;
    memset(&ev, 0, sizeof(ev.kind) + sizeof(ev.len) + sizeof(ev.value));
    ev.kind = kind;
    ev.value = value;
    ev.len = (uint8_t)((len > sizeof(ev.data)) ? sizeof(ev.data) : len);
    if (ev.len > 0) memcpy(ev.data, data, ev.len);
    /* Never block the radio. A dropped receipt frame breaks the sequence and the
     * upload is refused cleanly; a dropped control write is retried by the phone. */
    if (xQueueSend(s_queue, &ev, 0) != pdTRUE) {
        CAIRN_LOGW(TAG, "offload queue full; dropped event %u", (unsigned)kind);
    }
}

class OffloadCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *chr, NimBLEConnInfo &) override {
        NimBLEAttValue v = chr->getValue();
        if (chr == s_chr_control) {
            s_last_activity_ms = millis();
            post(EV_CONTROL, 0, v.data(), v.size());
        } else {
            post(EV_DATA, 0, v.data(), v.size());
        }
    }
};

static OffloadCallbacks s_cbs;

/* ── api ──────────────────────────────────────────────────────────────────── */

void ble_offload_set_trip_source(bool (*trip_active)(void)) { s_trip_active = trip_active; }

bool ble_offload_register(NimBLEService *svc)
{
    s_have_pinned = unhex32(CAIRN_SERVER_RECEIPT_KEY_HEX, s_pinned);
    if (!s_have_pinned) {
        CAIRN_LOGW(TAG, "the pinned receipt key is not valid hex; no bundle will be pruned");
    }

    cairn_offload_io_t io = { nullptr, io_indicate, io_notify, io_now, io_trip,
                              s_have_pinned ? s_pinned : nullptr };
    cairn_offload_init(&s_offload, &io);

    s_queue = xQueueCreate(16, sizeof(Event));
    if (s_queue == nullptr) {
        CAIRN_LOGE(TAG, "cannot create the offload queue");
        return false;
    }

    s_chr_control = svc->createCharacteristic(
        "A8E30030-4F5B-11EF-A017-325096B39F47",
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN |
        NIMBLE_PROPERTY::INDICATE | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN);
    s_chr_control->setCallbacks(&s_cbs);

    s_chr_data = svc->createCharacteristic(
        "A8E30031-4F5B-11EF-A017-325096B39F47",
        NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC |
        NIMBLE_PROPERTY::WRITE_AUTHEN | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN);
    s_chr_data->setCallbacks(&s_cbs);

    /* 12 KB: receipt verification (Ed25519) and manifest decoding are not small. */
    BaseType_t ok = xTaskCreatePinnedToCore(offload_task, "cairn-offload", 12288, nullptr, 1,
                                            nullptr, 1);
    if (ok != pdPASS) {
        CAIRN_LOGE(TAG, "cannot start the offload task");
        return false;
    }
    CAIRN_LOGI(TAG, "bundle offload ready (%s)",
               s_have_pinned ? "receipt key pinned" : "NO receipt key pinned: nothing will be pruned");
    return true;
}

void ble_offload_on_connect(uint16_t conn_handle)
{
    s_connected = true;
    s_session_start_ms = millis();
    s_last_activity_ms = millis();
    if (s_queue) post(EV_CONNECT, conn_handle, nullptr, 0);
}

void ble_offload_on_disconnect(void)
{
    s_connected = false;
    s_busy = false;
    if (s_queue) post(EV_DISCONNECT, 0, nullptr, 0);
}

void ble_offload_on_mtu(uint16_t att_mtu)
{
    if (s_queue) post(EV_MTU, att_mtu, nullptr, 0);
}

bool ble_offload_active(void)
{
    if (!s_connected) return false;
    uint32_t now = millis();
    if ((uint32_t)(now - s_session_start_ms) > SESSION_CAP_MS) return false;
    return s_busy || (uint32_t)(now - s_last_activity_ms) < ACTIVITY_WINDOW_MS;
}

#endif /* CAIRN_BLE_COMPANION */
