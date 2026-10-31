// main/app_voice.c - Voice recording and playback task for AI Passport.
//
// Recording task:
//   1. Starts bsp_audio_set_format() for 16 kHz 16-bit mono record.
//   2. Reads PCM in 512-sample (1024 byte) chunks.
//   3. Streams each chunk over BLE AUDIO_OUT.
//   4. On stop signal (or state change away from TALKING): sends final chunk with
//      is_last=1 and terminates.
//
// Playback:
//   When BLE AUDIO_IN is fully reassembled, app_voice_on_audio_received() is called.
//   The buffer is handed to bsp_audio_write() after reconfiguring for 16 kHz.

#include "app_voice.h"
#include "app_ble.h"
#include "app_state.h"
#include "screen_talking.h"
#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app_voice";

#define RECORD_SAMPLE_RATE   16000
#define RECORD_BITS          16
#define RECORD_CHANNELS      1
#define RECORD_CHUNK_SAMPLES 512
#define RECORD_CHUNK_BYTES   (RECORD_CHUNK_SAMPLES * (RECORD_BITS / 8) * RECORD_CHANNELS)

// Max MTU payload: BLE_ATT_ATTR_MAX_LEN (512) minus ATT header (3) minus chunk header (2).
// In practice negotiated MTU may be smaller; clip to 244 bytes for safety.
#define BLE_MAX_AUDIO_PAYLOAD 244

static TaskHandle_t s_record_task = NULL;
static volatile bool s_stop_flag = false;

// ---- recording task ---------------------------------------------------------

static void record_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "recording started");

    esp_err_t err = bsp_audio_set_format(RECORD_SAMPLE_RATE, RECORD_BITS, RECORD_CHANNELS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_format failed: %s", esp_err_to_name(err));
        app_state_set(APP_STATE_MENU);
        s_record_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    uint8_t *pcm_buf = malloc(RECORD_CHUNK_BYTES);
    if (!pcm_buf) {
        ESP_LOGE(TAG, "OOM for PCM buffer");
        app_state_set(APP_STATE_MENU);
        s_record_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    screen_talking_set_status("RECORDING");

    while (!s_stop_flag && app_state_get() == APP_STATE_TALKING) {
        err = bsp_audio_read(pcm_buf, RECORD_CHUNK_BYTES);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "audio read error: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Send over BLE in sub-chunks if needed.
        size_t offset = 0;
        while (offset < RECORD_CHUNK_BYTES) {
            size_t payload = RECORD_CHUNK_BYTES - offset;
            if (payload > BLE_MAX_AUDIO_PAYLOAD) {
                payload = BLE_MAX_AUDIO_PAYLOAD;
            }
            bool is_last = (s_stop_flag || app_state_get() != APP_STATE_TALKING)
                           && (offset + payload >= RECORD_CHUNK_BYTES);
            app_ble_send_audio(pcm_buf + offset, payload, is_last);
            offset += payload;
        }
    }

    // Send a final empty chunk with is_last=1 if we haven't already.
    app_ble_send_audio(NULL, 0, true);

    free(pcm_buf);
    ESP_LOGI(TAG, "recording stopped, waiting for reply");
    screen_talking_set_status("SENDING...");

    s_record_task = NULL;
    vTaskDelete(NULL);
}

// ---- playback ---------------------------------------------------------------

void app_voice_on_audio_received(uint8_t *buf, size_t len)
{
    ESP_LOGI(TAG, "playing received audio: %zu bytes", len);
    screen_talking_set_status("PLAYING");
    app_state_set(APP_STATE_PLAYING);

    esp_err_t err = bsp_audio_set_format(RECORD_SAMPLE_RATE, RECORD_BITS, RECORD_CHANNELS);
    if (err == ESP_OK) {
        bsp_audio_set_volume(80);
        err = bsp_audio_write(buf, len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "audio write failed: %s", esp_err_to_name(err));
        }
    }

    free(buf);

    // Return to menu after playback.
    app_state_set(APP_STATE_MENU);
}

// ---- public API -------------------------------------------------------------

esp_err_t app_voice_init(void)
{
    // Nothing extra to initialise beyond bsp_audio_init().
    return ESP_OK;
}

esp_err_t app_voice_start_recording(void)
{
    if (s_record_task) {
        ESP_LOGW(TAG, "recording already active");
        return ESP_ERR_INVALID_STATE;
    }

    s_stop_flag = false;

    BaseType_t rc = xTaskCreate(record_task, "passport_rec", 4096, NULL, 5, &s_record_task);
    if (rc != pdPASS) {
        s_record_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void app_voice_stop_recording(void)
{
    s_stop_flag = true;
}
