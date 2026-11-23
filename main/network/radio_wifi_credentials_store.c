#include "radio_wifi_credentials_store.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "radio_snapshot_store.h"

#define WIFI_NVS_NAMESPACE "radio_cfg"
#define WIFI_SSID_KEY "wifi_ssid"
#define WIFI_PASSWORD_KEY "wifi_pass"
#define WIFI_AUTHMODE_KEY "wifi_auth"

static const char *TAG = "radio_wifi_store";

static void secure_clear(void *memory, size_t length)
{
    volatile uint8_t *bytes = memory;
    while (length-- > 0U) *bytes++ = 0U;
}

static esp_err_t load_legacy_credentials(
    radio_wifi_credentials_load_result_t *result)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    size_t ssid_size = sizeof(result->credentials.ssid);
    err = nvs_get_str(nvs, WIFI_SSID_KEY, result->credentials.ssid,
                      &ssid_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs);
        return ESP_OK;
    }
    if (err != ESP_OK ||
        result->credentials.ssid[0] == '\0') {
        result->legacy_invalid = err != ESP_OK;
        secure_clear(&result->credentials, sizeof(result->credentials));
        nvs_close(nvs);
        return ESP_OK;
    }

    size_t password_size = sizeof(result->credentials.password);
    const esp_err_t password_err = nvs_get_str(
        nvs, WIFI_PASSWORD_KEY, result->credentials.password,
        &password_size);
    if (password_err != ESP_OK && password_err != ESP_ERR_NVS_NOT_FOUND) {
        result->legacy_invalid = true;
        secure_clear(&result->credentials, sizeof(result->credentials));
        nvs_close(nvs);
        return ESP_OK;
    }

    uint8_t authmode = 0U;
    const esp_err_t authmode_err =
        nvs_get_u8(nvs, WIFI_AUTHMODE_KEY, &authmode);
    if (authmode_err == ESP_OK) {
        result->credentials.authmode_stored = true;
        result->credentials.authmode = authmode;
    } else if (authmode_err != ESP_ERR_NVS_NOT_FOUND) {
        result->legacy_invalid = true;
    }
    nvs_close(nvs);

    result->has_credentials = true;
    result->needs_snapshot_migration = true;
    return ESP_OK;
}

esp_err_t radio_wifi_credentials_store_load(
    radio_wifi_credentials_load_result_t *result)
{
    if (result == NULL) return ESP_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));

    uint8_t payload[RADIO_WIFI_CREDENTIALS_PAYLOAD_SIZE] = {0};
    size_t payload_size = sizeof(payload);
    uint32_t generation = 0U;
    esp_err_t err = radio_snapshot_store_read(
        "wifi", RADIO_SNAPSHOT_KIND_WIFI_CREDENTIALS, payload,
        sizeof(payload), &payload_size, &generation);
    if (err == ESP_OK) {
        result->from_snapshot = true;
        err = radio_wifi_credentials_decode(
            payload, payload_size, &result->credentials);
        secure_clear(payload, sizeof(payload));
        if (err != ESP_OK) {
            secure_clear(result, sizeof(*result));
            return ESP_ERR_INVALID_STATE;
        }
        result->has_credentials = true;
        return ESP_OK;
    }
    secure_clear(payload, sizeof(payload));
    if (err != ESP_ERR_NVS_NOT_FOUND) return err;

    err = load_legacy_credentials(result);
    if (err != ESP_OK) {
        secure_clear(result, sizeof(*result));
    }
    return err;
}

esp_err_t radio_wifi_credentials_store_save_job(void *context)
{
    const radio_wifi_credentials_t *credentials = context;
    if (credentials == NULL) return ESP_ERR_INVALID_ARG;

    uint8_t payload[RADIO_WIFI_CREDENTIALS_PAYLOAD_SIZE] = {0};
    esp_err_t err = radio_wifi_credentials_encode(
        credentials, payload, sizeof(payload));
    if (err == ESP_OK) {
        radio_snapshot_store_write_report_t report = {0};
        err = radio_snapshot_store_write(
            "wifi", RADIO_SNAPSHOT_KIND_WIFI_CREDENTIALS, payload,
            sizeof(payload), &report);
        if (report.reconciled_after_error) {
            ESP_LOGW(TAG, "Snapshot Wi-Fi reconciliado tras error NVS: %s",
                     esp_err_to_name(report.operation_error));
        }
    }
    secure_clear(payload, sizeof(payload));
    return err;
}
