#include "radio_task_supervisor.h"

#include <stddef.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "radio_board.h"

#define RADIO_TASK_SUPERVISOR_MAGIC UINT32_C(0x4F4E4457)
#define RADIO_TASK_SUPERVISOR_VERSION 1U
#define RADIO_TASK_SUPERVISOR_STACK_SIZE 4096U
#define RADIO_TASK_SUPERVISOR_PRIORITY 7U
#define RADIO_TASK_SUPERVISOR_SLOW_PROGRESS_LOG_US (1000LL * 1000LL)

static const char *TAG = "radio_supervisor";
static RTC_NOINIT_ATTR radio_task_supervisor_record_t s_retained_record;
static TaskHandle_t s_supervisor_task;
static portMUX_TYPE s_registry_mux = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    bool active;
    uint32_t registration;
    int64_t last_progress_us;
} supervised_task_slot_t;

static supervised_task_slot_t s_tasks[RADIO_SUPERVISED_TASK_COUNT];
static uint32_t s_registration_generation;
static int64_t s_healthy_window_started_us;
static bool s_healthy_window_active;
static bool s_healthy_window_cleared;

static uint32_t checksum_word(uint32_t checksum, uint32_t word)
{
    for (unsigned int i = 0; i < 4U; ++i) {
        checksum ^= (word >> (i * 8U)) & UINT32_C(0xff);
        checksum *= UINT32_C(16777619);
    }
    return checksum;
}

static uint32_t record_checksum(const radio_task_supervisor_record_t *record)
{
    uint32_t checksum = UINT32_C(2166136261);
    checksum = checksum_word(checksum, record->magic);
    checksum = checksum_word(checksum,
                             ((uint32_t)record->version << 16) |
                                 record->consecutive_failures);
    checksum = checksum_word(checksum, record->task_id);
    checksum = checksum_word(checksum, (uint32_t)record->last_error);
    checksum = checksum_word(checksum, (uint32_t)record->reset_reason);
    checksum = checksum_word(checksum, record->reset_pending);
    return checksum;
}

static void record_seal(radio_task_supervisor_record_t *record)
{
    record->magic = RADIO_TASK_SUPERVISOR_MAGIC;
    record->version = RADIO_TASK_SUPERVISOR_VERSION;
    record->checksum = record_checksum(record);
}

static bool task_id_is_valid(uint32_t task_id)
{
    return task_id > RADIO_SUPERVISED_TASK_NONE &&
           task_id < RADIO_SUPERVISED_TASK_COUNT;
}

static bool record_is_valid(const radio_task_supervisor_record_t *record)
{
    return record != NULL &&
           record->magic == RADIO_TASK_SUPERVISOR_MAGIC &&
           record->version == RADIO_TASK_SUPERVISOR_VERSION &&
           record->consecutive_failures <=
               RADIO_TASK_SUPERVISOR_FAILURE_LIMIT &&
           record->reset_pending <= 1U &&
           (record->consecutive_failures == 0U ||
            task_id_is_valid(record->task_id)) &&
           (record->task_id == RADIO_SUPERVISED_TASK_NONE ||
            task_id_is_valid(record->task_id)) &&
           record->checksum == record_checksum(record);
}

static void record_clear(radio_task_supervisor_record_t *record)
{
    memset(record, 0, sizeof(*record));
    record->last_error = ESP_OK;
    record->reset_reason = INT32_MIN;
    record_seal(record);
}

void radio_task_supervisor_record_prepare(
    radio_task_supervisor_record_t *record)
{
    if (record != NULL && !record_is_valid(record)) {
        record_clear(record);
    }
}

bool radio_task_supervisor_record_is_safe(
    const radio_task_supervisor_record_t *record)
{
    return record_is_valid(record) &&
           record->consecutive_failures >=
               RADIO_TASK_SUPERVISOR_FAILURE_LIMIT;
}

void radio_task_supervisor_record_stall(
    radio_task_supervisor_record_t *record, radio_supervised_task_id_t task,
    esp_err_t error)
{
    if (record == NULL || task <= RADIO_SUPERVISED_TASK_NONE ||
        task >= RADIO_SUPERVISED_TASK_COUNT) {
        return;
    }
    radio_task_supervisor_record_prepare(record);
    if (radio_task_supervisor_record_is_safe(record)) return;

    if (record->consecutive_failures > 0U &&
        record->task_id == (uint32_t)task) {
        ++record->consecutive_failures;
    } else {
        record->consecutive_failures = 1U;
        record->task_id = (uint32_t)task;
    }
    record->last_error = error;
    record->reset_reason = INT32_MIN;
    record->reset_pending = 1U;
    record_seal(record);
}

void radio_task_supervisor_record_note_reset(
    radio_task_supervisor_record_t *record, esp_reset_reason_t reason)
{
    if (record == NULL) return;
    radio_task_supervisor_record_prepare(record);
    if (record->reset_pending == 0U) return;
    record->reset_reason = (int32_t)reason;
    record->reset_pending = 0U;
    record_seal(record);
}

