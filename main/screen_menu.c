// main/screen_menu.c - Agent select menu for AI Passport.
//
// Black screen with green Matrix colour scheme.
// Title "SELECT AGENT" at the top; scrollable list of agents below.
// Selected item has a bright-green bordered box.
// Battery % shown top-right.
//
// Callers outside the LVGL task must hold bsp_lvgl_lock() / bsp_lvgl_unlock()
// around screen_menu_enter(), screen_menu_exit(), and screen_menu_update().

#include "screen_menu.h"
#include "app_state.h"
#include "app_nvs.h"
#include "bsp_button.h"
#include "bsp_battery.h"
#include "esp_log.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "screen_menu";

#define COL_BG      0x000000u
#define COL_BRIGHT  0x00FF41u
#define COL_MID     0x00CC33u
#define COL_DIM     0x005C16u

#define MAX_VISIBLE 5   // rows visible without scrolling

static lv_obj_t *s_scr        = NULL;
static lv_obj_t *s_list       = NULL;
static lv_obj_t *s_batt_label = NULL;
static lv_obj_t *s_no_agent   = NULL;
static lv_timer_t *s_batt_timer = NULL;

static int       s_agent_count   = 0;
static int       s_cursor        = 0;   // currently highlighted index

// Row objects — allocated dynamically up to MAX_AGENTS.
static lv_obj_t *s_rows[MAX_AGENTS];

// ---- helpers ----------------------------------------------------------------

static void update_battery_label(void)
{
    if (!s_batt_label) return;
    int soc = bsp_battery_soc();
    if (soc >= 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", soc);
        lv_label_set_text(s_batt_label, buf);
    } else {
        lv_label_set_text(s_batt_label, "");
    }
}

static void batt_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    update_battery_label();
}

static void highlight_row(int idx, bool selected)
{
    if (idx < 0 || idx >= s_agent_count || !s_rows[idx]) return;
    lv_obj_t *row = s_rows[idx];
    if (selected) {
        lv_obj_set_style_border_color(row, lv_color_hex(COL_BRIGHT), 0);
        lv_obj_set_style_border_width(row, 2, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x001A00u), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        // Update child label colour.
        lv_obj_t *lbl = lv_obj_get_child(row, 0);
        if (lbl) {
            lv_obj_set_style_text_color(lbl, lv_color_hex(COL_BRIGHT), 0);
        }
    } else {
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_t *lbl = lv_obj_get_child(row, 0);
        if (lbl) {
            lv_obj_set_style_text_color(lbl, lv_color_hex(COL_MID), 0);
        }
    }
}

static void scroll_to_cursor(void)
{
    if (!s_list || s_cursor < 0 || s_cursor >= s_agent_count) return;
    lv_obj_t *row = s_rows[s_cursor];
    if (row) {
        lv_obj_scroll_to_view(row, LV_ANIM_OFF);
    }
}

// ---- public API -------------------------------------------------------------

void screen_menu_enter(const app_config_t *cfg)
{
    ESP_LOGI(TAG, "enter (agents=%d)", cfg ? cfg->agent_count : 0);

    s_agent_count = 0;
    s_cursor      = cfg ? cfg->selected_agent : 0;
    memset(s_rows, 0, sizeof(s_rows));

    // Root screen.
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    // Title label.
    lv_obj_t *title = lv_label_create(s_scr);
    lv_label_set_text(title, "SELECT AGENT");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_BRIGHT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    // Battery label.
    s_batt_label = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_batt_label, lv_color_hex(COL_MID), 0);
    lv_obj_set_style_text_font(s_batt_label, &lv_font_montserrat_8, 0);
    lv_obj_align(s_batt_label, LV_ALIGN_TOP_RIGHT, -4, 4);
    update_battery_label();

    if (!cfg || cfg->agent_count == 0) {
        // Show "No agents" message.
        s_no_agent = lv_label_create(s_scr);
        lv_label_set_text(s_no_agent, "No agents\nPair with app");
        lv_obj_set_style_text_color(s_no_agent, lv_color_hex(COL_MID), 0);
        lv_obj_set_style_text_font(s_no_agent, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(s_no_agent, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(s_no_agent, LV_ALIGN_CENTER, 0, 0);
    } else {
        s_no_agent    = NULL;
        s_agent_count = cfg->agent_count;

        // Scrollable list container.
        s_list = lv_obj_create(s_scr);
        lv_obj_set_size(s_list, 220, 260);
        lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 38);
        lv_obj_set_style_bg_color(s_list, lv_color_hex(COL_BG), 0);
        lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_list, 0, 0);
        lv_obj_set_style_pad_all(s_list, 4, 0);
        lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

        for (int i = 0; i < s_agent_count; i++) {
            lv_obj_t *row = lv_obj_create(s_list);
            lv_obj_set_size(row, LV_PCT(100), 40);
            lv_obj_set_style_bg_color(row, lv_color_hex(COL_BG), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_pad_all(row, 4, 0);

            lv_obj_t *lbl = lv_label_create(row);
            lv_label_set_text(lbl, cfg->agents[i].name);
            lv_obj_set_style_text_color(lbl, lv_color_hex(COL_MID), 0);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
            lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);

            s_rows[i] = row;
        }

        // Clamp cursor.
        if (s_cursor >= s_agent_count) s_cursor = 0;
        highlight_row(s_cursor, true);
        scroll_to_cursor();
    }

    lv_screen_load(s_scr);

    s_batt_timer = lv_timer_create(batt_timer_cb, 5000, NULL);
}

void screen_menu_exit(void)
{
    ESP_LOGI(TAG, "exit");

    if (s_batt_timer) {
        lv_timer_delete(s_batt_timer);
        s_batt_timer = NULL;
    }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr        = NULL;
        s_list       = NULL;
        s_batt_label = NULL;
        s_no_agent   = NULL;
        memset(s_rows, 0, sizeof(s_rows));
    }
}

void screen_menu_key(int btn, int event)
{
    if (event != BSP_BTN_CLICK && event != BSP_BTN_LONG) return;

    if (btn == BSP_BTN_OK && event == BSP_BTN_LONG) {
        // Long OK → back to rain.
        app_state_set(APP_STATE_IDLE);
        return;
    }

    if (btn == BSP_BTN_OK && event == BSP_BTN_CLICK) {
        if (s_agent_count > 0) {
            // Select agent → start recording.
            app_state_set(APP_STATE_TALKING);
        }
        return;
    }

    if (s_agent_count == 0) return;

    // UP / DOWN navigation.
    int prev = s_cursor;
    if (btn == BSP_BTN_UP && event == BSP_BTN_CLICK) {
        s_cursor = (s_cursor > 0) ? s_cursor - 1 : s_agent_count - 1;
    } else if (btn == BSP_BTN_DOWN && event == BSP_BTN_CLICK) {
        s_cursor = (s_cursor + 1) % s_agent_count;
    }

    if (s_cursor != prev) {
        // Must update LVGL objects — do so in LVGL task context.
        // This callback originates from the input task; post to LVGL via timer
        // won't work easily here. Instead we take the lock briefly.
        extern bool bsp_lvgl_lock(int);
        extern void bsp_lvgl_unlock(void);
        if (bsp_lvgl_lock(100)) {
            highlight_row(prev, false);
            highlight_row(s_cursor, true);
            scroll_to_cursor();
            bsp_lvgl_unlock();
        }
    }
}

void screen_menu_update(const app_config_t *cfg)
{
    // Simple implementation: re-enter to rebuild.
    screen_menu_exit();
    screen_menu_enter(cfg);
}

int screen_menu_selected_index(void)
{
    return s_cursor;
}
