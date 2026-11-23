#include "radio_ui_theme.h"

typedef struct {
    radio_ui_palette_t palette;
    const char *name;
} theme_preset_t;

/* Colors are expressed as 0xRRGGBB, independent of LVGL or the display. */
static const theme_preset_t s_themes[RADIO_UI_THEME_COUNT] = {
    [RADIO_UI_THEME_NIGHT] = {
        .palette = {
            .background = 0x101A2A, .surface = 0x1A293B,
            .surface_alt = 0x22344A, .pressed = 0x38536A,
            .accent = 0x86E1BF, .warning = 0xFFD071,
            .text = 0xF6F3EC, .muted = 0xA9B5C5, .error = 0xFF7777,
        },
        .name = "Noche · Azul profundo",
    },
    [RADIO_UI_THEME_PAPER] = {
        .palette = {
            .background = 0xF3EFE6, .surface = 0xE6E0D4,
            .surface_alt = 0xDAD2C3, .pressed = 0xC9BDA9,
            .accent = 0x557C6B, .warning = 0xA97932,
            .text = 0x242922, .muted = 0x687167, .error = 0xB94F4F,
        },
        .name = "Papel · Cálido",
    },
    [RADIO_UI_THEME_FOREST] = {
        .palette = {
            .background = 0x101D19, .surface = 0x1B2C26,
            .surface_alt = 0x263B32, .pressed = 0x365448,
            .accent = 0xB8CF83, .warning = 0xE0B968,
            .text = 0xF2F2E9, .muted = 0xAAB8AD, .error = 0xE98178,
        },
        .name = "Bosque · Sereno",
    },
};

static const char *const s_accent_names[RADIO_UI_ACCENT_COUNT] = {
    [RADIO_UI_ACCENT_MINT] = "Menta",
    [RADIO_UI_ACCENT_BLUE] = "Azul",
    [RADIO_UI_ACCENT_CORAL] = "Coral",
};

/* Dark accents preserve contrast on the light paper theme. */
static const uint32_t s_accent_on_dark[RADIO_UI_ACCENT_COUNT] = {
    0x86E1BF, 0x82B5E8, 0xEF9A91,
};
static const uint32_t s_accent_on_light[RADIO_UI_ACCENT_COUNT] = {
    0x287B61, 0x2B6FA6, 0xB84F49,
};

int radio_ui_theme_normalize_id(int theme_id)
{
    return theme_id >= 0 && theme_id < RADIO_UI_THEME_COUNT
               ? theme_id : RADIO_UI_THEME_NIGHT;
}

int radio_ui_accent_normalize_id(int accent_id)
{
    return accent_id >= 0 && accent_id < RADIO_UI_ACCENT_COUNT
               ? accent_id : RADIO_UI_ACCENT_MINT;
}

uint32_t radio_ui_theme_accent_color(int theme_id, int accent_id)
{
    theme_id = radio_ui_theme_normalize_id(theme_id);
    accent_id = radio_ui_accent_normalize_id(accent_id);
    return theme_id == RADIO_UI_THEME_PAPER ? s_accent_on_light[accent_id]
                                           : s_accent_on_dark[accent_id];
}

radio_ui_palette_t radio_ui_theme_palette(int theme_id, int accent_id)
{
    theme_id = radio_ui_theme_normalize_id(theme_id);
    radio_ui_palette_t palette = s_themes[theme_id].palette;
    palette.accent = radio_ui_theme_accent_color(theme_id, accent_id);
    return palette;
}

uint32_t radio_ui_theme_text_on_accent(int theme_id)
{
    theme_id = radio_ui_theme_normalize_id(theme_id);
    return theme_id == RADIO_UI_THEME_PAPER ? 0xFFFFFF
                                            : s_themes[theme_id].palette.background;
}

const char *radio_ui_theme_name(int theme_id)
{
    return s_themes[radio_ui_theme_normalize_id(theme_id)].name;
}

const char *radio_ui_accent_name(int accent_id)
{
    return s_accent_names[radio_ui_accent_normalize_id(accent_id)];
}
