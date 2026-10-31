// main/app_state.c - Global application state machine for AI Passport.
#include "app_state.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "app_state";

static app_state_t s_state = APP_STATE_IDLE;
static app_state_cb_t s_callback = NULL;
static SemaphoreHandle_t s_mutex = NULL;

void app_state_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
    s_state = APP_STATE_IDLE;
    s_callback = NULL;
}

app_state_t app_state_get(void)
{
    app_state_t st;
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
    st = s_state;
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }
    return st;
}

void app_state_set(app_state_t state)
{
    app_state_t old;
    app_state_cb_t cb = NULL;

    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
    old = s_state;
    s_state = state;
    cb = s_callback;
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }

    if (old != state) {
        ESP_LOGI(TAG, "state %d -> %d", (int)old, (int)state);
        if (cb) {
            cb(old, state);
        }
    }
}

void app_state_set_callback(app_state_cb_t cb)
{
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
    s_callback = cb;
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }
}
