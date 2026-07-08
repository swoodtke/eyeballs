#include "ble.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble";

// Custom 128-bit service UUID: 4579ba11-0000-1000-8000-00805f9b34fb
// "Eyeball" in hex = 0x4579ba11
static const ble_uuid128_t svc_uuid = BLE_UUID128_INIT(
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x11, 0xba, 0x79, 0x45
);

// Device name characteristic UUID: 0x0001 in our service
#define CHR_UUID_DEVICE_NAME  0x0001

#define MAX_DEVICE_NAME  20
#define MAX_PARAMS       40
#define NVS_NAMESPACE    "eyeball"
#define NVS_KEY_NAME     "dev_name"

static char device_name[MAX_DEVICE_NAME + 1];
static const ble_param_t *s_params;
static int s_param_count;

// Concurrent central connections (phone + Mac + spare) — a single slot let
// any stranger's connection lock the owner out entirely
#define MAX_CONNS 3
static uint16_t s_conn_handles[MAX_CONNS] = {
    BLE_HS_CONN_HANDLE_NONE, BLE_HS_CONN_HANDLE_NONE, BLE_HS_CONN_HANDLE_NONE
};

// Bond persistence (NimBLE NVS store) — implemented by the ESP port
void ble_store_config_init(void);

// Set when the firmware version differs from the last boot: bonded centrals
// (iOS especially) cache our GATT table aggressively, so indicate Service
// Changed to force them to re-discover the characteristics.
static bool s_svc_changed_pending = false;

// Notification value handles — one per param
static uint16_t notify_handles[MAX_PARAMS];

// GATT characteristic definitions — built dynamically
// Max: 1 device_name + MAX_PARAMS + 1 terminator
static struct ble_gatt_chr_def chr_defs[MAX_PARAMS + 2];
static ble_uuid16_t chr_uuids[MAX_PARAMS + 2];

// GATT service table (1 service + terminator)
static struct ble_gatt_svc_def svc_defs[2];

// ─────────────────────────────────────────────────────────────────────────────
// NVS device name
// ─────────────────────────────────────────────────────────────────────────────
static void load_or_generate_name(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        strcpy(device_name, "Eyeball");
        return;
    }

    size_t len = sizeof(device_name);
    err = nvs_get_str(h, NVS_KEY_NAME, device_name, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_BT);
        snprintf(device_name, sizeof(device_name), "Eyeball-%02X%02X", mac[4], mac[5]);
        nvs_set_str(h, NVS_KEY_NAME, device_name);
        nvs_commit(h);
        ESP_LOGI(TAG, "Generated device name: %s", device_name);
    } else {
        ESP_LOGI(TAG, "Loaded device name: %s", device_name);
    }
    nvs_close(h);
}

static void save_device_name(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, NVS_KEY_NAME, device_name);
        nvs_commit(h);
        nvs_close(h);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// GATT access callbacks
// ─────────────────────────────────────────────────────────────────────────────
static int device_name_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        int rc = os_mbuf_append(ctxt->om, device_name, strlen(device_name));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len == 0 || len > MAX_DEVICE_NAME) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        uint16_t copy_len = len;
        os_mbuf_copydata(ctxt->om, 0, copy_len, device_name);
        device_name[copy_len] = '\0';
        save_device_name();
        ble_svc_gap_device_name_set(device_name);
        // Restart advertising with new name
        ESP_LOGI(TAG, "Device renamed to: %s", device_name);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int param_access(uint16_t conn_handle, uint16_t attr_handle,
                        struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    int idx = (int)(intptr_t)arg;
    if (idx < 0 || idx >= s_param_count) return BLE_ATT_ERR_UNLIKELY;
    const ble_param_t *p = &s_params[idx];

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        int rc = os_mbuf_append(ctxt->om, p->data, p->data_len);
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len != p->data_len) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        os_mbuf_copydata(ctxt->om, 0, len, p->data);
        ESP_LOGI(TAG, "Param '%s' written (%d bytes)", p->name, len);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

