#include <math.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include "radio_ui_screensaver.h"
#include "radio_ui_lvgl.h"
#include "radio_ui_text.h"

typedef struct {
    lv_obj_t *screensaver;
    lv_obj_t *clock_label;
    lv_obj_t *analog_face;
    lv_obj_t *mascot_face;
    lv_obj_t *hour_hand;
    lv_obj_t *minute_hand;
    lv_obj_t *clock_center;
    lv_obj_t *analog_ticks[12];
    lv_obj_t *mascot_lines[7];
    lv_obj_t *mascot_eyes[2];
    lv_obj_t *mascot_blink[2];
    lv_point_precise_t hour_points[2];
    lv_point_precise_t minute_points[2];
} screensaver_instance_t;

static screensaver_instance_t s_active_instance;
static screensaver_instance_t s_staging_instance;
static screensaver_instance_t *s_instance = &s_active_instance;

#define s_screensaver (s_instance->screensaver)
#define s_screensaver_clock_label (s_instance->clock_label)
#define s_screensaver_analog_face (s_instance->analog_face)
#define s_screensaver_mascot_face (s_instance->mascot_face)
#define s_screensaver_hour_hand (s_instance->hour_hand)
#define s_screensaver_minute_hand (s_instance->minute_hand)
#define s_screensaver_clock_center (s_instance->clock_center)
#define s_screensaver_analog_ticks (s_instance->analog_ticks)
#define s_screensaver_mascot_lines (s_instance->mascot_lines)
#define s_screensaver_mascot_eyes (s_instance->mascot_eyes)
#define s_screensaver_mascot_blink (s_instance->mascot_blink)
#define s_saver_hour_points (s_instance->hour_points)
#define s_saver_minute_points (s_instance->minute_points)

static void screensaver_analog_refresh_locked(const struct tm *local_time);
static void screensaver_mascot_refresh_locked(uint32_t elapsed_ms);
static void screensaver_style_apply_locked(int style);
static void screensaver_destroy_current(void);
static esp_err_t screensaver_create_current(lv_event_cb_t activity_callback,
                                            uint32_t accent_color, int style);
static void screensaver_refresh_current(int style, uint32_t elapsed_ms);

static lv_obj_t *screensaver_clock_label_create(lv_obj_t *parent)
{
    lv_obj_t *label = radio_ui_lvgl_label_create(parent);
    if (label == NULL) return NULL;
    radio_ui_text_set_label(label, "--:--");
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(label, radio_ui_text_font_for_size(18), 0);
    lv_obj_set_width(label, 240);
    lv_obj_set_pos(label, 0, 83);
    lv_obj_set_height(label, 72);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_clickable(label, false);
    return label;
}

void radio_ui_screensaver_refresh(int style, uint32_t elapsed_ms)
{
    s_instance = &s_active_instance;
    screensaver_refresh_current(style, elapsed_ms);
}

static void screensaver_refresh_current(int style, uint32_t elapsed_ms)
{
    if (s_screensaver == NULL) return;
    time_t now = time(NULL);
    struct tm local_time;
    char clock_text[8] = "--:--";
    if (now >= 1735689600 && localtime_r(&now, &local_time) != NULL) {
        strftime(clock_text, sizeof(clock_text), "%H:%M", &local_time);
    } else {
        memset(&local_time, 0, sizeof(local_time));
    }

    if (style == 0) {
        if (s_screensaver_clock_label != NULL) {
            radio_ui_text_set_label(s_screensaver_clock_label, clock_text);
        }
    } else if (style == 1) {
        screensaver_analog_refresh_locked(&local_time);
    } else {
        screensaver_mascot_refresh_locked(elapsed_ms);
    }
}

