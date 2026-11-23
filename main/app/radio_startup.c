#include <string.h>

#include "radio_startup.h"

static bool operations_are_valid(const radio_startup_ops_t *ops)
{
    return ops != NULL && ops->power_hold_init != NULL &&
           ops->nvs_flash_init != NULL && ops->station_store_init != NULL &&
           ops->board_init != NULL && ops->recovery_view_reserve != NULL &&
           ops->touch_status != NULL && ops->audio_init != NULL &&
           ops->wifi_init != NULL && ops->catalog_init != NULL &&
           ops->ui_init != NULL && ops->bind_services != NULL &&
           ops->buttons_init != NULL && ops->show_recovery != NULL;
}

static uint32_t capability_mask(radio_startup_capability_t capability)
{
    if (capability < 0 || capability >= RADIO_STARTUP_CAP_COUNT) return 0;
    return UINT32_C(1) << (unsigned)capability;
}

bool radio_startup_has_capability(const radio_startup_report_t *report,
                                  radio_startup_capability_t capability)
{
    const uint32_t mask = capability_mask(capability);
    return report != NULL && mask != 0 &&
           (report->capabilities & mask) != 0;
}

const radio_startup_stage_result_t *radio_startup_stage_result(
    const radio_startup_report_t *report, radio_startup_stage_t stage)
{
    if (report == NULL || stage <= RADIO_STARTUP_STAGE_NONE ||
        stage >= RADIO_STARTUP_STAGE_COUNT) {
        return NULL;
    }
    return &report->stages[stage];
}

static void grant_capability(radio_startup_report_t *report,
                             radio_startup_capability_t capability)
{
    report->capabilities |= capability_mask(capability);
}

static void note_failure(radio_startup_report_t *report,
                         radio_startup_stage_t stage, bool attempted,
                         esp_err_t error, radio_startup_stage_t cause)
{
    radio_startup_stage_result_t *result = &report->stages[stage];
    result->attempted = attempted;
    result->available = false;
    result->error = error;
    result->cause = cause;
    if (report->first_failure == RADIO_STARTUP_STAGE_NONE) {
        report->first_failure = cause;
        report->first_error = error;
    }
    if (report->mode == RADIO_STARTUP_MODE_READY) {
        report->mode = RADIO_STARTUP_MODE_DEGRADED;
    }
}

static esp_err_t run_stage(radio_startup_report_t *report,
                           radio_startup_stage_t stage,
                           radio_startup_step_fn operation, void *context,
                           radio_startup_capability_t capability)
{
    radio_startup_stage_result_t *result = &report->stages[stage];
    result->attempted = true;
    result->error = operation(context);
    result->available = result->error == ESP_OK;
    result->cause = result->available ? RADIO_STARTUP_STAGE_NONE : stage;
    if (result->available) {
        if (capability < RADIO_STARTUP_CAP_COUNT) {
            grant_capability(report, capability);
        }
    } else {
        note_failure(report, stage, true, result->error, stage);
    }
    return result->error;
}

static void enter_storage_recovery(const radio_startup_ops_t *ops,
                                   void *context,
                                   radio_startup_report_t *report,
                                   radio_startup_stage_t cause,
                                   esp_err_t error)
{
    if (run_stage(report, RADIO_STARTUP_STAGE_BOARD, ops->board_init,
                  context, RADIO_STARTUP_CAP_DISPLAY) != ESP_OK) {
        report->mode = RADIO_STARTUP_MODE_STOPPED;
        return;
    }

    const esp_err_t reserve_error = run_stage(
        report, RADIO_STARTUP_STAGE_RECOVERY_VIEW,
        ops->recovery_view_reserve, context,
        RADIO_STARTUP_CAP_RECOVERY_VIEW);
    if (reserve_error == ESP_OK) {
        ops->show_recovery(context, cause, error);
    }
    report->mode = RADIO_STARTUP_MODE_RECOVERY;
}

static void record_catalog_dependency_failure(
    radio_startup_report_t *report)
{
    note_failure(report, RADIO_STARTUP_STAGE_CATALOG, false,
                 ESP_ERR_INVALID_STATE, RADIO_STARTUP_STAGE_WIFI);
}

