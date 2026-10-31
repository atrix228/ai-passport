// main/screen_rain.c - Matrix rain screensaver for AI Passport.
//
// 30 columns of falling green "characters" on a black background rendered via an
// LVGL canvas using the LVGL v9 layer-based drawing API.
//
// Each column: one bright head cell (#00FF41) + 8 dimming tail cells.
// Timer tick: 20 ms. Column advance speed: 1-3 ticks per row.
//
// Callers outside the LVGL task must hold bsp_lvgl_lock() before calling
// screen_rain_enter() or screen_rain_exit(). The timer callback executes inside
// the LVGL task and requires no lock.

#include "screen_rain.h"
#include "app_state.h"
#include "bsp_button.h"
#include "bsp_battery.h"
#include "esp_log.h"
#include "lvgl.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "screen_rain";

// ---- geometry ---------------------------------------------------------------
#define RAIN_W          240
#define RAIN_H          320
#define RAIN_COLS       30
#define RAIN_TAIL_LEN   8
#define RAIN_CHAR_W     8    // pixel width of each glyph cell
#define RAIN_CHAR_H     10   // pixel height of each glyph cell

// Matrix green palette (0xRRGGBB).
#define COL_BG          0x000000u
#define COL_BRIGHT      0x00FF41u
#define COL_MID         0x00CC33u
#define COL_DIM4        0x009922u
#define COL_DIM3        0x007018u
#define COL_DIM2        0x005C16u
#define COL_DIM1        0x003D0Eu

// Tail intensity colours: index 0 = closest to head, 7 = faintest.
static const uint32_t TAIL_COLORS[RAIN_TAIL_LEN] = {
    COL_MID,  COL_MID,  COL_DIM4, COL_DIM4,
    COL_DIM3, COL_DIM2, COL_DIM1, COL_DIM1
};

// Glyph set for the rain.
static const char GLYPHS[] = "0123456789ABCDEF+-=|/\\[]{}";
#define GLYPH_COUNT (sizeof(GLYPHS) - 1)

// ---- per-column state -------------------------------------------------------
typedef struct {
    int  head_row;               // current head row (in char units); may be negative
    int  speed;                  // advance every N ticks (1..3)
    int  tick_count;             // ticks accumulated since last advance
    char cells[RAIN_TAIL_LEN + 1]; // [0]=head glyph, [1..8]=tail glyphs
} rain_col_t;

// Canvas pixel buffer — RGB565, RAIN_W * RAIN_H * 2 bytes.
// Declared static so it lives in BSS (not stack), staying below stack limits.
static lv_color_t s_cbuf[RAIN_W * RAIN_H];

static lv_obj_t   *s_scr        = NULL;
static lv_obj_t   *s_canvas     = NULL;
static lv_obj_t   *s_batt_label = NULL;
static lv_timer_t *s_timer      = NULL;
static rain_col_t  s_cols[RAIN_COLS];

// ---- helpers ----------------------------------------------------------------

static char rand_glyph(void) { return GLYPHS[rand() % GLYPH_COUNT]; }

static void col_reset(int c)
{
    rain_col_t *col = &s_cols[c];
    col->head_row   = -(rand() % 20);
    col->speed      = 1 + rand() % 3;
    col->tick_count = 0;
    memset(col->cells, 0, sizeof(col->cells));
}

// Draw a single character cell at pixel position (px, py) using the LVGL v9
// canvas layer API.
static void draw_cell(lv_layer_t *layer, int px, int py, char glyph, lv_color_t color)
{
    if (py < 0 || py >= RAIN_H || px < 0 || px >= RAIN_W) return;

    // First erase the background cell.
    lv_draw_rect_dsc_t rdsc;
    lv_draw_rect_dsc_init(&rdsc);
    rdsc.bg_color   = lv_color_hex(COL_BG);
    rdsc.bg_opa     = LV_OPA_COVER;
    rdsc.border_width = 0;
    rdsc.radius     = 0;
    lv_area_t cell = {
        .x1 = (lv_coord_t)px,
        .y1 = (lv_coord_t)py,
        .x2 = (lv_coord_t)(px + RAIN_CHAR_W - 1),
        .y2 = (lv_coord_t)(py + RAIN_CHAR_H - 1),
    };
    lv_draw_rect(layer, &rdsc, &cell);

    // Draw glyph.
    if (!glyph) return;

    char buf[2] = { glyph, '\0' };
    lv_draw_label_dsc_t ldsc;
    lv_draw_label_dsc_init(&ldsc);
    ldsc.color = color;
    ldsc.font  = &lv_font_montserrat_8;
    ldsc.align = LV_TEXT_ALIGN_LEFT;
    lv_draw_label(layer, &ldsc, &cell, buf, NULL);
}

