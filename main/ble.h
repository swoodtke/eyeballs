#pragma once

#include <stdint.h>

// Flag bits (match NimBLE BLE_GATT_CHR_F_* values)
#define BLE_PARAM_F_READ    0x02
#define BLE_PARAM_F_WRITE   0x08
#define BLE_PARAM_F_NOTIFY  0x10

// Convenience combos
#define BLE_PARAM_RW     (BLE_PARAM_F_READ | BLE_PARAM_F_WRITE)
#define BLE_PARAM_RWN    (BLE_PARAM_F_READ | BLE_PARAM_F_WRITE | BLE_PARAM_F_NOTIFY)
#define BLE_PARAM_STAT   (BLE_PARAM_F_READ | BLE_PARAM_F_NOTIFY)

typedef struct {
    uint16_t    uuid16;     // 16-bit UUID offset within our service
    const char *name;       // human-readable name (served as GATT descriptor)
    uint8_t     flags;      // BLE_PARAM_F_READ | _WRITE | _NOTIFY
    void       *data;       // pointer to the actual variable
    uint8_t     data_len;   // size in bytes (e.g. 4 for float, 1 for uint8)
} ble_param_t;

void ble_init(const ble_param_t *params, int count);
void ble_notify_all(void);

// Status accessors (for the on-device status screen)
const char *ble_get_device_name(void);
int ble_connected_count(void);
