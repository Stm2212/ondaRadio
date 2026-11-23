#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

esp_err_t radio_board_init(void);
esp_err_t radio_board_touch_status(void);
esp_err_t radio_board_power_hold_init(void);
void radio_board_power_off(void);
esp_err_t radio_board_display_set_brightness(uint8_t percent);
esp_err_t radio_board_battery_read(uint16_t *millivolts, uint8_t *percent);
esp_err_t radio_board_battery_read_percent(uint8_t *percent);
esp_err_t radio_board_battery_is_charging(bool *charging);
esp_err_t radio_board_audio_init(void);
esp_err_t radio_board_audio_open(uint32_t sample_rate, uint8_t channels,
                                 uint8_t bits_per_sample);
esp_err_t radio_board_audio_write(const uint8_t *data, size_t length);
void radio_board_audio_mute(void);
void radio_board_audio_unmute(void);
esp_err_t radio_board_audio_set_volume(int volume);
esp_err_t radio_board_audio_close(void);
