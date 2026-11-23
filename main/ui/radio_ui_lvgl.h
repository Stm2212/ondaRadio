#pragma once

#include "lvgl.h"

/* Centralized LVGL widget creation keeps allocation-failure handling
   consistent across the UI and gives integration tests one deterministic seam. */
lv_obj_t *radio_ui_lvgl_obj_create(lv_obj_t *parent);
lv_obj_t *radio_ui_lvgl_label_create(lv_obj_t *parent);
lv_obj_t *radio_ui_lvgl_button_create(lv_obj_t *parent);
lv_obj_t *radio_ui_lvgl_bar_create(lv_obj_t *parent);
lv_obj_t *radio_ui_lvgl_line_create(lv_obj_t *parent);

#ifdef RADIO_UI_TEST_FAULT_INJECTION
#include <stddef.h>

/* Fail the one-based Nth widget creation; zero disables injection. */
void radio_ui_lvgl_test_fail_on(size_t nth_creation);
size_t radio_ui_lvgl_test_creation_attempts(void);
void radio_ui_lvgl_test_clear_fault(void);
#endif
