// main/screen_rain.h - Matrix rain screensaver for AI Passport.
#pragma once

// Initialise and show the Matrix rain screen.
// Must be called while holding bsp_lvgl_lock().
void screen_rain_enter(void);

// Destroy the Matrix rain screen and stop its timer.
// Must be called while holding bsp_lvgl_lock().
void screen_rain_exit(void);

// Button event handler — call from the input dispatch task.
// btn: BSP_BTN_UP / BSP_BTN_DOWN / BSP_BTN_OK
// event: BSP_BTN_CLICK / BSP_BTN_LONG
void screen_rain_key(int btn, int event);
