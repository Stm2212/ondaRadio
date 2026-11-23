#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

/* Renders the clock or mascot on LVGL's top layer. The caller owns the idle
   timer, brightness and selected style. Call LVGL operations under its lock. */
esp_err_t radio_ui_screensaver_create(lv_event_cb_t activity_callback,
                                      uint32_t accent_color, int style);
void radio_ui_screensaver_destroy(void);
esp_err_t radio_ui_screensaver_stage_create(lv_event_cb_t activity_callback,
                                            uint32_t accent_color, int style);
void radio_ui_screensaver_stage_discard(void);
void radio_ui_screensaver_stage_publish(void);
bool radio_ui_screensaver_exists(void);
void radio_ui_screensaver_refresh(int style, uint32_t elapsed_ms);
void radio_ui_screensaver_show(int style);
void radio_ui_screensaver_hide(void);
