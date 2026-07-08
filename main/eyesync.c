#include "eyesync.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "ble.h"

static const char *TAG = "eyesync";

// Sync payload lives in a GATT characteristic (0x0040, RWN) on every device:
//   left (central) ──write-no-rsp──▶ right (peripheral)
//   right ──notify──▶ left
// Packet: {seq, mode, flags, rsvd, anim_pos_ms(le32)}
#define FLAG_BLINK  0x01
#define FLAG_MODE   0x02
#define FLAG_GAZE   0x04   // aux byte carries the glance direction

#define BEACON_INTERVAL_US 200000   // leader clock beacon: 5 Hz

// The GATT-visible buffer, registered in main.c's BLE param table.
uint8_t eyesync_gatt_buf[EYESYNC_PKT_LEN];

static uint8_t s_group = 0;
static uint8_t s_role = 0;
static uint8_t s_seq = 0;
static int64_t s_last_beacon_us = 0;

// Central-side link state (left eye only)
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_val_handle = 0;
static bool s_scanning = false;
static bool s_ready = false;   // encrypted + characteristic discovered

// Last seq consumed from (or written into) the GATT buffer — outgoing
// notifications share the buffer, so own writes must not echo back
static uint8_t s_gatt_last_seq = 0;

// Pending RX (NimBLE host task / GATT writes → main loop)
static volatile int      s_rx_mode = -1;
static volatile bool     s_rx_blink = false;
static volatile uint32_t s_rx_pos_ms = UINT32_MAX;
static volatile uint8_t  s_rx_pos_mode = 0xFF;
static volatile int      s_rx_gaze = -1;   // aux byte of a gaze packet, -1 = none

static void start_scan(void);

static void parse_pkt(const uint8_t *p)
{
    uint8_t flags = p[2];
    if (flags & FLAG_MODE)  s_rx_mode = p[1];
    if (flags & FLAG_BLINK) s_rx_blink = true;
    if (flags & FLAG_GAZE)  s_rx_gaze = p[3];
    uint32_t pos = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                   ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
    if (pos != UINT32_MAX) {
        s_rx_pos_ms   = pos;
        s_rx_pos_mode = p[1];
    }
}

static void fill_pkt(uint8_t *p, uint8_t mode, uint8_t flags, uint32_t pos_ms,
                     uint8_t aux)
{
    p[0] = ++s_seq;
    if (s_seq == 0) p[0] = ++s_seq;   // 0 means "never written"
    p[1] = mode;
    p[2] = flags;
    p[3] = aux;
    p[4] = pos_ms & 0xFF;
    p[5] = (pos_ms >> 8) & 0xFF;
    p[6] = (pos_ms >> 16) & 0xFF;
    p[7] = (pos_ms >> 24) & 0xFF;
}

// ── Left/central: send by writing the peer's characteristic ──
static void central_send(uint8_t mode, uint8_t flags, uint32_t pos_ms,
                         uint8_t aux)
{
    if (!s_ready || s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    uint8_t pkt[EYESYNC_PKT_LEN];
    fill_pkt(pkt, mode, flags, pos_ms, aux);
    ble_gattc_write_no_rsp_flat(s_conn, s_val_handle, pkt, sizeof(pkt));
}

// ── Right/peripheral: send by updating the buffer and notifying ──
static void peripheral_send(uint8_t mode, uint8_t flags, uint32_t pos_ms,
                            uint8_t aux)
{
    fill_pkt(eyesync_gatt_buf, mode, flags, pos_ms, aux);
    s_gatt_last_seq = eyesync_gatt_buf[0];   // don't echo our own packet
    ble_notify_param(EYESYNC_CHR_UUID);
}

static void send_pkt(uint8_t mode, uint8_t flags, uint32_t pos_ms, uint8_t aux)
{
    if (s_group == 0) return;
    if (s_role == 0) central_send(mode, flags, pos_ms, aux);
    else             peripheral_send(mode, flags, pos_ms, aux);
}

// ─────────────────────────────────────────────────────────────────────────────
// Central role: scan → connect → encrypt → discover → subscribe
// ─────────────────────────────────────────────────────────────────────────────
static int chr_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status == 0 && chr) {
        s_val_handle = chr->val_handle;
        return 0;
    }
    if (error->status == BLE_HS_EDONE && s_val_handle != 0) {
        // Subscribe to right→left notifications. The CCCD directly follows
        // the value handle in NimBLE's server layout (the peer runs this
        // same firmware).
        uint8_t cccd[2] = { 0x01, 0x00 };
        ble_gattc_write_flat(conn_handle, s_val_handle + 1, cccd, 2, NULL, NULL);
        s_ready = true;
        ESP_LOGI(TAG, "Pair link ready (val handle %d)", s_val_handle);
    } else if (error->status == BLE_HS_EDONE) {
        ESP_LOGW(TAG, "Peer lacks sync characteristic — old firmware?");
    }
    return 0;
}

