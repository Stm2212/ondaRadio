#pragma once

#include <stdint.h>

/* These values are stored in NVS. Keep their numeric order stable. */
typedef enum {
    RADIO_UI_THEME_NIGHT = 0,
    RADIO_UI_THEME_PAPER,
    RADIO_UI_THEME_FOREST,
    RADIO_UI_THEME_COUNT,
} radio_ui_theme_id_t;

typedef enum {
    RADIO_UI_ACCENT_MINT = 0,
    RADIO_UI_ACCENT_BLUE,
    RADIO_UI_ACCENT_CORAL,
    RADIO_UI_ACCENT_COUNT,
} radio_ui_accent_id_t;

typedef struct {
    uint32_t background;
    uint32_t surface;
    uint32_t surface_alt;
    uint32_t pressed;
    uint32_t accent;
    uint32_t warning;
    uint32_t text;
    uint32_t muted;
    uint32_t error;
} radio_ui_palette_t;

/* Invalid persisted IDs fall back to the default night and mint choices. */
int radio_ui_theme_normalize_id(int theme_id);
int radio_ui_accent_normalize_id(int accent_id);
radio_ui_palette_t radio_ui_theme_palette(int theme_id, int accent_id);
uint32_t radio_ui_theme_accent_color(int theme_id, int accent_id);
uint32_t radio_ui_theme_text_on_accent(int theme_id);
const char *radio_ui_theme_name(int theme_id);
const char *radio_ui_accent_name(int accent_id);
