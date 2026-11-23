#pragma once

#include <stdint.h>

typedef enum {
    RADIO_UI_IDLE_DISPLAY_NORMAL = 0,
    RADIO_UI_IDLE_DISPLAY_SCREENSAVER,
    RADIO_UI_IDLE_DISPLAY_BACKLIGHT_OFF,
} radio_ui_idle_display_t;

enum {
    RADIO_UI_IDLE_EVENT_DISPLAY = 1U << 0,
    RADIO_UI_IDLE_EVENT_REFRESH = 1U << 1,
    RADIO_UI_IDLE_EVENT_SLEEP_EXPIRED = 1U << 2,
};

typedef struct {
    uint32_t last_activity_tick;
    uint32_t screensaver_started_tick;
    uint32_t sleep_remaining_seconds;
    uint32_t sleep_last_tick;
    int sleep_preset_minutes;
    int screen_brightness;
    int screen_timeout_seconds;
    int screensaver_style;
    radio_ui_idle_display_t display;
    uint32_t pending_events;
} radio_ui_idle_state_t;

typedef radio_ui_idle_state_t radio_ui_idle_snapshot_t;

/* Pure transitions; callers must serialize access to each state instance. */
void radio_ui_idle_state_init(radio_ui_idle_state_t *state, uint32_t now_tick,
                              int sleep_preset_minutes, int screen_brightness,
                              int screen_timeout_seconds, int screensaver_style);
void radio_ui_idle_set_activity(radio_ui_idle_state_t *state, uint32_t now_tick);
void radio_ui_idle_set_sleep_preset(radio_ui_idle_state_t *state, int minutes);
void radio_ui_idle_set_sleep_timer(radio_ui_idle_state_t *state,
                                   uint32_t now_tick, int minutes);
void radio_ui_idle_set_brightness(radio_ui_idle_state_t *state, int brightness);
void radio_ui_idle_set_timeout(radio_ui_idle_state_t *state, uint32_t now_tick,
                               int timeout_seconds);
void radio_ui_idle_set_screensaver_style(radio_ui_idle_state_t *state, int style);
uint32_t radio_ui_idle_advance(radio_ui_idle_state_t *state, uint32_t now_tick,
                               uint32_t ticks_per_second,
                               uint32_t screensaver_visible_ticks);
void radio_ui_idle_snapshot(const radio_ui_idle_state_t *state,
                            radio_ui_idle_snapshot_t *snapshot);
uint32_t radio_ui_idle_take_events(radio_ui_idle_state_t *state,
                                   radio_ui_idle_snapshot_t *snapshot);
