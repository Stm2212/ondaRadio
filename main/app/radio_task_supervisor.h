#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_system.h"
#include "radio_boot_guard.h"

/* Initial stall deadline; validate against legitimate slow paths on hardware. */
#ifndef RADIO_TASK_SUPERVISOR_STALL_TIMEOUT_US
#define RADIO_TASK_SUPERVISOR_STALL_TIMEOUT_US (60LL * 1000LL * 1000LL)
#endif
#define RADIO_TASK_SUPERVISOR_STABLE_WINDOW_US (10LL * 60LL * 1000LL * 1000LL)
#define RADIO_TASK_SUPERVISOR_FAILURE_LIMIT 3U
#define RADIO_TASK_SUPERVISOR_CHECK_PERIOD_MS 250U

typedef enum {
    RADIO_SUPERVISED_TASK_NONE = 0,
    RADIO_SUPERVISED_TASK_AUDIO_PLAYER,
    RADIO_SUPERVISED_TASK_AUDIO_SINK,
    RADIO_SUPERVISED_TASK_CATALOG,
    RADIO_SUPERVISED_TASK_NVS,
    RADIO_SUPERVISED_TASK_STORE,
    RADIO_SUPERVISED_TASK_STATION_SEARCH,
    RADIO_SUPERVISED_TASK_COUNT,
} radio_supervised_task_id_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t consecutive_failures;
    uint32_t task_id;
    int32_t last_error;
    int32_t reset_reason;
    uint32_t reset_pending;
    uint32_t checksum;
} radio_task_supervisor_record_t;

/* Pure retained-record operations are public for deterministic Unity tests. */
void radio_task_supervisor_record_prepare(
    radio_task_supervisor_record_t *record);
bool radio_task_supervisor_record_is_safe(
    const radio_task_supervisor_record_t *record);
void radio_task_supervisor_record_stall(
    radio_task_supervisor_record_t *record, radio_supervised_task_id_t task,
    esp_err_t error);
void radio_task_supervisor_record_note_reset(
    radio_task_supervisor_record_t *record, esp_reset_reason_t reason);
void radio_task_supervisor_record_mark_healthy(
    radio_task_supervisor_record_t *record);
bool radio_task_supervisor_record_clear_for_service(
    radio_task_supervisor_record_t *record, const char *command_line);
bool radio_task_supervisor_deadline_expired(int64_t now_us,
                                            int64_t last_progress_us,
                                            int64_t timeout_us);

/* Initialize before startup; this records and reports the preceding reset. */
void radio_task_supervisor_initialize(esp_reset_reason_t reset_reason);
esp_err_t radio_task_supervisor_start(void);
/* Registers the calling task and returns its registration token. The token
   identifies this particular registration so a task that is being torn down
   cannot unregister a newer instance that reused the same slot. */
uint32_t radio_task_supervisor_register(radio_supervised_task_id_t task);
void radio_task_supervisor_progress(radio_supervised_task_id_t task);
/* Clears the slot only while `registration` is still the active token; a stale
   token is ignored so it cannot disable a newer task. Pass the value returned
   by radio_task_supervisor_register(). */
void radio_task_supervisor_unregister(radio_supervised_task_id_t task,
                                      uint32_t registration);
bool radio_task_supervisor_is_safe(void);
bool radio_task_supervisor_clear_for_service(const char *command_line);
bool radio_task_supervisor_get_record(radio_task_supervisor_record_t *record);
const char *radio_task_supervisor_task_name(radio_supervised_task_id_t task);
