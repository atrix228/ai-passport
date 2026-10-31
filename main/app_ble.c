// main/app_ble.c - BLE GATT server for AI Passport using NimBLE.
//
// Service: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
//   6E400002 PROVISION_WRITE  (Write)
//   6E400003 AUDIO_OUT        (Notify)
//   6E400004 AUDIO_IN         (Write)
//   6E400005 STATUS           (Notify)
//
// Advertising starts on boot; stops when a central connects; resumes on disconnect.

#include "app_ble.h"
#include "app_nvs.h"
#include "app_state.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "app_ble";

#define DEVICE_NAME     "PassportAgent"

// Max reassembly buffer (approx 10 s of 16kHz 16-bit mono = 320 kB — cap at 512 kB).
#define AUDIO_IN_BUF_MAX (512 * 1024)

// Audio chunk header: [1 byte seq][1 byte is_last][data...]
#define CHUNK_HDR_LEN   2

// ---- UUIDs ------------------------------------------------------------------

// Base: 6E40xxxx-B5A3-F393-E0A9-E50E24DCCA9E
static const ble_uuid128_t UUID_SVC = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e
);
static const ble_uuid128_t UUID_PROVISION = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e
);
static const ble_uuid128_t UUID_AUDIO_OUT = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e
);
static const ble_uuid128_t UUID_AUDIO_IN = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x04, 0x00, 0x40, 0x6e
);
static const ble_uuid128_t UUID_STATUS = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x05, 0x00, 0x40, 0x6e
);

// ---- state ------------------------------------------------------------------

static uint16_t s_conn_handle       = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_audio_out_handle  = 0;
static uint16_t s_status_handle     = 0;

static SemaphoreHandle_t s_mutex;

// AUDIO_IN reassembly.
static uint8_t  *s_audio_in_buf    = NULL;
static size_t    s_audio_in_len    = 0;
static uint8_t   s_audio_in_seq    = 0;

// AUDIO_OUT sequence counter.
static uint8_t   s_audio_out_seq   = 0;

// Callbacks.
static app_ble_audio_ready_cb_t s_audio_ready_cb  = NULL;
static app_ble_provision_cb_t   s_provision_cb    = NULL;

// ---- forward declarations ---------------------------------------------------

static void start_advertising(void);

// ---- provisioning JSON parser -----------------------------------------------

static void handle_provision(const uint8_t *data, size_t len)
{
    char *json = malloc(len + 1);
    if (!json) {
        ESP_LOGE(TAG, "OOM for provision JSON");
        return;
    }
    memcpy(json, data, len);
    json[len] = '\0';

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse failed");
        return;
    }

    app_config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) {
        cJSON_Delete(root);
        return;
    }

    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "wifi_ssid");
    cJSON *pass = cJSON_GetObjectItemCaseSensitive(root, "wifi_pass");
    cJSON *sel  = cJSON_GetObjectItemCaseSensitive(root, "selected");
    cJSON *agents = cJSON_GetObjectItemCaseSensitive(root, "agents");

    if (cJSON_IsString(ssid)) {
        strncpy(cfg->wifi_ssid, ssid->valuestring, sizeof(cfg->wifi_ssid) - 1);
    }
    if (cJSON_IsString(pass)) {
        strncpy(cfg->wifi_pass, pass->valuestring, sizeof(cfg->wifi_pass) - 1);
    }
    if (cJSON_IsNumber(sel)) {
        cfg->selected_agent = (int)sel->valuedouble;
    }

    if (cJSON_IsArray(agents)) {
        int count = cJSON_GetArraySize(agents);
        if (count > MAX_AGENTS) count = MAX_AGENTS;
        cfg->agent_count = count;

        for (int i = 0; i < count; i++) {
            cJSON *a   = cJSON_GetArrayItem(agents, i);
            cJSON *aid = cJSON_GetObjectItemCaseSensitive(a, "id");
            cJSON *aname = cJSON_GetObjectItemCaseSensitive(a, "name");
            cJSON *acid  = cJSON_GetObjectItemCaseSensitive(a, "chat_id");

            if (cJSON_IsString(aid)) {
                strncpy(cfg->agents[i].id, aid->valuestring, sizeof(cfg->agents[i].id) - 1);
            }
            if (cJSON_IsString(aname)) {
                strncpy(cfg->agents[i].name, aname->valuestring, sizeof(cfg->agents[i].name) - 1);
            }
            if (cJSON_IsNumber(acid)) {
                cfg->agents[i].chat_id = (int64_t)acid->valuedouble;
            }
        }
    }

    cJSON_Delete(root);

    // Save to NVS.
    app_nvs_save(cfg);
    ESP_LOGI(TAG, "provisioned: %d agents, ssid=%s", cfg->agent_count, cfg->wifi_ssid);

    // Notify app.
    if (s_provision_cb) {
        s_provision_cb(cfg);
    }
    free(cfg);
}

