// main/screen_talking.c - Talking/recording screen for AI Passport.
//
// Shows the selected agent name and an animated status line.
// Status transitions: RECORDING... → SENDING... → WAITING... → PLAYING
// Animated dots cycle on RECORDING status.

#include "screen_talking.h"
#include "app_state.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "screen_talking";

#define COL_BG      0x000000u
#define COL_BRIGHT  0x00FF41u
#define COL_MID     0x00CC33u

static lv_obj_t   *s_scr         = NULL;
static lv_obj_t   *s_agent_label = NULL;
static lv_obj_t   *s_status_label = NULL;
static lv_timer_t *s_dot_timer   = NULL;

static char s_status_base[64]    = "";
static int  s_dot_count          = 0;

// ---- dot animation timer ----------------------------------------------------

static void dot_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_status_label) return;

    // Only animate for RECORDING status.
    if (strncmp(s_status_base, "RECORDING", 9) != 0) return;

    s_dot_count = (s_dot_count + 1) % 4;
    char buf[72];
    snprintf(buf, sizeof(buf), "RECORDING%.*s", s_dot_count, "...");
    lv_label_set_text(s_status_label, buf);
}

// ---- public API -------------------------------------------------------------

void screen_talking_enter(const char *agent_name)
{
    ESP_LOGI(TAG, "enter agent='%s'", agent_name ? agent_name : "");

    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    // Agent name.
    s_agent_label = lv_label_create(s_scr);
    lv_label_set_text(s_agent_label, agent_name ? agent_name : "Agent");
    lv_obj_set_style_text_color(s_agent_label, lv_color_hex(COL_BRIGHT), 0);
    lv_obj_set_style_text_font(s_agent_label, &lv_font_montserrat_20, 0);
    lv_obj_align(s_agent_label, LV_ALIGN_TOP_MID, 0, 60);

    // Status label.
    s_status_label = lv_label_create(s_scr);
    lv_label_set_text(s_status_label, "RECORDING");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(COL_MID), 0);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, 0);

    strncpy(s_status_base, "RECORDING", sizeof(s_status_base) - 1);
    s_dot_count = 0;

    // Animated dots timer.
    s_dot_timer = lv_timer_create(dot_timer_cb, 500, NULL);

    lv_screen_load(s_scr);
}

void screen_talking_exit(void)
{
    ESP_LOGI(TAG, "exit");

    if (s_dot_timer) {
        lv_timer_delete(s_dot_timer);
        s_dot_timer = NULL;
    }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr          = NULL;
        s_agent_label  = NULL;
        s_status_label = NULL;
    }
}

void screen_talking_set_status(const char *status)
{
    // Safe to call from any task.
    extern bool bsp_lvgl_lock(int);
    extern void bsp_lvgl_unlock(void);

    if (!status) return;
    strncpy(s_status_base, status, sizeof(s_status_base) - 1);
    s_status_base[sizeof(s_status_base) - 1] = '\0';
    s_dot_count = 0;

    if (bsp_lvgl_lock(100)) {
        if (s_status_label) {
            lv_label_set_text(s_status_label, s_status_base);
        }
        bsp_lvgl_unlock();
    }
}

void screen_talking_key(int btn, int event)
{
    if (btn == BSP_BTN_OK && event == BSP_BTN_LONG) {
        // Long OK → back to menu.
        app_state_set(APP_STATE_MENU);
        return;
    }
    if (btn == BSP_BTN_OK && event == BSP_BTN_CLICK) {
        // Short OK → cancel / stop recording early.
        app_state_set(APP_STATE_MENU);
    }
}
