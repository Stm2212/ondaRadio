#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "radio_types.h"

#define RADIO_FAVORITES_MAX 8
#define RADIO_RECENTS_MAX 8

typedef void (*radio_station_store_result_cb_t)(esp_err_t result,
                                                 bool is_favorite,
                                                 void *context);
typedef void (*radio_station_store_error_cb_t)(esp_err_t result,
                                                void *context);

/* Initialize after nvs_flash_init(). Calls are synchronized after init.
   Favorite changes commit immediately; recent stations, current station and
   preferences are cached and saved after a short quiet period. Flush pending
   changes before an orderly power-off. */
esp_err_t radio_station_store_init(void);
esp_err_t radio_station_store_flush(void);
void radio_station_store_set_error_callback(
    radio_station_store_error_cb_t callback, void *context);
int radio_station_store_volume(void);
esp_err_t radio_station_store_set_volume(int volume);
size_t radio_station_store_favorites(radio_station_t *out, size_t capacity);
size_t radio_station_store_recents(radio_station_t *out, size_t capacity);
bool radio_station_store_is_favorite(const radio_station_t *station);
esp_err_t radio_station_store_toggle_favorite(const radio_station_t *station,
                                               bool *is_favorite);
esp_err_t radio_station_store_toggle_favorite_async(
    const radio_station_t *station, radio_station_store_result_cb_t callback,
    void *context);
esp_err_t radio_station_store_move_favorite(const char *uuid, int direction);
esp_err_t radio_station_store_move_favorite_async(
    const char *uuid, int direction,
    radio_station_store_result_cb_t callback, void *context);
esp_err_t radio_station_store_add_recent(const radio_station_t *station);
esp_err_t radio_station_store_set_current(const radio_station_t *station);
bool radio_station_store_get_current(radio_station_t *station);
int radio_station_store_last_view(void);
esp_err_t radio_station_store_set_last_view(int view);
int radio_station_store_sleep_preset(void);
esp_err_t radio_station_store_set_sleep_preset(int minutes);
int radio_station_store_screen_brightness(void);
esp_err_t radio_station_store_set_screen_brightness(int percent);
int radio_station_store_screen_timeout_seconds(void);
esp_err_t radio_station_store_set_screen_timeout_seconds(int seconds);
int radio_station_store_ui_theme(void);
int radio_station_store_ui_accent(void);
int radio_station_store_screensaver_style(void);
esp_err_t radio_station_store_set_ui_appearance(int theme, int accent,
                                                 int screensaver_style);
