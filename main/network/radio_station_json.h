#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "radio_types.h"

/* Decode a bounded station object without silently truncating any field. */
esp_err_t radio_station_json_decode(const char *json, size_t length,
                                    radio_station_t *station);
