#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "radio_startup.h"

#define RADIO_BOOT_GUARD_FAILURE_LIMIT 3U
#define RADIO_BOOT_GUARD_SERVICE_CLEAR_COMMAND "SERVICE CLEAR SAFE MODE"

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t consecutive_failures;
    uint32_t cause;
    int32_t last_error;
    uint32_t checksum;
} radio_boot_guard_state_t;

/* Invalid or uninitialized retained memory is reset to an empty record. */
void radio_boot_guard_prepare(radio_boot_guard_state_t *state);
bool radio_boot_guard_is_safe(const radio_boot_guard_state_t *state);

/* READY clears the failure sequence; DEGRADED/RECOVERY/STOPPED do not. */
bool radio_boot_guard_record_startup(radio_boot_guard_state_t *state,
                                     const radio_startup_report_t *report);

/* A latched safe mode can only be cleared by the exact USB service command. */
bool radio_boot_guard_clear_for_service(radio_boot_guard_state_t *state,
                                        const char *command_line);