void radio_task_supervisor_record_mark_healthy(
    radio_task_supervisor_record_t *record)
{
    if (record == NULL) return;
    radio_task_supervisor_record_prepare(record);
    if (radio_task_supervisor_record_is_safe(record) ||
        record->reset_pending != 0U) {
        return;
    }
    record->consecutive_failures = 0U;
    record_seal(record);
}

bool radio_task_supervisor_record_clear_for_service(
    radio_task_supervisor_record_t *record, const char *command_line)
{
    static const char expected[] = RADIO_BOOT_GUARD_SERVICE_CLEAR_COMMAND;
    if (record == NULL || command_line == NULL) return false;
    radio_task_supervisor_record_prepare(record);
    if (!radio_task_supervisor_record_is_safe(record)) return false;

    size_t length = strlen(command_line);
    while (length > 0U &&
           (command_line[length - 1U] == '\r' ||
            command_line[length - 1U] == '\n')) {
        --length;
    }
    if (length != sizeof(expected) - 1U ||
        memcmp(command_line, expected, length) != 0) {
        return false;
    }
    record_clear(record);
    return true;
}

bool radio_task_supervisor_deadline_expired(int64_t now_us,
                                            int64_t last_progress_us,
                                            int64_t timeout_us)
{
    return timeout_us > 0 && now_us >= last_progress_us &&
           ((uint64_t)now_us - (uint64_t)last_progress_us) >=
               (uint64_t)timeout_us;
}

const char *radio_task_supervisor_task_name(radio_supervised_task_id_t task)
{
    switch (task) {
    case RADIO_SUPERVISED_TASK_AUDIO_PLAYER: return "audio_player";
    case RADIO_SUPERVISED_TASK_AUDIO_SINK: return "audio_sink";
    case RADIO_SUPERVISED_TASK_CATALOG: return "catalog";
    case RADIO_SUPERVISED_TASK_NVS: return "nvs";
    case RADIO_SUPERVISED_TASK_STORE: return "store";
    case RADIO_SUPERVISED_TASK_STATION_SEARCH: return "station_search";
    case RADIO_SUPERVISED_TASK_NONE:
    case RADIO_SUPERVISED_TASK_COUNT: break;
    }
    return "unknown";
}

void radio_task_supervisor_initialize(esp_reset_reason_t reset_reason)
{
    radio_task_supervisor_record_prepare(&s_retained_record);
    if (s_retained_record.reset_pending != 0U) {
        radio_task_supervisor_record_note_reset(&s_retained_record,
                                                reset_reason);
        ESP_LOGW(TAG,
                 "reset posterior a bloqueo: tarea=%s reset_reason=%d fallos=%u error=%s",
                 radio_task_supervisor_task_name(
                     (radio_supervised_task_id_t)s_retained_record.task_id),
                 (int)reset_reason,
                 (unsigned)s_retained_record.consecutive_failures,
                 esp_err_to_name((esp_err_t)s_retained_record.last_error));
    } else if (s_retained_record.consecutive_failures > 0U) {
        ESP_LOGI(TAG,
                 "diagnostico retenido tarea=%s reset_reason=%d fallos=%u",
                 radio_task_supervisor_task_name(
                     (radio_supervised_task_id_t)s_retained_record.task_id),
                 (int)s_retained_record.reset_reason,
                 (unsigned)s_retained_record.consecutive_failures);
    }
}

bool radio_task_supervisor_is_safe(void)
{
    return radio_task_supervisor_record_is_safe(&s_retained_record);
}

bool radio_task_supervisor_clear_for_service(const char *command_line)
{
    return radio_task_supervisor_record_clear_for_service(
        &s_retained_record, command_line);
}

bool radio_task_supervisor_get_record(radio_task_supervisor_record_t *record)
{
    if (record == NULL) return false;
    radio_task_supervisor_record_prepare(&s_retained_record);
    *record = s_retained_record;
    return true;
}

uint32_t radio_task_supervisor_register(radio_supervised_task_id_t task)
{
    if (task <= RADIO_SUPERVISED_TASK_NONE ||
        task >= RADIO_SUPERVISED_TASK_COUNT) {
        return 0U;
    }

    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_registry_mux);
    if (++s_registration_generation == 0U) ++s_registration_generation;
    supervised_task_slot_t *slot = &s_tasks[task];
    slot->active = true;
    slot->registration = s_registration_generation;
    slot->last_progress_us = now;
    s_healthy_window_started_us = now;
    s_healthy_window_active = true;
    s_healthy_window_cleared = false;
    const uint32_t registration = slot->registration;
    portEXIT_CRITICAL(&s_registry_mux);
    return registration;
}

