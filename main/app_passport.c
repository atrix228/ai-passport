// main/app_passport.c - AI Passport application entry point.
//
// This file owns app_main() and wires up:
//   - BSP initialisation (I2C, display, LVGL, buttons, audio, battery)
//   - NVS load
//   - BLE GATT server
//   - App state machine + screen transitions
//   - Button dispatch (lightweight: posts to queue)
//
// main/main.c is NOT modified — this file is the new entry point set in
// CMakeLists.txt for the feature/passport-agent branch.

#include "app_state.h"
#include "app_nvs.h"
#include "app_ble.h"
#include "app_voice.h"
#include "screen_rain.h"
#include "screen_menu.h"
#include "screen_talking.h"

#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"

#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "passport";

// ---- global config ----------------------------------------------------------

static app_config_t s_config;

// ---- input queue ------------------------------------------------------------

typedef struct {
    int btn;
    int event;
} btn_event_t;

#define INPUT_QUEUE_DEPTH 8

static QueueHandle_t s_input_queue = NULL;
static TaskHandle_t  s_input_task  = NULL;
static volatile bool s_input_ready = false;

// ---- screen state management ------------------------------------------------

static void enter_state(app_state_t state)
{
    switch (state) {
    case APP_STATE_IDLE:
        if (bsp_lvgl_lock(500)) {
            screen_rain_enter();
            bsp_lvgl_unlock();
        }
        break;

    case APP_STATE_MENU:
        if (bsp_lvgl_lock(500)) {
            screen_menu_enter(&s_config);
            bsp_lvgl_unlock();
        }
        break;

    case APP_STATE_TALKING: {
        int idx = screen_menu_selected_index();
        const char *agent_name = (s_config.agent_count > 0 && idx < s_config.agent_count)
                                 ? s_config.agents[idx].name
                                 : "Agent";
        s_config.selected_agent = idx;

        if (bsp_lvgl_lock(500)) {
            screen_talking_enter(agent_name);
            bsp_lvgl_unlock();
        }
        app_voice_start_recording();
        break;
    }

    case APP_STATE_PLAYING:
        // Screen already showing from TALKING state; status updated by app_voice.
        break;

    case APP_STATE_PROVISIONING:
        // BLE advertising already running; optionally show a special screen.
        break;
    }
}

static void exit_state(app_state_t state)
{
    switch (state) {
    case APP_STATE_IDLE:
        if (bsp_lvgl_lock(500)) {
            screen_rain_exit();
            bsp_lvgl_unlock();
        }
        break;

    case APP_STATE_MENU:
        if (bsp_lvgl_lock(500)) {
            screen_menu_exit();
            bsp_lvgl_unlock();
        }
        break;

    case APP_STATE_TALKING:
        app_voice_stop_recording();
        if (bsp_lvgl_lock(500)) {
            screen_talking_exit();
            bsp_lvgl_unlock();
        }
        break;

    case APP_STATE_PLAYING:
        if (bsp_lvgl_lock(500)) {
            screen_talking_exit();
            bsp_lvgl_unlock();
        }
        break;

    case APP_STATE_PROVISIONING:
        break;
    }
}

static void on_state_change(app_state_t old_state, app_state_t new_state)
{
    exit_state(old_state);
    enter_state(new_state);
    app_ble_send_status((uint8_t)new_state);
}

// ---- button dispatch --------------------------------------------------------

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (!s_input_ready || !s_input_queue) return;

    // Only enqueue CLICK and LONG events; ignore PRESS and RELEASE.
    if (ev != BSP_BTN_CLICK && ev != BSP_BTN_LONG) return;

    const btn_event_t e = { .btn = (int)btn, .event = (int)ev };
    xQueueSend(s_input_queue, &e, 0);
}

static void input_task(void *arg)
{
    (void)arg;
    btn_event_t e;
    for (;;) {
        if (xQueueReceive(s_input_queue, &e, portMAX_DELAY) == pdTRUE) {
            app_state_t state = app_state_get();
            switch (state) {
            case APP_STATE_IDLE:
                screen_rain_key(e.btn, e.event);
                break;
            case APP_STATE_MENU:
                screen_menu_key(e.btn, e.event);
                break;
            case APP_STATE_TALKING:
            case APP_STATE_PLAYING:
                screen_talking_key(e.btn, e.event);
                break;
            default:
                break;
            }
        }
    }
}

// ---- BLE callbacks ----------------------------------------------------------

static void on_provision(const app_config_t *cfg)
{
    // Update global config and reload menu if visible.
    memcpy(&s_config, cfg, sizeof(s_config));

    if (app_state_get() == APP_STATE_MENU) {
        if (bsp_lvgl_lock(300)) {
            screen_menu_update(&s_config);
            bsp_lvgl_unlock();
        }
    }
}

static void on_audio_received(uint8_t *buf, size_t len)
{
    app_voice_on_audio_received(buf, len);
}

// ---- app_main ---------------------------------------------------------------

void app_main(void)
{
    ESP_LOGI(TAG, "AI Passport starting");

    // Initialise NVS flash (required for BLE stack and app config).
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS erasing and reinitialising");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
    }

    // Load persisted config.
    memset(&s_config, 0, sizeof(s_config));
    app_nvs_load(&s_config);

    // I2C bus (shared by ES8311 and CW2017).
    bsp_i2c_init();

    // Display + LVGL.
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display/LVGL init failed — cannot continue");
        return;
    }
    bsp_display_backlight(100);

    // Audio codec.
    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "audio init failed — recording/playback unavailable");
    } else {
        bsp_audio_set_volume(80);
        app_voice_init();
    }

    // Battery gauge (optional — ignored if hardware absent).
    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "battery init failed — SOC unavailable");
    }

    // Input dispatch queue + task.
    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(btn_event_t));
    if (!s_input_queue) {
        ESP_LOGE(TAG, "input queue creation failed");
        return;
    }
    if (xTaskCreate(input_task, "passport_input", 4096, NULL, 5, &s_input_task) != pdPASS) {
        ESP_LOGE(TAG, "input task creation failed");
        return;
    }

    // Buttons.
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "button init failed — buttons unavailable");
    }

    // App state machine.
    app_state_init();
    app_state_set_callback(on_state_change);

    // BLE GATT server.
    if (app_ble_init() != ESP_OK) {
        ESP_LOGW(TAG, "BLE init failed — provisioning unavailable");
    } else {
        app_ble_set_provision_cb(on_provision);
        app_ble_set_audio_ready_cb(on_audio_received);
    }

    // Enable input and show the rain screensaver.
    s_input_ready = true;
    if (bsp_lvgl_lock(1000)) {
        screen_rain_enter();
        bsp_lvgl_unlock();
    }

    ESP_LOGI(TAG, "AI Passport ready");
}
