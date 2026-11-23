#include <stddef.h>
#include <string.h>

#include "radio_boot_guard.h"

#define RADIO_BOOT_GUARD_MAGIC UINT32_C(0x4F4E4441)
#define RADIO_BOOT_GUARD_VERSION 1U

static uint32_t checksum_word(uint32_t checksum, uint32_t word)
{
    for (unsigned int i = 0; i < 4U; ++i) {
        checksum ^= (word >> (i * 8U)) & UINT32_C(0xff);
        checksum *= UINT32_C(16777619);
    }
    return checksum;
}

static uint32_t state_checksum(const radio_boot_guard_state_t *state)
{
    uint32_t checksum = UINT32_C(2166136261);
    checksum = checksum_word(checksum, state->magic);
    checksum = checksum_word(checksum, state->version);
    checksum = checksum_word(checksum, state->consecutive_failures);
    checksum = checksum_word(checksum, state->cause);
    checksum = checksum_word(checksum, (uint32_t)state->last_error);
    return checksum;
}

static void state_seal(radio_boot_guard_state_t *state)
{
    state->magic = RADIO_BOOT_GUARD_MAGIC;
    state->version = RADIO_BOOT_GUARD_VERSION;
    state->checksum = state_checksum(state);
}

static bool state_is_valid(const radio_boot_guard_state_t *state)
{
    if (state == NULL || state->magic != RADIO_BOOT_GUARD_MAGIC ||
        state->version != RADIO_BOOT_GUARD_VERSION ||
        state->consecutive_failures > RADIO_BOOT_GUARD_FAILURE_LIMIT ||
        state->checksum != state_checksum(state)) {
        return false;
    }

    if (state->consecutive_failures == 0U) {
        return state->cause == RADIO_STARTUP_STAGE_NONE &&
               state->last_error == ESP_OK;
    }
    return state->cause > RADIO_STARTUP_STAGE_NONE &&
           state->cause < RADIO_STARTUP_STAGE_COUNT;
}

static void state_clear(radio_boot_guard_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state_seal(state);
}

void radio_boot_guard_prepare(radio_boot_guard_state_t *state)
{
    if (state != NULL && !state_is_valid(state)) {
        state_clear(state);
    }
}

bool radio_boot_guard_is_safe(const radio_boot_guard_state_t *state)
{
    return state_is_valid(state) &&
           state->consecutive_failures >= RADIO_BOOT_GUARD_FAILURE_LIMIT;
}

bool radio_boot_guard_record_startup(radio_boot_guard_state_t *state,
                                     const radio_startup_report_t *report)
{
    if (state == NULL || report == NULL) return false;
    radio_boot_guard_prepare(state);
    if (radio_boot_guard_is_safe(state)) return true;

    if (report->mode == RADIO_STARTUP_MODE_READY) {
        state_clear(state);
        return false;
    }

    if (report->first_failure <= RADIO_STARTUP_STAGE_NONE ||
        report->first_failure >= RADIO_STARTUP_STAGE_COUNT) {
        return false;
    }

    if (state->consecutive_failures > 0U &&
        state->cause == (uint32_t)report->first_failure) {
        if (state->consecutive_failures < RADIO_BOOT_GUARD_FAILURE_LIMIT) {
            ++state->consecutive_failures;
        }
    } else {
        state->consecutive_failures = 1U;
        state->cause = (uint32_t)report->first_failure;
    }
    state->last_error = report->first_error;
    state_seal(state);
    return radio_boot_guard_is_safe(state);
}

bool radio_boot_guard_clear_for_service(radio_boot_guard_state_t *state,
                                        const char *command_line)
{
    static const char expected[] = RADIO_BOOT_GUARD_SERVICE_CLEAR_COMMAND;
    if (state == NULL || command_line == NULL) return false;
    radio_boot_guard_prepare(state);
    if (!radio_boot_guard_is_safe(state)) return false;

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

    state_clear(state);
    return true;
}
