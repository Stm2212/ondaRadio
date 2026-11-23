#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define RADIO_WIFI_CREDENTIALS_SSID_SIZE 33U
#define RADIO_WIFI_CREDENTIALS_PASSWORD_SIZE 65U
#define RADIO_WIFI_CREDENTIALS_PAYLOAD_SIZE 100U

typedef struct {
    char ssid[RADIO_WIFI_CREDENTIALS_SSID_SIZE];
    char password[RADIO_WIFI_CREDENTIALS_PASSWORD_SIZE];
    bool authmode_stored;
    uint8_t authmode;
} radio_wifi_credentials_t;

/* Fixed-width, zero-padded payload for the snapshot store. */
esp_err_t radio_wifi_credentials_encode(
    const radio_wifi_credentials_t *credentials, uint8_t *payload,
    size_t capacity);

/* Decodes only complete, bounded records; the caller validates authmode. */
esp_err_t radio_wifi_credentials_decode(
    const uint8_t *payload, size_t payload_size,
    radio_wifi_credentials_t *credentials);
