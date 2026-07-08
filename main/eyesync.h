#pragma once
#include <stdint.h>
#include <stdbool.h>

// ─────────────────────────────────────────────────────────────────────────────
// Eye-to-eye sync over BLE (goggle configuration).
//
// The left eye (role 0) runs as a BLE central: it scans for a right eye
// advertising the same group id in manufacturer data, connects, bonds, and
// writes sync packets to the peer's Sync Data characteristic (0x0040). The
// right eye pushes its own events (swipes, blinks) back as notifications on
// the same characteristic. BLE keeps the radio cost at ~1-3 mA vs ESP-NOW's
// always-listening WiFi (~60-90 mA). Group 0 = sync disabled. Mode changes
// are symmetric (either eye can announce); the left additionally beacons
// its animation clock at 5 Hz so looping animations stay in step.
// ─────────────────────────────────────────────────────────────────────────────

// Sync Data characteristic UUID and packet size (registered in main.c's
// BLE param table; the buffer itself lives in eyesync.c)
#define EYESYNC_CHR_UUID  0x0040
#define EYESYNC_PKT_LEN   8
extern uint8_t eyesync_gatt_buf[EYESYNC_PKT_LEN];

/** Set group + role; starts/stops the central-side scan as needed. */
void eyesync_set(uint8_t group, uint8_t role);

/** Announce a locally-originated mode change (burst, reliable-ish). */
void eyesync_notify_mode(uint8_t mode);

/** Announce a locally-originated blink. */
void eyesync_notify_blink(uint8_t mode);

/** Announce a leader-originated glance (Sauron gaze); `gaze` is the
 *  packed direction byte defined in main.c (bit 7 = horizontal sign,
 *  low 7 bits = vertical fraction). */
void eyesync_notify_gaze(uint8_t mode, uint8_t gaze);

/** Call every main-loop iteration: the leader broadcasts a rate-limited
 *  beacon carrying the current mode and animation clock position
 *  (UINT32_MAX when no animation is active). */
void eyesync_beacon(uint8_t mode, uint32_t anim_pos_ms);

/** Collect pending received state (WiFi task → main loop). Returns true if
 *  anything was pending. mode_out = -1 if no mode update; pos_ms_out =
 *  UINT32_MAX if no clock update; pos_mode_out = the mode the clock refers
 *  to (only apply when it matches the local mode); gaze_out = -1 if no
 *  glance, else the packed direction byte. */
bool eyesync_poll(int *mode_out, bool *blink_out,
                  uint32_t *pos_ms_out, uint8_t *pos_mode_out, int *gaze_out);
