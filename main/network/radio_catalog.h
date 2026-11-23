#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "radio_types.h"

#define RADIO_CATALOG_PAGE_SIZE 20

typedef enum {
    RADIO_CATALOG_FILTER_COUNTRY = 0,
    RADIO_CATALOG_FILTER_GENRE,
    RADIO_CATALOG_FILTER_LANGUAGE,
    RADIO_CATALOG_FILTER_CODEC,
} radio_catalog_filter_t;

esp_err_t radio_catalog_init(void);
esp_err_t radio_catalog_load_popular(size_t offset, uint32_t request_id);
esp_err_t radio_catalog_search_name(const char *name, size_t offset,
                                    uint32_t request_id);
esp_err_t radio_catalog_search_filter(radio_catalog_filter_t filter,
                                      const char *value, size_t offset,
                                      uint32_t request_id);
void radio_catalog_set_callback(radio_catalog_callback_t callback);
