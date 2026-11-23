#include "radio_stream_watchdog.h"

#include <stddef.h>

static bool elapsed_at_least(int64_t now_us, int64_t since_us)
{
    return now_us >= since_us &&
           ((uint64_t)now_us - (uint64_t)since_us) >=
               (uint64_t)RADIO_STREAM_PROGRESS_TIMEOUT_US;
}

static uint64_t elapsed_us(int64_t now_us, int64_t since_us)
{
    if (now_us < since_us) return 0;
    return (uint64_t)now_us - (uint64_t)since_us;
}

bool radio_stream_watchdog_init(radio_stream_watchdog_t *watchdog,
                                radio_stream_clock_fn_t clock,
                                void *clock_context)
{
    if (watchdog == NULL || clock == NULL) return false;

    const int64_t now = clock(clock_context);
    *watchdog = (radio_stream_watchdog_t){
        .clock = clock,
        .clock_context = clock_context,
        .started_at_us = now,
        .last_progress_at_us = now,
        .active = true,
    };
    return true;
}

void radio_stream_watchdog_note_progress(radio_stream_watchdog_t *watchdog)
{
    if (watchdog == NULL || !watchdog->active || watchdog->clock == NULL) return;
    const int64_t now = watchdog->clock(watchdog->clock_context);
    const int64_t previous = watchdog->has_progress
                                 ? watchdog->last_progress_at_us
                                 : watchdog->started_at_us;
    if (now >= previous) {
        const uint64_t gap_us = (uint64_t)now - (uint64_t)previous;
        if (gap_us > watchdog->max_progress_gap_us) {
            watchdog->max_progress_gap_us = gap_us;
        }
    }
    watchdog->last_progress_at_us = now;
    watchdog->has_progress = true;
}

radio_stream_watchdog_result_t radio_stream_watchdog_check(
    const radio_stream_watchdog_t *watchdog)
{
    if (watchdog == NULL || !watchdog->active || watchdog->clock == NULL) {
        return RADIO_STREAM_WATCHDOG_OK;
    }

    const int64_t now = watchdog->clock(watchdog->clock_context);
    if (!watchdog->has_progress) {
        return elapsed_at_least(now, watchdog->started_at_us)
                   ? RADIO_STREAM_WATCHDOG_START_TIMEOUT
                   : RADIO_STREAM_WATCHDOG_OK;
    }
    return elapsed_at_least(now, watchdog->last_progress_at_us)
               ? RADIO_STREAM_WATCHDOG_PROGRESS_TIMEOUT
               : RADIO_STREAM_WATCHDOG_OK;
}

int64_t radio_stream_watchdog_remaining_us(
    const radio_stream_watchdog_t *watchdog)
{
    if (watchdog == NULL || !watchdog->active || watchdog->clock == NULL) {
        return 0;
    }
    const int64_t reference = watchdog->has_progress
                                  ? watchdog->last_progress_at_us
                                  : watchdog->started_at_us;
    const uint64_t elapsed = elapsed_us(watchdog->clock(watchdog->clock_context),
                                        reference);
    const uint64_t budget = (uint64_t)RADIO_STREAM_PROGRESS_TIMEOUT_US;
    return elapsed >= budget ? 0 : (int64_t)(budget - elapsed);
}

uint64_t radio_stream_watchdog_max_progress_gap_us(
    const radio_stream_watchdog_t *watchdog)
{
    return watchdog != NULL ? watchdog->max_progress_gap_us : 0U;
}
