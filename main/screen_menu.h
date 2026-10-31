// main/screen_menu.h - Agent select menu for AI Passport.
#pragma once

#include "app_nvs.h"

// Initialise and show the agent select menu.
// Must be called while holding bsp_lvgl_lock().
void screen_menu_enter(const app_config_t *cfg);

// Destroy the agent select menu.
// Must be called while holding bsp_lvgl_lock().
void screen_menu_exit(void);

// Button event handler — call from the input dispatch task.
void screen_menu_key(int btn, int event);

// Update the menu's agent list (e.g. after BLE provisioning).
// Must be called while holding bsp_lvgl_lock().
void screen_menu_update(const app_config_t *cfg);

// Return the currently highlighted agent index (for the caller to use after OK).
int screen_menu_selected_index(void);