// ---- AUDIO_IN reassembly ----------------------------------------------------

static void handle_audio_in_chunk(const uint8_t *data, size_t len)
{
    if (len < CHUNK_HDR_LEN) return;

    uint8_t seq     = data[0];
    uint8_t is_last = data[1];
    const uint8_t *payload = data + CHUNK_HDR_LEN;
    size_t payload_len     = len - CHUNK_HDR_LEN;

    // Simple in-order reassembly (seq unused for strict ordering here).
    (void)seq;

    if (s_audio_in_len + payload_len > AUDIO_IN_BUF_MAX) {
        ESP_LOGW(TAG, "AUDIO_IN overflow, dropping");
        return;
    }

    uint8_t *new_buf = realloc(s_audio_in_buf, s_audio_in_len + payload_len);
    if (!new_buf) {
        ESP_LOGE(TAG, "OOM for AUDIO_IN buf");
        return;
    }
    s_audio_in_buf = new_buf;
    memcpy(s_audio_in_buf + s_audio_in_len, payload, payload_len);
    s_audio_in_len += payload_len;

    if (is_last) {
        ESP_LOGI(TAG, "AUDIO_IN complete: %zu bytes", s_audio_in_len);
        if (s_audio_ready_cb && s_audio_in_len > 0) {
            // Transfer ownership to callback.
            uint8_t *buf = s_audio_in_buf;
            size_t   blen = s_audio_in_len;
            s_audio_in_buf = NULL;
            s_audio_in_len = 0;
            s_audio_ready_cb(buf, blen);
        } else {
            free(s_audio_in_buf);
            s_audio_in_buf = NULL;
            s_audio_in_len = 0;
        }
    }
}

// ---- GATT characteristic access callbacks -----------------------------------

static int gatt_provision_write_cb(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_REQ_NOT_SUPPORTED;

    size_t len = OS_MBUF_PKTLEN(ctxt->om);
    uint8_t *buf = malloc(len);
    if (!buf) return BLE_ATT_ERR_INSUFFICIENT_RES;

    uint16_t out_len = (uint16_t)len;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, len, &out_len);
    if (rc != 0) {
        free(buf);
        return BLE_ATT_ERR_UNLIKELY;
    }

    handle_provision(buf, out_len);
    free(buf);
    return 0;
}

static int gatt_audio_out_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    // Read not supported; only notify.
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }
    return 0;
}

static int gatt_audio_in_cb(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_REQ_NOT_SUPPORTED;

    size_t len = OS_MBUF_PKTLEN(ctxt->om);
    uint8_t *buf = malloc(len);
    if (!buf) return BLE_ATT_ERR_INSUFFICIENT_RES;

    uint16_t out_len = (uint16_t)len;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, len, &out_len);
    if (rc != 0) {
        free(buf);
        return BLE_ATT_ERR_UNLIKELY;
    }

    handle_audio_in_chunk(buf, out_len);
    free(buf);
    return 0;
}

static int gatt_status_cb(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }
    return 0;
}