void radio_task_supervisor_progress(radio_supervised_task_id_t task)
{
    if (task <= RADIO_SUPERVISED_TASK_NONE ||
        task >= RADIO_SUPERVISED_TASK_COUNT) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    int64_t progress_gap_us = 0;
    portENTER_CRITICAL(&s_registry_mux);
    if (s_tasks[task].active) {
        const int64_t last_progress_us = s_tasks[task].last_progress_us;
        if (now >= last_progress_us) {
            progress_gap_us = now - last_progress_us;
        }
        s_tasks[task].last_progress_us = now;
    }
    portEXIT_CRITICAL(&s_registry_mux);
    if (progress_gap_us >= RADIO_TASK_SUPERVISOR_SLOW_PROGRESS_LOG_US) {
        ESP_LOGD(TAG, "intervalo de progreso: tarea=%s gap_us=%lld",
                 radio_task_supervisor_task_name(task),
                 (long long)progress_gap_us);
    }
}

void radio_task_supervisor_unregister(radio_supervised_task_id_t task,
                                      uint32_t registration)
{
    if (task <= RADIO_SUPERVISED_TASK_NONE ||
        task >= RADIO_SUPERVISED_TASK_COUNT) {
        return;
    }
    portENTER_CRITICAL(&s_registry_mux);
    supervised_task_slot_t *slot = &s_tasks[task];
    /* Ignore a stale token: the slot may already belong to a newer task. */
    if (!slot->active || slot->registration != registration) {
        portEXIT_CRITICAL(&s_registry_mux);
        return;
    }
    slot->active = false;
    slot->registration = 0U;
    bool any_active = false;
    for (radio_supervised_task_id_t id =
             RADIO_SUPERVISED_TASK_AUDIO_PLAYER;
         id < RADIO_SUPERVISED_TASK_COUNT; ++id) {
        any_active |= s_tasks[id].active;
    }
    if (!any_active) {
        s_healthy_window_active = false;
        s_healthy_window_cleared = false;
    }
    portEXIT_CRITICAL(&s_registry_mux);
}

static void request_controlled_recovery(radio_supervised_task_id_t task,
                                        int64_t elapsed_us)
{
    radio_task_supervisor_record_stall(&s_retained_record, task,
                                       ESP_ERR_TIMEOUT);
    /* This GPIO path does not wait for the codec mutex or I2S. */
    radio_board_audio_mute();
    ESP_LOGE(TAG,
             "progreso vencido: tarea=%s timeout_ms=%lld elapsed_us=%lld fallos=%u safe_mode=%s; NVS no modificada",
             radio_task_supervisor_task_name(task),
             (long long)(RADIO_TASK_SUPERVISOR_STALL_TIMEOUT_US / 1000LL),
             (long long)elapsed_us,
             (unsigned)s_retained_record.consecutive_failures,
             radio_task_supervisor_is_safe() ? "true" : "false");
    esp_restart();
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

static void supervisor_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(RADIO_TASK_SUPERVISOR_CHECK_PERIOD_MS));
        const int64_t now = esp_timer_get_time();
        radio_supervised_task_id_t stalled_task = RADIO_SUPERVISED_TASK_NONE;
        int64_t stalled_elapsed_us = 0;
        bool clear_failure_streak = false;

        portENTER_CRITICAL(&s_registry_mux);
        for (radio_supervised_task_id_t task =
                 RADIO_SUPERVISED_TASK_AUDIO_PLAYER;
             task < RADIO_SUPERVISED_TASK_COUNT; ++task) {
            if (s_tasks[task].active &&
                radio_task_supervisor_deadline_expired(
                    now, s_tasks[task].last_progress_us,
                    RADIO_TASK_SUPERVISOR_STALL_TIMEOUT_US)) {
                stalled_task = task;
                stalled_elapsed_us = now - s_tasks[task].last_progress_us;
                break;
            }
        }
        if (stalled_task == RADIO_SUPERVISED_TASK_NONE &&
            s_healthy_window_active && !s_healthy_window_cleared &&
            radio_task_supervisor_deadline_expired(
                now, s_healthy_window_started_us,
                RADIO_TASK_SUPERVISOR_STABLE_WINDOW_US)) {
            s_healthy_window_cleared = true;
            clear_failure_streak = true;
        }
        portEXIT_CRITICAL(&s_registry_mux);

        if (stalled_task != RADIO_SUPERVISED_TASK_NONE) {
            request_controlled_recovery(stalled_task, stalled_elapsed_us);
        }
        if (clear_failure_streak) {
            radio_task_supervisor_record_mark_healthy(&s_retained_record);
            ESP_LOGI(TAG,
                     "secuencia runtime limpiada tras 10 minutos de progreso estable");
        }
    }
}

esp_err_t radio_task_supervisor_start(void)
{
    if (s_supervisor_task != NULL) return ESP_OK;
    if (xTaskCreate(supervisor_task, "radio_supervisor",
                    RADIO_TASK_SUPERVISOR_STACK_SIZE, NULL,
                    RADIO_TASK_SUPERVISOR_PRIORITY,
                    &s_supervisor_task) != pdPASS) {
        s_supervisor_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
