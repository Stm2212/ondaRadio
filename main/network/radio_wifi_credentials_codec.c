#include "radio_wifi_credentials_codec.h"

#include <string.h>

esp_err_t radio_wifi_credentials_encode(
    const radio_wifi_credentials_t *credentials, uint8_t *payload,
    size_t capacity)
{
    if (credentials == NULL || payload == NULL ||
        capacity != RADIO_WIFI_CREDENTIALS_PAYLOAD_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(payload, 0, capacity);
    const size_t ssid_length =
        strnlen(credentials->ssid, sizeof(credentials->ssid));
    const size_t password_length =
        strnlen(credentials->password, sizeof(credentials->password));
    if (ssid_length == 0U ||
        ssid_length >= sizeof(credentials->ssid) ||
        password_length >= sizeof(credentials->password)) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(payload, credentials->ssid, ssid_length + 1U);
    memcpy(payload + RADIO_WIFI_CREDENTIALS_SSID_SIZE,
           credentials->password, password_length + 1U);
    payload[98] = credentials->authmode_stored ? 1U : 0U;
    payload[99] = credentials->authmode;
    return ESP_OK;
}

esp_err_t radio_wifi_credentials_decode(
    const uint8_t *payload, size_t payload_size,
    radio_wifi_credentials_t *credentials)
{
    if (payload == NULL || credentials == NULL) return ESP_ERR_INVALID_ARG;
    memset(credentials, 0, sizeof(*credentials));
    if (payload_size != RADIO_WIFI_CREDENTIALS_PAYLOAD_SIZE) {
        return ESP_ERR_INVALID_STATE;
    }

    const char *ssid = (const char *)payload;
    const char *password =
        (const char *)payload + RADIO_WIFI_CREDENTIALS_SSID_SIZE;
    const size_t ssid_length =
        strnlen(ssid, RADIO_WIFI_CREDENTIALS_SSID_SIZE);
    const size_t password_length =
        strnlen(password, RADIO_WIFI_CREDENTIALS_PASSWORD_SIZE);
    if (ssid_length == 0U ||
        ssid_length >= RADIO_WIFI_CREDENTIALS_SSID_SIZE ||
        password_length >= RADIO_WIFI_CREDENTIALS_PASSWORD_SIZE ||
        payload[98] > 1U) {
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(credentials->ssid, ssid, ssid_length + 1U);
    memcpy(credentials->password, password, password_length + 1U);
    credentials->authmode_stored = payload[98] == 1U;
    credentials->authmode = payload[99];
    return ESP_OK;
}
