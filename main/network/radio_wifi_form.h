#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Bounds match the Wi-Fi driver limits: 32-byte SSID and 64-byte passphrase. */
#define RADIO_WIFI_FORM_SSID_MAX 32
#define RADIO_WIFI_FORM_PASSWORD_MAX 64
/* Sentinel for "detect the security mode automatically". The portal sends it
   when the network is entered manually instead of picked from a scan result;
   radio_wifi_save_and_connect() maps any negative value to auto-detection. */
#define RADIO_WIFI_FORM_AUTHMODE_AUTO (-1)

typedef struct {
    char ssid[RADIO_WIFI_FORM_SSID_MAX + 1];
    char password[RADIO_WIFI_FORM_PASSWORD_MAX + 1];
    int authmode;
} radio_wifi_form_t;

/* Parse an application/x-www-form-urlencoded Wi-Fi submission from the local
   portal. `body` is modified in place and must be NUL-terminated at
   `body[length]`. On success returns true and fills `out`; on any malformed
   field, duplicate key, missing SSID or embedded NUL it returns false and
   clears `out`. Unknown keys are ignored and `password` may be empty (open
   network). `authmode` is RADIO_WIFI_FORM_AUTHMODE_AUTO when the field is
   absent. */
bool radio_wifi_form_parse(char *body, size_t length, radio_wifi_form_t *out);
