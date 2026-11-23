#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    RADIO_STARTUP_STAGE_NONE = 0,
    RADIO_STARTUP_STAGE_POWER_HOLD,
    RADIO_STARTUP_STAGE_NVS_FLASH,
    RADIO_STARTUP_STAGE_STATION_STORE,
    RADIO_STARTUP_STAGE_BOARD,
    RADIO_STARTUP_STAGE_RECOVERY_VIEW,
    RADIO_STARTUP_STAGE_TOUCH,
    RADIO_STARTUP_STAGE_AUDIO,
    RADIO_STARTUP_STAGE_WIFI,
    RADIO_STARTUP_STAGE_CATALOG,
    RADIO_STARTUP_STAGE_UI,
    RADIO_STARTUP_STAGE_BUTTONS,
    RADIO_STARTUP_STAGE_COUNT,
} radio_startup_stage_t;

typedef enum {
    RADIO_STARTUP_CAP_POWER_HOLD = 0,
    RADIO_STARTUP_CAP_STORAGE,
    RADIO_STARTUP_CAP_DISPLAY,
    RADIO_STARTUP_CAP_RECOVERY_VIEW,
    RADIO_STARTUP_CAP_TOUCH,
    RADIO_STARTUP_CAP_AUDIO,
    RADIO_STARTUP_CAP_WIFI,
    RADIO_STARTUP_CAP_CATALOG,
    RADIO_STARTUP_CAP_UI,
    RADIO_STARTUP_CAP_BUTTONS,
    RADIO_STARTUP_CAP_COUNT,
} radio_startup_capability_t;

typedef enum {
    RADIO_STARTUP_MODE_READY = 0,
    RADIO_STARTUP_MODE_DEGRADED,
    RADIO_STARTUP_MODE_RECOVERY,
    RADIO_STARTUP_MODE_STOPPED,
} radio_startup_mode_t;

typedef struct {
    bool attempted;
    bool available;
    esp_err_t error;
    radio_startup_stage_t cause;
} radio_startup_stage_result_t;

typedef struct {
    radio_startup_mode_t mode;
    radio_startup_stage_t first_failure;
    esp_err_t first_error;
    uint32_t capabilities;
    radio_startup_stage_result_t stages[RADIO_STARTUP_STAGE_COUNT];
} radio_startup_report_t;

typedef esp_err_t (*radio_startup_step_fn)(void *context);
typedef void (*radio_startup_bind_fn)(void *context);
typedef void (*radio_startup_recovery_fn)(void *context,
                                          radio_startup_stage_t cause,
                                          esp_err_t error);

typedef struct {
    radio_startup_step_fn power_hold_init;
    radio_startup_step_fn nvs_flash_init;
    radio_startup_step_fn station_store_init;
    radio_startup_step_fn board_init;
    radio_startup_step_fn recovery_view_reserve;
    radio_startup_step_fn touch_status;
    radio_startup_step_fn audio_init;
    radio_startup_step_fn wifi_init;
    radio_startup_step_fn catalog_init;
    radio_startup_step_fn ui_init;
    radio_startup_bind_fn bind_services;
    radio_startup_step_fn buttons_init;
    radio_startup_recovery_fn show_recovery;
} radio_startup_ops_t;

/* Runs ordered startup stages and returns their capability/error table.
   Expected service failures are represented in report and return ESP_OK. */
esp_err_t radio_startup_run(const radio_startup_ops_t *ops, void *context,
                            radio_startup_report_t *report);

bool radio_startup_has_capability(const radio_startup_report_t *report,
                                  radio_startup_capability_t capability);
const radio_startup_stage_result_t *radio_startup_stage_result(
    const radio_startup_report_t *report, radio_startup_stage_t stage);
const char *radio_startup_stage_name(radio_startup_stage_t stage);
const char *radio_startup_capability_name(
    radio_startup_capability_t capability);
const char *radio_startup_mode_name(radio_startup_mode_t mode);
