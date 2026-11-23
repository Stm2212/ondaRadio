#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "radio_snapshot_codec.h"

#define RADIO_SNAPSHOT_STORE_PARTITION "onda_state"
#define RADIO_SNAPSHOT_STORE_NAMESPACE "radio_state"
#define RADIO_SNAPSHOT_STORE_MAX_PAYLOAD_SIZE 4424U

enum {
    RADIO_SNAPSHOT_KIND_FAVORITES = 0x1001U,
    RADIO_SNAPSHOT_KIND_RECENTS = 0x1002U,
    RADIO_SNAPSHOT_KIND_CURRENT_STATION = 0x1003U,
    RADIO_SNAPSHOT_KIND_PREFERENCES = 0x1004U,
    RADIO_SNAPSHOT_KIND_VOLUME = 0x1005U,
    RADIO_SNAPSHOT_KIND_WIFI_CREDENTIALS = 0x2001U,
};

typedef struct {
    uint32_t generation;
    esp_err_t operation_error;
    bool reconciled_after_error;
    bool uncertain;
} radio_snapshot_store_write_report_t;

/* Initialize the dedicated NVS partition. Never erases or reformats it. */
esp_err_t radio_snapshot_store_init(void);

/* unit is a lowercase ASCII key prefix (1..13 chars); slots are unit_a/b.
   These functions serialize their own scratch storage. Product callers should
   still route writes through radio_nvs_worker. */
esp_err_t radio_snapshot_store_read(
    const char *unit, uint32_t kind, uint8_t *payload, size_t capacity,
    size_t *payload_size, uint32_t *generation);

/* A write is successful if the requested complete record is observed after
   commit/reconciliation. A failed flash operation may therefore be reported
   as success when the new record is found after reopening NVS. */
esp_err_t radio_snapshot_store_write(
    const char *unit, uint32_t kind, const uint8_t *payload,
    size_t payload_size, radio_snapshot_store_write_report_t *report);
