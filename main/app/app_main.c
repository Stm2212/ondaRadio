#include "cJSON.h"
#include <stdio.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "radio_board.h"
#include "radio_boot_guard.h"
#include "radio_buttons.h"
#include "radio_catalog.h"
#include "radio_player.h"
#include "radio_startup.h"
#include "radio_station_store.h"
#include "radio_snapshot_store.h"
#include "radio_test_adapters.h"
#include "radio_task_supervisor.h"
#include "radio_ui.h"
#include "radio_ui_recovery.h"
#include "radio_wifi.h"

static const char *TAG = "radio_app";
static RTC_NOINIT_ATTR radio_boot_guard_state_t s_retained_boot_guard;

static void *cjson_psram_alloc(size_t size)
{
    return RADIO_TEST_HEAP_CAPS_MALLOC(size,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void cjson_psram_free(void *ptr)
{
    heap_caps_free(ptr);
}

static esp_err_t startup_power_hold_init(void *context)
{
    (void)context;
    return radio_board_power_hold_init();
}

static esp_err_t startup_nvs_flash_init(void *context)
{
    (void)context;
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) return err;
    return radio_snapshot_store_init();
}

static esp_err_t startup_station_store_init(void *context)
{
    (void)context;
    return radio_station_store_init();
}

static esp_err_t startup_board_init(void *context)
{
    (void)context;
    return radio_board_init();
}

static esp_err_t startup_recovery_view_reserve(void *context)
{
    (void)context;
    return radio_ui_recovery_reserve();
}

static esp_err_t startup_touch_status(void *context)
{
    (void)context;
    return radio_board_touch_status();
}

static esp_err_t startup_audio_init(void *context)
{
    (void)context;
    return radio_player_init();
}

static esp_err_t startup_wifi_init(void *context)
{
    (void)context;
    return radio_wifi_init();
}

static esp_err_t startup_catalog_init(void *context)
{
    (void)context;
    return radio_catalog_init();
}

static esp_err_t startup_ui_init(void *context)
{
    (void)context;
    if (!lvgl_port_lock(0)) return ESP_ERR_TIMEOUT;
    const esp_err_t err = radio_ui_init();
    lvgl_port_unlock();
    return err;
}

static void startup_bind_services(void *context)
{
    (void)context;
    radio_ui_bind_services();
}

static esp_err_t startup_buttons_init(void *context)
{
    (void)context;
    return radio_buttons_init();
}

static void startup_show_recovery(void *context,
                                  radio_startup_stage_t cause,
                                  esp_err_t error)
{
    (void)context;
    radio_ui_recovery_cause_t ui_cause;
    switch (cause) {
    case RADIO_STARTUP_STAGE_NVS_FLASH:
    case RADIO_STARTUP_STAGE_STATION_STORE:
        ui_cause = RADIO_UI_RECOVERY_SETTINGS;
        break;
    case RADIO_STARTUP_STAGE_WIFI:
        ui_cause = RADIO_UI_RECOVERY_NETWORK;
        break;
    case RADIO_STARTUP_STAGE_AUDIO:
        ui_cause = RADIO_UI_RECOVERY_AUDIO;
        break;
    case RADIO_STARTUP_STAGE_CATALOG:
        ui_cause = RADIO_UI_RECOVERY_CATALOG;
        break;
    case RADIO_STARTUP_STAGE_TOUCH:
        ui_cause = RADIO_UI_RECOVERY_TOUCH;
        break;
    case RADIO_STARTUP_STAGE_UI:
        ui_cause = RADIO_UI_RECOVERY_UI;
        break;
    default:
        ESP_LOGE(TAG, "No existe vista de recuperacion para etapa=%s error=%s",
                 radio_startup_stage_name(cause), esp_err_to_name(error));
        return;
    }

    const esp_err_t show_error = radio_ui_recovery_show(ui_cause, error);
    if (show_error != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo presentar recuperacion etapa=%s error=%s",
                 radio_startup_stage_name(cause),
                 esp_err_to_name(show_error));
    }
}

static const radio_startup_ops_t s_startup_ops = {
    .power_hold_init = startup_power_hold_init,
    .nvs_flash_init = startup_nvs_flash_init,
    .station_store_init = startup_station_store_init,
    .board_init = startup_board_init,
    .recovery_view_reserve = startup_recovery_view_reserve,
    .touch_status = startup_touch_status,
    .audio_init = startup_audio_init,
    .wifi_init = startup_wifi_init,
    .catalog_init = startup_catalog_init,
    .ui_init = startup_ui_init,
    .bind_services = startup_bind_services,
    .buttons_init = startup_buttons_init,
    .show_recovery = startup_show_recovery,
};