esp_err_t radio_startup_run(const radio_startup_ops_t *ops, void *context,
                            radio_startup_report_t *report)
{
    if (report == NULL || !operations_are_valid(ops)) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(report, 0, sizeof(*report));
    report->mode = RADIO_STARTUP_MODE_READY;

    if (run_stage(report, RADIO_STARTUP_STAGE_POWER_HOLD,
                  ops->power_hold_init, context,
                  RADIO_STARTUP_CAP_POWER_HOLD) != ESP_OK) {
        report->mode = RADIO_STARTUP_MODE_STOPPED;
        return ESP_OK;
    }

    esp_err_t error = run_stage(report, RADIO_STARTUP_STAGE_NVS_FLASH,
                                ops->nvs_flash_init, context,
                                RADIO_STARTUP_CAP_COUNT);
    if (error != ESP_OK) {
        enter_storage_recovery(ops, context, report,
                               RADIO_STARTUP_STAGE_NVS_FLASH, error);
        return ESP_OK;
    }

    error = run_stage(report, RADIO_STARTUP_STAGE_STATION_STORE,
                      ops->station_store_init, context,
                      RADIO_STARTUP_CAP_STORAGE);
    if (error != ESP_OK) {
        enter_storage_recovery(ops, context, report,
                               RADIO_STARTUP_STAGE_STATION_STORE, error);
        return ESP_OK;
    }

    if (run_stage(report, RADIO_STARTUP_STAGE_BOARD, ops->board_init,
                  context, RADIO_STARTUP_CAP_DISPLAY) != ESP_OK) {
        report->mode = RADIO_STARTUP_MODE_STOPPED;
        return ESP_OK;
    }

    (void)run_stage(report, RADIO_STARTUP_STAGE_RECOVERY_VIEW,
                    ops->recovery_view_reserve, context,
                    RADIO_STARTUP_CAP_RECOVERY_VIEW);
    (void)run_stage(report, RADIO_STARTUP_STAGE_TOUCH, ops->touch_status,
                    context, RADIO_STARTUP_CAP_TOUCH);
    (void)run_stage(report, RADIO_STARTUP_STAGE_AUDIO, ops->audio_init,
                    context, RADIO_STARTUP_CAP_AUDIO);

    error = run_stage(report, RADIO_STARTUP_STAGE_WIFI, ops->wifi_init,
                      context, RADIO_STARTUP_CAP_WIFI);
    if (error == ESP_OK) {
        (void)run_stage(report, RADIO_STARTUP_STAGE_CATALOG,
                        ops->catalog_init, context,
                        RADIO_STARTUP_CAP_CATALOG);
    } else {
        record_catalog_dependency_failure(report);
    }

    error = run_stage(report, RADIO_STARTUP_STAGE_UI, ops->ui_init,
                      context, RADIO_STARTUP_CAP_UI);
    if (error != ESP_OK) {
        if (radio_startup_has_capability(
                report, RADIO_STARTUP_CAP_RECOVERY_VIEW)) {
            ops->show_recovery(context, RADIO_STARTUP_STAGE_UI, error);
        }
        report->mode = RADIO_STARTUP_MODE_RECOVERY;
        return ESP_OK;
    }

    ops->bind_services(context);
    (void)run_stage(report, RADIO_STARTUP_STAGE_BUTTONS,
                    ops->buttons_init, context,
                    RADIO_STARTUP_CAP_BUTTONS);
    return ESP_OK;
}

const char *radio_startup_stage_name(radio_startup_stage_t stage)
{
    switch (stage) {
    case RADIO_STARTUP_STAGE_NONE: return "none";
    case RADIO_STARTUP_STAGE_POWER_HOLD: return "power_hold";
    case RADIO_STARTUP_STAGE_NVS_FLASH: return "nvs_flash";
    case RADIO_STARTUP_STAGE_STATION_STORE: return "station_store";
    case RADIO_STARTUP_STAGE_BOARD: return "board_display";
    case RADIO_STARTUP_STAGE_RECOVERY_VIEW: return "recovery_view";
    case RADIO_STARTUP_STAGE_TOUCH: return "touch";
    case RADIO_STARTUP_STAGE_AUDIO: return "audio";
    case RADIO_STARTUP_STAGE_WIFI: return "wifi";
    case RADIO_STARTUP_STAGE_CATALOG: return "catalog";
    case RADIO_STARTUP_STAGE_UI: return "ui";
    case RADIO_STARTUP_STAGE_BUTTONS: return "buttons";
    case RADIO_STARTUP_STAGE_COUNT: break;
    }
    return "unknown";
}

const char *radio_startup_capability_name(
    radio_startup_capability_t capability)
{
    switch (capability) {
    case RADIO_STARTUP_CAP_POWER_HOLD: return "power_hold";
    case RADIO_STARTUP_CAP_STORAGE: return "storage";
    case RADIO_STARTUP_CAP_DISPLAY: return "display";
    case RADIO_STARTUP_CAP_RECOVERY_VIEW: return "recovery_view";
    case RADIO_STARTUP_CAP_TOUCH: return "touch";
    case RADIO_STARTUP_CAP_AUDIO: return "audio";
    case RADIO_STARTUP_CAP_WIFI: return "wifi";
    case RADIO_STARTUP_CAP_CATALOG: return "catalog";
    case RADIO_STARTUP_CAP_UI: return "ui";
    case RADIO_STARTUP_CAP_BUTTONS: return "buttons";
    case RADIO_STARTUP_CAP_COUNT: break;
    }
    return "unknown";
}

const char *radio_startup_mode_name(radio_startup_mode_t mode)
{
    switch (mode) {
    case RADIO_STARTUP_MODE_READY: return "ready";
    case RADIO_STARTUP_MODE_DEGRADED: return "degraded";
    case RADIO_STARTUP_MODE_RECOVERY: return "recovery";
    case RADIO_STARTUP_MODE_STOPPED: return "stopped";
    }
    return "unknown";
}
