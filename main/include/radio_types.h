#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define RADIO_STATION_NAME_MAX 72
#define RADIO_STATION_COUNTRY_MAX 40
#define RADIO_STATION_CODEC_MAX 16
#define RADIO_STATION_URL_MAX 384
#define RADIO_STATION_UUID_MAX 40

typedef struct {
    char name[RADIO_STATION_NAME_MAX];
    char country[RADIO_STATION_COUNTRY_MAX];
    char codec[RADIO_STATION_CODEC_MAX];
    char url[RADIO_STATION_URL_MAX];
    char uuid[RADIO_STATION_UUID_MAX];
} radio_station_t;

/* The station array is owned by the catalog worker and valid only in callback. */
typedef void (*radio_catalog_callback_t)(const radio_station_t *stations,
                                         size_t count,
                                         size_t offset,
                                         size_t next_offset,
                                         bool has_more,
                                         uint32_t request_id,
                                         esp_err_t result);
