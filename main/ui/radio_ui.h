#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "radio_types.h"

esp_err_t radio_ui_init(void);
void radio_ui_bind_services(void);
void radio_ui_refresh_volume(void);
void radio_ui_wake_display(void);
void radio_ui_request_power_off(void);

#ifdef RADIO_UI_TEST_FAULT_INJECTION
#include "radio_wifi.h"

typedef enum {
    RADIO_UI_TEST_MODAL_APPEARANCE = 0,
    RADIO_UI_TEST_MODAL_SCREEN,
    RADIO_UI_TEST_MODAL_CLOCK,
    RADIO_UI_TEST_MODAL_SLEEP_TIMER,
    RADIO_UI_TEST_MODAL_PORTAL_QR,
    RADIO_UI_TEST_MODAL_COUNT,
} radio_ui_test_modal_t;

typedef enum {
    RADIO_UI_TEST_DYNAMIC_STATION_ROW = 0,
    RADIO_UI_TEST_DYNAMIC_EXPLORE_ROW,
    RADIO_UI_TEST_DYNAMIC_EMPTY_STATE,
    RADIO_UI_TEST_DYNAMIC_CATALOG_FAILURE,
    RADIO_UI_TEST_DYNAMIC_PREVIOUS_BUTTON,
    RADIO_UI_TEST_DYNAMIC_EXPLORE_OPTIONS,
    RADIO_UI_TEST_DYNAMIC_COUNT,
} radio_ui_test_dynamic_builder_t;

esp_err_t radio_ui_test_build_views(void);
esp_err_t radio_ui_test_init_with_idle_failure(bool fail_timer);
esp_err_t radio_ui_test_init_with_player_failure(void);
esp_err_t radio_ui_test_init_with_wifi_failure(void);
esp_err_t radio_ui_test_init_with_favorite_failure(void);
esp_err_t radio_ui_test_init_with_catalog_failure(void);
esp_err_t radio_ui_test_init_with_volume_failure(void);
esp_err_t radio_ui_test_init_with_station_buffer_failure(void);
esp_err_t radio_ui_test_init_with_header_failure(void);
bool radio_ui_test_unpublished_resources_clear(void);
bool radio_ui_test_failed_catalog_navigation_keeps_state(void);
bool radio_ui_test_home_station_scroll_configured(void);
bool radio_ui_test_layout_contract(void);
bool radio_ui_test_wifi_out_of_order(const radio_wifi_info_t *newer,
                                     const radio_wifi_info_t *older);
void radio_ui_test_discard_views(void);
bool radio_ui_test_views_are_clear(void);
bool radio_ui_test_staging_is_clear(void);
esp_err_t radio_ui_test_rebuild_theme(void);
void radio_ui_test_load_page(unsigned page);
void *radio_ui_test_active_screen(void);
size_t radio_ui_test_view_root_count(void);
int radio_ui_test_theme_id(void);
int radio_ui_test_accent_id(void);
uint32_t radio_ui_test_catalog_request_id(void);
size_t radio_ui_test_station_count(void);
void radio_ui_test_build_modal(unsigned modal_id);
void radio_ui_test_close_modal(void);
bool radio_ui_test_modal_exists(void);
uint32_t radio_ui_test_settings_child_count(void);
esp_err_t radio_ui_test_build_dynamic(unsigned builder_id);
uint32_t radio_ui_test_radio_list_child_count(void);
/* Seeds the saved-station list used to render the Favorites tab. */
esp_err_t radio_ui_test_seed_stations(const radio_station_t *stations,
                                      size_t count);
/* True when every radio-list row still resolves its click token to its own
   live context (guards against stale tokens after a view publish). */
bool radio_ui_test_row_contexts_resolve(void);
#endif
