// main/app_ble.h - Full BLE GATT server for AI Passport provisioning and audio relay.
#pragma once

#include "app_nvs.h"
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

// Service UUID (Nordic UART style):
// 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
//
// Characteristics:
//  6E400002 — PROVISION_WRITE  (Write)
//  6E400003 — AUDIO_OUT        (Notify)
//  6E400004 — AUDIO_IN         (Write)
//  6E400005 — STATUS           (Notify)

// Initialise NimBLE stack and GATT server, begin advertising.
esp_err_t app_ble_init(void);

// Send an audio chunk (raw PCM bytes) to the connected client via AUDIO_OUT notify.
// Returns ESP_ERR_INVALID_STATE if no client is connected or notifications not enabled.
esp_err_t app_ble_send_audio(const uint8_t *data, size_t len, bool is_last);

// Send a STATUS notify (1 byte = current app_state_t value).
esp_err_t app_ble_send_status(uint8_t state);

// Return true if a central is currently connected.
bool app_ble_connected(void);

// Callback type invoked when AUDIO_IN data has been fully reassembled.
// The buffer is owned by the caller and must be freed with free().
typedef void (*app_ble_audio_ready_cb_t)(uint8_t *buf, size_t len);

// Register callback for complete received audio.
void app_ble_set_audio_ready_cb(app_ble_audio_ready_cb_t cb);

// Callback type invoked when a provisioning write is received.
typedef void (*app_ble_provision_cb_t)(const app_config_t *cfg);

// Register callback for provisioning writes.
void app_ble_set_provision_cb(app_ble_provision_cb_t cb);
