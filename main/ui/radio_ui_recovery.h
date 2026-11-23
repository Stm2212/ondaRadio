#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    RADIO_UI_RECOVERY_SETTINGS = 0,
    RADIO_UI_RECOVERY_NETWORK,
    RADIO_UI_RECOVERY_AUDIO,
    RADIO_UI_RECOVERY_CATALOG,
    RADIO_UI_RECOVERY_TOUCH,
    RADIO_UI_RECOVERY_UI,
} radio_ui_recovery_cause_t;

/* Build and retain the small recovery view before optional services start. */
esp_err_t radio_ui_recovery_reserve(void);

/* Switch to the already-reserved view; this path creates no LVGL objects. */
esp_err_t radio_ui_recovery_show(radio_ui_recovery_cause_t cause,
                                 esp_err_t failure);

#ifdef RADIO_UI_TEST_FAULT_INJECTION
bool radio_ui_recovery_test_has_reserved_view(void);
#endif
