// main/app_state.h - Global application state machine for AI Passport.
#pragma once

typedef enum {
    APP_STATE_IDLE,          // Matrix rain screensaver
    APP_STATE_MENU,          // Agent select menu
    APP_STATE_TALKING,       // Recording + waiting for reply
    APP_STATE_PLAYING,       // Playing back agent reply
    APP_STATE_PROVISIONING,  // BLE provisioning mode
} app_state_t;

// Initialise the state machine (must call before app_state_set/get).
void app_state_init(void);

// Return the current application state.
app_state_t app_state_get(void);

// Transition to a new state; notifies registered listener.
void app_state_set(app_state_t state);

// Callback type invoked on every state transition.
typedef void (*app_state_cb_t)(app_state_t old_state, app_state_t new_state);

// Register a listener; only one listener supported.
void app_state_set_callback(app_state_cb_t cb);