// ---- GATT service definition ------------------------------------------------

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SVC.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                // PROVISION_WRITE
                .uuid       = &UUID_PROVISION.u,
                .access_cb  = gatt_provision_write_cb,
                .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                // AUDIO_OUT (notify)
                .uuid       = &UUID_AUDIO_OUT.u,
                .access_cb  = gatt_audio_out_cb,
                .val_handle = &s_audio_out_handle,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
            },
            {
                // AUDIO_IN (write)
                .uuid       = &UUID_AUDIO_IN.u,
                .access_cb  = gatt_audio_in_cb,
                .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                // STATUS (notify)
                .uuid       = &UUID_STATUS.u,
                .access_cb  = gatt_status_cb,
                .val_handle = &s_status_handle,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 } // terminator
        },
    },
    { 0 } // terminator
};

// ---- GAP event handler ------------------------------------------------------

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "connected handle=%d", s_conn_handle);

            // Request larger MTU.
            ble_att_set_preferred_mtu(512);
            ble_gattc_exchange_mtu(s_conn_handle, NULL, NULL);
        } else {
            ESP_LOGW(TAG, "connect failed status=%d", event->connect.status);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected reason=%d", event->disconnect.reason);
        s_conn_handle  = BLE_HS_CONN_HANDLE_NONE;
        s_audio_out_seq = 0;
        // Free any partial AUDIO_IN buffer.
        if (s_audio_in_buf) {
            free(s_audio_in_buf);
            s_audio_in_buf = NULL;
            s_audio_in_len = 0;
        }
        start_advertising();
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU negotiated: %d", event->mtu.value);
        break;

    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGI(TAG, "conn params updated");
        break;

    default:
        break;
    }
    return 0;
}

// ---- advertising ------------------------------------------------------------

static void start_advertising(void)
{
    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name  = (const uint8_t *)DEVICE_NAME;
    fields.name_len = strlen(DEVICE_NAME);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields rc=%d", rc);
        return;
    }

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                            &adv_params, gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv_start rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "advertising started");
    }
}

// ---- NimBLE host task -------------------------------------------------------

static void nimble_host_task(void *param)
{
    (void)param;
    nimble_port_run(); // blocks until nimble_port_stop()
    nimble_port_freertos_deinit();
}

static void ble_on_sync(void)
{
    // Set public address.
    uint8_t addr[6] = {0};
    int rc = ble_hs_util_ensure_addr(0);
    (void)rc;
    ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, addr, NULL);
    ESP_LOGI(TAG, "BLE addr: %02x:%02x:%02x:%02x:%02x:%02x",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

    start_advertising();
}

static void ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE reset reason=%d", reason);
}

// ---- public API -------------------------------------------------------------

esp_err_t app_ble_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb  = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs rc=%d", rc);
        return ESP_FAIL;
    }

    rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGW(TAG, "device name set rc=%d", rc);
    }

    nimble_port_freertos_init(nimble_host_task);
    ESP_LOGI(TAG, "BLE GATT server initialised");
    return ESP_OK;
}

esp_err_t app_ble_send_audio(const uint8_t *data, size_t len, bool is_last)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_audio_out_handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    // Build chunk: [seq][is_last][data]
    size_t chunk_len = CHUNK_HDR_LEN + len;
    uint8_t *chunk = malloc(chunk_len);
    if (!chunk) return ESP_ERR_NO_MEM;

    chunk[0] = s_audio_out_seq++;
    chunk[1] = is_last ? 1 : 0;
    if (len > 0 && data) {
        memcpy(chunk + CHUNK_HDR_LEN, data, len);
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(chunk, (uint16_t)chunk_len);
    free(chunk);
    if (!om) return ESP_ERR_NO_MEM;

    int rc = ble_gatts_notify_custom(s_conn_handle, s_audio_out_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "notify audio rc=%d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t app_ble_send_status(uint8_t state)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_status_handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(&state, 1);
    if (!om) return ESP_ERR_NO_MEM;

    int rc = ble_gatts_notify_custom(s_conn_handle, s_status_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "notify status rc=%d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool app_ble_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

void app_ble_set_audio_ready_cb(app_ble_audio_ready_cb_t cb)
{
    s_audio_ready_cb = cb;
}

void app_ble_set_provision_cb(app_ble_provision_cb_t cb)
{
    s_provision_cb = cb;
}