// ─────────────────────────────────────────────────────────────────────────────
// Build GATT table dynamically from param registry
// ─────────────────────────────────────────────────────────────────────────────
static void build_gatt_table(void)
{
    int ci = 0;

    // Device name characteristic
    chr_uuids[ci] = (ble_uuid16_t)BLE_UUID16_INIT(CHR_UUID_DEVICE_NAME);
    chr_defs[ci] = (struct ble_gatt_chr_def){
        .uuid       = &chr_uuids[ci].u,
        .access_cb  = device_name_access,
        // Writes require an encrypted (bonded) link; reads stay open
        .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                      BLE_GATT_CHR_F_WRITE_ENC,
    };
    ci++;

    // Registered params
    for (int i = 0; i < s_param_count && ci < MAX_PARAMS + 1; i++, ci++) {
        uint16_t flags = s_params[i].flags & ~BLE_PARAM_F_QUIET;
        if (flags & BLE_PARAM_F_WRITE)
            flags |= BLE_GATT_CHR_F_WRITE_ENC;
        chr_uuids[ci] = (ble_uuid16_t)BLE_UUID16_INIT(s_params[i].uuid16);
        chr_defs[ci] = (struct ble_gatt_chr_def){
            .uuid       = &chr_uuids[ci].u,
            .access_cb  = param_access,
            .arg        = (void *)(intptr_t)i,
            .val_handle = &notify_handles[i],
            .flags      = flags,
        };
    }

    // Terminator
    chr_defs[ci] = (struct ble_gatt_chr_def){ 0 };

    // Service definition
    svc_defs[0] = (struct ble_gatt_svc_def){
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &svc_uuid.u,
        .characteristics = chr_defs,
    };
    svc_defs[1] = (struct ble_gatt_svc_def){ 0 };
}

// ─────────────────────────────────────────────────────────────────────────────
// Advertising
// ─────────────────────────────────────────────────────────────────────────────
static int gap_event_cb(struct ble_gap_event *event, void *arg);

// Eye-sync group/role, embedded in advertising manufacturer data so a
// left eye can find its partner without connecting
static uint8_t s_sync_mfg[5] = { 0xFF, 0xFF, 'E', 0, 0 };
static bool s_sync_adv_enabled = false;
static bool s_host_synced = false;

static void start_advertising(void)
{
    // Ensure GAP name is current before every advertising start
    ble_svc_gap_device_name_set(device_name);

    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };

    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    // Put service UUID in advertising data
    fields.uuids128 = (ble_uuid128_t[]){ svc_uuid };
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    // Eye-sync pairing info (fills the 31-byte adv payload exactly)
    if (s_sync_adv_enabled) {
        fields.mfg_data = s_sync_mfg;
        fields.mfg_data_len = sizeof(s_sync_mfg);
    }

    ble_gap_adv_set_fields(&fields);

    // Put name in scan response
    struct ble_hs_adv_fields rsp = { 0 };
    rsp.name = (uint8_t *)device_name;
    rsp.name_len = strlen(device_name);
    rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    int rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                               &adv_params, gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "Advertising start failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "Advertising as '%s'", device_name);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// GAP event handler
// ─────────────────────────────────────────────────────────────────────────────
static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            for (int i = 0; i < MAX_CONNS; i++) {
                if (s_conn_handles[i] == BLE_HS_CONN_HANDLE_NONE) {
                    s_conn_handles[i] = event->connect.conn_handle;
                    break;
                }
            }
            ESP_LOGI(TAG, "Connected (handle=%d)", event->connect.conn_handle);
            // First boot on new firmware: tell every client that connects
            // to drop its cached GATT table — the boot-time indication only
            // reaches peers that were already connected (i.e., nobody)
            if (s_svc_changed_pending)
                ble_svc_gatt_changed(0x0001, 0xffff);
        } else {
            ESP_LOGW(TAG, "Connection failed: %d", event->connect.status);
        }
        // Keep advertising while connection slots remain
        start_advertising();
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnected (reason=%d)", event->disconnect.reason);
        for (int i = 0; i < MAX_CONNS; i++) {
            if (s_conn_handles[i] == event->disconnect.conn.conn_handle)
                s_conn_handles[i] = BLE_HS_CONN_HANDLE_NONE;
        }
        start_advertising();
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "Subscribe: attr_handle=%d, cur_notify=%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify);
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "Encryption %s (handle=%d status=%d)",
                 event->enc_change.status == 0 ? "enabled" : "failed",
                 event->enc_change.conn_handle, event->enc_change.status);
        if (event->enc_change.status != 0) {
            // Stale bond (peer kept a key we no longer have, or vice
            // versa): drop our copy and start a fresh Just Works pairing
            // instead of letting the peer retry-and-fail forever
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
                ble_gap_security_initiate(event->enc_change.conn_handle);
            }
        }
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        // Peer lost its copy of the bond (re-flashed / forgot device) —
        // drop ours so pairing can start fresh
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0)
            ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    default:
        break;
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// NimBLE host task + sync callback
// ─────────────────────────────────────────────────────────────────────────────
static void ble_on_sync(void)
{
    ESP_LOGI(TAG, "BLE host synced");
    s_host_synced = true;
    if (s_svc_changed_pending) {
        // New firmware since last boot — tell bonded centrals to re-discover
        ble_svc_gatt_changed(0x0001, 0xffff);
        ESP_LOGI(TAG, "Indicated GATT Service Changed (new firmware)");
    }
    start_advertising();
}