static int central_gap_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        // Match our manufacturer data: {0xFF,0xFF,'E',group,role}
        struct ble_hs_adv_fields f;
        if (ble_hs_adv_parse_fields(&f, event->disc.data,
                                    event->disc.length_data) != 0) return 0;
        if (f.mfg_data == NULL || f.mfg_data_len < 5) return 0;
        if (f.mfg_data[2] != 'E' || f.mfg_data[3] != s_group) return 0;
        if (f.mfg_data[4] != 1) return 0;   // only pair with a right eye
        ESP_LOGI(TAG, "Found right eye (group %d), connecting", s_group);
        ble_gap_disc_cancel();
        s_scanning = false;
        ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &event->disc.addr, 10000,
                        NULL, central_gap_cb, NULL);
        return 0;
    }

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            // Writable chars need encryption — pair/bond first
            ble_gap_security_initiate(s_conn);
        } else {
            start_scan();
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0 &&
            event->enc_change.conn_handle == s_conn) {
            s_val_handle = 0;
            ble_gattc_disc_chrs_by_uuid(s_conn, 1, 0xFFFF,
                BLE_UUID16_DECLARE(EYESYNC_CHR_UUID), chr_disc_cb, NULL);
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX:
        if (event->notify_rx.conn_handle == s_conn &&
            event->notify_rx.attr_handle == s_val_handle) {
            uint8_t pkt[EYESYNC_PKT_LEN];
            if (os_mbuf_copydata(event->notify_rx.om, 0, sizeof(pkt), pkt) == 0)
                parse_pkt(pkt);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        if (event->disconnect.conn.conn_handle == s_conn) {
            ESP_LOGI(TAG, "Pair link lost (reason=%d)", event->disconnect.reason);
            s_conn = BLE_HS_CONN_HANDLE_NONE;
            s_ready = false;
            start_scan();
        }
        return 0;

    default:
        return 0;
    }
}

static void start_scan(void)
{
    if (s_scanning || s_group == 0 || s_role != 0) return;
    // Duty-cycled passive scan (~10%) keeps discovery cheap on the battery
    struct ble_gap_disc_params p = {
        .itvl = 0x0140,     // 320 * 0.625 ms = 200 ms
        .window = 0x0020,   // 32 * 0.625 ms = 20 ms
        .passive = 1,
        .filter_duplicates = 0,
    };
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &p,
                          central_gap_cb, NULL);
    if (rc == 0 || rc == BLE_HS_EALREADY) {
        s_scanning = true;
        ESP_LOGI(TAG, "Scanning for right eye (group %d)", s_group);
    } else {
        ESP_LOGW(TAG, "Scan start failed: %d", rc);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API (same shape as the ESP-NOW version)
// ─────────────────────────────────────────────────────────────────────────────
void eyesync_set(uint8_t group, uint8_t role)
{
    s_group = group;
    s_role  = role & 1;
    ble_set_sync_adv(s_group, s_role);   // advertise group+role in mfg data
    ESP_LOGI(TAG, "Sync group=%d role=%s%s", group,
             s_role == 0 ? "left/leader" : "right",
             group == 0 ? " (disabled)" : "");

    if (s_group != 0 && s_role == 0) {
        start_scan();
    } else {
        if (s_scanning) { ble_gap_disc_cancel(); s_scanning = false; }
        if (s_conn != BLE_HS_CONN_HANDLE_NONE)
            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

void eyesync_notify_mode(uint8_t mode)
{
    send_pkt(mode, FLAG_MODE, UINT32_MAX, 0);
}

void eyesync_notify_blink(uint8_t mode)
{
    send_pkt(mode, FLAG_BLINK, UINT32_MAX, 0);
}

void eyesync_notify_gaze(uint8_t mode, uint8_t gaze)
{
    send_pkt(mode, FLAG_GAZE, UINT32_MAX, gaze);
}

void eyesync_beacon(uint8_t mode, uint32_t anim_pos_ms)
{
    if (s_role != 0 || s_group == 0) return;   // leader only
    int64_t now = esp_timer_get_time();
    if (now - s_last_beacon_us < BEACON_INTERVAL_US) return;
    s_last_beacon_us = now;
    send_pkt(mode, FLAG_MODE, anim_pos_ms, 0);
}

bool eyesync_poll(int *mode_out, bool *blink_out,
                  uint32_t *pos_ms_out, uint8_t *pos_mode_out, int *gaze_out)
{
    // Left/central: (re)start the partner scan if it isn't running — covers
    // the host not being synced yet at eyesync_set time and scan failures
    if (s_group != 0 && s_role == 0 && !s_scanning &&
        s_conn == BLE_HS_CONN_HANDLE_NONE) {
        static int64_t last_try_us = 0;
        int64_t now = esp_timer_get_time();
        if (now - last_try_us > 1000000) {
            last_try_us = now;
            start_scan();
        }
    }

    // Right/peripheral: drain packets the left eye wrote into our GATT buffer
    if (s_group != 0 && s_role == 1) {
        uint8_t seq = eyesync_gatt_buf[0];
        if (seq != 0 && seq != s_gatt_last_seq) {
            s_gatt_last_seq = seq;
            parse_pkt(eyesync_gatt_buf);
        }
    }

    *mode_out     = s_rx_mode;
    *blink_out    = s_rx_blink;
    *pos_ms_out   = s_rx_pos_ms;
    *pos_mode_out = s_rx_pos_mode;
    *gaze_out     = s_rx_gaze;
    bool any = (s_rx_mode >= 0) || s_rx_blink ||
               (s_rx_pos_ms != UINT32_MAX) || (s_rx_gaze >= 0);
    s_rx_mode   = -1;
    s_rx_blink  = false;
    s_rx_pos_ms = UINT32_MAX;
    s_rx_gaze   = -1;
    return any;
}