static lv_obj_t *screensaver_line_create(lv_obj_t *parent,
                                         const lv_point_precise_t *points,
                                         uint32_t point_count, uint32_t color,
                                         int32_t width)
{
    lv_obj_t *line = radio_ui_lvgl_line_create(parent);
    if (line == NULL) return NULL;
    lv_obj_set_pos(line, 0, 0);
    lv_obj_set_size(line, 240, 240);
    lv_obj_set_style_bg_opa(line, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_line_color(line, lv_color_hex(color), 0);
    lv_obj_set_style_line_width(line, width, 0);
    lv_line_set_points(line, points, point_count);
    lv_obj_set_clickable(line, false);
    return line;
}

static void screensaver_analog_refresh_locked(const struct tm *local_time)
{
    if (local_time == NULL || s_screensaver_analog_face == NULL ||
        s_screensaver_hour_hand == NULL || s_screensaver_minute_hand == NULL) {
        return;
    }
    const float minute = (float)local_time->tm_min + (float)local_time->tm_sec / 60.0f;
    const float hour = (float)(local_time->tm_hour % 12) + minute / 60.0f;
    const float minute_angle = (minute * 6.0f - 90.0f) * 0.01745329252f;
    const float hour_angle = (hour * 30.0f - 90.0f) * 0.01745329252f;
    s_saver_hour_points[0] = (lv_point_precise_t){120, 120};
    s_saver_hour_points[1] = (lv_point_precise_t){
        (lv_value_precise_t)(120 + cosf(hour_angle) * 34.0f),
        (lv_value_precise_t)(120 + sinf(hour_angle) * 34.0f)};
    s_saver_minute_points[0] = (lv_point_precise_t){120, 120};
    s_saver_minute_points[1] = (lv_point_precise_t){
        (lv_value_precise_t)(120 + cosf(minute_angle) * 51.0f),
        (lv_value_precise_t)(120 + sinf(minute_angle) * 51.0f)};
    lv_line_set_points_mutable(s_screensaver_hour_hand, s_saver_hour_points, 2);
    lv_line_set_points_mutable(s_screensaver_minute_hand, s_saver_minute_points, 2);
}

static void screensaver_mascot_refresh_locked(uint32_t elapsed_ms)
{
    if (s_screensaver_mascot_eyes[0] == NULL ||
        s_screensaver_mascot_blink[0] == NULL) return;
    const bool blink = ((elapsed_ms / 900U) % 2U) != 0;
    for (size_t i = 0; i < 2; ++i) {
        if (s_screensaver_mascot_eyes[i] != NULL)
            lv_obj_set_hidden(s_screensaver_mascot_eyes[i], blink);
        if (s_screensaver_mascot_blink[i] != NULL)
            lv_obj_set_hidden(s_screensaver_mascot_blink[i], !blink);
    }
}

static esp_err_t screensaver_create_current(lv_event_cb_t activity_callback,
                                            uint32_t accent_color, int style)
{
    if (s_screensaver != NULL) return ESP_OK;
    s_screensaver = radio_ui_lvgl_obj_create(lv_layer_top());
    if (s_screensaver == NULL) return ESP_ERR_NO_MEM;
    lv_obj_set_size(s_screensaver, 240, 240);
    lv_obj_set_pos(s_screensaver, 0, 0);
    lv_obj_set_style_bg_color(s_screensaver, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screensaver, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screensaver, 0, 0);
    lv_obj_set_style_radius(s_screensaver, 0, 0);
    lv_obj_set_style_pad_all(s_screensaver, 0, 0);
    lv_obj_set_scrollable(s_screensaver, false);
    lv_obj_set_clickable(s_screensaver, true);
    lv_obj_set_hidden(s_screensaver, true);
    lv_obj_add_event_cb(s_screensaver, activity_callback, LV_EVENT_PRESSED, NULL);

    s_screensaver_clock_label = screensaver_clock_label_create(s_screensaver);
    if (s_screensaver_clock_label == NULL) goto no_memory;
    lv_obj_set_style_text_align(s_screensaver_clock_label,
                                LV_TEXT_ALIGN_CENTER, 0);
#if LV_FONT_MONTSERRAT_48
    lv_obj_set_style_text_font(s_screensaver_clock_label,
                               radio_ui_text_font_for_size(48), 0);
#else
    lv_obj_set_style_text_font(s_screensaver_clock_label,
                               radio_ui_text_font_for_size(24), 0);
#endif

    s_screensaver_mascot_face = radio_ui_lvgl_obj_create(s_screensaver);
    if (s_screensaver_mascot_face == NULL) goto no_memory;
    lv_obj_set_size(s_screensaver_mascot_face, 100, 82);
    lv_obj_set_pos(s_screensaver_mascot_face, 70, 86);
    lv_obj_set_style_bg_opa(s_screensaver_mascot_face, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_screensaver_mascot_face, 2, 0);
    lv_obj_set_style_border_color(s_screensaver_mascot_face,
                                  lv_color_white(), 0);
    lv_obj_set_style_radius(s_screensaver_mascot_face, 38, 0);
    lv_obj_set_style_pad_all(s_screensaver_mascot_face, 0, 0);
    lv_obj_set_clickable(s_screensaver_mascot_face, false);
    lv_obj_set_hidden(s_screensaver_mascot_face, true);

    static const lv_point_precise_t left_ear[] = {
        {78, 94}, {83, 53}, {111, 84}, {78, 94}};
    static const lv_point_precise_t right_ear[] = {
        {129, 84}, {157, 53}, {162, 94}, {129, 84}};
    static const lv_point_precise_t nose_mouth[] = {
        {116, 123}, {120, 127}, {124, 123}};
    static const lv_point_precise_t whisker_left_top[] = {{84, 118}, {105, 121}};
    static const lv_point_precise_t whisker_left_bottom[] = {{83, 132}, {105, 129}};
    static const lv_point_precise_t whisker_right_top[] = {{135, 121}, {156, 118}};
    static const lv_point_precise_t whisker_right_bottom[] = {{135, 129}, {157, 132}};
    const lv_point_precise_t *const mascot_points[] = {
        left_ear, right_ear, nose_mouth,
        whisker_left_top, whisker_left_bottom,
        whisker_right_top, whisker_right_bottom,
    };
    static const uint32_t mascot_point_counts[] = {4, 4, 3, 2, 2, 2, 2};
    for (size_t i = 0; i < 7; ++i) {
        s_screensaver_mascot_lines[i] = screensaver_line_create(
            s_screensaver, mascot_points[i], mascot_point_counts[i],
            i == 2 ? accent_color : 0xFFFFFF, i < 3 ? 3 : 2);
        if (s_screensaver_mascot_lines[i] == NULL) goto no_memory;
    }

    for (size_t i = 0; i < 2; ++i) {
        s_screensaver_mascot_eyes[i] = radio_ui_lvgl_obj_create(s_screensaver);
        if (s_screensaver_mascot_eyes[i] == NULL) goto no_memory;
        lv_obj_t *eye = s_screensaver_mascot_eyes[i];
        lv_obj_set_size(eye, 7, 7);
        lv_obj_set_pos(eye, i == 0 ? 101 : 132, 107);
        lv_obj_set_style_bg_color(eye, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(eye, 0, 0);
        lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_clickable(eye, false);
        lv_obj_set_hidden(eye, true);

        s_screensaver_mascot_blink[i] = radio_ui_lvgl_obj_create(s_screensaver);
        if (s_screensaver_mascot_blink[i] == NULL) goto no_memory;
        lv_obj_t *blink = s_screensaver_mascot_blink[i];
        lv_obj_set_size(blink, 13, 3);
        lv_obj_set_pos(blink, i == 0 ? 98 : 129, 109);
        lv_obj_set_style_bg_color(blink, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(blink, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(blink, 0, 0);
        lv_obj_set_style_radius(blink, 2, 0);
        lv_obj_set_clickable(blink, false);
        lv_obj_set_hidden(blink, true);
    }

    s_screensaver_analog_face = radio_ui_lvgl_obj_create(s_screensaver);
    if (s_screensaver_analog_face == NULL) goto no_memory;
    lv_obj_set_size(s_screensaver_analog_face, 144, 144);
    lv_obj_set_pos(s_screensaver_analog_face, 48, 48);
    lv_obj_set_style_bg_opa(s_screensaver_analog_face, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_screensaver_analog_face, 2, 0);
    lv_obj_set_style_border_color(s_screensaver_analog_face,
                                  lv_color_white(), 0);
    lv_obj_set_style_radius(s_screensaver_analog_face, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(s_screensaver_analog_face, 0, 0);
    lv_obj_set_clickable(s_screensaver_analog_face, false);

    static const lv_point_precise_t analog_ticks[][2] = {
        {{118, 51}, {122, 51}}, {{152, 60}, {155, 63}}, {{177, 85}, {180, 88}},
        {{186, 118}, {190, 122}}, {{177, 152}, {180, 155}}, {{152, 177}, {155, 180}},
        {{118, 186}, {122, 190}}, {{85, 177}, {88, 180}}, {{60, 152}, {63, 155}},
        {{51, 118}, {55, 122}}, {{60, 85}, {63, 88}}, {{85, 60}, {88, 63}},
    };
    for (size_t i = 0; i < 12; ++i) {
        s_screensaver_analog_ticks[i] = screensaver_line_create(
            s_screensaver, analog_ticks[i], 2, 0xFFFFFF, 2);
        if (s_screensaver_analog_ticks[i] == NULL) goto no_memory;
    }

    s_screensaver_hour_hand = radio_ui_lvgl_line_create(s_screensaver);
    if (s_screensaver_hour_hand == NULL) goto no_memory;
    s_screensaver_minute_hand = radio_ui_lvgl_line_create(s_screensaver);
    if (s_screensaver_minute_hand == NULL) goto no_memory;
    lv_obj_t *hands[] = {s_screensaver_hour_hand, s_screensaver_minute_hand};
    for (size_t i = 0; i < 2; ++i) {
        lv_obj_set_pos(hands[i], 0, 0);
        lv_obj_set_size(hands[i], 240, 240);
        lv_obj_set_style_bg_opa(hands[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(hands[i], 0, 0);
        lv_obj_set_style_line_width(hands[i], i == 0 ? 4 : 2, 0);
        lv_obj_set_style_line_color(hands[i],
                                    lv_color_hex(i == 0 ? 0xFFFFFF : accent_color), 0);
        lv_obj_set_clickable(hands[i], false);
    }
    static const lv_point_precise_t hand_seed[] = {{120, 120}, {120, 70}};
    lv_line_set_points(s_screensaver_hour_hand, hand_seed, 2);
    lv_line_set_points(s_screensaver_minute_hand, hand_seed, 2);

    s_screensaver_clock_center = radio_ui_lvgl_obj_create(s_screensaver);
    if (s_screensaver_clock_center == NULL) goto no_memory;
    lv_obj_set_size(s_screensaver_clock_center, 8, 8);
    lv_obj_set_pos(s_screensaver_clock_center, 116, 116);
    lv_obj_set_style_bg_color(s_screensaver_clock_center,
                              lv_color_hex(accent_color), 0);
    lv_obj_set_style_bg_opa(s_screensaver_clock_center, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screensaver_clock_center, 0, 0);
    lv_obj_set_style_radius(s_screensaver_clock_center, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_clickable(s_screensaver_clock_center, false);

    screensaver_refresh_current(style, 0);
    return ESP_OK;

no_memory:
    screensaver_destroy_current();
    return ESP_ERR_NO_MEM;
}

esp_err_t radio_ui_screensaver_create(lv_event_cb_t activity_callback,
                                      uint32_t accent_color, int style)
{
    s_instance = &s_active_instance;
    return screensaver_create_current(activity_callback, accent_color, style);
}

esp_err_t radio_ui_screensaver_stage_create(lv_event_cb_t activity_callback,
                                            uint32_t accent_color, int style)
{
    s_instance = &s_staging_instance;
    screensaver_destroy_current();
    const esp_err_t err = screensaver_create_current(activity_callback,
                                                     accent_color, style);
    s_instance = &s_active_instance;
    return err;
}

static void screensaver_style_apply_locked(int style)
{
    const bool digital = style == 0;
    const bool analog = style == 1;
    const bool mascot = style == 2;
    if (s_screensaver_clock_label != NULL)
        lv_obj_set_hidden(s_screensaver_clock_label, !digital);
    if (s_screensaver_analog_face != NULL)
        lv_obj_set_hidden(s_screensaver_analog_face, !analog);
    if (s_screensaver_clock_center != NULL)
        lv_obj_set_hidden(s_screensaver_clock_center, !analog);
    if (s_screensaver_hour_hand != NULL)
        lv_obj_set_hidden(s_screensaver_hour_hand, !analog);
    if (s_screensaver_minute_hand != NULL)
        lv_obj_set_hidden(s_screensaver_minute_hand, !analog);
    for (size_t i = 0; i < 12; ++i) {
        if (s_screensaver_analog_ticks[i] != NULL)
            lv_obj_set_hidden(s_screensaver_analog_ticks[i], !analog);
    }
    if (s_screensaver_mascot_face != NULL)
        lv_obj_set_hidden(s_screensaver_mascot_face, !mascot);
    for (size_t i = 0; i < 7; ++i) {
        if (s_screensaver_mascot_lines[i] != NULL)
            lv_obj_set_hidden(s_screensaver_mascot_lines[i], !mascot);
    }
    for (size_t i = 0; i < 2; ++i) {
        if (s_screensaver_mascot_eyes[i] != NULL)
            lv_obj_set_hidden(s_screensaver_mascot_eyes[i], !mascot);
        if (s_screensaver_mascot_blink[i] != NULL)
            lv_obj_set_hidden(s_screensaver_mascot_blink[i], true);
    }
}


bool radio_ui_screensaver_exists(void)
{
    return s_active_instance.screensaver != NULL;
}

void radio_ui_screensaver_show(int style)
{
    s_instance = &s_active_instance;
    if (s_screensaver == NULL) return;
    screensaver_refresh_current(style, 0);
    screensaver_style_apply_locked(style);
    lv_obj_set_hidden(s_screensaver, false);
}

void radio_ui_screensaver_hide(void)
{
    s_instance = &s_active_instance;
    if (s_screensaver != NULL) lv_obj_set_hidden(s_screensaver, true);
}

static void screensaver_destroy_current(void)
{
    if (s_screensaver != NULL) lv_obj_delete(s_screensaver);
    s_screensaver = NULL;
    s_screensaver_clock_label = NULL;
    s_screensaver_analog_face = NULL;
    s_screensaver_mascot_face = NULL;
    s_screensaver_hour_hand = NULL;
    s_screensaver_minute_hand = NULL;
    s_screensaver_clock_center = NULL;
    memset(s_screensaver_analog_ticks, 0, sizeof(s_screensaver_analog_ticks));
    memset(s_screensaver_mascot_lines, 0, sizeof(s_screensaver_mascot_lines));
    memset(s_screensaver_mascot_eyes, 0, sizeof(s_screensaver_mascot_eyes));
    memset(s_screensaver_mascot_blink, 0, sizeof(s_screensaver_mascot_blink));
}

void radio_ui_screensaver_destroy(void)
{
    s_instance = &s_active_instance;
    screensaver_destroy_current();
}

void radio_ui_screensaver_stage_discard(void)
{
    s_instance = &s_staging_instance;
    screensaver_destroy_current();
    s_instance = &s_active_instance;
}

void radio_ui_screensaver_stage_publish(void)
{
    screensaver_instance_t previous = s_active_instance;
    s_active_instance = s_staging_instance;
    s_staging_instance = previous;
    s_instance = &s_staging_instance;
    screensaver_destroy_current();
    s_instance = &s_active_instance;
}