static void ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset: %d", reason);
}

static void nimble_host_task(void *param)
{
    nimble_port_run();          // blocks until nimble_port_stop()
    nimble_port_freertos_deinit();
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────
void ble_init(const ble_param_t *params, int count)
{
    s_params = params;
    if (count > MAX_PARAMS) {
        ESP_LOGE(TAG, "ble_params[] has %d entries but MAX_PARAMS is %d — "
                 "characteristics beyond the limit will NOT be registered! "
                 "Raise MAX_PARAMS in ble.c.", count, MAX_PARAMS);
        count = MAX_PARAMS;
    }
    s_param_count = count;

    // Init NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    load_or_generate_name();

    // Detect firmware changes across boots (GATT table may have changed)
    {
        nvs_handle_t h;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
            const char *cur = esp_app_get_description()->version;
            char last[32] = {0};
            size_t len = sizeof(last);
            nvs_get_str(h, "last_fw", last, &len);
            if (strcmp(last, cur) != 0) {
                s_svc_changed_pending = true;
                nvs_set_str(h, "last_fw", cur);
                nvs_commit(h);
            }
            nvs_close(h);
        }
    }

    // Init NimBLE
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return;
    }

    ble_hs_cfg.sync_cb  = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;

    // Security: Just Works bonding with LE Secure Connections. Writable
    // characteristics require an encrypted link (WRITE_ENC), so a central
    // must pair before it can change anything; reads stay open. Bonds
    // persist in NVS so reconnects re-encrypt without re-pairing.
    ble_hs_cfg.sm_io_cap  = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc      = 1;
    ble_hs_cfg.sm_our_key_dist   |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();

    ble_svc_gap_init();
    ble_svc_gatt_init();
    // Set name AFTER gap_init, otherwise gap_init overwrites it with "nimble"
    ble_svc_gap_device_name_set(device_name);

    build_gatt_table();

    int rc = ble_gatts_count_cfg(svc_defs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        return;
    }
    rc = ble_gatts_add_svcs(svc_defs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return;
    }

    nimble_port_freertos_init(nimble_host_task);
    ESP_LOGI(TAG, "BLE initialized with %d params", s_param_count);
}

const char *ble_get_device_name(void)
{
    return device_name;
}

void ble_set_sync_adv(uint8_t group, uint8_t role)
{
    s_sync_mfg[3] = group;
    s_sync_mfg[4] = role;
    s_sync_adv_enabled = (group != 0);
    if (s_host_synced) {
        // Restart advertising so the new manufacturer data takes effect
        ble_gap_adv_stop();
        start_advertising();
    }
    // Not synced yet: ble_on_sync's start_advertising picks the values up
}

void ble_notify_param(uint16_t uuid16)
{
    int idx = -1;
    for (int i = 0; i < s_param_count; i++) {
        if (s_params[i].uuid16 == uuid16) { idx = i; break; }
    }
    if (idx < 0 || notify_handles[idx] == 0) return;

    for (int c = 0; c < MAX_CONNS; c++) {
        if (s_conn_handles[c] == BLE_HS_CONN_HANDLE_NONE) continue;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(s_params[idx].data,
                                                   s_params[idx].data_len);
        if (om)
            ble_gatts_notify_custom(s_conn_handles[c], notify_handles[idx], om);
    }
}

int ble_connected_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_CONNS; i++)
        if (s_conn_handles[i] != BLE_HS_CONN_HANDLE_NONE) n++;
    return n;
}

void ble_notify_all(void)
{
    for (int c = 0; c < MAX_CONNS; c++) {
        if (s_conn_handles[c] == BLE_HS_CONN_HANDLE_NONE) continue;

        for (int i = 0; i < s_param_count; i++) {
            if (!(s_params[i].flags & BLE_PARAM_F_NOTIFY)) continue;
            if (s_params[i].flags & BLE_PARAM_F_QUIET) continue;
            if (notify_handles[i] == 0) continue;

            struct os_mbuf *om = ble_hs_mbuf_from_flat(s_params[i].data, s_params[i].data_len);
            if (om) {
                ble_gatts_notify_custom(s_conn_handles[c], notify_handles[i], om);
            }
        }
    }
}
