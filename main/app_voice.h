// main/app_voice.h - Voice recording and playback task for AI Passport.
#pragma once

#include "esp_err.h"

// Start voice recording (creates internal FreeRTOS task).
// Should be called when app enters APP_STATE_TALKING.
esp_err_t app_voice_start_recording(void);

// Signal the recording task to stop (non-blocking; task finalises and exits).
void app_voice_stop_recording(void);

// Called when AUDIO_IN reassembly completes (from app_ble callback).
// Takes ownership of buf (must be freed after use).
void app_voice_on_audio_received(uint8_t *buf, size_t len);

// Initialise voice subsystem (call once after bsp_audio_init).
esp_err_t app_voice_init(void);
