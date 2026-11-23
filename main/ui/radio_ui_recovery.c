#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "radio_ui_lvgl.h"
#include "radio_ui_recovery.h"

static const char *TAG = "radio_ui_recovery";

static lv_obj_t *s_recovery_screen;
static lv_obj_t *s_recovery_message;

/* Recovery labels stay static while using the bundled Spanish glyphs. */
extern const lv_font_t radio_font_latin_14;

static const char s_initial_message[] =
    "RECUPERACIÓN\nServicio no disponible.\nConectá por USB a una PC.";
static const char s_settings_message[] =
    "RECUPERACIÓN\nAjustes no disponibles.\nConectá por USB a una PC.";
static const char s_network_message[] =
    "RECUPERACIÓN\nRed no disponible.\nConectá por USB a una PC.";
static const char s_audio_message[] =
    "RECUPERACIÓN\nAudio no disponible.\nConectá por USB a una PC.";
static const char s_catalog_message[] =
    "RECUPERACIÓN\nCatálogo no disponible.\nConectá por USB a una PC.";
static const char s_touch_message[] =
    "RECUPERACIÓN\nEntrada táctil no disponible.\nConectá por USB a una PC.";
static const char s_ui_message[] =
    "RECUPERACIÓN\nInterfaz no disponible.\nConectá por USB a una PC.";

static const char *recovery_message_for(radio_ui_recovery_cause_t cause)
{
    switch (cause) {
    case RADIO_UI_RECOVERY_SETTINGS: return s_settings_message;
    case RADIO_UI_RECOVERY_NETWORK: return s_network_message;
    case RADIO_UI_RECOVERY_AUDIO: return s_audio_message;
    case RADIO_UI_RECOVERY_CATALOG: return s_catalog_message;
    case RADIO_UI_RECOVERY_TOUCH: return s_touch_message;
    case RADIO_UI_RECOVERY_UI: return s_ui_message;
    default: return NULL;
    }
}

esp_err_t radio_ui_recovery_reserve(void)
{
    if (s_recovery_screen != NULL && s_recovery_message != NULL) {
        return ESP_OK;
    }
    if (!lvgl_port_lock(0)) return ESP_ERR_TIMEOUT;

    if (s_recovery_screen != NULL) {
        lv_obj_delete(s_recovery_screen);
        s_recovery_screen = NULL;
        s_recovery_message = NULL;
    }

    lv_obj_t *screen = radio_ui_lvgl_obj_create(NULL);
    if (screen == NULL) {
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101A2A), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_scrollable(screen, false);

    lv_obj_t *message = radio_ui_lvgl_label_create(screen);
    if (message == NULL) {
        lv_obj_delete(screen);
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }
    lv_label_set_text_static(message, s_initial_message);
    lv_label_set_long_mode(message, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_size(message, 216, 144);
    lv_obj_center(message);
    lv_obj_set_style_text_color(message, lv_color_hex(0xF6F3EC), 0);
    lv_obj_set_style_text_font(message, &radio_font_latin_14, 0);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_clickable(message, false);

    s_recovery_screen = screen;
    s_recovery_message = message;
    lvgl_port_unlock();
    ESP_LOGI(TAG, "Vista minima de recuperacion reservada antes de servicios opcionales");
    return ESP_OK;
}

esp_err_t radio_ui_recovery_show(radio_ui_recovery_cause_t cause,
                                 esp_err_t failure)
{
    const char *message = recovery_message_for(cause);
    if (message == NULL) return ESP_ERR_INVALID_ARG;
    if (!lvgl_port_lock(0)) return ESP_ERR_TIMEOUT;
    if (s_recovery_screen == NULL || s_recovery_message == NULL) {
        lvgl_port_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    /* The label points to static text and both LVGL objects were prebuilt. */
    lv_label_set_text_static(s_recovery_message, message);
    lv_screen_load(s_recovery_screen);
    lvgl_port_unlock();

    ESP_LOGE(TAG, "Modo de recuperacion cause=%d error=%s",
             (int)cause, esp_err_to_name(failure));
    return ESP_OK;
}

#ifdef RADIO_UI_TEST_FAULT_INJECTION
bool radio_ui_recovery_test_has_reserved_view(void)
{
    return s_recovery_screen != NULL && s_recovery_message != NULL;
}
#endif
