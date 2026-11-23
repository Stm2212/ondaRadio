#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    /* Increments whenever externally visible Wi-Fi status changes. */
    uint32_t status_generation;
    bool connected;
    bool provisioning_active;
    char ssid[33];
    char provisioning_ssid[33];
} radio_wifi_info_t;

/* The callback receives a temporary snapshot; copy it if it must be retained. */
typedef void (*radio_wifi_status_cb_t)(const radio_wifi_info_t *info);

esp_err_t radio_wifi_init(void);
esp_err_t radio_wifi_save_and_connect(const char *ssid, const char *password,
                                      int authmode);
bool radio_wifi_is_connected(void);
esp_err_t radio_wifi_start_provisioning(void);
esp_err_t radio_wifi_get_info(radio_wifi_info_t *info);
/* Copies the active SoftAP credentials for an explicit local QR display. */
esp_err_t radio_wifi_get_portal_credentials(char *ssid, size_t ssid_capacity,
                                            char *password,
                                            size_t password_capacity);
void radio_wifi_set_status_callback(radio_wifi_status_cb_t callback);
