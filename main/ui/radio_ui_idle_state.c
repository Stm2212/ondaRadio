#include "radio_ui_idle_state.h"

#include <stddef.h>
#include <string.h>

static void publish_display(radio_ui_idle_state_t *state)
{
    state->pending_events |= RADIO_UI_IDLE_EVENT_DISPLAY;
}

void radio_ui_idle_state_init(radio_ui_idle_state_t *state, uint32_t now_tick,
                              int sleep_preset_minutes, int screen_brightness,
                              int screen_timeout_seconds, int screensaver_style)
{
    if (state == NULL) return;
    memset(state, 0, sizeof(*state));
    state->last_activity_tick = now_tick;
    state->sleep_last_tick = now_tick;
    state->sleep_preset_minutes = sleep_preset_minutes;
    state->screen_brightness = screen_brightness;
    state->screen_timeout_seconds = screen_timeout_seconds;
    state->screensaver_style = screensaver_style;
    state->display = RADIO_UI_IDLE_DISPLAY_NORMAL;
    publish_display(state);
}

void radio_ui_idle_set_activity(radio_ui_idle_state_t *state, uint32_t now_tick)
{
    if (state == NULL) return;
    state->last_activity_tick = now_tick;
    if (state->display != RADIO_UI_IDLE_DISPLAY_NORMAL) {
        state->display = RADIO_UI_IDLE_DISPLAY_NORMAL;
        publish_display(state);
    }
}

void radio_ui_idle_set_sleep_preset(radio_ui_idle_state_t *state, int minutes)
{
    if (state == NULL || minutes < 0 || minutes > 60) return;
    state->sleep_preset_minutes = minutes;
}

void radio_ui_idle_set_sleep_timer(radio_ui_idle_state_t *state,
                                   uint32_t now_tick, int minutes)
{
    if (state == NULL || minutes < 0 || minutes > 60) return;
    state->sleep_preset_minutes = minutes;
    state->sleep_remaining_seconds = (uint32_t)minutes * 60U;
    /* The countdown is measured from now; activity must not push it back. */
    state->sleep_last_tick = now_tick;
    /* A newly selected timer supersedes an expiry not yet consumed by LVGL. */
    state->pending_events &= ~RADIO_UI_IDLE_EVENT_SLEEP_EXPIRED;
}

void radio_ui_idle_set_brightness(radio_ui_idle_state_t *state, int brightness)
{
    if (state == NULL || brightness < 0 || brightness > 100) return;
    if (state->screen_brightness == brightness) return;
    state->screen_brightness = brightness;
    publish_display(state);
}

void radio_ui_idle_set_timeout(radio_ui_idle_state_t *state, uint32_t now_tick,
                               int timeout_seconds)
{
    if (state == NULL || timeout_seconds < 0 || timeout_seconds > 3600) return;
    state->screen_timeout_seconds = timeout_seconds;
    state->last_activity_tick = now_tick;
    if (state->display != RADIO_UI_IDLE_DISPLAY_NORMAL) {
        state->display = RADIO_UI_IDLE_DISPLAY_NORMAL;
    }
    publish_display(state);
}

void radio_ui_idle_set_screensaver_style(radio_ui_idle_state_t *state, int style)
{
    if (state == NULL || style < 0 || style > 2) return;
    if (state->screensaver_style == style) return;
    state->screensaver_style = style;
    publish_display(state);
}

uint32_t radio_ui_idle_advance(radio_ui_idle_state_t *state, uint32_t now_tick,
                               uint32_t ticks_per_second,
                               uint32_t screensaver_visible_ticks)
{
    if (state == NULL || ticks_per_second == 0) return 0;

    uint32_t events = 0;

    if (state->sleep_remaining_seconds > 0) {
        /* Count elapsed time, not calls: activity and polling cadence must not
           stretch or shorten the configured duration. Unsigned subtraction
           stays valid across tick wrap, and only whole seconds are consumed so
           the sub-second remainder is preserved. */
        const uint32_t elapsed_ticks = now_tick - state->sleep_last_tick;
        const uint32_t elapsed_seconds = elapsed_ticks / ticks_per_second;
        if (elapsed_seconds > 0) {
            state->sleep_last_tick += elapsed_seconds * ticks_per_second;
            if (elapsed_seconds >= state->sleep_remaining_seconds) {
                state->sleep_remaining_seconds = 0;
                state->pending_events |= RADIO_UI_IDLE_EVENT_SLEEP_EXPIRED;
                events |= RADIO_UI_IDLE_EVENT_SLEEP_EXPIRED;
            } else {
                state->sleep_remaining_seconds -= elapsed_seconds;
            }
        }
    }

    if (state->screen_timeout_seconds <= 0) return events;

    /* Unsigned subtraction keeps elapsed-time checks valid across tick wrap. */
    const uint32_t idle_limit =
        (uint32_t)state->screen_timeout_seconds * ticks_per_second;
    if (state->display == RADIO_UI_IDLE_DISPLAY_NORMAL &&
        (uint32_t)(now_tick - state->last_activity_tick) >= idle_limit) {
        state->display = RADIO_UI_IDLE_DISPLAY_SCREENSAVER;
        state->screensaver_started_tick = now_tick;
        publish_display(state);
        events |= RADIO_UI_IDLE_EVENT_DISPLAY;
    } else if (state->display == RADIO_UI_IDLE_DISPLAY_SCREENSAVER) {
        const uint32_t saver_ticks = now_tick - state->screensaver_started_tick;
        if (saver_ticks >= screensaver_visible_ticks) {
            state->display = RADIO_UI_IDLE_DISPLAY_BACKLIGHT_OFF;
            publish_display(state);
            events |= RADIO_UI_IDLE_EVENT_DISPLAY;
        } else {
            state->pending_events |= RADIO_UI_IDLE_EVENT_REFRESH;
            events |= RADIO_UI_IDLE_EVENT_REFRESH;
        }
    }

    return events;
}

void radio_ui_idle_snapshot(const radio_ui_idle_state_t *state,
                            radio_ui_idle_snapshot_t *snapshot)
{
    if (state == NULL || snapshot == NULL) return;
    *snapshot = *state;
}

uint32_t radio_ui_idle_take_events(radio_ui_idle_state_t *state,
                                   radio_ui_idle_snapshot_t *snapshot)
{
    if (state == NULL || snapshot == NULL) return 0;
    const uint32_t events = state->pending_events;
    *snapshot = *state;
    state->pending_events = 0;
    snapshot->pending_events = events;
    return events;
}
