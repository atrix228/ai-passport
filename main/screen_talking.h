// main/screen_talking.h - Talking/recording screen for AI Passport.
#pragma once

#include "app_state.h"

// Initialise and show the talking screen with agent name.
// Must be called while holding bsp_lvgl_lock().
void screen_talking_enter(const char *agent_name);

// Destroy the talking screen.
// Must be called while holding bsp_lvgl_lock().
void screen_talking_exit(void);

// Update the status message shown on the talking screen.
// Safe to call from any task (takes the lock internally).
void screen_talking_set_status(const char *status);

// Button event handler — call from the input dispatch task.
void screen_talking_key(int btn, int event);
