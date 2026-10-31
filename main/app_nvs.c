// main/app_nvs.c - NVS persistence for AI Passport.
#include "app_nvs.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG        = "app_nvs";
#define NVS_NAMESPACE           "passport"
#define NVS_KEY_CONFIG          "cfg"

esp_err_t app_nvs_load(app_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    // Start with safe defaults.
    memset(cfg, 0, sizeof(*cfg));
    cfg->agent_count    = 0;
    cfg->selected_agent = 0;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "namespace not found, using defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t required = sizeof(*cfg);
    err = nvs_get_blob(handle, NVS_KEY_CONFIG, cfg, &required);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "config key not found, using defaults");
        memset(cfg, 0, sizeof(*cfg));
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob failed: %s", esp_err_to_name(err));
        return err;
    }

    // Clamp to sane values.
    if (cfg->agent_count < 0 || cfg->agent_count > MAX_AGENTS) {
        cfg->agent_count = 0;
    }
    if (cfg->selected_agent < 0 || cfg->selected_agent >= MAX_AGENTS) {
        cfg->selected_agent = 0;
    }

    // Ensure null termination of all strings.
    cfg->wifi_ssid[sizeof(cfg->wifi_ssid) - 1] = '\0';
    cfg->wifi_pass[sizeof(cfg->wifi_pass) - 1] = '\0';
    for (int i = 0; i < cfg->agent_count; i++) {
        cfg->agents[i].id[sizeof(cfg->agents[i].id) - 1]     = '\0';
        cfg->agents[i].name[sizeof(cfg->agents[i].name) - 1] = '\0';
    }

    ESP_LOGI(TAG, "loaded config: %d agents, selected=%d, ssid=%s",
             cfg->agent_count, cfg->selected_agent, cfg->wifi_ssid);
    return ESP_OK;
}

esp_err_t app_nvs_save(const app_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(handle, NVS_KEY_CONFIG, cfg, sizeof(*cfg));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs save failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "saved config: %d agents, selected=%d, ssid=%s",
             cfg->agent_count, cfg->selected_agent, cfg->wifi_ssid);
    return ESP_OK;
}
