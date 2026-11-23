#pragma once

#include "lvgl.h"

/* Adapts text to the glyphs in the bundled fonts and updates an LVGL label.
   Call from the LVGL task or while holding the LVGL port lock. */
void radio_ui_text_set_label(lv_obj_t *label, const char *text);

/* Return the UI font with Latin accents for a supported UI size. */
const lv_font_t *radio_ui_text_font_for_size(int32_t font_size);