// Erase a cell (fill with background).
static void erase_cell(lv_layer_t *layer, int px, int py)
{
    if (py < 0 || py >= RAIN_H || px < 0 || px >= RAIN_W) return;

    lv_draw_rect_dsc_t rdsc;
    lv_draw_rect_dsc_init(&rdsc);
    rdsc.bg_color     = lv_color_hex(COL_BG);
    rdsc.bg_opa       = LV_OPA_COVER;
    rdsc.border_width = 0;
    rdsc.radius       = 0;
    lv_area_t cell = {
        .x1 = (lv_coord_t)px,
        .y1 = (lv_coord_t)py,
        .x2 = (lv_coord_t)(px + RAIN_CHAR_W - 1),
        .y2 = (lv_coord_t)(py + RAIN_CHAR_H - 1),
    };
    lv_draw_rect(layer, &rdsc, &cell);
}

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

// ---- timer callback (runs in LVGL task — no lock needed) --------------------

static void rain_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_canvas) return;

    lv_layer_t layer;
    lv_canvas_init_layer(s_canvas, &layer);

    int total_rows = RAIN_H / RAIN_CHAR_H;

    for (int c = 0; c < RAIN_COLS; c++) {
        rain_col_t *col = &s_cols[c];
        col->tick_count++;
        if (col->tick_count < col->speed) continue;
        col->tick_count = 0;

        int px = c * RAIN_CHAR_W;

        // Erase the cell that's about to drop off the tail.
        int tail_end_row = col->head_row - RAIN_TAIL_LEN;
        erase_cell(&layer, px, tail_end_row * RAIN_CHAR_H);

        // Shift cells: [0]=head, [1..8]=tail; head moves down by 1 row.
        memmove(&col->cells[1], &col->cells[0], RAIN_TAIL_LEN);
        col->head_row++;

        if (col->head_row >= 0) {
            col->cells[0] = rand_glyph();
        }

        // Redraw all visible cells in this column.
        for (int t = 0; t <= RAIN_TAIL_LEN; t++) {
            int row = col->head_row - t;
            if (row < 0 || row >= total_rows + RAIN_TAIL_LEN) continue;
            char g = col->cells[t];
            if (!g) continue;
            lv_color_t color = (t == 0)
                ? lv_color_hex(COL_BRIGHT)
                : lv_color_hex(TAIL_COLORS[t - 1 < RAIN_TAIL_LEN ? t - 1 : RAIN_TAIL_LEN - 1]);
            draw_cell(&layer, px, row * RAIN_CHAR_H, g, color);
        }

        // Reset column when head drops off the bottom.
        if (col->head_row >= total_rows + RAIN_TAIL_LEN) {
            col_reset(c);
        }
    }

    lv_canvas_finish_layer(s_canvas, &layer);

    // Refresh battery label every ~2 s (100 * 20 ms).
    static int batt_tick = 0;
    if (++batt_tick >= 100) {
        batt_tick = 0;
        update_battery_label();
    }
}

// ---- public API -------------------------------------------------------------

void screen_rain_enter(void)
{
    ESP_LOGI(TAG, "enter");

    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    // Canvas.
    memset(s_cbuf, 0, sizeof(s_cbuf));
    s_canvas = lv_canvas_create(s_scr);
    lv_canvas_set_buffer(s_canvas, s_cbuf, RAIN_W, RAIN_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_canvas, 0, 0);

    // Fill canvas with black background.
    lv_canvas_fill_bg(s_canvas, lv_color_hex(COL_BG), LV_OPA_COVER);

    // Battery label (top-right).
    s_batt_label = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_batt_label, lv_color_hex(COL_MID), 0);
    lv_obj_set_style_text_font(s_batt_label, &lv_font_montserrat_8, 0);
    lv_obj_align(s_batt_label, LV_ALIGN_TOP_RIGHT, -4, 4);
    update_battery_label();

    // Initialise column state.
    for (int c = 0; c < RAIN_COLS; c++) {
        col_reset(c);
    }

    lv_screen_load(s_scr);

    s_timer = lv_timer_create(rain_timer_cb, 20, NULL);
}

void screen_rain_exit(void)
{
    ESP_LOGI(TAG, "exit");

    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr        = NULL;
        s_canvas     = NULL;
        s_batt_label = NULL;
    }
}

void screen_rain_key(int btn, int event)
{
    // Any short press wakes to menu.
    (void)btn;
    if (event == BSP_BTN_CLICK) {
        app_state_set(APP_STATE_MENU);
    }
}
