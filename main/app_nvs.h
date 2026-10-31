// main/app_nvs.h - NVS persistence for AI Passport agent list and WiFi creds.
#pragma once

#include "esp_err.h"
#include <stdint.h>

#define MAX_AGENTS 10

typedef struct {
    char    id[32];
    char    name[64];
    int64_t chat_id;  // Telegram chat_id of the agent bot
} agent_entry_t;

typedef struct {
    agent_entry_t agents[MAX_AGENTS];
    int           agent_count;
    int           selected_agent;  // index into agents[]
    char          wifi_ssid[64];
    char          wifi_pass[64];
} app_config_t;

// Load configuration from NVS into *cfg.
// Returns ESP_OK on success; on ESP_ERR_NVS_NOT_FOUND, fills *cfg with defaults.
esp_err_t app_nvs_load(app_config_t *cfg);

// Save configuration to NVS.
esp_err_t app_nvs_save(const app_config_t *cfg);