static void log_startup_report(const radio_startup_report_t *report)
{
    for (radio_startup_stage_t stage = RADIO_STARTUP_STAGE_POWER_HOLD;
         stage < RADIO_STARTUP_STAGE_COUNT; ++stage) {
        const radio_startup_stage_result_t *result =
            radio_startup_stage_result(report, stage);
        const char *state = !result->attempted
                                ? (result->error == ESP_OK ? "not_run" : "blocked")
                                : result->available ? "available" : "failed";
        const char *cause = result->cause == RADIO_STARTUP_STAGE_NONE
                                ? "none"
                                : radio_startup_stage_name(result->cause);
        ESP_LOGI(TAG, "startup stage=%s state=%s error=%s cause=%s",
                 radio_startup_stage_name(stage), state,
                 esp_err_to_name(result->error), cause);
    }

    for (radio_startup_capability_t capability =
             RADIO_STARTUP_CAP_POWER_HOLD;
         capability < RADIO_STARTUP_CAP_COUNT; ++capability) {
        ESP_LOGI(TAG, "startup capability=%s available=%s",
                 radio_startup_capability_name(capability),
                 radio_startup_has_capability(report, capability)
                     ? "true" : "false");
    }
    ESP_LOGI(TAG, "startup mode=%s first_failure=%s first_error=%s",
             radio_startup_mode_name(report->mode),
             radio_startup_stage_name(report->first_failure),
             esp_err_to_name(report->first_error));
}

static void wait_for_recovery(void)
{
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

static void safe_mode_usb_recovery_console(void)
{
    char command[64];
    radio_task_supervisor_record_t supervisor_record;
    (void)radio_task_supervisor_get_record(&supervisor_record);
    if (radio_task_supervisor_is_safe()) {
        ESP_LOGE(TAG,
                 "SAFE MODE: supervisor task=%s reset_reason=%d error=%s fallos=%u; NVS conservada",
                 radio_task_supervisor_task_name(
                     (radio_supervised_task_id_t)supervisor_record.task_id),
                 (int)supervisor_record.reset_reason,
                 esp_err_to_name((esp_err_t)supervisor_record.last_error),
                 (unsigned)supervisor_record.consecutive_failures);
    }
    if (radio_boot_guard_is_safe(&s_retained_boot_guard)) {
        ESP_LOGE(TAG,
                 "SAFE MODE: arranque causa=%s error=%s fallos=%u; NVS conservada",
                 radio_startup_stage_name(
                     (radio_startup_stage_t)s_retained_boot_guard.cause),
                 esp_err_to_name((esp_err_t)s_retained_boot_guard.last_error),
                 (unsigned)s_retained_boot_guard.consecutive_failures);
    }

    for (;;) {
        printf("Modo seguro. Conecta por USB Serial/JTAG y escribe: %s\n> ",
               RADIO_BOOT_GUARD_SERVICE_CLEAR_COMMAND);
        fflush(stdout);
        if (fgets(command, sizeof(command), stdin) == NULL) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        const bool startup_guard_cleared =
            radio_boot_guard_clear_for_service(&s_retained_boot_guard,
                                               command);
        const bool task_supervisor_cleared =
            radio_task_supervisor_clear_for_service(command);
        if (startup_guard_cleared || task_supervisor_cleared) {
            printf("Intervencion de servicio aceptada; reiniciando sin borrar NVS.\n");
            fflush(stdout);
            esp_restart();
            wait_for_recovery();
        } else {
            printf("Comando no valido; el modo seguro permanece activo.\n");
        }
    }
}

void app_main(void)
{
    radio_boot_guard_prepare(&s_retained_boot_guard);
    radio_task_supervisor_initialize(esp_reset_reason());
    if (radio_boot_guard_is_safe(&s_retained_boot_guard) ||
        radio_task_supervisor_is_safe()) {
        safe_mode_usb_recovery_console();
        return;
    }

    cJSON_Hooks json_hooks = {
        .malloc_fn = cjson_psram_alloc,
        .free_fn = cjson_psram_free,
    };
    /* Install global hooks before the staged startup can create service tasks. */
    cJSON_InitHooks(&json_hooks);
    const esp_err_t supervisor_error = radio_task_supervisor_start();
    if (supervisor_error != ESP_OK) {
        ESP_LOGE(TAG,
                 "No se pudo iniciar supervisor de progreso: %s; servicios detenidos, NVS conservada",
                 esp_err_to_name(supervisor_error));
        wait_for_recovery();
        return;
    }
    ESP_LOGI(TAG, "Arranque: reset=%d, RAM interna=%u KiB, PSRAM=%u KiB",
             (int)esp_reset_reason(),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                                MALLOC_CAP_8BIT) / 1024U),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U));

    radio_startup_report_t report;
    const esp_err_t startup_error =
        radio_startup_run(&s_startup_ops, NULL, &report);
    if (startup_error != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo ejecutar el plan de arranque: %s",
                 esp_err_to_name(startup_error));
        wait_for_recovery();
        return;
    }

    log_startup_report(&report);
    const bool safe_mode =
        radio_boot_guard_record_startup(&s_retained_boot_guard, &report);
    ESP_LOGI(TAG,
             "boot_guard failures=%u cause=%s safe_mode=%s",
             (unsigned)s_retained_boot_guard.consecutive_failures,
             s_retained_boot_guard.cause == RADIO_STARTUP_STAGE_NONE
                 ? "none"
                 : radio_startup_stage_name(
                       (radio_startup_stage_t)s_retained_boot_guard.cause),
             safe_mode ? "true" : "false");
    if (safe_mode) {
        safe_mode_usb_recovery_console();
        return;
    }

    if (report.mode == RADIO_STARTUP_MODE_RECOVERY ||
        report.mode == RADIO_STARTUP_MODE_STOPPED) {
        wait_for_recovery();
    }
}
