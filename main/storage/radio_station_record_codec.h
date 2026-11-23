#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "radio_types.h"

#define RADIO_STATION_RECORD_MAGIC 0x4F4E4441u
#define RADIO_STATION_RECORD_VERSION 2u
#define RADIO_STATION_RECORD_HEADER_SIZE 8u

/* Decode a compact version-2 station-list blob. This function is pure: it
   reads only the supplied bytes and never opens, writes, or erases NVS. */
esp_err_t radio_station_record_decode_v2(
    const uint8_t *blob, size_t length, radio_station_t *stations,
    size_t station_capacity, uint16_t *station_count);
