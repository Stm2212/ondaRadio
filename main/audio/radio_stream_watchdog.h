#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RADIO_STREAM_PROGRESS_TIMEOUT_US (30LL * 1000LL * 1000LL)

typedef int64_t (*radio_stream_clock_fn_t)(void *context);

typedef enum {
    RADIO_STREAM_WATCHDOG_OK = 0,
    RADIO_STREAM_WATCHDOG_START_TIMEOUT,
    RADIO_STREAM_WATCHDOG_PROGRESS_TIMEOUT,
} radio_stream_watchdog_result_t;

typedef struct {
    radio_stream_clock_fn_t clock;
    void *clock_context;
    int64_t started_at_us;
    int64_t last_progress_at_us;
    uint64_t max_progress_gap_us;
    bool active;
    bool has_progress;
} radio_stream_watchdog_t;

/* The clock is injected so the same deadline policy can be tested without I/O. */
bool radio_stream_watchdog_init(radio_stream_watchdog_t *watchdog,
                                radio_stream_clock_fn_t clock,
                                void *clock_context);
void radio_stream_watchdog_note_progress(radio_stream_watchdog_t *watchdog);
radio_stream_watchdog_result_t radio_stream_watchdog_check(
    const radio_stream_watchdog_t *watchdog);
int64_t radio_stream_watchdog_remaining_us(
    const radio_stream_watchdog_t *watchdog);
uint64_t radio_stream_watchdog_max_progress_gap_us(
    const radio_stream_watchdog_t *watchdog);
