#pragma once

#include "esp_err.h"
#include "radio_types.h"

typedef enum {
    RADIO_PLAYER_STATE_IDLE = 0,
    RADIO_PLAYER_STATE_CONNECTING,
    RADIO_PLAYER_STATE_PLAYING,
    RADIO_PLAYER_STATE_PAUSED,
    RADIO_PLAYER_STATE_STOPPING,
    RADIO_PLAYER_STATE_ERROR,
    RADIO_PLAYER_STATE_RECOVERING,
} radio_player_state_t;

/* Text arguments are temporary snapshots; copy them if retained. */
typedef void (*radio_player_status_cb_t)(radio_player_state_t state,
                                         uint32_t sequence,
                                         const char *station,
                                         const char *message);

esp_err_t radio_player_init(void);
void radio_player_restore_station(const radio_station_t *station);
esp_err_t radio_player_play(const radio_station_t *station);
void radio_player_stop(void);
void radio_player_pause_toggle(void);
void radio_player_toggle(void);
int radio_player_volume(void);
void radio_player_volume_preview(int volume);
void radio_player_volume_commit(void);
void radio_player_volume_step(int delta);
void radio_player_set_status_callback(radio_player_status_cb_t callback);
