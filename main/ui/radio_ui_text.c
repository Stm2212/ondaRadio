#include "radio_ui_text.h"
#include "radio_ui_text_codec.h"

#include <stdbool.h>
#include <string.h>

extern const lv_font_t radio_font_latin_12;
extern const lv_font_t radio_font_latin_14;
extern const lv_font_t radio_font_latin_16;
extern const lv_font_t radio_font_latin_18;

static const lv_font_t *font_with_latin_glyphs(const lv_font_t *font)
{
    if (font == &lv_font_montserrat_12 || font == &radio_font_latin_12) {
        return &radio_font_latin_12;
    }
    if (font == &lv_font_montserrat_14 || font == &radio_font_latin_14) {
        return &radio_font_latin_14;
    }
    if (font == &lv_font_montserrat_16 || font == &radio_font_latin_16) {
        return &radio_font_latin_16;
    }
    if (font == &lv_font_montserrat_18 || font == &radio_font_latin_18) {
        return &radio_font_latin_18;
    }
    return font;
}

static const lv_font_t *font_without_latin_glyphs(const lv_font_t *font)
{
    if (font == &radio_font_latin_12) return &lv_font_montserrat_12;
    if (font == &radio_font_latin_14) return &lv_font_montserrat_14;
    if (font == &radio_font_latin_16) return &lv_font_montserrat_16;
    if (font == &radio_font_latin_18) return &lv_font_montserrat_18;
    return font;
}

const lv_font_t *radio_ui_text_font_for_size(int32_t font_size)
{
    switch (font_size) {
    case 12:
        return &radio_font_latin_12;
    case 14:
        return &radio_font_latin_14;
    case 16:
        return &radio_font_latin_16;
    case 18:
        return &radio_font_latin_18;
    case 24:
        return &lv_font_montserrat_24;
    case 48:
        return &lv_font_montserrat_48;
    default:
        return &lv_font_montserrat_14;
    }
}

static void label_layout_update(lv_obj_t *label)
{
    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    if (font == NULL) return;
    const int32_t line_height = (int32_t)lv_font_get_line_height(font);
    lv_obj_t *parent = lv_obj_get_parent(label);
    if (parent != NULL && lv_obj_check_type(parent, &lv_button_class)) {
        lv_obj_set_height(label, line_height);
        lv_obj_center(label);
    } else if (lv_obj_get_height(label) < line_height) {
        lv_obj_set_height(label, line_height);
    }
}

void radio_ui_text_set_label(lv_obj_t *label, const char *text)
{
    if (label == NULL) return;

    char display[512];
    (void)radio_ui_text_format_for_font(display, sizeof(display), text);
    bool needs_extended_glyphs = false;
    for (const unsigned char *p = (const unsigned char *)display; *p != '\0';
         ++p) {
        if (*p >= 0x80U) {
            needs_extended_glyphs = true;
            break;
        }
    }

    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    const lv_font_t *display_font = needs_extended_glyphs
                                        ? font_with_latin_glyphs(font)
                                        : font_without_latin_glyphs(font);
    if (display_font != NULL && display_font != font) {
        lv_obj_set_style_text_font(label, display_font, LV_PART_MAIN);
    }

    const char *current = lv_label_get_text(label);
    if (current == NULL || strcmp(current, display) != 0) {
        lv_label_set_text(label, display);
    }
    label_layout_update(label);
}
