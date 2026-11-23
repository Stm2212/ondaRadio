#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "radio_wifi_credentials_codec.h"

typedef struct {
    radio_wifi_credentials_t credentials;
    bool has_credentials;
    bool from_snapshot;
    bool needs_snapshot_migration;
    bool legacy_invalid;
} radio_wifi_credentials_load_result_t;

/* Reads the snapshot first, or validates the legacy STA keys if it is absent.
   This function never writes the legacy namespace. */
esp_err_t radio_wifi_credentials_store_load(
    radio_wifi_credentials_load_result_t *result);

/* NVS-worker job: stores credentials in the independent snapshot unit. */
esp_err_t radio_wifi_credentials_store_save_job(void *context);
