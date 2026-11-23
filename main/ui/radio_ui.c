#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "radio_board.h"
#include "radio_catalog.h"
#include "radio_generation.h"
#include "radio_player.h"
#include "radio_station_store.h"
#include "radio_types.h"
#include "radio_ui.h"
#include "radio_ui_idle_state.h"
#include "radio_ui_lvgl.h"
#include "radio_test_adapters.h"
#include "radio_ui_screensaver.h"
#include "radio_ui_text.h"
#include "radio_ui_theme.h"
#include "radio_wifi.h"

#define UI_STATION_LIMIT (RADIO_CATALOG_PAGE_SIZE * 2)
#define CATALOG_UI_RENDER_BATCH 3
#define RADIO_TAB_COUNT 3
#define SCREEN_SAVER_BRIGHTNESS 35
#define SCREEN_SAVER_VISIBLE_MS 3000
/* Compact selectors use 42 px; primary actions remain at least 44 px. */
#define UI_MIN_TOUCH_TARGET_PX 40
#define SETTINGS_STATUS_Y 51
#define SETTINGS_INFO_Y 70
#define SETTINGS_INFO_HEIGHT 38
#define SETTINGS_LIST_Y 111
#define SETTINGS_LIST_HEIGHT 125

static radio_ui_palette_t s_theme;

#define COLOR_BG       (s_theme.background)
#define COLOR_CARD     (s_theme.surface)
#define COLOR_CARD_2   (s_theme.surface_alt)
#define COLOR_PRESS    (s_theme.pressed)
#define COLOR_MINT     (s_theme.accent)
#define COLOR_GOLD     (s_theme.warning)
#define COLOR_TEXT     (s_theme.text)
#define COLOR_MUTED    (s_theme.muted)
#define COLOR_ERROR    (s_theme.error)

static const char *TAG = "radio_ui";

typedef enum { PAGE_LISTEN = 0, PAGE_RADIOS, PAGE_SETTINGS } main_page_t;
typedef enum { RADIO_TAB_FAVORITES = 0, RADIO_TAB_RECENTS, RADIO_TAB_EXPLORE } radio_tab_t;

static bool s_battery_charge_flash;
static bool s_battery_charge_state_known;
static bool s_battery_is_charging;
static bool s_home_favorite_active;
static bool s_wifi_connected;
static bool s_wifi_provisioning;
static uint32_t s_wifi_status_generation;
static bool s_wifi_status_generation_valid;
static bool s_theme_rebuild_pending;
static int s_requested_theme_id;
static int s_requested_accent_id;
static TaskHandle_t s_sleep_task_handle;
static radio_ui_idle_state_t s_idle_state;
static portMUX_TYPE s_idle_state_mux = portMUX_INITIALIZER_UNLOCKED;
static lv_timer_t *s_idle_event_timer;
static radio_ui_idle_display_t s_idle_rendered_display = RADIO_UI_IDLE_DISPLAY_NORMAL;
static int s_idle_rendered_screensaver_style = -1;
static bool s_idle_render_initialized;
static radio_station_t *s_stations;
static size_t s_station_capacity;
static size_t s_station_count;
static size_t s_next_catalog_offset;
static size_t s_catalog_oldest_offset;
static size_t s_catalog_page_offsets[2];
static size_t s_catalog_page_counts[2];
static size_t s_catalog_page_count;
static size_t s_catalog_retry_offset;
static bool s_catalog_has_more;
static bool s_catalog_scroll_anchor_pending;
static size_t s_catalog_scroll_anchor_index;
static int32_t s_catalog_scroll_anchor_y;
static QueueHandle_t s_catalog_result_queue;
static lv_timer_t *s_catalog_result_timer;

typedef struct {
    lv_obj_t *row;
    size_t station_index;
    bool in_use;
} station_row_context_t;

typedef struct {
    lv_obj_t *screens[3];
    lv_obj_t *home_wifi_label;
    lv_obj_t *home_station_label;
    lv_obj_t *home_playback_label;
    lv_obj_t *home_meta_label;
    lv_obj_t *home_volume_label;
    lv_obj_t *home_volume_bar;
    lv_obj_t *home_time_label;
    lv_obj_t *home_battery_body;
    lv_obj_t *home_battery_fill;
    lv_obj_t *home_battery_cap;
    lv_obj_t *home_battery_label;
    lv_obj_t *home_battery_charge_icon;
    lv_obj_t *favorite_button;
    lv_obj_t *play_button_label;
    lv_obj_t *radio_status_label;
    lv_obj_t *radio_list;
    lv_obj_t *radio_back_button;
    lv_obj_t *radio_result_title;
    lv_obj_t *load_more_button;
    lv_obj_t *load_previous_button;
    lv_obj_t *settings_status_label;
    lv_obj_t *settings_info_label;
    lv_obj_t *settings_list;
    lv_obj_t *timer_button_label;
    lv_obj_t *screen_button_label;
    lv_obj_t *screen_brightness_choice_label;
    lv_obj_t *screen_timeout_choice_label;
    lv_obj_t *screensaver_choice_label;
    lv_obj_t *clock_settings_value_label;
    lv_obj_t *modal_overlay;
    lv_obj_t *error_overlay;
    lv_obj_t *error_message;
    lv_obj_t *portal_qr_code;
    lv_obj_t *portal_qr_hint;
    lv_obj_t *portal_qr_manual_label;
    lv_obj_t *portal_qr_toggle_label;
    bool portal_qr_manual_visible;
    lv_obj_t *radio_tab_buttons[RADIO_TAB_COUNT];
    station_row_context_t station_row_contexts[UI_STATION_LIMIT];
} ui_view_handles_t;

static ui_view_handles_t s_ui_view_handles;
static ui_view_handles_t s_staging_view_handles;
static ui_view_handles_t s_previous_view_handles;
static ui_view_handles_t *s_view_handles = &s_ui_view_handles;

#define s_screens (s_view_handles->screens)
#define s_home_wifi_label (s_view_handles->home_wifi_label)
#define s_home_station_label (s_view_handles->home_station_label)
#define s_home_playback_label (s_view_handles->home_playback_label)
#define s_home_meta_label (s_view_handles->home_meta_label)
#define s_home_volume_label (s_view_handles->home_volume_label)
#define s_home_volume_bar (s_view_handles->home_volume_bar)
#define s_home_time_label (s_view_handles->home_time_label)
#define s_home_battery_body (s_view_handles->home_battery_body)
#define s_home_battery_fill (s_view_handles->home_battery_fill)
#define s_home_battery_cap (s_view_handles->home_battery_cap)
#define s_home_battery_label (s_view_handles->home_battery_label)
#define s_home_battery_charge_icon (s_view_handles->home_battery_charge_icon)
#define s_favorite_button (s_view_handles->favorite_button)
#define s_play_button_label (s_view_handles->play_button_label)
#define s_radio_status_label (s_view_handles->radio_status_label)
#define s_radio_list (s_view_handles->radio_list)
#define s_radio_back_button (s_view_handles->radio_back_button)
#define s_radio_result_title (s_view_handles->radio_result_title)
#define s_load_more_button (s_view_handles->load_more_button)
#define s_load_previous_button (s_view_handles->load_previous_button)
#define s_settings_status_label (s_view_handles->settings_status_label)
#define s_settings_info_label (s_view_handles->settings_info_label)
#define s_settings_list (s_view_handles->settings_list)
#define s_timer_button_label (s_view_handles->timer_button_label)
#define s_screen_button_label (s_view_handles->screen_button_label)
#define s_screen_brightness_choice_label (s_view_handles->screen_brightness_choice_label)
#define s_screen_timeout_choice_label (s_view_handles->screen_timeout_choice_label)
#define s_screensaver_choice_label (s_view_handles->screensaver_choice_label)
#define s_clock_settings_value_label (s_view_handles->clock_settings_value_label)
#define s_modal_overlay (s_view_handles->modal_overlay)
#define s_error_overlay (s_view_handles->error_overlay)
#define s_error_message (s_view_handles->error_message)
#define s_portal_qr_code (s_view_handles->portal_qr_code)
#define s_portal_qr_hint (s_view_handles->portal_qr_hint)
#define s_portal_qr_manual_label (s_view_handles->portal_qr_manual_label)
#define s_portal_qr_toggle_label (s_view_handles->portal_qr_toggle_label)
#define s_portal_qr_manual_visible (s_view_handles->portal_qr_manual_visible)
#define s_radio_tab_buttons (s_view_handles->radio_tab_buttons)
#define s_station_row_contexts (s_view_handles->station_row_contexts)

#ifdef RADIO_UI_TEST_FAULT_INJECTION
static size_t s_test_view_root_count;
static lv_obj_t *s_test_fallback_screen;

static void test_view_root_deleted(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_DELETE &&
        s_test_view_root_count > 0) {
        --s_test_view_root_count;
    }
}
#endif

static void view_root_track(lv_obj_t *root)
{
#ifdef RADIO_UI_TEST_FAULT_INJECTION
    if (root != NULL) {
        ++s_test_view_root_count;
        lv_obj_add_event_cb(root, test_view_root_deleted, LV_EVENT_DELETE, NULL);
    }
#else
    (void)root;
#endif
}

typedef struct {
    radio_station_t *stations;
    size_t count;
    size_t offset;
    size_t next_offset;
    size_t first_index;
    size_t append_count;
    size_t rendered_count;
    bool has_more;
    bool prepared;
    uint32_t request_id;
    esp_err_t result;
} catalog_ui_result_t;

static catalog_ui_result_t *s_catalog_render_pending;
static radio_station_t s_current_station;
static bool s_has_current_station;
static radio_tab_t s_radio_tab = RADIO_TAB_FAVORITES;
static radio_catalog_filter_t s_explore_filter = RADIO_CATALOG_FILTER_COUNTRY;
static bool s_explore_category_open;
static bool s_showing_catalog_results;
static bool s_catalog_popular;
static char s_catalog_query[64];
static uint32_t s_catalog_request_id;
static uint32_t s_player_status_sequence;
static bool s_player_status_sequence_valid;
static uint32_t s_favorite_mutation_generation;
static radio_player_state_t s_player_state = RADIO_PLAYER_STATE_IDLE;
static int s_theme_id;
static int s_accent_id;
static uint8_t s_clock_edit_hour = 12;
static uint8_t s_clock_edit_minute;
static int s_volume_last_displayed = -1;
static lv_timer_t *s_volume_sync_timer;
static lv_timer_t *s_header_status_timer;
static bool s_power_off_pending;
static bool s_power_off_launch_failed;
static portMUX_TYPE s_power_off_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_portal_start_pending;
static bool s_portal_qr_pending;
static portMUX_TYPE s_portal_start_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_swipe_tracking;
static lv_point_t s_swipe_start;
static lv_anim_t s_station_name_scroll_anim;
static bool s_station_name_scroll_anim_ready;

typedef struct {
    radio_player_state_t state;
    uint32_t sequence;
    char message[72];
} player_ui_status_event_t;

static StaticQueue_t s_player_status_queue_state;
static player_ui_status_event_t s_player_status_queue_storage[1];
static QueueHandle_t s_player_status_queue;
static lv_timer_t *s_player_status_timer;

typedef struct {
    esp_err_t result;
    bool persistence_error;
    uint32_t generation;
} favorite_mutation_event_t;

static StaticQueue_t s_favorite_mutation_queue_state;
static favorite_mutation_event_t s_favorite_mutation_queue_storage[1];
static QueueHandle_t s_favorite_mutation_queue;
static lv_timer_t *s_favorite_mutation_timer;

static StaticQueue_t s_wifi_status_queue_state;
static uint8_t s_wifi_status_queue_storage[1];
static QueueHandle_t s_wifi_status_queue;
static StaticQueue_t s_wifi_portal_result_queue_state;
static esp_err_t s_wifi_portal_result_queue_storage[1];
static QueueHandle_t s_wifi_portal_result_queue;
static lv_timer_t *s_wifi_status_timer;

static void page_select_event(lv_event_t *event);
static void page_swipe_event(lv_event_t *event);
static void radio_tab_event(lv_event_t *event);
static void close_overlay_event(lv_event_t *event);
static void overlay_background_clicked(lv_event_t *event);
static void ui_activity_event(lv_event_t *event);
static void show_screen_settings(lv_event_t *event);
static void show_appearance_settings(lv_event_t *event);
static void theme_choice_event(lv_event_t *event);
static void accent_choice_event(lv_event_t *event);
static void screensaver_style_event(lv_event_t *event);
static void screen_clock_settings_event(lv_event_t *event);
static void screen_brightness_event(lv_event_t *event);
static void screen_timeout_event(lv_event_t *event);
static void show_clock_settings(lv_event_t *event);
static void clock_adjust_event(lv_event_t *event);
static void clock_save_event(lv_event_t *event);
static void navigation_back_event(lv_event_t *event);
static void explore_back_event(lv_event_t *event);
static esp_err_t screensaver_stage_build(void);
static void idle_event_timer_cb(lv_timer_t *timer);
static void wifi_status_timer_cb(lv_timer_t *timer);
static void idle_apply_pending(void);
static void idle_render_display(const radio_ui_idle_snapshot_t *snapshot);
static void idle_record_activity(void);
static radio_ui_idle_snapshot_t idle_state_snapshot(void);
static esp_err_t ui_theme_rebuild(bool persist);
static void ui_theme_apply_values(void);
static void ui_theme_rebuild_async(void *unused);
static void ui_theme_rebuild_deferred(void);
static void favorite_update_home(void);
static void update_home_station(void);
static void home_playback_status_update(void);
static void heart_draw_event(lv_event_t *event);
static void load_more_stations(lv_event_t *event);
static void load_previous_stations(lv_event_t *event);
static void retry_catalog_request(lv_event_t *event);
static void catalog_clear_widgets(void);
static void catalog_ui_result_free(catalog_ui_result_t *result);
static void player_status_callback(radio_player_state_t state, uint32_t sequence,
                                   const char *station, const char *message);
static void player_status_timer_cb(lv_timer_t *timer);
static void favorite_mutation_timer_cb(lv_timer_t *timer);
static void favorite_mutation_complete(esp_err_t result, bool is_favorite,
                                      void *context);
static esp_err_t favorite_toggle_request(const radio_station_t *station);
static void store_error_callback(esp_err_t result, void *context);
static void favorite_mutation_apply(const favorite_mutation_event_t *event);
static void wifi_status_snapshot_apply(const radio_wifi_info_t *info);
static void wifi_portal_start_task(void *arg);
static void catalog_callback(const radio_station_t *stations, size_t count,
                             size_t offset, size_t next_offset, bool has_more,
                             uint32_t request_id, esp_err_t result);
static void catalog_result_timer_cb(lv_timer_t *timer);
static void volume_status_update(void);
static void volume_sync_timer_cb(lv_timer_t *timer);
static void header_status_timer_cb(lv_timer_t *timer);
static void header_status_refresh(void);
static void battery_charge_timer_cb(lv_timer_t *timer);
static void battery_charge_refresh(void);
static esp_err_t populate_radio_view(void);
static void populate_radio_view_report(void);
static void catalog_request_page(size_t offset);
static void update_radio_tab_styles(void);
static void refresh_radio_view_async(void *unused);
static void error_popup_show(const char *message);
static void error_popup_hide(void);
static void error_popup_dismiss_event(lv_event_t *event);
static void error_popup_open_radios_event(lv_event_t *event);
static void explore_from_empty_event(lv_event_t *event);

static void apply_screen_style(lv_obj_t *screen)
{
    if (screen == NULL) return;
    lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_scrollable(screen, false);
    lv_obj_add_event_cb(screen, ui_activity_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(screen, page_swipe_event, LV_EVENT_PRESSED, screen);
    lv_obj_add_event_cb(screen, page_swipe_event, LV_EVENT_RELEASED, screen);
}

static radio_ui_idle_snapshot_t idle_state_snapshot(void)
{
    radio_ui_idle_snapshot_t snapshot = {0};
    portENTER_CRITICAL(&s_idle_state_mux);
    radio_ui_idle_snapshot(&s_idle_state, &snapshot);
    portEXIT_CRITICAL(&s_idle_state_mux);
    return snapshot;
}

static void idle_record_activity(void)
{
    const uint32_t now_tick = (uint32_t)xTaskGetTickCount();
    portENTER_CRITICAL(&s_idle_state_mux);
    radio_ui_idle_set_activity(&s_idle_state, now_tick);
    portEXIT_CRITICAL(&s_idle_state_mux);
    if (s_sleep_task_handle != NULL) xTaskNotifyGive(s_sleep_task_handle);
}

static void idle_render_display(const radio_ui_idle_snapshot_t *snapshot)
{
    if (snapshot == NULL) return;

    if (snapshot->display == RADIO_UI_IDLE_DISPLAY_NORMAL) {
        if (!s_idle_render_initialized ||
            s_idle_rendered_display != RADIO_UI_IDLE_DISPLAY_NORMAL) {
            radio_ui_screensaver_hide();
        }
        radio_board_display_set_brightness((uint8_t)snapshot->screen_brightness);
        s_idle_rendered_display = RADIO_UI_IDLE_DISPLAY_NORMAL;
        s_idle_rendered_screensaver_style = snapshot->screensaver_style;
        s_idle_render_initialized = true;
        return;
    }

    const bool show_screensaver =
        !s_idle_render_initialized ||
        s_idle_rendered_display == RADIO_UI_IDLE_DISPLAY_NORMAL ||
        s_idle_rendered_screensaver_style != snapshot->screensaver_style;
    if (show_screensaver && radio_ui_screensaver_exists()) {
        radio_ui_screensaver_show(snapshot->screensaver_style);
    }

    if (snapshot->display == RADIO_UI_IDLE_DISPLAY_SCREENSAVER) {
        radio_board_display_set_brightness(SCREEN_SAVER_BRIGHTNESS);
        const uint32_t elapsed_ms =
            (uint32_t)(xTaskGetTickCount() - snapshot->screensaver_started_tick) *
            portTICK_PERIOD_MS;
        radio_ui_screensaver_refresh(snapshot->screensaver_style, elapsed_ms);
    } else {
        radio_board_display_set_brightness(0);
    }

    s_idle_rendered_display = snapshot->display;
    s_idle_rendered_screensaver_style = snapshot->screensaver_style;
    s_idle_render_initialized = true;
}

static void idle_apply_pending(void)
{
    radio_ui_idle_snapshot_t snapshot = {0};
    uint32_t events;
    portENTER_CRITICAL(&s_idle_state_mux);
    events = radio_ui_idle_take_events(&s_idle_state, &snapshot);
    portEXIT_CRITICAL(&s_idle_state_mux);
    if (events == 0) return;

    if ((events & RADIO_UI_IDLE_EVENT_DISPLAY) != 0) {
        idle_render_display(&snapshot);
    } else if ((events & RADIO_UI_IDLE_EVENT_REFRESH) != 0 &&
               snapshot.display == RADIO_UI_IDLE_DISPLAY_SCREENSAVER) {
        const uint32_t elapsed_ms =
            (uint32_t)(xTaskGetTickCount() - snapshot.screensaver_started_tick) *
            portTICK_PERIOD_MS;
        radio_ui_screensaver_refresh(snapshot.screensaver_style, elapsed_ms);
    }

    if ((events & RADIO_UI_IDLE_EVENT_SLEEP_EXPIRED) != 0) {
        if (s_timer_button_label != NULL) {
            radio_ui_text_set_label(s_timer_button_label,
                                    "Radio detenida por temporizador");
        }
    }
}

static void idle_event_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    idle_apply_pending();
    portENTER_CRITICAL(&s_power_off_mux);
    const bool launch_failed = s_power_off_launch_failed;
    s_power_off_launch_failed = false;
    portEXIT_CRITICAL(&s_power_off_mux);
    if (launch_failed) {
        radio_ui_text_set_label(s_settings_status_label,
                                "No se pudo completar el apagado; intentá otra vez");
    }
}

void radio_ui_wake_display(void)
{
    /* Button-task callers publish state only; LVGL applies the visual event. */
    idle_record_activity();
}

static void ui_activity_event(lv_event_t *event)
{
    (void)event;
    idle_record_activity();
    idle_apply_pending();
}

static lv_obj_t *label_create(lv_obj_t *parent, const char *text, uint32_t color,
                              int32_t x, int32_t y, int32_t width, int32_t height,
                              int32_t font_size)
{
    if (parent == NULL) return NULL;
    lv_obj_t *label = radio_ui_lvgl_label_create(parent);
    if (label == NULL) {
        ESP_LOGE(TAG, "LVGL sin memoria para crear etiqueta");
        return NULL;
    }
    lv_obj_set_style_text_font(label,
                               radio_ui_text_font_for_size(font_size), 0);
    radio_ui_text_set_label(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_width(label, width);
    lv_obj_set_pos(label, x, y);
    const lv_font_t *font =
        lv_obj_get_style_text_font(label, LV_PART_MAIN);
    const int32_t minimum_height = (int32_t)lv_font_get_line_height(font);
    lv_obj_set_height(label, height < minimum_height ? minimum_height : height);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_clickable(label, false);
    return label;
}

static void station_name_scroll_enable(lv_obj_t *label)
{
    if (label == NULL) return;
    if (!s_station_name_scroll_anim_ready) {
        lv_anim_init(&s_station_name_scroll_anim);
        lv_anim_set_repeat_count(&s_station_name_scroll_anim,
                                 LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_repeat_delay(&s_station_name_scroll_anim, 900);
        lv_anim_set_reverse_delay(&s_station_name_scroll_anim, 900);
        s_station_name_scroll_anim_ready = true;
    }
    lv_obj_set_style_anim(label, &s_station_name_scroll_anim, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_SCROLL);
}

static lv_obj_t *button_create(lv_obj_t *parent, const char *text,
                               lv_event_cb_t callback, void *user_data,
                               int32_t x, int32_t y, int32_t width, int32_t height,
                               uint32_t color, uint32_t text_color, int32_t font_size)
{
    if (parent == NULL) return NULL;
    lv_obj_t *button = radio_ui_lvgl_button_create(parent);
    if (button == NULL) {
        ESP_LOGE(TAG, "LVGL sin memoria para crear boton");
        return NULL;
    }
    lv_obj_set_size(button, width, height);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_PRESS), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_border_width(button, 1, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(button, lv_color_hex(COLOR_MINT), LV_STATE_PRESSED);
    lv_obj_set_gesture_bubble(button, true);
    lv_obj_set_event_bubble(button, true);
    lv_obj_add_event_cb(button, ui_activity_event, LV_EVENT_PRESSED, NULL);
    if (callback != NULL) lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    lv_obj_t *label = radio_ui_lvgl_label_create(button);
    if (label == NULL) {
        lv_obj_delete(button);
        ESP_LOGE(TAG, "LVGL sin memoria para crear texto de boton");
        return NULL;
    }
    lv_obj_set_style_text_font(label,
                               radio_ui_text_font_for_size(font_size), 0);
    radio_ui_text_set_label(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(text_color), 0);
    lv_obj_set_width(label, width > 16 ? width - 16 : width);
    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    lv_obj_set_height(label, (int32_t)lv_font_get_line_height(font));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    return button;
}

static void touch_feedback_apply(lv_obj_t *object)
{
    if (object == NULL) return;
    lv_obj_set_style_bg_color(object, lv_color_hex(COLOR_PRESS), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(object, 1, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(object, lv_color_hex(COLOR_MINT), LV_STATE_PRESSED);
}

static void page_swipe_event(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *screen = lv_event_get_user_data(event);
    if (screen == NULL || lv_screen_active() != screen || s_modal_overlay != NULL) return;
    lv_indev_t *indev = lv_event_get_indev(event);
    if (indev == NULL) return;
    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(indev, &s_swipe_start);
        s_swipe_tracking = true;
        return;
    }
    if (code != LV_EVENT_RELEASED || !s_swipe_tracking) return;
    s_swipe_tracking = false;

    lv_point_t end;
    lv_indev_get_point(indev, &end);
    const int32_t dx = end.x - s_swipe_start.x;
    const int32_t dy = end.y - s_swipe_start.y;
    const int32_t abs_dx = LV_ABS(dx);
    const int32_t abs_dy = LV_ABS(dy);
    if (abs_dx < 36 || abs_dx * 4 < abs_dy * 5) return;

    const main_page_t page = screen == s_screens[PAGE_LISTEN] ? PAGE_LISTEN :
                             screen == s_screens[PAGE_RADIOS] ? PAGE_RADIOS : PAGE_SETTINGS;
    if (dx < 0 && page != PAGE_LISTEN) navigation_back_event(NULL);
}

static void page_select_event(lv_event_t *event)
{
    const int page = (int)(intptr_t)lv_event_get_user_data(event);
    if (page >= PAGE_LISTEN && page <= PAGE_SETTINGS) lv_screen_load(s_screens[page]);
}

static void navigation_back_event(lv_event_t *event)
{
    (void)event;
    if (lv_screen_active() == s_screens[PAGE_RADIOS]) {
        if (s_showing_catalog_results || s_explore_category_open) {
            explore_back_event(NULL);
        } else {
            lv_screen_load(s_screens[PAGE_LISTEN]);
        }
    } else if (lv_screen_active() == s_screens[PAGE_SETTINGS]) {
        lv_screen_load(s_screens[PAGE_LISTEN]);
    }
}

static esp_err_t create_home_navigation(lv_obj_t *screen)
{
    if (button_create(screen, "Radios", page_select_event,
                      (void *)(intptr_t)PAGE_RADIOS, 8, 194, 108, 44,
                      COLOR_CARD_2, COLOR_TEXT, 14) == NULL ||
        button_create(screen, "Ajustes", page_select_event,
                      (void *)(intptr_t)PAGE_SETTINGS, 124, 194, 108, 44,
                      COLOR_CARD, COLOR_TEXT, 14) == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void update_radio_tab_styles(void)
{
    const bool results = s_radio_tab == RADIO_TAB_EXPLORE &&
                         s_showing_catalog_results;
    for (int i = 0; i < RADIO_TAB_COUNT; ++i) {
        if (s_radio_tab_buttons[i] == NULL) continue;
        const bool selected = i == s_radio_tab;
        lv_obj_t *label = lv_obj_get_child(s_radio_tab_buttons[i], 0);
        lv_obj_set_style_bg_color(s_radio_tab_buttons[i],
                                  lv_color_hex(selected ? COLOR_MINT : COLOR_CARD), 0);
        if (label != NULL) {
            lv_obj_set_style_text_color(label,
                                        lv_color_hex(selected
                                                         ? radio_ui_theme_text_on_accent(s_theme_id)
                                                         : COLOR_TEXT), 0);
        }
        lv_obj_set_hidden(s_radio_tab_buttons[i], results);
    }
    if (s_radio_back_button != NULL) {
        lv_obj_set_hidden(s_radio_back_button, !results);
    }
    if (s_radio_result_title != NULL) {
        lv_obj_set_hidden(s_radio_result_title, !results);
    }
}

static void close_overlay_event(lv_event_t *event)
{
    (void)event;
    if (s_modal_overlay != NULL) {
        lv_obj_delete(s_modal_overlay);
        s_modal_overlay = NULL;
        s_screen_brightness_choice_label = NULL;
        s_screen_timeout_choice_label = NULL;
        s_screensaver_choice_label = NULL;
        s_clock_settings_value_label = NULL;
        s_portal_qr_code = NULL;
        s_portal_qr_hint = NULL;
        s_portal_qr_manual_label = NULL;
        s_portal_qr_toggle_label = NULL;
        s_portal_qr_manual_visible = false;
    }
}

static void overlay_background_clicked(lv_event_t *event)
{
    if (lv_event_get_target(event) == s_modal_overlay) {
        close_overlay_event(event);
    }
}

static lv_obj_t *modal_create(const char *title, int32_t card_height)
{
    if (s_modal_overlay != NULL || lv_screen_active() == NULL) return NULL;
    s_modal_overlay = radio_ui_lvgl_obj_create(lv_screen_active());
    if (s_modal_overlay == NULL) {
        ESP_LOGE(TAG, "LVGL sin memoria para crear modal");
        return NULL;
    }
    lv_obj_set_size(s_modal_overlay, 240, 240);
    lv_obj_set_pos(s_modal_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_modal_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_modal_overlay, 190, 0);
    lv_obj_set_style_border_width(s_modal_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_modal_overlay, 0, 0);
    lv_obj_set_clickable(s_modal_overlay, true);
    lv_obj_add_event_cb(s_modal_overlay, ui_activity_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_modal_overlay, overlay_background_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = radio_ui_lvgl_obj_create(s_modal_overlay);
    if (card == NULL) {
        lv_obj_delete(s_modal_overlay);
        s_modal_overlay = NULL;
        ESP_LOGE(TAG, "LVGL sin memoria para crear tarjeta modal");
        return NULL;
    }
    lv_obj_set_size(card, 216, card_height);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_clickable(card, true);
    if (label_create(card, title, COLOR_TEXT, 12, 8, 192, 24, 16) == NULL) {
        close_overlay_event(NULL);
        ESP_LOGE(TAG, "LVGL sin memoria para crear el titulo modal");
        return NULL;
    }
    return card;
}

static bool modal_require_object(lv_obj_t *object)
{
    if (object != NULL) return true;
    close_overlay_event(NULL);
    ESP_LOGE(TAG, "LVGL sin memoria para completar el modal");
    return false;
}

static void volume_status_update(void)
{
    const int volume = radio_player_volume();
    if (s_home_volume_label != NULL) {
        char value[12];
        snprintf(value, sizeof(value), "%d%%", volume);
        radio_ui_text_set_label(s_home_volume_label, value);
    }
    if (s_home_volume_bar != NULL) {
        lv_bar_set_value(s_home_volume_bar, volume, LV_ANIM_OFF);
    }
    s_volume_last_displayed = volume;
}

static void volume_sync_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (radio_player_volume() == s_volume_last_displayed) return;
    volume_status_update();
}

static void error_popup_hide(void)
{
    if (s_error_overlay != NULL) lv_obj_set_hidden(s_error_overlay, true);
}

static void error_popup_show(const char *message)
{
    if (s_error_overlay == NULL || s_error_message == NULL) return;
    radio_ui_text_set_label(s_error_message,
                      message != NULL && message[0] != '\0'
                          ? message : "No se pudo iniciar la radio.");
    lv_obj_set_hidden(s_error_overlay, false);
}

static void error_popup_dismiss_event(lv_event_t *event)
{
    (void)event;
    error_popup_hide();
}

static void error_popup_open_radios_event(lv_event_t *event)
{
    (void)event;
    error_popup_hide();
    if (s_screens[PAGE_RADIOS] != NULL)
        lv_screen_load(s_screens[PAGE_RADIOS]);
}

static void error_popup_discard(void)
{
    if (s_error_overlay != NULL) lv_obj_delete(s_error_overlay);
    s_error_overlay = NULL;
    s_error_message = NULL;
}

static esp_err_t build_error_popup(void)
{
    s_error_overlay = radio_ui_lvgl_obj_create(lv_layer_top());
    if (s_error_overlay == NULL) return ESP_ERR_NO_MEM;
    view_root_track(s_error_overlay);
    lv_obj_set_size(s_error_overlay, 240, 240);
    lv_obj_set_pos(s_error_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_error_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_error_overlay, 140, 0);
    lv_obj_set_style_border_width(s_error_overlay, 0, 0);
    lv_obj_set_style_radius(s_error_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_error_overlay, 0, 0);
    lv_obj_set_scrollable(s_error_overlay, false);
    lv_obj_set_clickable(s_error_overlay, true);

    lv_obj_t *card = radio_ui_lvgl_obj_create(s_error_overlay);
    if (card == NULL) goto no_memory;
    lv_obj_set_size(card, 220, 152);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(card, 245, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COLOR_ERROR), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_scrollable(card, false);
    lv_obj_t *title = label_create(card, "NO SE PUDO COMPLETAR", COLOR_ERROR,
                                   14, 10, 192, 22, 14);
    if (title == NULL) goto no_memory;
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    s_error_message = label_create(card, "", COLOR_TEXT, 14, 36, 192, 56, 12);
    if (s_error_message == NULL) goto no_memory;
    lv_label_set_long_mode(s_error_message, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_error_message, LV_TEXT_ALIGN_CENTER, 0);
    if (button_create(card, "Ver radios", error_popup_open_radios_event, NULL,
                      12, 98, 92, 44, COLOR_CARD_2, COLOR_TEXT, 12) == NULL ||
        button_create(card, "Cerrar", error_popup_dismiss_event, NULL,
                      116, 98, 92, 44, COLOR_MINT, COLOR_BG, 12) == NULL) {
        goto no_memory;
    }
    error_popup_hide();
    return ESP_OK;

no_memory:
    error_popup_discard();
    ESP_LOGE(TAG, "LVGL sin memoria para crear el aviso de error");
    return ESP_ERR_NO_MEM;
}

static void favorite_update_home(void)
{
    if (s_favorite_button == NULL) return;
    s_home_favorite_active = s_has_current_station &&
        radio_station_store_is_favorite(&s_current_station);
    lv_obj_invalidate(s_favorite_button);
}

static bool heart_pixel(int x, int y)
{
    static const char *const rows[] = {
        "    ####    ####    ",
        "  #######  #######  ",
        " ########  ######## ",
        "####################",
        "####################",
        "####################",
        " ################## ",
        "  ################  ",
        "   ##############   ",
        "    ############    ",
        "     ##########     ",
        "      ########      ",
        "       ######       ",
        "        ####        ",
        "         ##         ",
    };
    return x >= 0 && x < 20 && y >= 0 && y < 15 && rows[y][x] == '#';
}

static station_row_context_t *station_row_context_acquire(lv_obj_t *row, size_t index)
{
    for (size_t i = 0; i < UI_STATION_LIMIT; ++i) {
        station_row_context_t *context = &s_station_row_contexts[i];
        if (!context->in_use) {
            context->row = row;
            context->station_index = index;
            context->in_use = true;
            return context;
        }
    }
    return NULL;
}

static void station_row_context_release(station_row_context_t *context)
{
    if (context != NULL) memset(context, 0, sizeof(*context));
}

/* Row callbacks carry the slot index into s_station_row_contexts rather than a
   raw pointer. The per-view handles are copied by value when a staged view is
   published (theme rebuild), so a pointer captured while building the staged
   view would dangle once the staging array is cleared. An index stays valid
   across that copy because the context data moves with it. */
static station_row_context_t *station_row_context_from_event(lv_event_t *event)
{
    const size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    if (index >= UI_STATION_LIMIT) return NULL;
    station_row_context_t *context = &s_station_row_contexts[index];
    return context->in_use && context->row != NULL &&
                   context->station_index < s_station_count
               ? context
               : NULL;
}

static bool catalog_scroll_anchor_capture(size_t station_index, int32_t *screen_y)
{
    if (screen_y == NULL || s_radio_list == NULL) return false;
    lv_obj_update_layout(s_radio_list);
    for (size_t i = 0; i < UI_STATION_LIMIT; ++i) {
        const station_row_context_t *context = &s_station_row_contexts[i];
        if (context->in_use && context->row != NULL &&
            context->station_index == station_index) {
            lv_area_t coordinates;
            lv_obj_get_coords(context->row, &coordinates);
            *screen_y = coordinates.y1;
            return true;
        }
    }
    return false;
}

static void catalog_scroll_anchor_restore(size_t station_index, int32_t screen_y)
{
    if (s_radio_list == NULL) return;
    lv_obj_update_layout(s_radio_list);
    for (size_t i = 0; i < UI_STATION_LIMIT; ++i) {
        const station_row_context_t *context = &s_station_row_contexts[i];
        if (context->in_use && context->row != NULL &&
            context->station_index == station_index) {
            lv_area_t coordinates;
            lv_obj_get_coords(context->row, &coordinates);
            int32_t target_scroll = lv_obj_get_scroll_y(s_radio_list) +
                                    coordinates.y1 - screen_y;
            if (target_scroll < 0) target_scroll = 0;
            lv_obj_scroll_to_y(s_radio_list, target_scroll, LV_ANIM_OFF);
            return;
        }
    }
}

static void heart_draw_event(lv_event_t *event)
{
    lv_obj_t *button = lv_event_get_target(event);
    const int scale = button == s_favorite_button ? 2 : 1;
    bool active = s_home_favorite_active;
    if (button != s_favorite_button) {
        station_row_context_t *context = station_row_context_from_event(event);
        active = context != NULL &&
                 radio_station_store_is_favorite(&s_stations[context->station_index]);
    }
    lv_draw_fill_dsc_t fill;
    lv_draw_fill_dsc_init(&fill);
    fill.color = lv_color_hex(active ? COLOR_GOLD : COLOR_MUTED);
    fill.opa = LV_OPA_COVER;
    lv_area_t area;
    lv_obj_get_coords(button, &area);
    const int32_t origin_x = area.x1 + (lv_obj_get_width(button) - 20 * scale) / 2;
    const int32_t origin_y = area.y1 + (lv_obj_get_height(button) - 15 * scale) / 2;
    lv_layer_t *layer = lv_event_get_layer(event);
    for (int y = 0; y < 15; ++y) {
        int run_start = -1;
        for (int x = 0; x <= 20; ++x) {
            const bool inside = x < 20 && heart_pixel(x, y);
            const bool visible = inside && (active || !heart_pixel(x - 1, y) ||
                !heart_pixel(x + 1, y) || !heart_pixel(x, y - 1) ||
                !heart_pixel(x, y + 1));
            if (visible && run_start < 0) run_start = x;
            if (!visible && run_start >= 0) {
                const lv_area_t pixels = {
                    .x1 = origin_x + run_start * scale,
                    .x2 = origin_x + x * scale - 1,
                    .y1 = origin_y + y * scale,
                    .y2 = origin_y + (y + 1) * scale - 1,
                };
                lv_draw_fill(layer, &fill, &pixels);
                run_start = -1;
            }
        }
    }
}

static esp_err_t favorite_toggle_request(const radio_station_t *station)
{
    if (station == NULL) return ESP_ERR_INVALID_ARG;
    /* The callback carries only its token; it never retains this UI station. */
    const uint32_t generation =
        radio_generation_next(s_favorite_mutation_generation);
    const esp_err_t err = radio_station_store_toggle_favorite_async(
        station, favorite_mutation_complete, (void *)(uintptr_t)generation);
    if (err == ESP_OK) s_favorite_mutation_generation = generation;
    return err;
}

static void home_favorite_event(lv_event_t *event)
{
    (void)event;
    if (!s_has_current_station) return;
    const esp_err_t err = favorite_toggle_request(&s_current_station);
    if (err != ESP_OK) {
        error_popup_show(err == ESP_ERR_NO_MEM
                              ? "No se pudo guardar: se alcanzó el límite de 8 favoritas o falta memoria"
                              : "No se pudo actualizar la lista de favoritas");
    }
}

static void play_pause_event(lv_event_t *event)
{
    (void)event;
    if (!s_has_current_station) {
        lv_screen_load(s_screens[PAGE_RADIOS]);
        return;
    }
    if (s_player_state == RADIO_PLAYER_STATE_CONNECTING ||
        s_player_state == RADIO_PLAYER_STATE_RECOVERING) {
        radio_player_stop();
    } else {
        radio_player_toggle();
    }
}

static void stop_event(lv_event_t *event)
{
    (void)event;
    radio_player_stop();
}

static void next_previous_event(lv_event_t *event)
{
    const int direction = (int)(intptr_t)lv_event_get_user_data(event);
    radio_station_t stations[RADIO_FAVORITES_MAX];
    size_t count = radio_station_store_favorites(stations, RADIO_FAVORITES_MAX);
    bool current_in_favorites = false;
    for (size_t i = 0; i < count; ++i) {
        if (s_has_current_station && strcmp(s_current_station.uuid, stations[i].uuid) == 0)
            current_in_favorites = true;
    }
    const bool use_recents = !current_in_favorites;
    if (use_recents) count = radio_station_store_recents(stations, RADIO_FAVORITES_MAX);
    if (count == 0) return;
    size_t current_index = 0;
    for (size_t i = 0; i < count; ++i) {
        if (s_has_current_station && strcmp(s_current_station.uuid, stations[i].uuid) == 0) {
            current_index = i;
            break;
        }
    }
    const int step = use_recents ? -direction : direction;
    const int next = (int)current_index + step;
    if (next < 0 || next >= (int)count) return;
    radio_station_t station = stations[next];
    if (radio_station_store_set_current(&station) == ESP_OK) {
        s_current_station = station;
        s_has_current_station = true;
        update_home_station();
    }
    const esp_err_t err = radio_player_play(&station);
    if (err != ESP_OK) {
        const char *message = err == ESP_ERR_INVALID_STATE
                                  ? "Revisá la conexión Wi-Fi"
                                  : err == ESP_ERR_NO_MEM
                                        ? "No hay memoria para iniciar la radio"
                                        : "No se pudo iniciar la radio";
        error_popup_show(message);
    }
}

static void timer_update_label(void)
{
    if (s_timer_button_label == NULL) return;
    const radio_ui_idle_snapshot_t idle = idle_state_snapshot();
    if (idle.sleep_remaining_seconds == 0) {
        if (idle.sleep_preset_minutes == 0)
            radio_ui_text_set_label(s_timer_button_label, "Temporizador de radio");
        else {
            char message[36];
            snprintf(message, sizeof(message), "Radio en %d min", idle.sleep_preset_minutes);
            radio_ui_text_set_label(s_timer_button_label, message);
        }
    } else {
        char message[48];
        const uint32_t minutes = (idle.sleep_remaining_seconds + 59) / 60;
        snprintf(message, sizeof(message), "Radio en %lu min",
                 (unsigned long)minutes);
        radio_ui_text_set_label(s_timer_button_label, message);
    }
}

static void sleep_timer_task(void *arg)
{
    (void)arg;
    for (;;) {
        const radio_ui_idle_snapshot_t idle = idle_state_snapshot();
        const bool watch_for_screen = idle.screen_timeout_seconds > 0 &&
            idle.display != RADIO_UI_IDLE_DISPLAY_BACKLIGHT_OFF;
        if (idle.sleep_remaining_seconds == 0 && !watch_for_screen) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        /* A notification wakes the task to re-read state (new timer, timeout or
           activity). The countdown is measured from elapsed ticks, so waking
           early must still advance it rather than skip a step. */
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        portENTER_CRITICAL(&s_idle_state_mux);
        const uint32_t now_tick = (uint32_t)xTaskGetTickCount();
        const uint32_t events = radio_ui_idle_advance(
            &s_idle_state, now_tick, configTICK_RATE_HZ,
            pdMS_TO_TICKS(SCREEN_SAVER_VISIBLE_MS));
        portEXIT_CRITICAL(&s_idle_state_mux);
        if ((events & RADIO_UI_IDLE_EVENT_SLEEP_EXPIRED) != 0) {
            /* Stop the player outside both the state mux and LVGL context. */
            radio_player_stop();
        }
    }
}

static void set_sleep_timer(int minutes)
{
    if (minutes < 0 || minutes > 60) return;
    (void)radio_station_store_set_sleep_preset(minutes);
    portENTER_CRITICAL(&s_idle_state_mux);
    const uint32_t now_tick = (uint32_t)xTaskGetTickCount();
    radio_ui_idle_set_sleep_timer(&s_idle_state, now_tick, minutes);
    portEXIT_CRITICAL(&s_idle_state_mux);
    if (s_sleep_task_handle != NULL) xTaskNotifyGive(s_sleep_task_handle);
    timer_update_label();
}

static void update_screen_choice_labels(void)
{
    const radio_ui_idle_snapshot_t idle = idle_state_snapshot();
    if (s_screen_button_label != NULL) {
        radio_ui_text_set_label(s_screen_button_label, "Pantalla y reloj");
    }
    if (s_screen_brightness_choice_label != NULL) {
        char message[36];
        snprintf(message, sizeof(message), "Brillo · %d%%", idle.screen_brightness);
        radio_ui_text_set_label(s_screen_brightness_choice_label, message);
    }
    if (s_screen_timeout_choice_label != NULL) {
        char message[40];
        if (idle.screen_timeout_seconds == 0) {
            snprintf(message, sizeof(message), "Reposo · Siempre activa");
        } else {
            snprintf(message, sizeof(message), "Reposo · %d s",
                     idle.screen_timeout_seconds);
        }
        radio_ui_text_set_label(s_screen_timeout_choice_label, message);
    }
    if (s_screensaver_choice_label != NULL) {
        static const char *const styles[] = {
            "Reloj · Digital",
            "Reloj · Analógico",
            "Mascota · Gato ONDA",
        };
        radio_ui_text_set_label(s_screensaver_choice_label,
                          styles[idle.screensaver_style >= 0 &&
                                         idle.screensaver_style < 3
                                     ? idle.screensaver_style : 0]);
    }
}

static void ui_theme_apply_values(void)
{
    s_theme_id = radio_ui_theme_normalize_id(s_theme_id);
    s_accent_id = radio_ui_accent_normalize_id(s_accent_id);
    s_theme = radio_ui_theme_palette(s_theme_id, s_accent_id);
    s_requested_theme_id = s_theme_id;
    s_requested_accent_id = s_accent_id;
}

static void ui_theme_rebuild_async(void *unused)
{
    (void)unused;
    /* No choice is captured: coalesced work rebuilds the latest theme values. */
    s_theme_rebuild_pending = false;
    const esp_err_t err = ui_theme_rebuild(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo reconstruir la interfaz con el nuevo tema: %s",
                 esp_err_to_name(err));
    }
}

static void ui_theme_rebuild_deferred(void)
{
    if (s_theme_rebuild_pending) return;
    s_theme_rebuild_pending = true;
    if (lv_async_call(ui_theme_rebuild_async, NULL) != LV_RESULT_OK) {
        s_theme_rebuild_pending = false;
        const esp_err_t err = ui_theme_rebuild(true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "No se pudo reconstruir la interfaz: %s",
                     esp_err_to_name(err));
        }
    }
}

static void theme_choice_event(lv_event_t *event)
{
    const int theme = (int)(intptr_t)lv_event_get_user_data(event);
    if (theme < 0 || theme >= RADIO_UI_THEME_COUNT) return;
    s_requested_theme_id = theme;
    ui_theme_rebuild_deferred();
}

static void accent_choice_event(lv_event_t *event)
{
    const int accent = (int)(intptr_t)lv_event_get_user_data(event);
    if (accent < 0 || accent >= RADIO_UI_ACCENT_COUNT) return;
    s_requested_accent_id = accent;
    ui_theme_rebuild_deferred();
}

static void show_appearance_settings(lv_event_t *event)
{
    (void)event;
    lv_obj_t *card = modal_create("APARIENCIA", 232);
    if (card == NULL) return;
    for (int i = 0; i < RADIO_UI_THEME_COUNT; ++i) {
        const bool selected = i == s_theme_id;
        lv_obj_t *choice = button_create(
            card, radio_ui_theme_name(i), theme_choice_event, (void *)(intptr_t)i,
            10, 34 + i * 44, 196, 42,
            selected ? COLOR_MINT : COLOR_CARD_2,
            selected ? radio_ui_theme_text_on_accent(s_theme_id) : COLOR_TEXT, 12);
        if (!modal_require_object(choice)) return;
        lv_obj_set_style_radius(choice, 10, 0);
    }
    if (!modal_require_object(label_create(card, "Color de acento", COLOR_MUTED,
                                           14, 166, 92, 14, 12))) return;
    for (int i = 0; i < RADIO_UI_ACCENT_COUNT; ++i) {
        lv_obj_t *choice = button_create(
            card, radio_ui_accent_name(i), accent_choice_event, (void *)(intptr_t)i,
            12 + i * 66, 184, 60, 44,
            radio_ui_theme_accent_color(s_theme_id, i),
            radio_ui_theme_text_on_accent(s_theme_id), 12);
        if (!modal_require_object(choice)) return;
        lv_obj_set_style_border_width(choice, i == s_accent_id ? 2 : 0, 0);
        lv_obj_set_style_border_color(choice, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_set_style_radius(choice, 9, 0);
    }
}

static void screen_brightness_event(lv_event_t *event)
{
    (void)event;
    const int current = idle_state_snapshot().screen_brightness;
    const int brightness = current == 35 ? 70 : current == 70 ? 100 : 35;
    (void)radio_station_store_set_screen_brightness(brightness);
    portENTER_CRITICAL(&s_idle_state_mux);
    radio_ui_idle_set_brightness(&s_idle_state, brightness);
    portEXIT_CRITICAL(&s_idle_state_mux);
    idle_apply_pending();
    update_screen_choice_labels();
}

static void screen_timeout_event(lv_event_t *event)
{
    (void)event;
    const int current = idle_state_snapshot().screen_timeout_seconds;
    const int timeout_seconds = current == 0 ? 5 : current == 5 ? 15 :
                                current == 15 ? 30 : 0;
    (void)radio_station_store_set_screen_timeout_seconds(timeout_seconds);
    const uint32_t now_tick = (uint32_t)xTaskGetTickCount();
    portENTER_CRITICAL(&s_idle_state_mux);
    radio_ui_idle_set_timeout(&s_idle_state, now_tick, timeout_seconds);
    portEXIT_CRITICAL(&s_idle_state_mux);
    if (s_sleep_task_handle != NULL) xTaskNotifyGive(s_sleep_task_handle);
    idle_apply_pending();
    update_screen_choice_labels();
}

static void screensaver_style_event(lv_event_t *event)
{
    (void)event;
    const int style = (idle_state_snapshot().screensaver_style + 1) % 3;
    portENTER_CRITICAL(&s_idle_state_mux);
    radio_ui_idle_set_screensaver_style(&s_idle_state, style);
    portEXIT_CRITICAL(&s_idle_state_mux);
    (void)radio_station_store_set_ui_appearance(s_theme_id, s_accent_id,
                                                style);
    idle_apply_pending();
    update_screen_choice_labels();
}

static void screen_clock_settings_event(lv_event_t *event)
{
    (void)event;
    close_overlay_event(NULL);
    show_clock_settings(NULL);
}

static void show_screen_settings(lv_event_t *event)
{
    (void)event;
    lv_obj_t *card = modal_create("PANTALLA Y RELOJ", 226);
    if (card == NULL) return;
    lv_obj_t *brightness = button_create(card, "", screen_brightness_event, NULL,
                                         10, 38, 196, 44, COLOR_CARD_2, COLOR_TEXT, 14);
    if (!modal_require_object(brightness)) return;
    s_screen_brightness_choice_label = brightness != NULL
                                           ? lv_obj_get_child(brightness, 0) : NULL;
    if (!modal_require_object(s_screen_brightness_choice_label)) return;
    lv_obj_set_style_text_font(s_screen_brightness_choice_label,
                               radio_ui_text_font_for_size(14), 0);
    lv_obj_t *timeout = button_create(card, "", screen_timeout_event, NULL,
                                       10, 86, 196, 44, COLOR_CARD_2, COLOR_TEXT, 14);
    if (!modal_require_object(timeout)) return;
    s_screen_timeout_choice_label = timeout != NULL
                                        ? lv_obj_get_child(timeout, 0) : NULL;
    if (!modal_require_object(s_screen_timeout_choice_label)) return;
    lv_obj_set_style_text_font(s_screen_timeout_choice_label,
                               radio_ui_text_font_for_size(14), 0);
    lv_obj_t *screensaver = button_create(card, "", screensaver_style_event, NULL,
                                           10, 132, 196, 42, COLOR_CARD_2,
                                           COLOR_TEXT, 14);
    if (!modal_require_object(screensaver)) return;
    s_screensaver_choice_label = screensaver != NULL
                                     ? lv_obj_get_child(screensaver, 0) : NULL;
    if (!modal_require_object(s_screensaver_choice_label)) return;
    lv_obj_set_style_text_font(s_screensaver_choice_label,
                               radio_ui_text_font_for_size(12), 0);
    if (!modal_require_object(button_create(
            card, "Ajustar hora", screen_clock_settings_event, NULL,
            10, 178, 94, 44, COLOR_CARD_2, COLOR_TEXT, 12)) ||
        !modal_require_object(button_create(
            card, "Listo", close_overlay_event, NULL,
            112, 178, 94, 44, COLOR_MINT, COLOR_BG, 12))) return;
    update_screen_choice_labels();
}

enum {
    CLOCK_HOUR_DOWN = -2,
    CLOCK_HOUR_UP = -1,
    CLOCK_MINUTE_DOWN = 1,
    CLOCK_MINUTE_UP = 2,
};

static void clock_settings_preview_update(void)
{
    if (s_clock_settings_value_label == NULL) return;
    char value[8];
    snprintf(value, sizeof(value), "%02u:%02u",
             (unsigned)s_clock_edit_hour, (unsigned)s_clock_edit_minute);
    radio_ui_text_set_label(s_clock_settings_value_label, value);
}

static void show_clock_settings(lv_event_t *event)
{
    (void)event;
    time_t now = time(NULL);
    struct tm local_time;
    if (now >= 1735689600 && localtime_r(&now, &local_time) != NULL) {
        s_clock_edit_hour = (uint8_t)local_time.tm_hour;
        s_clock_edit_minute = (uint8_t)local_time.tm_min;
    } else {
        s_clock_edit_hour = 12;
        s_clock_edit_minute = 0;
    }

    lv_obj_t *card = modal_create("AJUSTAR HORA", 226);
    if (card == NULL) return;
    s_clock_settings_value_label = label_create(card, "12:00", COLOR_TEXT,
                                                40, 34, 136, 36, 18);
    if (!modal_require_object(s_clock_settings_value_label)) return;
    lv_obj_set_style_text_align(s_clock_settings_value_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_clock_settings_value_label,
                               radio_ui_text_font_for_size(24), 0);

    if (!modal_require_object(button_create(
            card, "-", clock_adjust_event, (void *)(intptr_t)CLOCK_HOUR_DOWN,
            12, 72, 46, 44, COLOR_CARD_2, COLOR_TEXT, 18)) ||
        !modal_require_object(label_create(card, "HORA", COLOR_MUTED,
                                           65, 84, 86, 20, 12)) ||
        !modal_require_object(button_create(
            card, "+", clock_adjust_event, (void *)(intptr_t)CLOCK_HOUR_UP,
            158, 72, 46, 44, COLOR_CARD_2, COLOR_TEXT, 18)) ||
        !modal_require_object(button_create(
            card, "-", clock_adjust_event, (void *)(intptr_t)CLOCK_MINUTE_DOWN,
            12, 118, 46, 44, COLOR_CARD_2, COLOR_TEXT, 18)) ||
        !modal_require_object(label_create(card, "MINUTOS", COLOR_MUTED,
                                           65, 130, 86, 20, 12)) ||
        !modal_require_object(button_create(
            card, "+", clock_adjust_event, (void *)(intptr_t)CLOCK_MINUTE_UP,
            158, 118, 46, 44, COLOR_CARD_2, COLOR_TEXT, 18)) ||
        !modal_require_object(label_create(
            card, "La hora se sincroniza al conectar Wi-Fi",
            COLOR_MUTED, 12, 164, 192, 14, 12)) ||
        !modal_require_object(button_create(
            card, "Guardar", clock_save_event, NULL,
            12, 180, 92, 44, COLOR_MINT, COLOR_BG, 12)) ||
        !modal_require_object(button_create(
            card, "Cancelar", close_overlay_event, NULL,
            112, 180, 92, 44, COLOR_CARD_2, COLOR_TEXT, 12))) return;
    clock_settings_preview_update();
}

static void clock_adjust_event(lv_event_t *event)
{
    const int adjustment = (int)(intptr_t)lv_event_get_user_data(event);
    if (adjustment == CLOCK_HOUR_DOWN) {
        s_clock_edit_hour = (uint8_t)((s_clock_edit_hour + 23U) % 24U);
    } else if (adjustment == CLOCK_HOUR_UP) {
        s_clock_edit_hour = (uint8_t)((s_clock_edit_hour + 1U) % 24U);
    } else if (adjustment == CLOCK_MINUTE_DOWN) {
        s_clock_edit_minute = (uint8_t)((s_clock_edit_minute + 55U) % 60U);
    } else if (adjustment == CLOCK_MINUTE_UP) {
        s_clock_edit_minute = (uint8_t)((s_clock_edit_minute + 5U) % 60U);
    }
    clock_settings_preview_update();
}

static void clock_save_event(lv_event_t *event)
{
    (void)event;
    time_t now = time(NULL);
    struct tm local_time = {0};
    if (now >= 1735689600 && localtime_r(&now, &local_time) != NULL) {
        /* Keep the current calendar date when network time is available. */
    } else {
        local_time.tm_year = 126; /* 2026, fallback until SNTP connects. */
        local_time.tm_mon = 0;
        local_time.tm_mday = 1;
    }
    local_time.tm_hour = s_clock_edit_hour;
    local_time.tm_min = s_clock_edit_minute;
    local_time.tm_sec = 0;
    local_time.tm_isdst = -1;
    const time_t requested_time = mktime(&local_time);
    const struct timeval value = {.tv_sec = requested_time, .tv_usec = 0};
    if (requested_time == (time_t)-1 || settimeofday(&value, NULL) != 0) {
        radio_ui_text_set_label(s_settings_status_label, "No se pudo ajustar la hora");
        lv_obj_set_style_text_color(s_settings_status_label,
                                    lv_color_hex(COLOR_ERROR), 0);
    } else {
        radio_ui_text_set_label(s_settings_status_label,
                          "Hora ajustada; se sincronizará con Wi-Fi");
        lv_obj_set_style_text_color(s_settings_status_label,
                                    lv_color_hex(COLOR_MINT), 0);
        header_status_refresh();
    }
    close_overlay_event(NULL);
}

static void sleep_timer_choice_event(lv_event_t *event)
{
    const int minutes = (int)(intptr_t)lv_event_get_user_data(event);
    set_sleep_timer(minutes);
    close_overlay_event(event);
}

static void show_sleep_timer(lv_event_t *event)
{
    (void)event;
    lv_obj_t *card = modal_create("TEMPORIZADOR DE RADIO", 226);
    if (card == NULL) return;
    const int sleep_preset_minutes = idle_state_snapshot().sleep_preset_minutes;
    static const int choices[] = {0, 15, 30, 60};
    static const char *labels[] = {"Sin temporizador", "Detener en 15 min",
                                   "Detener en 30 min", "Detener en 60 min"};
    for (size_t i = 0; i < 4; ++i) {
        if (!modal_require_object(button_create(
                card, labels[i], sleep_timer_choice_event,
                (void *)(intptr_t)choices[i], 10, 34 + (int32_t)i * 46,
                196, 44,
                choices[i] == sleep_preset_minutes ? COLOR_MINT : COLOR_CARD_2,
                choices[i] == sleep_preset_minutes ? COLOR_BG : COLOR_TEXT, 14))) {
            return;
        }
    }
}

static void update_wifi_settings_info(void)
{
    radio_wifi_info_t info = {0};
    if (radio_wifi_get_info(&info) != ESP_OK) {
        radio_ui_text_set_label(s_settings_info_label, "Wi-Fi no disponible");
        return;
    }
    wifi_status_snapshot_apply(&info);
}

static void ui_sensitive_clear(void *memory, size_t length)
{
    volatile uint8_t *bytes = (volatile uint8_t *)memory;
    while (length-- > 0U) *bytes++ = 0U;
}

static void portal_qr_toggle_event(lv_event_t *event)
{
    (void)event;
    if (s_portal_qr_code == NULL || s_portal_qr_manual_label == NULL ||
        s_portal_qr_toggle_label == NULL) {
        return;
    }

    s_portal_qr_manual_visible = !s_portal_qr_manual_visible;
    lv_obj_set_hidden(s_portal_qr_code, s_portal_qr_manual_visible);
    lv_obj_set_hidden(s_portal_qr_hint, s_portal_qr_manual_visible);
    lv_obj_set_hidden(s_portal_qr_manual_label,
                      !s_portal_qr_manual_visible);
    radio_ui_text_set_label(s_portal_qr_toggle_label,
                            s_portal_qr_manual_visible
                                ? "Volver al QR" : "Ver clave");
}

static esp_err_t show_portal_qr(void)
{
    char ssid[33] = {0};
    char password[33] = {0};
    char payload[112] = {0};
    char manual_text[112] = {0};
    esp_err_t err = radio_wifi_get_portal_credentials(
        ssid, sizeof(ssid), password, sizeof(password));
    if (err != ESP_OK) {
        radio_ui_text_set_label(s_settings_status_label,
                                "No se pudieron leer las credenciales del portal");
        return err;
    }

    const int payload_length = snprintf(
        payload, sizeof(payload), "WIFI:T:WPA;S:%s;P:%s;;", ssid, password);
    if (payload_length < 0 || (size_t)payload_length >= sizeof(payload)) {
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }
    snprintf(manual_text, sizeof(manual_text),
             "Red: %s\nClave Wi-Fi:\n%.16s\n%.16s\nPortal: 192.168.4.1",
             ssid, password, password + 16);

    lv_obj_t *card = modal_create("CONECTÁ TU CELULAR", 232);
    if (card == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    s_portal_qr_code = lv_qrcode_create(card);
    if (s_portal_qr_code == NULL) {
        close_overlay_event(NULL);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    lv_qrcode_set_size(s_portal_qr_code, 132);
    lv_qrcode_set_dark_color(s_portal_qr_code, lv_color_black());
    lv_qrcode_set_light_color(s_portal_qr_code, lv_color_white());
    lv_qrcode_set_quiet_zone(s_portal_qr_code, true);
    lv_obj_set_pos(s_portal_qr_code, 42, 32);
    if (lv_qrcode_update(s_portal_qr_code, payload,
                         (uint32_t)payload_length) != LV_RESULT_OK) {
        close_overlay_event(NULL);
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    s_portal_qr_hint = label_create(
        card, "Escaneá el QR · portal 192.168.4.1",
        COLOR_TEXT, 8, 165, 200, 18, 12);
    if (s_portal_qr_hint == NULL) {
        close_overlay_event(NULL);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    s_portal_qr_manual_label = label_create(
        card, manual_text, COLOR_TEXT, 8, 42, 200, 124, 12);
    if (s_portal_qr_manual_label == NULL) {
        close_overlay_event(NULL);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    lv_obj_set_style_text_align(s_portal_qr_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(s_portal_qr_manual_label,
                                LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_hidden(s_portal_qr_manual_label, true);

    lv_obj_t *toggle = button_create(card, "Ver clave",
                                     portal_qr_toggle_event, NULL,
                                     8, 184, 96, 44,
                                     COLOR_CARD_2, COLOR_TEXT, 12);
    if (toggle == NULL) {
        close_overlay_event(NULL);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    s_portal_qr_toggle_label = lv_obj_get_child(toggle, 0);
    if (s_portal_qr_toggle_label == NULL) {
        close_overlay_event(NULL);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    if (button_create(card, "Cerrar", close_overlay_event, NULL,
                      112, 184, 96, 44,
                      COLOR_CARD, COLOR_TEXT, 12) == NULL) {
        close_overlay_event(NULL);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    s_portal_qr_manual_visible = false;
    err = ESP_OK;

cleanup:
    ui_sensitive_clear(ssid, sizeof(ssid));
    ui_sensitive_clear(password, sizeof(password));
    ui_sensitive_clear(payload, sizeof(payload));
    ui_sensitive_clear(manual_text, sizeof(manual_text));
    return err;
}

static void wifi_status_snapshot_apply(const radio_wifi_info_t *info)
{
    if (info == NULL) return;
    if (s_wifi_status_generation_valid &&
        info->status_generation != s_wifi_status_generation &&
        !radio_generation_is_newer(info->status_generation,
                                   s_wifi_status_generation)) {
        return;
    }
    if (!s_wifi_status_generation_valid ||
        radio_generation_is_newer(info->status_generation,
                                  s_wifi_status_generation)) {
        s_wifi_status_generation = info->status_generation;
        s_wifi_status_generation_valid = true;
    }

    s_wifi_connected = info->connected;
    s_wifi_provisioning = info->provisioning_active;
    if (!info->provisioning_active && s_portal_qr_code != NULL) {
        close_overlay_event(NULL);
    }
    if (info->connected) {
        radio_ui_text_set_label(s_home_wifi_label, "En línea");
        if (s_home_wifi_label != NULL)
            lv_obj_set_style_text_color(s_home_wifi_label,
                                        lv_color_hex(COLOR_MINT), 0);
        radio_ui_text_set_label(s_settings_status_label, "Wi-Fi conectado");
        if (s_settings_status_label != NULL)
            lv_obj_set_style_text_color(s_settings_status_label,
                                        lv_color_hex(COLOR_MINT), 0);
    } else if (info->provisioning_active) {
        radio_ui_text_set_label(s_home_wifi_label, "Portal");
        if (s_home_wifi_label != NULL)
            lv_obj_set_style_text_color(s_home_wifi_label,
                                        lv_color_hex(COLOR_GOLD), 0);
        radio_ui_text_set_label(s_settings_status_label, "Portal ONDA activo");
        if (s_settings_status_label != NULL)
            lv_obj_set_style_text_color(s_settings_status_label,
                                        lv_color_hex(COLOR_GOLD), 0);
    } else {
        radio_ui_text_set_label(s_home_wifi_label, "Sin Wi-Fi");
        if (s_home_wifi_label != NULL)
            lv_obj_set_style_text_color(s_home_wifi_label,
                                        lv_color_hex(COLOR_MUTED), 0);
        radio_ui_text_set_label(s_settings_status_label,
                                "Sin conexión · reintentando");
        if (s_settings_status_label != NULL)
            lv_obj_set_style_text_color(s_settings_status_label,
                                        lv_color_hex(COLOR_MUTED), 0);
    }

    char details[140];
    if (info->provisioning_active) {
        snprintf(details, sizeof(details),
                  "Portal: %.22s\nTocá Wi-Fi y portal para ver el QR",
                 info->provisioning_ssid);
    } else if (info->connected) {
        snprintf(details, sizeof(details),
                  "Red: %.22s\nTocá Wi-Fi y portal para buscar radios",
                 info->ssid);
    } else {
        snprintf(details, sizeof(details),
                  "Guardada: %.22s\nTocá Wi-Fi y portal para cambiarla",
                 info->ssid[0] != '\0' ? info->ssid : "ninguna");
    }
    radio_ui_text_set_label(s_settings_info_label, details);
    if (s_settings_info_label != NULL) {
        lv_obj_set_size(s_settings_info_label, 216, SETTINGS_INFO_HEIGHT);
    }
    if (s_settings_list != NULL) {
        lv_obj_set_pos(s_settings_list, 8, SETTINGS_LIST_Y);
        lv_obj_set_size(s_settings_list, 224, SETTINGS_LIST_HEIGHT);
    }
    home_playback_status_update();
    update_home_station();
}

static void wifi_status_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    uint8_t event;
    if (s_wifi_status_queue != NULL &&
        xQueueReceive(s_wifi_status_queue, &event, 0) == pdTRUE) {
        radio_wifi_info_t current = {0};
        /* A late publisher may replace a newer notification; read the
           authoritative snapshot rather than applying the queued copy. */
        if (radio_wifi_get_info(&current) == ESP_OK) {
            wifi_status_snapshot_apply(&current);
        }
    }
    esp_err_t portal_result;
    if (s_wifi_portal_result_queue != NULL &&
        xQueueReceive(s_wifi_portal_result_queue, &portal_result, 0) == pdTRUE) {
        if (portal_result != ESP_OK) {
            radio_ui_text_set_label(s_settings_status_label,
                                    "No se pudo iniciar el portal Wi-Fi");
            if (s_settings_status_label != NULL) {
                lv_obj_set_style_text_color(s_settings_status_label,
                                            lv_color_hex(COLOR_ERROR), 0);
            }
            ESP_LOGE(TAG, "No se pudo iniciar el portal Wi-Fi: %s",
                     esp_err_to_name(portal_result));
        }
    }

    portENTER_CRITICAL(&s_portal_start_mux);
    const bool show_qr = s_portal_qr_pending;
    s_portal_qr_pending = false;
    portEXIT_CRITICAL(&s_portal_start_mux);
    if (show_qr) {
        radio_wifi_info_t info = {0};
        if (radio_wifi_get_info(&info) == ESP_OK &&
            info.provisioning_active) {
            wifi_status_snapshot_apply(&info);
            if (lv_screen_active() == s_screens[PAGE_SETTINGS]) {
                (void)show_portal_qr();
            }
        }
    }
}

static void wifi_portal_start_task(void *arg)
{
    (void)arg;
    const esp_err_t result = radio_wifi_start_provisioning();
    portENTER_CRITICAL(&s_portal_start_mux);
    s_portal_start_pending = false;
    s_portal_qr_pending = result == ESP_OK;
    portEXIT_CRITICAL(&s_portal_start_mux);
    if (s_wifi_portal_result_queue != NULL && s_wifi_status_timer != NULL) {
        (void)RADIO_TEST_QUEUE_OVERWRITE(s_wifi_portal_result_queue, &result);
    } else if (result != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo iniciar el portal Wi-Fi: %s",
                 esp_err_to_name(result));
    }
    vTaskDelete(NULL);
}

static void start_wifi_portal(lv_event_t *event)
{
    (void)event;
    portENTER_CRITICAL(&s_portal_start_mux);
    if (s_portal_start_pending) {
        portEXIT_CRITICAL(&s_portal_start_mux);
        return;
    }
    s_portal_start_pending = true;
    s_portal_qr_pending = false;
    portEXIT_CRITICAL(&s_portal_start_mux);
    radio_ui_text_set_label(s_settings_status_label, "Iniciando portal Wi-Fi…");
    if (xTaskCreate(wifi_portal_start_task, "wifi_portal_start", 5120, NULL,
                    4, NULL) == pdPASS) {
        lv_obj_set_style_text_color(s_settings_status_label,
                                    lv_color_hex(COLOR_GOLD), 0);
    } else {
        portENTER_CRITICAL(&s_portal_start_mux);
        s_portal_start_pending = false;
        s_portal_qr_pending = false;
        portEXIT_CRITICAL(&s_portal_start_mux);
        radio_ui_text_set_label(s_settings_status_label, "No se pudo iniciar el portal Wi-Fi");
        lv_obj_set_style_text_color(s_settings_status_label, lv_color_hex(COLOR_ERROR), 0);
    }
}

static void power_off_task(void *arg)
{
    (void)arg;
    radio_player_stop();
    vTaskDelay(pdMS_TO_TICKS(300));
    const esp_err_t flush_err = radio_station_store_flush();
    if (flush_err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo vaciar NVS antes del apagado: %s",
                 esp_err_to_name(flush_err));
        portENTER_CRITICAL(&s_power_off_mux);
        s_power_off_pending = false;
        s_power_off_launch_failed = true;
        portEXIT_CRITICAL(&s_power_off_mux);
        vTaskDelete(NULL);
        return;
    }
    radio_board_power_off();
    vTaskDelete(NULL);
}

void radio_ui_request_power_off(void)
{
    portENTER_CRITICAL(&s_power_off_mux);
    if (s_power_off_pending) {
        portEXIT_CRITICAL(&s_power_off_mux);
        return;
    }
    s_power_off_pending = true;
    portEXIT_CRITICAL(&s_power_off_mux);
    if (xTaskCreate(power_off_task, "radio_power_off", 4096, NULL, 6, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_power_off_mux);
        s_power_off_pending = false;
        s_power_off_launch_failed = true;
        portEXIT_CRITICAL(&s_power_off_mux);
        ESP_LOGE(TAG, "No se pudo iniciar la tarea de apagado; no se corta energía");
    }
}

static void power_off_requested(lv_event_t *event)
{
    (void)event;
    radio_ui_text_set_label(s_settings_status_label, "Apagando…");
    radio_ui_request_power_off();
}

static void catalog_status(const char *message, bool error)
{
    if (s_radio_status_label == NULL || s_radio_list == NULL) return;
    radio_ui_text_set_label(s_radio_status_label, message);
    if (error && message != NULL && message[0] != '\0') error_popup_show(message);
    lv_obj_set_style_text_color(s_radio_status_label,
                                lv_color_hex(error ? COLOR_ERROR : COLOR_MUTED), 0);
    if (lv_obj_get_child_count(s_radio_list) == 0)
        lv_obj_set_hidden(s_radio_status_label, false);
    else
        lv_obj_set_hidden(s_radio_status_label, true);
}

static void catalog_result_queue_drain(void)
{
    if (s_catalog_result_queue == NULL) return;

    catalog_ui_result_t *queued = NULL;
    while (xQueueReceive(s_catalog_result_queue, &queued, 0) == pdTRUE) {
        catalog_ui_result_free(queued);
        queued = NULL;
    }
}

static void catalog_clear_results_state(void)
{
    catalog_clear_widgets();
    s_station_count = 0;
    s_catalog_oldest_offset = 0;
    s_next_catalog_offset = 0;
    s_catalog_page_offsets[0] = 0;
    s_catalog_page_offsets[1] = 0;
    s_catalog_page_counts[0] = 0;
    s_catalog_page_counts[1] = 0;
    s_catalog_page_count = 0;
    s_catalog_retry_offset = 0;
    s_catalog_has_more = false;
    s_catalog_scroll_anchor_pending = false;
}

static void catalog_clear_results(void)
{
    s_catalog_request_id = radio_generation_next(s_catalog_request_id);

    if (s_catalog_render_pending != NULL) {
        catalog_ui_result_free(s_catalog_render_pending);
        s_catalog_render_pending = NULL;
    }
    catalog_result_queue_drain();
    catalog_clear_results_state();
}

static esp_err_t catalog_previous_button_create(void)
{
    if (s_load_previous_button != NULL || s_catalog_page_count == 0 ||
        s_catalog_page_offsets[0] == 0) {
        return ESP_OK;
    }
    s_load_previous_button = button_create(s_radio_list, "Cargar 20 anteriores",
                                           load_previous_stations, NULL, 0, 0,
                                           204, 48, COLOR_CARD_2,
                                           COLOR_TEXT, 14);
    if (s_load_previous_button == NULL) return ESP_ERR_NO_MEM;
    lv_obj_set_width(s_load_previous_button, lv_pct(100));
    lv_obj_move_to_index(s_load_previous_button, 0);
    return ESP_OK;
}

static esp_err_t catalog_show_failure(esp_err_t result)
{
    if (s_radio_list == NULL) return ESP_ERR_INVALID_STATE;
    const char *message = result == ESP_ERR_INVALID_STATE ? "Revisá la conexión Wi-Fi" :
                          result == ESP_ERR_INVALID_SIZE ? "Respuesta demasiado grande" :
                          result == ESP_ERR_INVALID_RESPONSE ? "Respuesta inválida del catálogo" :
                          result == ESP_ERR_NO_MEM ? "Memoria insuficiente" :
                          "El catálogo no responde";
    error_popup_show(message);
    lv_obj_t *failure_label = NULL;
    if (s_station_count == 0) {
        failure_label = label_create(s_radio_list, message, COLOR_ERROR,
                                     0, 0, 208, 36, 14);
        if (failure_label == NULL) return ESP_ERR_NO_MEM;
    }
    if (s_load_more_button == NULL) {
        lv_obj_t *retry_button = button_create(
            s_radio_list, "Reintentar", retry_catalog_request, NULL, 0, 0,
            204, 48, COLOR_CARD_2, COLOR_TEXT, 14);
        if (retry_button == NULL) {
            if (failure_label != NULL) lv_obj_delete(failure_label);
            return ESP_ERR_NO_MEM;
        }
        s_load_more_button = retry_button;
        lv_obj_set_width(retry_button, lv_pct(100));
    } else {
        lv_obj_clear_state(s_load_more_button, LV_STATE_DISABLED);
        radio_ui_text_set_label(lv_obj_get_child(s_load_more_button, 0), "Reintentar");
        lv_obj_remove_event_cb(s_load_more_button, load_more_stations);
        lv_obj_remove_event_cb(s_load_more_button, retry_catalog_request);
        lv_obj_add_event_cb(s_load_more_button, retry_catalog_request,
                            LV_EVENT_CLICKED, NULL);
    }
    if (s_load_previous_button != NULL) {
        lv_obj_clear_state(s_load_previous_button, LV_STATE_DISABLED);
        radio_ui_text_set_label(lv_obj_get_child(s_load_previous_button, 0),
                          "Cargar 20 anteriores");
    }
    catalog_status("", false);
    return ESP_OK;
}

#ifdef RADIO_UI_TEST_FAULT_INJECTION
static bool s_test_fail_radio_refresh;
static bool s_test_fail_idle_timer;
static bool s_test_fail_idle_task;
static bool s_test_fail_player_timer;
static bool s_test_fail_wifi_timer;
static bool s_test_fail_favorite_timer;
static bool s_test_fail_catalog_timer;
static bool s_test_fail_volume_timer;
static bool s_test_fail_station_buffer;
static bool s_test_fail_header_timer;
#endif

static bool schedule_radio_view_refresh(void)
{
#ifdef RADIO_UI_TEST_FAULT_INJECTION
    const bool failed = s_test_fail_radio_refresh ||
                        lv_async_call(refresh_radio_view_async, NULL) != LV_RESULT_OK;
#else
    const bool failed = lv_async_call(refresh_radio_view_async, NULL) != LV_RESULT_OK;
#endif
    if (failed) {
        ESP_LOGE(TAG, "No se pudo programar la actualizacion de radios");
        error_popup_show("No se pudo actualizar la lista de radios");
        return false;
    }
    return true;
}

static void station_selected(lv_event_t *event)
{
    station_row_context_t *context = station_row_context_from_event(event);
    if (context == NULL) return;
    const radio_station_t station = s_stations[context->station_index];
    radio_station_store_set_current(&station);
    radio_station_store_add_recent(&station);
    s_current_station = station;
    s_has_current_station = true;
    update_home_station();
    lv_screen_load(s_screens[PAGE_LISTEN]);
    const esp_err_t err = radio_player_play(&station);
    if (err != ESP_OK) {
        const char *message = err == ESP_ERR_INVALID_STATE
                                  ? "Conectá Wi-Fi en Ajustes"
                                  : err == ESP_ERR_NO_MEM
                                        ? "No hay memoria para iniciar la radio"
                                        : "No se pudo iniciar la radio";
        error_popup_show(message);
    }
    if (s_radio_tab == RADIO_TAB_RECENTS) schedule_radio_view_refresh();
}

static void row_favorite_event(lv_event_t *event)
{
    station_row_context_t *context = station_row_context_from_event(event);
    if (context == NULL) return;
    const esp_err_t err =
        favorite_toggle_request(&s_stations[context->station_index]);
    if (err != ESP_OK) {
        error_popup_show(err == ESP_ERR_NO_MEM
                              ? "Límite de 8 favoritas alcanzado o memoria insuficiente"
                              : "No se pudo actualizar la lista de favoritas");
    }
}

static bool station_row_create(const radio_station_t *station, size_t index)
{
    if (station == NULL || s_radio_list == NULL) return false;
    lv_obj_t *row = radio_ui_lvgl_obj_create(s_radio_list);
    if (row == NULL) return false;
    station_row_context_t *context = station_row_context_acquire(row, index);
    if (context == NULL) {
        lv_obj_delete(row);
        return false;
    }
    void *const context_token =
        (void *)(uintptr_t)(size_t)(context - s_station_row_contexts);
    lv_obj_set_size(row, lv_pct(100), 50);
    lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    touch_feedback_apply(row);
    lv_obj_set_clickable(row, true);
    lv_obj_set_gesture_bubble(row, true);
    lv_obj_set_event_bubble(row, true);
    lv_obj_set_scrollable(row, false);
    lv_obj_add_event_cb(row, ui_activity_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(row, station_selected, LV_EVENT_CLICKED, context_token);

    const bool is_current = s_has_current_station && station->uuid[0] != '\0' &&
                            strcmp(s_current_station.uuid, station->uuid) == 0;
    char title[RADIO_STATION_NAME_MAX + 2];
    if (is_current) {
        snprintf(title, sizeof(title), "> %s", station->name);
    } else {
        snprintf(title, sizeof(title), "%s", station->name);
    }
    if (label_create(row, title, is_current ? COLOR_MINT : COLOR_TEXT,
                     10, 6, 160, 20, 14) == NULL) {
        station_row_context_release(context);
        lv_obj_delete(row);
        return false;
    }
    char details[72];
    snprintf(details, sizeof(details), "%.36s%s%.14s",
             station->country[0] ? station->country : "En línea",
             station->codec[0] ? " · " : "",
             station->codec);
    if (label_create(row, details, COLOR_MUTED, 10, 29, 160, 16, 12) == NULL) {
        station_row_context_release(context);
        lv_obj_delete(row);
        return false;
    }

    lv_obj_t *star = radio_ui_lvgl_button_create(row);
    if (star == NULL) {
        station_row_context_release(context);
        lv_obj_delete(row);
        return false;
    }
    lv_obj_set_size(star, 46, 46);
    lv_obj_align(star, LV_ALIGN_RIGHT_MID, -1, 0);
    lv_obj_set_style_bg_opa(star, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(star, 0, 0);
    lv_obj_set_style_radius(star, 0, 0);
    touch_feedback_apply(star);
    lv_obj_set_gesture_bubble(star, true);
    lv_obj_add_event_cb(star, ui_activity_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(star, row_favorite_event, LV_EVENT_CLICKED, context_token);
    lv_obj_add_event_cb(star, heart_draw_event, LV_EVENT_DRAW_MAIN_END, context_token);
    lv_obj_add_event_cb(star, page_swipe_event, LV_EVENT_PRESSED,
                        s_screens[PAGE_RADIOS]);
    lv_obj_add_event_cb(star, page_swipe_event, LV_EVENT_RELEASED,
                        s_screens[PAGE_RADIOS]);
    return true;
}

static void catalog_ui_result_free(catalog_ui_result_t *result)
{
    if (result == NULL) return;
    if (result->stations != NULL) heap_caps_free(result->stations);
    heap_caps_free(result);
}

static void catalog_remove_oldest_rows(size_t count)
{
    if (s_stations == NULL || count == 0 || s_station_count == 0) return;
    if (count > s_station_count) count = s_station_count;

    /* Free the corresponding LVGL rows as well as their station records. */
    for (size_t i = 0; i < UI_STATION_LIMIT; ++i) {
        station_row_context_t *context = &s_station_row_contexts[i];
        if (!context->in_use) continue;
        if (context->station_index < count) {
            lv_obj_t *row = context->row;
            station_row_context_release(context);
            if (row != NULL) lv_obj_delete(row);
        } else {
            context->station_index -= count;
        }
    }

    const size_t retained = s_station_count - count;
    if (retained > 0) {
        memmove(s_stations, &s_stations[count], retained * sizeof(s_stations[0]));
    }
    s_station_count = retained;
    ESP_LOGI(TAG, "Catalogo: liberadas %u filas antiguas; quedan %u",
             (unsigned)count, (unsigned)s_station_count);
}

static void catalog_clear_widgets(void)
{
    if (s_radio_list != NULL) lv_obj_clean(s_radio_list);
    memset(s_station_row_contexts, 0, sizeof(s_station_row_contexts));
    s_load_more_button = NULL;
    s_load_previous_button = NULL;
}

static void catalog_drop_oldest_page(void)
{
    if (s_catalog_page_count == 0) return;
    const size_t drop_count = s_catalog_page_counts[0];
    int32_t anchor_y;
    if (drop_count > 0 && catalog_scroll_anchor_capture(drop_count, &anchor_y)) {
        s_catalog_scroll_anchor_pending = true;
        s_catalog_scroll_anchor_index = 0;
        s_catalog_scroll_anchor_y = anchor_y;
    }
    catalog_remove_oldest_rows(drop_count);
    if (s_catalog_page_count > 1) {
        s_catalog_page_offsets[0] = s_catalog_page_offsets[1];
        s_catalog_page_counts[0] = s_catalog_page_counts[1];
    }
    s_catalog_page_offsets[1] = 0;
    s_catalog_page_counts[1] = 0;
    --s_catalog_page_count;
    s_catalog_oldest_offset = s_catalog_page_count > 0
                                  ? s_catalog_page_offsets[0]
                                  : s_next_catalog_offset;
    ESP_LOGI(TAG, "Catalogo: pagina antigua liberada; quedan %u paginas en ventana",
             (unsigned)s_catalog_page_count);
}

static void catalog_callback(const radio_station_t *stations, size_t count,
                             size_t offset, size_t next_offset, bool has_more,
                             uint32_t request_id, esp_err_t result)
{
    if (s_catalog_result_queue == NULL) {
        ESP_LOGE(TAG, "Cola de resultados del catalogo no disponible");
        return;
    }

    catalog_ui_result_t *ui_result = heap_caps_calloc(
        1, sizeof(*ui_result), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ui_result == NULL) {
        /* This small envelope may use internal RAM; station data never does. */
        ui_result = calloc(1, sizeof(*ui_result));
    }
    if (ui_result == NULL) {
        ESP_LOGE(TAG, "Sin memoria incluso para notificar el resultado del catalogo");
        return;
    }

    ui_result->count = count > RADIO_CATALOG_PAGE_SIZE ? RADIO_CATALOG_PAGE_SIZE : count;
    ui_result->offset = offset;
    ui_result->next_offset = next_offset;
    ui_result->has_more = has_more;
    ui_result->request_id = request_id;
    ui_result->result = result;
    if (result == ESP_OK && ui_result->count > 0) {
        ui_result->stations = heap_caps_malloc(
            ui_result->count * sizeof(ui_result->stations[0]),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ui_result->stations == NULL) {
            ui_result->count = 0;
            ui_result->result = ESP_ERR_NO_MEM;
        } else {
            memcpy(ui_result->stations, stations,
                   ui_result->count * sizeof(ui_result->stations[0]));
        }
    }

    /* One-slot mailbox: stale results are replaced and reclaimed immediately. */
    catalog_ui_result_t *stale = NULL;
    if (xQueueReceive(s_catalog_result_queue, &stale, 0) == pdTRUE) {
        catalog_ui_result_free(stale);
    }
    if (RADIO_TEST_QUEUE_SEND(s_catalog_result_queue, &ui_result, 0) != pdTRUE) {
        catalog_ui_result_free(ui_result);
        ESP_LOGW(TAG, "Cola del catalogo llena; se descarta una respuesta");
    }
}

static void catalog_result_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_catalog_render_pending == NULL &&
        xQueueReceive(s_catalog_result_queue, &s_catalog_render_pending, 0) != pdTRUE) {
        return;
    }
    catalog_ui_result_t *result = s_catalog_render_pending;
    if (result == NULL) return;

    /* Rendering spans several LVGL ticks, so the target view may change
       after preparation. Validate ownership before every batch. */
    if (!radio_generation_is_current(result->request_id,
                                     s_catalog_request_id) ||
        s_radio_tab != RADIO_TAB_EXPLORE || !s_showing_catalog_results) {
        catalog_ui_result_free(result);
        s_catalog_render_pending = NULL;
        return;
    }

    if (!result->prepared) {
        if (result->result != ESP_OK) {
            const esp_err_t show_err = catalog_show_failure(result->result);
            if (show_err != ESP_OK) {
                ESP_LOGE(TAG, "No se pudo construir el estado de error del catalogo: %s",
                         esp_err_to_name(show_err));
            }
            catalog_ui_result_free(result);
            s_catalog_render_pending = NULL;
            return;
        }
        if (s_stations == NULL || s_station_capacity == 0) {
            catalog_status("Memoria insuficiente para listar radios", true);
            catalog_ui_result_free(result);
            s_catalog_render_pending = NULL;
            return;
        }

        if (result->count > s_station_capacity) {
            catalog_status("La página supera el límite de memoria", true);
            catalog_ui_result_free(result);
            s_catalog_render_pending = NULL;
            return;
        }

        const bool previous_page = s_catalog_page_count > 0 &&
                                   result->offset < s_catalog_page_offsets[0];
        if (result->offset == 0 && !previous_page) {
            /* Replace the visible window without cancelling this result. */
            catalog_clear_results_state();
        }

        if (previous_page) {
            const size_t retained_offset = s_catalog_page_offsets[0];
            size_t retained_count = s_catalog_page_counts[0];
            if (retained_count > s_station_capacity - result->count) {
                retained_count = s_station_capacity - result->count;
            }

            int32_t anchor_y;
            if (retained_count > 0 &&
                catalog_scroll_anchor_capture(0, &anchor_y)) {
                s_catalog_scroll_anchor_pending = true;
                s_catalog_scroll_anchor_index = result->count;
                s_catalog_scroll_anchor_y = anchor_y;
            }

            /* Rebuild the bounded window as [new older page, prior oldest page]. */
            catalog_clear_widgets();
            if (retained_count > 0) {
                memmove(&s_stations[result->count], s_stations,
                        retained_count * sizeof(s_stations[0]));
            }
            if (result->count > 0) {
                memcpy(s_stations, result->stations,
                       result->count * sizeof(s_stations[0]));
            }
            s_station_count = result->count + retained_count;
            s_catalog_page_offsets[0] = result->offset;
            s_catalog_page_counts[0] = result->count;
            s_catalog_page_count = 1;
            if (retained_count > 0) {
                s_catalog_page_offsets[1] = retained_offset;
                s_catalog_page_counts[1] = retained_count;
                s_catalog_page_count = 2;
                s_next_catalog_offset = retained_offset + RADIO_CATALOG_PAGE_SIZE;
                /* That adjacent page was already available in the old window. */
                result->has_more = true;
            } else {
                s_catalog_page_offsets[1] = 0;
                s_catalog_page_counts[1] = 0;
                s_next_catalog_offset = result->next_offset;
            }
            s_catalog_oldest_offset = result->offset;
            result->first_index = 0;
            result->append_count = s_station_count;
        } else {
            if (s_load_more_button != NULL) {
                lv_obj_delete(s_load_more_button);
                s_load_more_button = NULL;
            }

            /* Keep exactly the two newest API pages in RAM and in LVGL. */
            if (result->count > 0 && s_catalog_page_count >= 2) {
                catalog_drop_oldest_page();
            }
            while (s_station_count + result->count > s_station_capacity &&
                   s_catalog_page_count > 0) {
                catalog_drop_oldest_page();
            }

            const size_t first = s_station_count;
            const size_t room = s_station_capacity - s_station_count;
            result->append_count = result->count < room ? result->count : room;
            result->first_index = first;
            if (result->append_count > 0) {
                memcpy(&s_stations[first], result->stations,
                       result->append_count * sizeof(s_stations[0]));
                s_station_count += result->append_count;
                if (s_catalog_page_count < 2) {
                    const size_t page = s_catalog_page_count++;
                    s_catalog_page_offsets[page] = result->offset;
                    s_catalog_page_counts[page] = result->append_count;
                }
            }
            s_next_catalog_offset = result->next_offset;
            s_catalog_oldest_offset = s_catalog_page_count > 0
                                          ? s_catalog_page_offsets[0]
                                          : result->offset;
        }
        result->prepared = true;
        s_catalog_has_more = result->has_more;

        if (s_station_count == 0) {
            catalog_status("No encontramos emisoras.", false);
            catalog_ui_result_free(result);
            s_catalog_render_pending = NULL;
            return;
        }
    }

    size_t batch = 0;
    while (result->rendered_count < result->append_count &&
           batch < CATALOG_UI_RENDER_BATCH) {
        const size_t index = result->first_index + result->rendered_count;
        if (!station_row_create(&s_stations[index], index)) {
            /* A partial page cannot keep valid page offsets or controls. */
            s_catalog_request_id = radio_generation_next(s_catalog_request_id);
            catalog_result_queue_drain();
            catalog_clear_results_state();
            catalog_status("Sin memoria para mostrar más radios", true);
            catalog_ui_result_free(result);
            s_catalog_render_pending = NULL;
            return;
        }
        ++result->rendered_count;
        ++batch;
    }
    if (result->rendered_count < result->append_count) return;

    if (s_load_previous_button != NULL) {
        lv_obj_clear_state(s_load_previous_button, LV_STATE_DISABLED);
        radio_ui_text_set_label(lv_obj_get_child(s_load_previous_button, 0),
                          "Cargar 20 anteriores");
    }
    if (catalog_previous_button_create() != ESP_OK) {
        s_catalog_request_id = radio_generation_next(s_catalog_request_id);
        catalog_result_queue_drain();
        catalog_clear_results_state();
        catalog_status("Sin memoria para mostrar la navegación del catálogo", true);
        catalog_ui_result_free(result);
        s_catalog_render_pending = NULL;
        return;
    }
    if (result->has_more) {
        lv_obj_t *load_more = button_create(s_radio_list, "Cargar 20 más",
                                            load_more_stations, NULL, 0, 0,
                                            204, 48, COLOR_CARD_2,
                                            COLOR_TEXT, 14);
        if (load_more == NULL) {
            s_catalog_request_id = radio_generation_next(s_catalog_request_id);
            catalog_result_queue_drain();
            catalog_clear_results_state();
            catalog_status("Sin memoria para mostrar la navegación del catálogo", true);
            catalog_ui_result_free(result);
            s_catalog_render_pending = NULL;
            return;
        }
        s_load_more_button = load_more;
        lv_obj_set_width(load_more, lv_pct(100));
    }
    if (s_catalog_scroll_anchor_pending) {
        catalog_scroll_anchor_restore(s_catalog_scroll_anchor_index,
                                      s_catalog_scroll_anchor_y);
        s_catalog_scroll_anchor_pending = false;
    }
    catalog_status("", false);
    ESP_LOGI(TAG, "Catalogo visible: %u radios; ventana %u..%u; SRAM libre/mayor %u/%u KiB; PSRAM libre/mayor %u/%u KiB",
             (unsigned)s_station_count,
             (unsigned)s_catalog_oldest_offset,
             (unsigned)s_next_catalog_offset,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024U),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024U),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) / 1024U),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) / 1024U));
    catalog_ui_result_free(result);
    s_catalog_render_pending = NULL;
}

static void load_more_stations(lv_event_t *event)
{
    (void)event;
    if (s_load_more_button != NULL) {
        lv_obj_add_state(s_load_more_button, LV_STATE_DISABLED);
        radio_ui_text_set_label(lv_obj_get_child(s_load_more_button, 0), "Cargando…");
    }
    if (s_load_previous_button != NULL) {
        lv_obj_add_state(s_load_previous_button, LV_STATE_DISABLED);
    }
    catalog_request_page(s_next_catalog_offset);
}

static void load_previous_stations(lv_event_t *event)
{
    (void)event;
    if (s_catalog_page_count == 0 || s_catalog_page_offsets[0] == 0) return;

    const size_t offset = s_catalog_page_offsets[0] >= RADIO_CATALOG_PAGE_SIZE
                              ? s_catalog_page_offsets[0] - RADIO_CATALOG_PAGE_SIZE
                              : 0;
    if (s_load_previous_button != NULL) {
        lv_obj_add_state(s_load_previous_button, LV_STATE_DISABLED);
        radio_ui_text_set_label(lv_obj_get_child(s_load_previous_button, 0),
                          "Cargando anteriores…");
    }
    if (s_load_more_button != NULL) {
        lv_obj_add_state(s_load_more_button, LV_STATE_DISABLED);
    }
    catalog_request_page(offset);
}

static void retry_catalog_request(lv_event_t *event)
{
    (void)event;
    if (s_load_more_button != NULL) {
        lv_obj_add_state(s_load_more_button, LV_STATE_DISABLED);
        radio_ui_text_set_label(lv_obj_get_child(s_load_more_button, 0), "Reintentando…");
    }
    if (s_load_previous_button != NULL) {
        lv_obj_add_state(s_load_previous_button, LV_STATE_DISABLED);
    }
    catalog_request_page(s_catalog_retry_offset);
}

static void catalog_request_page(size_t offset)
{
    s_catalog_retry_offset = offset;
    s_catalog_request_id = radio_generation_next(s_catalog_request_id);
    esp_err_t err;
    if (s_catalog_popular) {
        err = radio_catalog_load_popular(offset, s_catalog_request_id);
    } else {
        err = radio_catalog_search_filter(s_explore_filter, s_catalog_query,
                                          offset, s_catalog_request_id);
    }
    if (err != ESP_OK) {
        const esp_err_t show_err = catalog_show_failure(err);
        if (show_err != ESP_OK) {
            ESP_LOGE(TAG, "No se pudo mostrar el error del catalogo: %s",
                     esp_err_to_name(show_err));
        }
        return;
    }
    const bool loading_previous = s_catalog_page_count > 0 &&
                                  offset < s_catalog_page_offsets[0];
    catalog_status(offset == 0 && !loading_previous
                       ? "Buscando emisoras…"
                       : loading_previous ? "Cargando emisoras anteriores…"
                                          : "Cargando más emisoras…",
                   false);
}

static void begin_catalog_search(const char *query, const char *title, bool popular)
{
    if (!schedule_radio_view_refresh()) return;
    s_showing_catalog_results = true;
    s_catalog_popular = popular;
    snprintf(s_catalog_query, sizeof(s_catalog_query), "%s", query != NULL ? query : "");
    radio_ui_text_set_label(s_radio_result_title, title);
    update_radio_tab_styles();
}

static const char *const s_country_options[] = {
    "AR", "BR", "CL", "MX", "ES", "US", "GB", "UY", "DE", "FR",
};
static const char *const s_country_labels[] = {
    "Argentina", "Brasil", "Chile", "México", "España", "EE.UU.",
    "Reino Unido", "Uruguay", "Alemania", "Francia",
};
static const char *const s_genre_options[] = {
    "rock", "pop", "jazz", "news", "classical", "electronic", "talk", "metal",
};
static const char *const s_genre_labels[] = {
    "Rock", "Pop", "Jazz", "Noticias", "Música clásica", "Electrónica",
    "Programas hablados", "Metal",
};
static const char *const s_language_options[] = {
    "spanish", "english", "portuguese", "french", "german", "italian",
};
static const char *const s_language_labels[] = {
    "Español", "Inglés", "Portugués", "Francés", "Alemán", "Italiano",
};
static const char *const s_codec_options[] = {"mp3", "aac"};
static const char *const s_codec_labels[] = {"MP3", "AAC / AAC+"};

static void explore_option_event(lv_event_t *event)
{
    const int option = (int)(intptr_t)lv_event_get_user_data(event);
    const char *value = NULL;
    const char *title = NULL;
    if (option == -1) {
        begin_catalog_search(NULL, "Populares", true);
        return;
    }
    if (option == -2) {
        start_wifi_portal(event);
        lv_screen_load(s_screens[PAGE_SETTINGS]);
        return;
    }
    const size_t index = (size_t)option;
    switch (s_explore_filter) {
    case RADIO_CATALOG_FILTER_COUNTRY:
        if (index < sizeof(s_country_options) / sizeof(s_country_options[0])) {
            value = s_country_options[index]; title = s_country_labels[index];
        }
        break;
    case RADIO_CATALOG_FILTER_GENRE:
        if (index < sizeof(s_genre_options) / sizeof(s_genre_options[0])) {
            value = s_genre_options[index]; title = s_genre_labels[index];
        }
        break;
    case RADIO_CATALOG_FILTER_LANGUAGE:
        if (index < sizeof(s_language_options) / sizeof(s_language_options[0])) {
            value = s_language_options[index]; title = s_language_labels[index];
        }
        break;
    case RADIO_CATALOG_FILTER_CODEC:
        if (index < sizeof(s_codec_options) / sizeof(s_codec_options[0])) {
            value = s_codec_options[index]; title = s_codec_labels[index];
        }
        break;
    }
    if (value != NULL) begin_catalog_search(value, title, false);
}

static void explore_filter_event(lv_event_t *event)
{
    if (!schedule_radio_view_refresh()) return;
    s_explore_filter = (radio_catalog_filter_t)(intptr_t)lv_event_get_user_data(event);
    s_explore_category_open = true;
}

static void explore_back_event(lv_event_t *event)
{
    (void)event;
    if (!schedule_radio_view_refresh()) return;
    if (s_showing_catalog_results) {
        s_showing_catalog_results = false;
        update_radio_tab_styles();
    } else {
        s_explore_category_open = false;
    }
}

static esp_err_t explore_row_create(const char *title, lv_event_cb_t callback,
                                    intptr_t choice, bool accented)
{
    if (s_radio_list == NULL) return ESP_ERR_INVALID_STATE;
    lv_obj_t *row = radio_ui_lvgl_obj_create(s_radio_list);
    if (row == NULL) return ESP_ERR_NO_MEM;
    lv_obj_set_size(row, lv_pct(100), 50);
    lv_obj_set_style_bg_color(row, lv_color_hex(accented ? COLOR_CARD_2 : COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    touch_feedback_apply(row);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_clickable(row, true);
    lv_obj_set_gesture_bubble(row, true);
    lv_obj_set_event_bubble(row, true);
    lv_obj_add_event_cb(row, ui_activity_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(row, callback, LV_EVENT_CLICKED, (void *)choice);
    if (label_create(row, title, accented ? COLOR_MINT : COLOR_TEXT,
                     14, 15, 178, 20, 14) == NULL ||
        label_create(row, ">", COLOR_MUTED, 198, 15, 18, 20, 14) == NULL) {
        lv_obj_delete(row);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t explore_options_create(void)
{
    esp_err_t err = ESP_OK;
    if (!s_explore_category_open) {
        err = explore_row_create("Populares", explore_option_event, -1, true);
        if (err == ESP_OK) err = explore_row_create(
            "Por país", explore_filter_event, RADIO_CATALOG_FILTER_COUNTRY, false);
        if (err == ESP_OK) err = explore_row_create(
            "Por género", explore_filter_event, RADIO_CATALOG_FILTER_GENRE, false);
        if (err == ESP_OK) err = explore_row_create(
            "Por idioma", explore_filter_event, RADIO_CATALOG_FILTER_LANGUAGE, false);
        if (err == ESP_OK) err = explore_row_create(
            "Por formato", explore_filter_event, RADIO_CATALOG_FILTER_CODEC, false);
        if (err == ESP_OK) err = explore_row_create(
            "Buscar desde el celular", explore_option_event, -2, false);
    } else {
        err = explore_row_create("Volver a explorar", explore_back_event, 0, true);
        const char *const *labels = s_country_labels;
        size_t count = sizeof(s_country_labels) / sizeof(s_country_labels[0]);
        if (s_explore_filter == RADIO_CATALOG_FILTER_GENRE) {
            labels = s_genre_labels;
            count = sizeof(s_genre_labels) / sizeof(s_genre_labels[0]);
        } else if (s_explore_filter == RADIO_CATALOG_FILTER_LANGUAGE) {
            labels = s_language_labels;
            count = sizeof(s_language_labels) / sizeof(s_language_labels[0]);
        } else if (s_explore_filter == RADIO_CATALOG_FILTER_CODEC) {
            labels = s_codec_labels;
            count = sizeof(s_codec_labels) / sizeof(s_codec_labels[0]);
        }
        for (size_t i = 0; err == ESP_OK && i < count; ++i) {
            err = explore_row_create(labels[i], explore_option_event, i, false);
        }
    }
    if (err != ESP_OK) {
        catalog_clear_widgets();
        catalog_status("Sin memoria para mostrar opciones", true);
        return err;
    }
    catalog_status("", false);
    return ESP_OK;
}

static esp_err_t populate_explore_options(void)
{
    catalog_clear_results();
    s_showing_catalog_results = false;
    return explore_options_create();
}

static void explore_from_empty_event(lv_event_t *event)
{
    (void)event;
    s_radio_tab = RADIO_TAB_EXPLORE;
    radio_station_store_set_last_view((int)s_radio_tab);
    s_showing_catalog_results = false;
    s_explore_category_open = false;
    update_radio_tab_styles();
    populate_radio_view_report();
}

static esp_err_t empty_saved_state_create(const char *title, const char *hint)
{
    if (s_radio_list == NULL) return ESP_ERR_INVALID_STATE;
    lv_obj_t *card = radio_ui_lvgl_obj_create(s_radio_list);
    if (card == NULL) return ESP_ERR_NO_MEM;
    lv_obj_set_size(card, lv_pct(100), 132);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_scrollable(card, false);
    lv_obj_set_clickable(card, false);

    lv_obj_t *title_label = label_create(card, title, COLOR_TEXT,
                                         10, 14, 204, 20, 14);
    if (title_label == NULL) goto no_memory;
    lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *hint_label = label_create(card, hint, COLOR_MUTED,
                                        12, 40, 200, 32, 12);
    if (hint_label == NULL) goto no_memory;
    lv_label_set_long_mode(hint_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(hint_label, LV_TEXT_ALIGN_CENTER, 0);
    if (button_create(card, "Explorar radios", explore_from_empty_event, NULL,
                      16, 78, 180, 44, COLOR_CARD_2, COLOR_TEXT, 14) == NULL) {
        goto no_memory;
    }
    return ESP_OK;

no_memory:
    lv_obj_delete(card);
    return ESP_ERR_NO_MEM;
}

static esp_err_t populate_saved_stations(void)
{
    catalog_clear_results();
    if (s_station_capacity == 0) {
        catalog_status("Memoria insuficiente para listar radios", true);
        return ESP_ERR_NO_MEM;
    }
    s_station_count = s_radio_tab == RADIO_TAB_FAVORITES
        ? radio_station_store_favorites(s_stations, s_station_capacity)
        : radio_station_store_recents(s_stations, s_station_capacity);
    if (s_station_count == 0) {
        const esp_err_t err = empty_saved_state_create(
            s_radio_tab == RADIO_TAB_FAVORITES
                ? "Sin radios favoritas" : "Sin emisoras recientes",
            s_radio_tab == RADIO_TAB_FAVORITES
                ? "Tocá el corazón para guardar una emisora."
                : "Elegí una emisora y aparecerá acá.");
        if (err != ESP_OK) {
            catalog_status("Sin memoria para mostrar el estado vacío", true);
            return err;
        }
        catalog_status("", false);
        return ESP_OK;
    }
    for (size_t i = 0; i < s_station_count; ++i) {
        if (station_row_create(&s_stations[i], i)) continue;
        catalog_clear_widgets();
        catalog_status("Sin memoria para mostrar radios guardadas", true);
        return ESP_ERR_NO_MEM;
    }
    catalog_status("", false);
    return ESP_OK;
}

static esp_err_t populate_radio_view(void)
{
    if (s_radio_list == NULL) return ESP_ERR_INVALID_STATE;
    if (s_radio_tab == RADIO_TAB_EXPLORE) {
        if (s_showing_catalog_results) {
            catalog_clear_results();
            catalog_request_page(0);
            return ESP_OK;
        }
        return populate_explore_options();
    } else {
        s_showing_catalog_results = false;
        return populate_saved_stations();
    }
    return ESP_OK;
}

static void populate_radio_view_report(void)
{
    const esp_err_t err = populate_radio_view();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo construir la vista de radios: %s",
                 esp_err_to_name(err));
    }
}

static void refresh_radio_view_async(void *unused)
{
    (void)unused;
    populate_radio_view_report();
}

static void radio_tab_event(lv_event_t *event)
{
    s_radio_tab = (radio_tab_t)(intptr_t)lv_event_get_user_data(event);
    radio_station_store_set_last_view((int)s_radio_tab);
    s_showing_catalog_results = false;
    s_explore_category_open = false;
    update_radio_tab_styles();
    populate_radio_view_report();
}

static esp_err_t render_radio_tabs(lv_obj_t *screen)
{
    static const char *const labels[] = {"Favoritas", "Recientes", "Explorar"};
    for (int i = 0; i < RADIO_TAB_COUNT; ++i) {
        const uint32_t bg = i == s_radio_tab ? COLOR_MINT : COLOR_CARD;
        const uint32_t fg = i == s_radio_tab
                                ? radio_ui_theme_text_on_accent(s_theme_id)
                                : COLOR_TEXT;
        s_radio_tab_buttons[i] = button_create(screen, labels[i], radio_tab_event,
                      (void *)(intptr_t)i, 5 + i * 77, 50, 74, 44, bg, fg, 12);
        if (s_radio_tab_buttons[i] == NULL) return ESP_ERR_NO_MEM;
    }
    s_radio_back_button = button_create(screen, "< Explorar", explore_back_event,
                                        NULL, 5, 50, 98, 44, COLOR_CARD_2,
                                        COLOR_MINT, 12);
    if (s_radio_back_button == NULL) return ESP_ERR_NO_MEM;
    s_radio_result_title = label_create(screen, "", COLOR_TEXT,
                                        112, 57, 116, 24, 16);
    if (s_radio_result_title == NULL) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

static void player_status_apply(const player_ui_status_event_t *event)
{
    if (event == NULL) return;
    if (s_player_status_sequence_valid &&
        !radio_generation_is_newer(event->sequence,
                                   s_player_status_sequence)) return;
    s_player_status_sequence = event->sequence;
    s_player_status_sequence_valid = true;
    s_player_state = event->state;
    home_playback_status_update();
    if (event->state == RADIO_PLAYER_STATE_ERROR) error_popup_show(event->message);
    else error_popup_hide();
    const char *button = event->state == RADIO_PLAYER_STATE_PLAYING ? LV_SYMBOL_PAUSE :
                         event->state == RADIO_PLAYER_STATE_CONNECTING ||
                                 event->state == RADIO_PLAYER_STATE_RECOVERING
                             ? LV_SYMBOL_STOP :
                         event->state == RADIO_PLAYER_STATE_PAUSED ? LV_SYMBOL_PLAY :
                         event->state == RADIO_PLAYER_STATE_STOPPING ? "..." : LV_SYMBOL_PLAY;
    radio_ui_text_set_label(s_play_button_label, button);
    favorite_update_home();
}

static void player_status_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    player_ui_status_event_t event;
    if (s_player_status_queue != NULL &&
        xQueueReceive(s_player_status_queue, &event, 0) == pdTRUE) {
        player_status_apply(&event);
    }
}

static void player_status_callback(radio_player_state_t state, uint32_t sequence,
                                    const char *station, const char *message)
{
    (void)station;
    player_ui_status_event_t event = {
        .state = state,
        .sequence = sequence,
    };
    snprintf(event.message, sizeof(event.message), "%s",
             message != NULL ? message : "En espera");
    /* Prefer the mailbox. If LVGL timer allocation failed during init, apply
       under a short lock rather than losing a terminal playback status. */
    if (s_player_status_queue != NULL && s_player_status_timer != NULL) {
        (void)RADIO_TEST_QUEUE_OVERWRITE(s_player_status_queue, &event);
    } else if (lvgl_port_lock(10)) {
        player_status_apply(&event);
        lvgl_port_unlock();
    }
}

static void favorite_mutation_complete(esp_err_t result, bool is_favorite,
                                      void *context)
{
    (void)is_favorite;
    const favorite_mutation_event_t event = {
        .result = result,
        .persistence_error = false,
        .generation = (uint32_t)(uintptr_t)context,
    };
    if (s_favorite_mutation_queue != NULL && s_favorite_mutation_timer != NULL) {
        favorite_mutation_event_t pending;
        if (xQueuePeek(s_favorite_mutation_queue, &pending, 0) != pdTRUE ||
            !pending.persistence_error) {
            (void)RADIO_TEST_QUEUE_OVERWRITE(s_favorite_mutation_queue,
                                             &event);
        }
    } else if (lvgl_port_lock(10)) {
        favorite_mutation_apply(&event);
        lvgl_port_unlock();
    } else {
        ESP_LOGW(TAG, "No se pudo notificar el resultado de favorita: %s",
                 esp_err_to_name(result));
    }
}

static void store_error_callback(esp_err_t result, void *context)
{
    (void)context;
    const favorite_mutation_event_t event = {
        .result = result,
        .persistence_error = true,
        .generation = 0,
    };
    if (s_favorite_mutation_queue != NULL && s_favorite_mutation_timer != NULL) {
        (void)RADIO_TEST_QUEUE_OVERWRITE(s_favorite_mutation_queue, &event);
    } else if (lvgl_port_lock(10)) {
        favorite_mutation_apply(&event);
        lvgl_port_unlock();
    } else {
        ESP_LOGW(TAG, "Falló el guardado NVS: %s", esp_err_to_name(result));
    }
}

static void favorite_mutation_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    favorite_mutation_event_t event;
    if (s_favorite_mutation_queue == NULL ||
        xQueueReceive(s_favorite_mutation_queue, &event, 0) != pdTRUE) {
        return;
    }
    favorite_mutation_apply(&event);
}

static void favorite_mutation_apply(const favorite_mutation_event_t *event)
{
    if (event == NULL ||
        (!event->persistence_error &&
         !radio_generation_is_current(event->generation,
                                      s_favorite_mutation_generation))) {
        return;
    }
    if (event->persistence_error) {
        const int sleep_preset_minutes = radio_station_store_sleep_preset();
        const int screen_brightness = radio_station_store_screen_brightness();
        const int screen_timeout_seconds =
            radio_station_store_screen_timeout_seconds();
        s_theme_id = radio_station_store_ui_theme();
        s_accent_id = radio_station_store_ui_accent();
        const int screensaver_style = radio_station_store_screensaver_style();
        const uint32_t now_tick = (uint32_t)xTaskGetTickCount();
        portENTER_CRITICAL(&s_idle_state_mux);
        radio_ui_idle_set_sleep_preset(&s_idle_state, sleep_preset_minutes);
        radio_ui_idle_set_brightness(&s_idle_state, screen_brightness);
        radio_ui_idle_set_timeout(&s_idle_state, now_tick,
                                  screen_timeout_seconds);
        radio_ui_idle_set_screensaver_style(&s_idle_state, screensaver_style);
        portEXIT_CRITICAL(&s_idle_state_mux);
        if (s_sleep_task_handle != NULL) xTaskNotifyGive(s_sleep_task_handle);
        radio_player_volume_preview(radio_station_store_volume());
        ui_theme_apply_values();
        const esp_err_t rebuild_err = ui_theme_rebuild(true);
        if (rebuild_err != ESP_OK) {
            ESP_LOGE(TAG, "No se pudo reconstruir tras rollback de preferencias: %s",
                     esp_err_to_name(rebuild_err));
        }
        idle_apply_pending();
        update_screen_choice_labels();
        timer_update_label();
        error_popup_show("Falló el guardado; se restauró el último valor confirmado");
    } else if (event->result == ESP_ERR_NO_MEM) {
        error_popup_show("No se pudo guardar: se alcanzó el límite de 8 favoritas o falta memoria");
    } else if (event->result != ESP_OK) {
        error_popup_show("No se pudo guardar la favorita");
    } else {
        error_popup_hide();
    }
    favorite_update_home();
    if (s_radio_tab == RADIO_TAB_FAVORITES) populate_radio_view_report();
}

static void wifi_status_callback(const radio_wifi_info_t *info)
{
    if (info == NULL || s_wifi_status_queue == NULL) return;
    const uint8_t event = 1U;
    /* Worker/event-loop callbacks never wait for LVGL or perform rendering. */
    (void)RADIO_TEST_QUEUE_OVERWRITE(s_wifi_status_queue, &event);
}

static void home_playback_status_update(void)
{
    if (s_home_playback_label == NULL) return;
    const char *text;
    uint32_t color = COLOR_TEXT;
    switch (s_player_state) {
    case RADIO_PLAYER_STATE_CONNECTING:
        text = "Conectando";
        color = COLOR_GOLD;
        break;
    case RADIO_PLAYER_STATE_RECOVERING:
        text = "Recuperando";
        color = COLOR_GOLD;
        break;
    case RADIO_PLAYER_STATE_PLAYING:
        text = "En vivo";
        color = COLOR_MINT;
        break;
    case RADIO_PLAYER_STATE_PAUSED:
        text = "En pausa";
        color = COLOR_GOLD;
        break;
    case RADIO_PLAYER_STATE_STOPPING:
        text = "Deteniendo";
        color = COLOR_MUTED;
        break;
    case RADIO_PLAYER_STATE_ERROR:
        text = "Error de reproducción";
        color = COLOR_ERROR;
        break;
    case RADIO_PLAYER_STATE_IDLE:
    default:
        text = s_wifi_connected
                   ? (s_has_current_station ? "Detenida" : "En espera")
                   : s_wifi_provisioning ? "Portal activo" : "Sin Wi-Fi";
        color = s_wifi_connected && !s_has_current_station ? COLOR_MINT :
                s_wifi_provisioning ? COLOR_GOLD : COLOR_MUTED;
        break;
    }
    radio_ui_text_set_label(s_home_playback_label, text);
    lv_obj_set_style_text_color(s_home_playback_label, lv_color_hex(color), 0);
}

static void update_home_station(void)
{
    if (s_home_station_label == NULL || s_home_meta_label == NULL) return;
    if (!s_has_current_station) {
        radio_ui_text_set_label(s_home_station_label, "ONDA");
        radio_ui_text_set_label(s_home_meta_label,
                          s_wifi_connected ? "Elegí una radio en Radios" :
                          s_wifi_provisioning ? "Conectá el celular al portal"
                                              : "Configurá Wi-Fi en Ajustes");
    } else {
        radio_ui_text_set_label(s_home_station_label, s_current_station.name);
        char metadata[76];
        snprintf(metadata, sizeof(metadata), "%.38s%s%.20s",
                 s_current_station.country[0] ? s_current_station.country : "Radio online",
                 s_current_station.codec[0] ? " · " : "",
                 s_current_station.codec);
        radio_ui_text_set_label(s_home_meta_label, metadata);
    }
    home_playback_status_update();
    favorite_update_home();
}

static void header_status_refresh(void)
{
    if (s_home_time_label != NULL) {
        time_t now = time(NULL);
        struct tm local_time;
        char clock_text[8] = "--:--";
        if (now >= 1735689600 && localtime_r(&now, &local_time) != NULL) {
            strftime(clock_text, sizeof(clock_text), "%H:%M", &local_time);
        }
        radio_ui_text_set_label(s_home_time_label, clock_text);
    }

    if (s_home_battery_fill == NULL) return;
    uint8_t percent = 0;
    uint16_t millivolts = 0;
    if (radio_board_battery_read(&millivolts, &percent) != ESP_OK) {
        lv_obj_set_hidden(s_home_battery_fill, true);
        lv_obj_set_style_bg_color(s_home_battery_body, lv_color_hex(COLOR_MUTED), 0);
        lv_obj_set_style_bg_color(s_home_battery_cap, lv_color_hex(COLOR_MUTED), 0);
        radio_ui_text_set_label(s_home_battery_label, "--%");
        lv_obj_set_style_text_color(s_home_battery_label, lv_color_hex(COLOR_MUTED), 0);
        return;
    }
    ESP_LOGD(TAG, "Batería: %u mV, %u%% (estimado por voltaje)",
             (unsigned)millivolts, (unsigned)percent);

    const int32_t fill_width = percent == 0 ? 0 : (15 * percent + 99) / 100;
    lv_obj_set_width(s_home_battery_fill, fill_width);
    lv_obj_set_hidden(s_home_battery_fill, false);
    const uint32_t color = percent <= 15 ? COLOR_ERROR :
                           percent <= 35 ? COLOR_GOLD : COLOR_MINT;
    lv_obj_set_style_bg_color(s_home_battery_fill, lv_color_hex(color), 0);
    lv_obj_set_style_border_color(s_home_battery_body, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_set_style_bg_color(s_home_battery_cap, lv_color_hex(COLOR_MUTED), 0);
    char value[8];
    snprintf(value, sizeof(value), "%u%%", (unsigned)percent);
    radio_ui_text_set_label(s_home_battery_label, value);
    lv_obj_set_style_text_color(s_home_battery_label, lv_color_hex(color), 0);
}

static void header_status_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    header_status_refresh();
}

static void battery_charge_refresh(void)
{
    if (s_home_battery_charge_icon == NULL) return;
    bool charging = false;
    if (radio_board_battery_is_charging(&charging) != ESP_OK) {
        lv_obj_set_hidden(s_home_battery_charge_icon, true);
        return;
    }

    if (!s_battery_charge_state_known || s_battery_is_charging != charging) {
        ESP_LOGI(TAG, "Cargador ETA6098: %s",
                 charging ? "cargando" : "sin carga activa");
        s_battery_charge_state_known = true;
        s_battery_is_charging = charging;
    }

    lv_obj_set_hidden(s_home_battery_charge_icon, !charging);
    if (!charging) {
        s_battery_charge_flash = false;
        return;
    }

    s_battery_charge_flash = !s_battery_charge_flash;
    lv_obj_set_style_text_color(s_home_battery_charge_icon,
                                lv_color_hex(s_battery_charge_flash
                                                 ? COLOR_GOLD : COLOR_TEXT), 0);
}

static void battery_charge_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    battery_charge_refresh();
}

static void home_screen_refs_clear(void)
{
    s_home_wifi_label = NULL;
    s_home_station_label = NULL;
    s_home_playback_label = NULL;
    s_home_meta_label = NULL;
    s_home_volume_label = NULL;
    s_home_volume_bar = NULL;
    s_home_time_label = NULL;
    s_home_battery_body = NULL;
    s_home_battery_fill = NULL;
    s_home_battery_cap = NULL;
    s_home_battery_label = NULL;
    s_home_battery_charge_icon = NULL;
    s_favorite_button = NULL;
    s_play_button_label = NULL;
}

static void radio_screen_refs_clear(void)
{
    s_radio_status_label = NULL;
    s_radio_list = NULL;
    s_radio_back_button = NULL;
    s_radio_result_title = NULL;
    s_load_more_button = NULL;
    s_load_previous_button = NULL;
    memset(s_radio_tab_buttons, 0, sizeof(s_radio_tab_buttons));
    memset(s_station_row_contexts, 0, sizeof(s_station_row_contexts));
}

static void settings_screen_refs_clear(void)
{
    s_settings_status_label = NULL;
    s_settings_info_label = NULL;
    s_settings_list = NULL;
    s_timer_button_label = NULL;
    s_screen_button_label = NULL;
}

static void ui_widget_refs_clear(void)
{
    home_screen_refs_clear();
    radio_screen_refs_clear();
    settings_screen_refs_clear();
    s_error_message = NULL;
    s_screen_brightness_choice_label = NULL;
    s_screen_timeout_choice_label = NULL;
    s_screensaver_choice_label = NULL;
    s_clock_settings_value_label = NULL;
}

static void ui_view_handles_discard_current(void)
{
    close_overlay_event(NULL);
    error_popup_discard();
    for (size_t i = 0; i < sizeof(s_screens) / sizeof(s_screens[0]); ++i) {
        if (s_screens[i] != NULL) lv_obj_delete(s_screens[i]);
        s_screens[i] = NULL;
    }
    ui_widget_refs_clear();
}

#ifdef RADIO_UI_TEST_FAULT_INJECTION
static void ui_views_discard(void)
{
    s_view_handles = &s_ui_view_handles;
    ui_view_handles_discard_current();
    radio_ui_screensaver_destroy();
    memset(&s_ui_view_handles, 0, sizeof(s_ui_view_handles));
}
#endif

static void ui_staging_views_discard(void)
{
    s_view_handles = &s_staging_view_handles;
    ui_view_handles_discard_current();
    radio_ui_screensaver_stage_discard();
    memset(&s_staging_view_handles, 0, sizeof(s_staging_view_handles));
    s_view_handles = &s_ui_view_handles;
}

static void ui_page_discard(main_page_t page)
{
    if (page < PAGE_LISTEN || page > PAGE_SETTINGS) return;
    if (s_screens[page] != NULL) lv_obj_delete(s_screens[page]);
    s_screens[page] = NULL;
    if (page == PAGE_LISTEN) home_screen_refs_clear();
    else if (page == PAGE_RADIOS) radio_screen_refs_clear();
    else settings_screen_refs_clear();
}

static esp_err_t screensaver_stage_build(void)
{
    const radio_ui_idle_snapshot_t idle = idle_state_snapshot();
    return radio_ui_screensaver_stage_create(ui_activity_event, COLOR_MINT,
                                            idle.screensaver_style);
}

static esp_err_t battery_icon_create(lv_obj_t *parent)
{
    s_home_battery_body = radio_ui_lvgl_obj_create(parent);
    if (s_home_battery_body == NULL) return ESP_ERR_NO_MEM;
    lv_obj_set_size(s_home_battery_body, 23, 14);
    lv_obj_set_pos(s_home_battery_body, 164, 13);
    lv_obj_set_style_bg_opa(s_home_battery_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_home_battery_body, 1, 0);
    lv_obj_set_style_border_color(s_home_battery_body, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_set_style_radius(s_home_battery_body, 3, 0);
    lv_obj_set_style_pad_all(s_home_battery_body, 0, 0);
    lv_obj_set_scrollable(s_home_battery_body, false);
    lv_obj_set_clickable(s_home_battery_body, false);

    s_home_battery_fill = radio_ui_lvgl_obj_create(s_home_battery_body);
    if (s_home_battery_fill == NULL) return ESP_ERR_NO_MEM;
    lv_obj_set_size(s_home_battery_fill, 15, 8);
    lv_obj_set_pos(s_home_battery_fill, 3, 2);
    lv_obj_set_style_bg_color(s_home_battery_fill, lv_color_hex(COLOR_MINT), 0);
    lv_obj_set_style_bg_opa(s_home_battery_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_home_battery_fill, 0, 0);
    lv_obj_set_style_radius(s_home_battery_fill, 1, 0);
    lv_obj_set_scrollable(s_home_battery_fill, false);
    lv_obj_set_clickable(s_home_battery_fill, false);

    s_home_battery_cap = radio_ui_lvgl_obj_create(parent);
    if (s_home_battery_cap == NULL) return ESP_ERR_NO_MEM;
    lv_obj_set_size(s_home_battery_cap, 3, 6);
    lv_obj_set_pos(s_home_battery_cap, 188, 17);
    lv_obj_set_style_bg_color(s_home_battery_cap, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_set_style_bg_opa(s_home_battery_cap, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_home_battery_cap, 0, 0);
    lv_obj_set_style_radius(s_home_battery_cap, 1, 0);
    lv_obj_set_scrollable(s_home_battery_cap, false);
    lv_obj_set_clickable(s_home_battery_cap, false);

    s_home_battery_label = label_create(parent, "--%", COLOR_MUTED,
                                        196, 10, 35, 20, 12);
    if (s_home_battery_label == NULL) return ESP_ERR_NO_MEM;
    s_home_battery_charge_icon = label_create(parent, LV_SYMBOL_CHARGE,
                                               COLOR_GOLD, 168, 8, 16, 20, 12);
    if (s_home_battery_charge_icon == NULL) return ESP_ERR_NO_MEM;
    lv_obj_set_style_text_align(s_home_battery_charge_icon,
                                LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_hidden(s_home_battery_charge_icon, true);
    return ESP_OK;
}

static esp_err_t build_home_screen(void)
{
    lv_obj_t *screen = radio_ui_lvgl_obj_create(NULL);
    if (screen == NULL) return ESP_ERR_NO_MEM;
    view_root_track(screen);
    s_screens[PAGE_LISTEN] = screen;
    apply_screen_style(screen);

    s_home_wifi_label = label_create(screen, "Sin Wi-Fi", COLOR_MUTED,
                                     10, 13, 80, 18, 12);
    if (s_home_wifi_label == NULL) goto no_memory;
    s_home_time_label = label_create(screen, "--:--", COLOR_TEXT,
                                     101, 10, 48, 22, 14);
    if (s_home_time_label == NULL) goto no_memory;
    lv_obj_set_style_text_align(s_home_time_label, LV_TEXT_ALIGN_RIGHT, 0);
    if (battery_icon_create(screen) != ESP_OK) goto no_memory;

    lv_obj_t *card = radio_ui_lvgl_obj_create(screen);
    if (card == NULL) goto no_memory;
    lv_obj_set_size(card, 224, 94);
    lv_obj_set_pos(card, 8, 40);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 15, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_scrollable(card, false);
    lv_obj_set_event_bubble(card, true);
    lv_obj_set_gesture_bubble(card, true);

    s_home_station_label = label_create(card, "ONDA", COLOR_TEXT,
                                        12, 5, 160, 24, 18);
    if (s_home_station_label == NULL) goto no_memory;
    station_name_scroll_enable(s_home_station_label);
    s_home_meta_label = label_create(card, "Explorá el catálogo", COLOR_MUTED,
                                     12, 30, 154, 17, 12);
    if (s_home_meta_label == NULL) goto no_memory;
    s_home_playback_label = label_create(card, "En espera", COLOR_MUTED,
                                         12, 51, 158, 17, 12);
    if (s_home_playback_label == NULL) goto no_memory;

    s_favorite_button = radio_ui_lvgl_obj_create(card);
    if (s_favorite_button == NULL) goto no_memory;
    lv_obj_set_size(s_favorite_button, 46, 46);
    lv_obj_set_pos(s_favorite_button, 174, 1);
    lv_obj_set_style_bg_opa(s_favorite_button, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(s_favorite_button, LV_OPA_TRANSP, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(s_favorite_button, 0, 0);
    lv_obj_set_style_shadow_width(s_favorite_button, 0, 0);
    lv_obj_set_style_outline_width(s_favorite_button, 0, 0);
    lv_obj_set_style_pad_all(s_favorite_button, 0, 0);
    touch_feedback_apply(s_favorite_button);
    lv_obj_set_scrollable(s_favorite_button, false);
    lv_obj_set_clickable(s_favorite_button, true);
    lv_obj_set_event_bubble(s_favorite_button, true);
    lv_obj_set_gesture_bubble(s_favorite_button, true);
    lv_obj_add_event_cb(s_favorite_button, ui_activity_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_favorite_button, home_favorite_event, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_favorite_button, heart_draw_event,
                        LV_EVENT_DRAW_MAIN_END, NULL);

    if (label_create(card, "VOL", COLOR_MUTED, 12, 74, 26, 16, 12) == NULL)
        goto no_memory;
    s_home_volume_bar = radio_ui_lvgl_bar_create(card);
    if (s_home_volume_bar == NULL) goto no_memory;
    lv_obj_set_size(s_home_volume_bar, 140, 8);
    lv_obj_set_pos(s_home_volume_bar, 40, 78);
    lv_bar_set_range(s_home_volume_bar, 0, 100);
    lv_obj_set_style_bg_color(s_home_volume_bar, lv_color_hex(COLOR_CARD_2), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_home_volume_bar, lv_color_hex(COLOR_MINT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_home_volume_bar, 5, LV_PART_MAIN);
    lv_obj_set_style_radius(s_home_volume_bar, 5, LV_PART_INDICATOR);
    s_home_volume_label = label_create(card, "55%", COLOR_MINT,
                                       188, 72, 29, 17, 12);
    if (s_home_volume_label == NULL) goto no_memory;

    if (button_create(screen, LV_SYMBOL_PREV, next_previous_event,
                      (void *)(intptr_t)-1, 12, 140, 52, 48,
                      COLOR_CARD, COLOR_TEXT, 16) == NULL) goto no_memory;
    lv_obj_t *play = button_create(screen, LV_SYMBOL_PLAY, play_pause_event, NULL,
                                   69, 140, 102, 48, COLOR_MINT, COLOR_BG, 18);
    if (play == NULL) goto no_memory;
    s_play_button_label = lv_obj_get_child(play, 0);
    if (s_play_button_label == NULL) goto no_memory;
    if (button_create(screen, LV_SYMBOL_NEXT, next_previous_event,
                      (void *)(intptr_t)1, 176, 140, 52, 48,
                      COLOR_CARD, COLOR_TEXT, 16) == NULL ||
        create_home_navigation(screen) != ESP_OK) goto no_memory;
    return ESP_OK;

no_memory:
    ui_page_discard(PAGE_LISTEN);
    return ESP_ERR_NO_MEM;
}

static esp_err_t build_radios_screen(void)
{
    lv_obj_t *screen = radio_ui_lvgl_obj_create(NULL);
    if (screen == NULL) return ESP_ERR_NO_MEM;
    view_root_track(screen);
    s_screens[PAGE_RADIOS] = screen;
    apply_screen_style(screen);
    if (button_create(screen, "<", navigation_back_event, NULL,
                      4, 0, 72, 48, COLOR_CARD, COLOR_TEXT, 18) == NULL ||
        label_create(screen, "RADIOS", COLOR_TEXT, 78, 13, 95, 28, 18) == NULL ||
        render_radio_tabs(screen) != ESP_OK) goto no_memory;
    s_radio_status_label = label_create(screen, "", COLOR_MUTED,
                                        16, 144, 208, 48, 14);
    if (s_radio_status_label == NULL) goto no_memory;
    lv_label_set_long_mode(s_radio_status_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_radio_status_label, LV_TEXT_ALIGN_CENTER, 0);
    s_radio_list = radio_ui_lvgl_obj_create(screen);
    if (s_radio_list == NULL) goto no_memory;
    lv_obj_set_size(s_radio_list, 224, 136);
    lv_obj_set_pos(s_radio_list, 8, 100);
    lv_obj_set_style_bg_opa(s_radio_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_radio_list, 0, 0);
    lv_obj_set_style_pad_all(s_radio_list, 0, 0);
    lv_obj_set_style_pad_row(s_radio_list, 4, 0);
    lv_obj_set_flex_flow(s_radio_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_radio_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_radio_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_gesture_bubble(s_radio_list, true);
    lv_obj_set_event_bubble(s_radio_list, true);
    update_radio_tab_styles();
    return ESP_OK;

no_memory:
    ui_page_discard(PAGE_RADIOS);
    return ESP_ERR_NO_MEM;
}

static esp_err_t build_settings_screen(void)
{
    lv_obj_t *screen = radio_ui_lvgl_obj_create(NULL);
    if (screen == NULL) return ESP_ERR_NO_MEM;
    view_root_track(screen);
    s_screens[PAGE_SETTINGS] = screen;
    apply_screen_style(screen);
    if (button_create(screen, "<", navigation_back_event, NULL,
                      4, 0, 72, 48, COLOR_CARD, COLOR_TEXT, 18) == NULL ||
        label_create(screen, "AJUSTES", COLOR_TEXT, 82, 13, 110, 28, 18) == NULL)
        goto no_memory;
    s_settings_status_label = label_create(screen, "", COLOR_MUTED,
                                           12, SETTINGS_STATUS_Y, 216, 16, 12);
    if (s_settings_status_label == NULL) goto no_memory;
    s_settings_info_label = label_create(screen, "", COLOR_TEXT,
                                         12, SETTINGS_INFO_Y, 216,
                                         SETTINGS_INFO_HEIGHT, 12);
    if (s_settings_info_label == NULL) goto no_memory;
    lv_label_set_long_mode(s_settings_info_label, LV_LABEL_LONG_MODE_WRAP);
    s_settings_list = radio_ui_lvgl_obj_create(screen);
    if (s_settings_list == NULL) goto no_memory;
    lv_obj_set_size(s_settings_list, 224, SETTINGS_LIST_HEIGHT);
    lv_obj_set_pos(s_settings_list, 8, SETTINGS_LIST_Y);
    lv_obj_set_style_bg_opa(s_settings_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_settings_list, 0, 0);
    lv_obj_set_style_pad_left(s_settings_list, 6, 0);
    lv_obj_set_style_pad_right(s_settings_list, 6, 0);
    lv_obj_set_style_pad_top(s_settings_list, 3, 0);
    lv_obj_set_style_pad_bottom(s_settings_list, 3, 0);
    lv_obj_set_style_pad_row(s_settings_list, 6, 0);
    lv_obj_set_flex_flow(s_settings_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_settings_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_settings_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_gesture_bubble(s_settings_list, true);
    lv_obj_set_event_bubble(s_settings_list, true);

    lv_obj_t *button = button_create(s_settings_list, "Apariencia y color",
                                     show_appearance_settings, NULL, 0, 0, 212, 44,
                                     COLOR_CARD_2, COLOR_TEXT, 14);
    if (button == NULL) goto no_memory;
    lv_obj_set_width(button, lv_pct(100));
    button = button_create(s_settings_list, "Wi-Fi y portal", start_wifi_portal,
                           NULL, 0, 0, 212, 44, COLOR_CARD_2, COLOR_TEXT, 14);
    if (button == NULL) goto no_memory;
    lv_obj_set_width(button, lv_pct(100));
    button = button_create(s_settings_list, "Temporizador de radio", show_sleep_timer,
                           NULL, 0, 0, 212, 44, COLOR_CARD, COLOR_TEXT, 14);
    if (button == NULL) goto no_memory;
    lv_obj_set_width(button, lv_pct(100));
    s_timer_button_label = lv_obj_get_child(button, 0);
    if (s_timer_button_label == NULL) goto no_memory;
    button = button_create(s_settings_list, "Pantalla y reloj", show_screen_settings,
                           NULL, 0, 0, 212, 44, COLOR_CARD, COLOR_TEXT, 14);
    if (button == NULL) goto no_memory;
    lv_obj_set_width(button, lv_pct(100));
    s_screen_button_label = lv_obj_get_child(button, 0);
    if (s_screen_button_label == NULL) goto no_memory;
    button = button_create(s_settings_list, "Detener radio", stop_event,
                           NULL, 0, 0, 212, 44, COLOR_CARD, COLOR_TEXT, 14);
    if (button == NULL) goto no_memory;
    lv_obj_set_width(button, lv_pct(100));
    button = button_create(s_settings_list, "Apagar equipo", power_off_requested,
                           NULL, 0, 0, 212, 44, COLOR_CARD_2, COLOR_ERROR, 14);
    if (button == NULL) goto no_memory;
    lv_obj_set_width(button, lv_pct(100));
    update_wifi_settings_info();
    return ESP_OK;

no_memory:
    ui_page_discard(PAGE_SETTINGS);
    return ESP_ERR_NO_MEM;
}

static esp_err_t radio_view_stage_build(const char *previous_status)
{
    if (s_radio_list == NULL) return ESP_ERR_INVALID_STATE;
    if (s_radio_tab == RADIO_TAB_EXPLORE && !s_showing_catalog_results) {
        return explore_options_create();
    }
    if (s_station_count > 0 && s_stations == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_radio_tab != RADIO_TAB_EXPLORE && s_station_count == 0) {
        if (s_station_capacity == 0) {
            catalog_status("Memoria insuficiente para listar radios", true);
            return ESP_OK;
        }
        const esp_err_t empty_err = empty_saved_state_create(
            s_radio_tab == RADIO_TAB_FAVORITES
                ? "Sin radios favoritas" : "Sin emisoras recientes",
            s_radio_tab == RADIO_TAB_FAVORITES
                ? "Tocá el corazón para guardar una emisora."
                : "Elegí una emisora y aparecerá acá.");
        if (empty_err != ESP_OK) goto no_memory;
        catalog_status("", false);
        return ESP_OK;
    }

    for (size_t i = 0; i < s_station_count; ++i) {
        if (s_stations == NULL || !station_row_create(&s_stations[i], i))
            goto no_memory;
    }
    if (catalog_previous_button_create() != ESP_OK) goto no_memory;
    if (s_catalog_has_more) {
        s_load_more_button = button_create(s_radio_list, "Cargar 20 más",
                                           load_more_stations, NULL, 0, 0,
                                           204, 48, COLOR_CARD_2,
                                           COLOR_TEXT, 14);
        if (s_load_more_button == NULL) goto no_memory;
        lv_obj_set_width(s_load_more_button, lv_pct(100));
    }
    if (s_radio_tab == RADIO_TAB_EXPLORE && s_station_count == 0) {
        catalog_status(previous_status != NULL && previous_status[0] != '\0'
                            ? previous_status : "Buscando radios…",
                       false);
    } else {
        catalog_status("", false);
    }
    return ESP_OK;

no_memory:
    catalog_clear_widgets();
    catalog_status("Sin memoria para reconstruir las radios", true);
    return ESP_ERR_NO_MEM;
}

static esp_err_t ui_views_stage_build(int theme_id, int accent_id)
{
    ui_staging_views_discard();

    char previous_radio_status[96] = {0};
    if (s_ui_view_handles.radio_status_label != NULL) {
        const char *status =
            lv_label_get_text(s_ui_view_handles.radio_status_label);
        if (status != NULL) {
            snprintf(previous_radio_status, sizeof(previous_radio_status),
                     "%s", status);
        }
    }

    const int previous_theme_id = s_theme_id;
    const int previous_accent_id = s_accent_id;
    const radio_ui_palette_t previous_theme = s_theme;
    s_theme_id = radio_ui_theme_normalize_id(theme_id);
    s_accent_id = radio_ui_accent_normalize_id(accent_id);
    s_theme = radio_ui_theme_palette(s_theme_id, s_accent_id);
    s_view_handles = &s_staging_view_handles;

    esp_err_t err = build_home_screen();
    if (err == ESP_OK) err = build_radios_screen();
    if (err == ESP_OK) err = build_settings_screen();
    if (err == ESP_OK) err = build_error_popup();
    if (err == ESP_OK) {
        update_home_station();
        volume_status_update();
        header_status_refresh();
        update_radio_tab_styles();
        err = radio_view_stage_build(previous_radio_status);
    }
    if (err == ESP_OK) {
        update_wifi_settings_info();
        timer_update_label();
        update_screen_choice_labels();
        err = screensaver_stage_build();
    }

    s_view_handles = &s_ui_view_handles;
    s_theme_id = previous_theme_id;
    s_accent_id = previous_accent_id;
    s_theme = previous_theme;
    if (err != ESP_OK) {
        ui_staging_views_discard();
    }
    return err;
}

static void ui_views_publish_staging(int active_page)
{
    if (active_page < PAGE_LISTEN || active_page > PAGE_SETTINGS) {
        active_page = PAGE_LISTEN;
    }

    s_previous_view_handles = s_ui_view_handles;
    s_ui_view_handles = s_staging_view_handles;
    memset(&s_staging_view_handles, 0, sizeof(s_staging_view_handles));
    s_view_handles = &s_ui_view_handles;
    lv_screen_load(s_screens[active_page]);
    radio_ui_screensaver_stage_publish();

    s_view_handles = &s_previous_view_handles;
    ui_view_handles_discard_current();
    memset(&s_previous_view_handles, 0, sizeof(s_previous_view_handles));
    s_view_handles = &s_ui_view_handles;
}

static int ui_active_page(void)
{
    const lv_obj_t *active = lv_screen_active();
    for (int i = PAGE_LISTEN; i <= PAGE_SETTINGS; ++i) {
        if (active == s_ui_view_handles.screens[i]) return i;
    }
    return PAGE_LISTEN;
}

static esp_err_t ui_theme_rebuild(bool persist)
{
    const int active_page = ui_active_page();
    const int requested_theme =
        radio_ui_theme_normalize_id(s_requested_theme_id);
    const int requested_accent =
        radio_ui_accent_normalize_id(s_requested_accent_id);
    const esp_err_t err = ui_views_stage_build(requested_theme,
                                               requested_accent);
    if (err != ESP_OK) {
        s_requested_theme_id = s_theme_id;
        s_requested_accent_id = s_accent_id;
        ESP_LOGE(TAG, "Falló el staging de vistas/tema: %s",
                 esp_err_to_name(err));
        return err;
    }

    s_theme_id = requested_theme;
    s_accent_id = requested_accent;
    s_theme = radio_ui_theme_palette(s_theme_id, s_accent_id);
    s_idle_render_initialized = false;
    s_idle_rendered_display = RADIO_UI_IDLE_DISPLAY_NORMAL;
    s_idle_rendered_screensaver_style = -1;
    ui_views_publish_staging(active_page);

    const radio_ui_idle_snapshot_t idle = idle_state_snapshot();
    idle_render_display(&idle);
    if (persist) {
        const esp_err_t store_err = radio_station_store_set_ui_appearance(
            s_theme_id, s_accent_id, idle.screensaver_style);
        if (store_err != ESP_OK) {
            ESP_LOGE(TAG, "No se pudo guardar la apariencia confirmada: %s",
                     esp_err_to_name(store_err));
        }
    }
    s_requested_theme_id = s_theme_id;
    s_requested_accent_id = s_accent_id;
    return ESP_OK;
}

#ifdef RADIO_UI_TEST_FAULT_INJECTION
static esp_err_t ui_views_build(void)
{
    const esp_err_t err = ui_views_stage_build(s_theme_id, s_accent_id);
    if (err != ESP_OK) return err;
    ui_views_publish_staging(PAGE_LISTEN);
    return ESP_OK;
}
#endif

/* Only before publishing views and before the sleep task has started. */
static void ui_init_discard_unpublished(void)
{
    if (s_header_status_timer != NULL) lv_timer_delete(s_header_status_timer);
    s_header_status_timer = NULL;
    if (s_volume_sync_timer != NULL) lv_timer_delete(s_volume_sync_timer);
    s_volume_sync_timer = NULL;
    if (s_catalog_result_timer != NULL) lv_timer_delete(s_catalog_result_timer);
    s_catalog_result_timer = NULL;
    if (s_catalog_result_queue != NULL) vQueueDelete(s_catalog_result_queue);
    s_catalog_result_queue = NULL;
    if (s_favorite_mutation_timer != NULL) lv_timer_delete(s_favorite_mutation_timer);
    s_favorite_mutation_timer = NULL;
    if (s_favorite_mutation_queue != NULL) vQueueDelete(s_favorite_mutation_queue);
    s_favorite_mutation_queue = NULL;
    if (s_wifi_status_timer != NULL) lv_timer_delete(s_wifi_status_timer);
    s_wifi_status_timer = NULL;
    if (s_wifi_status_queue != NULL) vQueueDelete(s_wifi_status_queue);
    s_wifi_status_queue = NULL;
    if (s_wifi_portal_result_queue != NULL) vQueueDelete(s_wifi_portal_result_queue);
    s_wifi_portal_result_queue = NULL;
    if (s_player_status_timer != NULL) lv_timer_delete(s_player_status_timer);
    s_player_status_timer = NULL;
    if (s_player_status_queue != NULL) vQueueDelete(s_player_status_queue);
    s_player_status_queue = NULL;
    if (s_idle_event_timer != NULL) lv_timer_delete(s_idle_event_timer);
    s_idle_event_timer = NULL;
    ui_staging_views_discard();
    heap_caps_free(s_stations);
    s_stations = NULL;
    s_station_capacity = 0;
}

esp_err_t radio_ui_init(void)
{
    if (s_screens[PAGE_LISTEN] != NULL || s_screens[PAGE_RADIOS] != NULL ||
        s_screens[PAGE_SETTINGS] != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    setenv("TZ", "ART3", 1);
    tzset();
#ifdef RADIO_UI_TEST_FAULT_INJECTION
    s_stations = s_test_fail_station_buffer ? NULL :
        heap_caps_calloc(UI_STATION_LIMIT, sizeof(*s_stations),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    s_stations = heap_caps_calloc(UI_STATION_LIMIT, sizeof(*s_stations),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (s_stations != NULL) {
        s_station_capacity = UI_STATION_LIMIT;
    } else {
        s_station_capacity = 0;
        ESP_LOGE(TAG, "No se pudo reservar el catalogo en PSRAM");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Catalogo %u radios; RAM interna libre %u, bloque mayor %u",
             (unsigned)s_station_capacity,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    const int last_view = radio_station_store_last_view();
    s_radio_tab = last_view >= RADIO_TAB_FAVORITES &&
                          last_view <= RADIO_TAB_EXPLORE
                      ? (radio_tab_t)last_view : RADIO_TAB_FAVORITES;
    s_station_count = 0;
    s_showing_catalog_results = false;
    s_explore_category_open = false;
    if (s_station_capacity > 0 && s_radio_tab == RADIO_TAB_FAVORITES) {
        s_station_count =
            radio_station_store_favorites(s_stations, s_station_capacity);
    } else if (s_station_capacity > 0 && s_radio_tab == RADIO_TAB_RECENTS) {
        s_station_count =
            radio_station_store_recents(s_stations, s_station_capacity);
    }
    const int sleep_preset_minutes = radio_station_store_sleep_preset();
    const int screen_brightness = radio_station_store_screen_brightness();
    const int screen_timeout_seconds =
        radio_station_store_screen_timeout_seconds();
    s_theme_id = radio_station_store_ui_theme();
    s_accent_id = radio_station_store_ui_accent();
    const int screensaver_style = radio_station_store_screensaver_style();
    radio_ui_idle_state_init(&s_idle_state, (uint32_t)xTaskGetTickCount(),
                             sleep_preset_minutes, screen_brightness,
                             screen_timeout_seconds, screensaver_style);
    s_idle_render_initialized = false;
    ui_theme_apply_values();

    const esp_err_t view_err = ui_views_stage_build(s_theme_id, s_accent_id);
    if (view_err != ESP_OK) {
        heap_caps_free(s_stations);
        s_stations = NULL;
        s_station_capacity = 0;
        ESP_LOGE(TAG, "No se pudieron crear las vistas iniciales: %s",
                 esp_err_to_name(view_err));
        return view_err;
    }

    /* Create essential idle resources before publishing the staged view. */
#ifdef RADIO_UI_TEST_FAULT_INJECTION
    s_idle_event_timer = s_test_fail_idle_timer
                             ? NULL : lv_timer_create(idle_event_timer_cb, 20, NULL);
#else
    s_idle_event_timer = lv_timer_create(idle_event_timer_cb, 20, NULL);
#endif
    if (s_idle_event_timer == NULL) {
        ESP_LOGE(TAG, "No se pudo crear el consumidor UI de reposo/actividad");
        ui_init_discard_unpublished();
        return ESP_ERR_NO_MEM;
    }
    s_player_status_queue = xQueueCreateStatic(
        1, sizeof(player_ui_status_event_t),
        (uint8_t *)s_player_status_queue_storage,
        &s_player_status_queue_state);
    if (s_player_status_queue != NULL) {
#ifdef RADIO_UI_TEST_FAULT_INJECTION
        s_player_status_timer = s_test_fail_player_timer
                                    ? NULL : lv_timer_create(player_status_timer_cb, 20, NULL);
#else
        s_player_status_timer = lv_timer_create(player_status_timer_cb, 20, NULL);
#endif
    }
    if (s_player_status_queue == NULL || s_player_status_timer == NULL) {
        ESP_LOGE(TAG, "No se pudo crear el buzon de estado del reproductor");
        ui_init_discard_unpublished();
        return ESP_ERR_NO_MEM;
    }
    s_wifi_status_queue = xQueueCreateStatic(
        1, sizeof(uint8_t), s_wifi_status_queue_storage,
        &s_wifi_status_queue_state);
    s_wifi_portal_result_queue = xQueueCreateStatic(
        1, sizeof(esp_err_t), (uint8_t *)s_wifi_portal_result_queue_storage,
        &s_wifi_portal_result_queue_state);
    if (s_wifi_status_queue != NULL && s_wifi_portal_result_queue != NULL) {
#ifdef RADIO_UI_TEST_FAULT_INJECTION
        s_wifi_status_timer = s_test_fail_wifi_timer
                                  ? NULL : lv_timer_create(wifi_status_timer_cb, 20, NULL);
#else
        s_wifi_status_timer = lv_timer_create(wifi_status_timer_cb, 20, NULL);
#endif
    }
    if (s_wifi_status_queue == NULL || s_wifi_portal_result_queue == NULL ||
        s_wifi_status_timer == NULL) {
        ESP_LOGE(TAG, "No se pudo crear el buzon de estado Wi-Fi");
        ui_init_discard_unpublished();
        return ESP_ERR_NO_MEM;
    }
    s_favorite_mutation_queue = xQueueCreateStatic(
        1, sizeof(favorite_mutation_event_t),
        (uint8_t *)s_favorite_mutation_queue_storage,
        &s_favorite_mutation_queue_state);
    if (s_favorite_mutation_queue != NULL) {
#ifdef RADIO_UI_TEST_FAULT_INJECTION
        s_favorite_mutation_timer = s_test_fail_favorite_timer
            ? NULL : lv_timer_create(favorite_mutation_timer_cb, 20, NULL);
#else
        s_favorite_mutation_timer = lv_timer_create(favorite_mutation_timer_cb, 20, NULL);
#endif
    }
    if (s_favorite_mutation_queue == NULL || s_favorite_mutation_timer == NULL) {
        ESP_LOGE(TAG, "No se pudo crear el buzon de cambios de favoritas");
        ui_init_discard_unpublished();
        return ESP_ERR_NO_MEM;
    }
    s_catalog_result_queue = xQueueCreate(1, sizeof(catalog_ui_result_t *));
    if (s_catalog_result_queue != NULL) {
#ifdef RADIO_UI_TEST_FAULT_INJECTION
        s_catalog_result_timer = s_test_fail_catalog_timer
            ? NULL : lv_timer_create(catalog_result_timer_cb, 20, NULL);
#else
        s_catalog_result_timer = lv_timer_create(catalog_result_timer_cb, 20, NULL);
#endif
    }
    if (s_catalog_result_queue == NULL || s_catalog_result_timer == NULL) {
        ESP_LOGE(TAG, "No se pudo crear el buzon UI del catalogo");
        ui_init_discard_unpublished();
        return ESP_ERR_NO_MEM;
    }
#ifdef RADIO_UI_TEST_FAULT_INJECTION
    s_volume_sync_timer = s_test_fail_volume_timer
                              ? NULL : lv_timer_create(volume_sync_timer_cb, 100, NULL);
#else
    s_volume_sync_timer = lv_timer_create(volume_sync_timer_cb, 100, NULL);
#endif
    if (s_volume_sync_timer == NULL) {
        ESP_LOGE(TAG, "No se pudo crear el sincronizador de volumen");
        ui_init_discard_unpublished();
        return ESP_ERR_NO_MEM;
    }
#ifdef RADIO_UI_TEST_FAULT_INJECTION
    s_header_status_timer = s_test_fail_header_timer
                                ? NULL : lv_timer_create(header_status_timer_cb, 30000, NULL);
#else
    s_header_status_timer = lv_timer_create(header_status_timer_cb, 30000, NULL);
#endif
    if (s_header_status_timer == NULL) {
        ESP_LOGE(TAG, "No se pudo crear el temporizador de encabezado");
        ui_init_discard_unpublished();
        return ESP_ERR_NO_MEM;
    }
#ifdef RADIO_UI_TEST_FAULT_INJECTION
    const BaseType_t task_created = s_test_fail_idle_task
        ? pdFAIL
        : xTaskCreate(sleep_timer_task, "radio_sleep_timer", 3072, NULL, 3,
                      &s_sleep_task_handle);
#else
    const BaseType_t task_created =
        xTaskCreate(sleep_timer_task, "radio_sleep_timer", 3072, NULL, 3,
                    &s_sleep_task_handle);
#endif
    if (task_created != pdPASS) {
        s_sleep_task_handle = NULL;
        ESP_LOGE(TAG, "No se pudo crear el temporizador de reposo");
        ui_init_discard_unpublished();
        return ESP_ERR_NO_MEM;
    }
    ui_views_publish_staging(PAGE_LISTEN);
    if (radio_station_store_get_current(&s_current_station)) {
        s_has_current_station = true;
        radio_player_restore_station(&s_current_station);
    }
    update_home_station();
    lv_screen_load(s_screens[PAGE_LISTEN]);
    volume_status_update();
    header_status_refresh();
    if (lv_timer_create(battery_charge_timer_cb, 450, NULL) != NULL) {
        battery_charge_refresh();
    } else {
        /* Keep the optional charge indicator hidden rather than stale. */
        if (s_home_battery_charge_icon != NULL) {
            lv_obj_set_hidden(s_home_battery_charge_icon, true);
        }
        ESP_LOGW(TAG, "Indicador de carga no disponible");
    }
    update_wifi_settings_info();
    timer_update_label();
    update_screen_choice_labels();
    idle_apply_pending();
    return ESP_OK;
}

#ifdef RADIO_UI_TEST_FAULT_INJECTION
bool radio_ui_test_unpublished_resources_clear(void)
{
    return s_stations == NULL && s_sleep_task_handle == NULL &&
           s_idle_event_timer == NULL && s_player_status_timer == NULL &&
           s_player_status_queue == NULL && s_wifi_status_timer == NULL &&
           s_wifi_status_queue == NULL && s_wifi_portal_result_queue == NULL &&
           s_favorite_mutation_timer == NULL && s_favorite_mutation_queue == NULL &&
           s_catalog_result_timer == NULL && s_catalog_result_queue == NULL &&
           s_volume_sync_timer == NULL && s_header_status_timer == NULL;
}

esp_err_t radio_ui_test_init_with_header_failure(void)
{
    heap_caps_free(s_stations);
    s_stations = NULL;
    s_station_capacity = 0;
    s_station_count = 0;
    s_test_fail_header_timer = true;
    const esp_err_t err = radio_ui_init();
    s_test_fail_header_timer = false;
    return err;
}

esp_err_t radio_ui_test_init_with_station_buffer_failure(void)
{
    heap_caps_free(s_stations);
    s_stations = NULL;
    s_station_capacity = 0;
    s_station_count = 0;
    s_test_fail_station_buffer = true;
    const esp_err_t err = radio_ui_init();
    s_test_fail_station_buffer = false;
    return err;
}

esp_err_t radio_ui_test_init_with_volume_failure(void)
{
    heap_caps_free(s_stations);
    s_stations = NULL;
    s_station_capacity = 0;
    s_station_count = 0;
    s_test_fail_volume_timer = true;
    const esp_err_t err = radio_ui_init();
    s_test_fail_volume_timer = false;
    return err;
}

esp_err_t radio_ui_test_init_with_catalog_failure(void)
{
    heap_caps_free(s_stations);
    s_stations = NULL;
    s_station_capacity = 0;
    s_station_count = 0;
    s_test_fail_catalog_timer = true;
    const esp_err_t err = radio_ui_init();
    s_test_fail_catalog_timer = false;
    return err;
}

esp_err_t radio_ui_test_init_with_favorite_failure(void)
{
    heap_caps_free(s_stations);
    s_stations = NULL;
    s_station_capacity = 0;
    s_station_count = 0;
    s_test_fail_favorite_timer = true;
    const esp_err_t err = radio_ui_init();
    s_test_fail_favorite_timer = false;
    return err;
}

esp_err_t radio_ui_test_init_with_wifi_failure(void)
{
    heap_caps_free(s_stations);
    s_stations = NULL;
    s_station_capacity = 0;
    s_station_count = 0;
    s_test_fail_wifi_timer = true;
    const esp_err_t err = radio_ui_init();
    s_test_fail_wifi_timer = false;
    return err;
}

esp_err_t radio_ui_test_init_with_player_failure(void)
{
    heap_caps_free(s_stations);
    s_stations = NULL;
    s_station_capacity = 0;
    s_station_count = 0;
    s_test_fail_player_timer = true;
    const esp_err_t err = radio_ui_init();
    s_test_fail_player_timer = false;
    return err;
}

esp_err_t radio_ui_test_init_with_idle_failure(bool fail_timer)
{
    heap_caps_free(s_stations);
    s_stations = NULL;
    s_station_capacity = 0;
    s_station_count = 0;
    s_test_fail_idle_timer = fail_timer;
    s_test_fail_idle_task = !fail_timer;
    const esp_err_t err = radio_ui_init();
    s_test_fail_idle_timer = false;
    s_test_fail_idle_task = false;
    return err;
}

bool radio_ui_test_wifi_out_of_order(const radio_wifi_info_t *newer,
                                     const radio_wifi_info_t *older)
{
    if (newer == NULL || older == NULL || s_settings_status_label == NULL)
        return false;
    s_wifi_status_queue = xQueueCreateStatic(
        1, sizeof(uint8_t), s_wifi_status_queue_storage, &s_wifi_status_queue_state);
    s_wifi_portal_result_queue = xQueueCreateStatic(
        1, sizeof(esp_err_t), (uint8_t *)s_wifi_portal_result_queue_storage,
        &s_wifi_portal_result_queue_state);
    if (s_wifi_status_queue == NULL || s_wifi_portal_result_queue == NULL)
        return false;
    s_wifi_status_generation_valid = false;
    wifi_status_callback(newer);
    const esp_err_t portal_result = ESP_FAIL;
    (void)xQueueOverwrite(s_wifi_portal_result_queue, &portal_result);
    wifi_status_callback(older);
    wifi_status_timer_cb(NULL);
    const bool valid = s_wifi_status_generation_valid &&
                       s_wifi_status_generation == newer->status_generation &&
                       s_wifi_connected == newer->connected &&
                       strcmp(lv_label_get_text(s_settings_status_label),
                              "No se pudo iniciar el portal Wi-Fi") == 0;
    vQueueDelete(s_wifi_status_queue);
    vQueueDelete(s_wifi_portal_result_queue);
    s_wifi_status_queue = NULL;
    s_wifi_portal_result_queue = NULL;
    return valid;
}

bool radio_ui_test_failed_catalog_navigation_keeps_state(void)
{
    const bool previous_results = s_showing_catalog_results;
    const bool previous_popular = s_catalog_popular;
    char previous_query[sizeof(s_catalog_query)];
    memcpy(previous_query, s_catalog_query, sizeof(previous_query));
    s_test_fail_radio_refresh = true;
    begin_catalog_search("fault-test", "Fault test", !previous_popular);
    s_test_fail_radio_refresh = false;
    return s_showing_catalog_results == previous_results &&
           s_catalog_popular == previous_popular &&
           memcmp(s_catalog_query, previous_query, sizeof(previous_query)) == 0;
}

bool radio_ui_test_home_station_scroll_configured(void)
{
    if (s_home_station_label == NULL ||
        lv_label_get_long_mode(s_home_station_label) !=
            LV_LABEL_LONG_MODE_SCROLL) {
        return false;
    }
    const lv_anim_t *anim = lv_obj_get_style_anim(s_home_station_label,
                                                   LV_PART_MAIN);
    return anim != NULL && anim->repeat_cnt == LV_ANIM_REPEAT_INFINITE &&
           anim->repeat_delay == 900U && anim->reverse_delay == 900U;
}

static bool radio_ui_test_tree_layout_valid(lv_obj_t *object, unsigned depth)
{
    if (object == NULL || depth > 16U) return false;
    lv_obj_update_layout(object);

    const int32_t width = lv_obj_get_width(object);
    const int32_t height = lv_obj_get_height(object);
    if (width < 0 || height < 0) return false;
    if (lv_obj_is_clickable(object) &&
        (width < UI_MIN_TOUCH_TARGET_PX || height < UI_MIN_TOUCH_TARGET_PX)) {
        return false;
    }
    if (lv_obj_check_type(object, &lv_button_class)) {
        lv_obj_t *label = lv_obj_get_child(object, 0);
        if (label != NULL) {
            lv_obj_update_layout(label);
            const lv_font_t *font =
                lv_obj_get_style_text_font(label, LV_PART_MAIN);
            if (font == NULL ||
                lv_obj_get_height(label) !=
                    (int32_t)lv_font_get_line_height(font)) {
                return false;
            }
            const int32_t center_x_error =
                2 * lv_obj_get_x(label) + lv_obj_get_width(label) - width;
            const int32_t center_y_error =
                2 * lv_obj_get_y(label) + lv_obj_get_height(label) - height;
            if (center_x_error < -2 || center_x_error > 2 ||
                center_y_error < -2 || center_y_error > 2) {
                return false;
            }
        }
    }

    lv_obj_t *parent = lv_obj_get_parent(object);
    if (parent != NULL) {
        lv_obj_update_layout(parent);
        const int32_t x = lv_obj_get_x(object);
        const int32_t y = lv_obj_get_y(object);
        const int32_t parent_width = lv_obj_get_width(parent);
        const int32_t parent_height = lv_obj_get_height(parent);
        if (x < 0 || x + width > parent_width) return false;
        if (!lv_obj_is_scrollable(parent) &&
            (y < 0 || y + height > parent_height)) {
            return false;
        }
    }

    const uint32_t child_count = lv_obj_get_child_count(object);
    for (uint32_t i = 0; i < child_count; ++i) {
        if (!radio_ui_test_tree_layout_valid(lv_obj_get_child(object, i),
                                             depth + 1U)) {
            return false;
        }
    }
    return true;
}

static bool radio_ui_test_boxes_overlap(const lv_obj_t *first,
                                        const lv_obj_t *second)
{
    if (first == NULL || second == NULL) return true;
    const int32_t first_x = lv_obj_get_x(first);
    const int32_t first_y = lv_obj_get_y(first);
    const int32_t second_x = lv_obj_get_x(second);
    const int32_t second_y = lv_obj_get_y(second);
    return first_x < second_x + lv_obj_get_width(second) &&
           second_x < first_x + lv_obj_get_width(first) &&
           first_y < second_y + lv_obj_get_height(second) &&
           second_y < first_y + lv_obj_get_height(first);
}

bool radio_ui_test_layout_contract(void)
{
    for (size_t i = 0; i < sizeof(s_ui_view_handles.screens) /
                            sizeof(s_ui_view_handles.screens[0]); ++i) {
        lv_obj_t *screen = s_ui_view_handles.screens[i];
        if (screen == NULL) return false;
        lv_obj_update_layout(screen);
        if (lv_obj_get_width(screen) != 240 ||
            lv_obj_get_height(screen) != 240 ||
            !radio_ui_test_tree_layout_valid(screen, 0)) {
            return false;
        }
    }
    if (s_ui_view_handles.error_overlay != NULL &&
        !radio_ui_test_tree_layout_valid(s_ui_view_handles.error_overlay, 0)) {
        return false;
    }
    if (s_ui_view_handles.modal_overlay != NULL &&
        !radio_ui_test_tree_layout_valid(s_ui_view_handles.modal_overlay, 0)) {
        return false;
    }
    const lv_font_t *settings_info_font = lv_obj_get_style_text_font(
        s_ui_view_handles.settings_info_label, LV_PART_MAIN);
    if (settings_info_font == NULL ||
        lv_obj_get_height(s_ui_view_handles.settings_info_label) <
            2 * (int32_t)lv_font_get_line_height(settings_info_font)) {
        return false;
    }
    return !radio_ui_test_boxes_overlap(s_ui_view_handles.home_station_label,
                                        s_ui_view_handles.favorite_button) &&
           !radio_ui_test_boxes_overlap(s_ui_view_handles.home_meta_label,
                                        s_ui_view_handles.favorite_button) &&
           !radio_ui_test_boxes_overlap(s_ui_view_handles.home_playback_label,
                                        s_ui_view_handles.favorite_button) &&
           !radio_ui_test_boxes_overlap(s_ui_view_handles.settings_status_label,
                                        s_ui_view_handles.settings_info_label) &&
           !radio_ui_test_boxes_overlap(s_ui_view_handles.settings_info_label,
                                        s_ui_view_handles.settings_list);
}

esp_err_t radio_ui_test_build_views(void)
{
    radio_ui_test_discard_views();
    radio_ui_idle_state_init(&s_idle_state, (uint32_t)xTaskGetTickCount(),
                             0, 70, 0, 0);
    ui_theme_apply_values();
    return ui_views_build();
}

void radio_ui_test_discard_views(void)
{
    lv_obj_t *active = lv_screen_active();
    bool active_is_published = false;
    for (size_t i = 0; i < sizeof(s_ui_view_handles.screens) /
                            sizeof(s_ui_view_handles.screens[0]); ++i) {
        if (active != NULL && active == s_ui_view_handles.screens[i]) {
            active_is_published = true;
            break;
        }
    }
    if (active_is_published) {
        if (s_test_fallback_screen == NULL) {
            s_test_fallback_screen = lv_obj_create(NULL);
        }
        if (s_test_fallback_screen == NULL) return;
        lv_screen_load(s_test_fallback_screen);
    }
    ui_staging_views_discard();
    ui_views_discard();
}

static bool test_view_handles_are_clear(ui_view_handles_t *handles)
{
    ui_view_handles_t *previous_handles = s_view_handles;
    s_view_handles = handles;
    if (s_screens[PAGE_LISTEN] != NULL || s_screens[PAGE_RADIOS] != NULL ||
        s_screens[PAGE_SETTINGS] != NULL || s_modal_overlay != NULL ||
        s_error_overlay != NULL) {
        s_view_handles = previous_handles;
        return false;
    }
    if (s_home_wifi_label != NULL || s_home_station_label != NULL ||
        s_home_playback_label != NULL || s_home_meta_label != NULL ||
        s_home_volume_label != NULL || s_home_volume_bar != NULL ||
        s_home_time_label != NULL || s_home_battery_body != NULL ||
        s_home_battery_fill != NULL || s_home_battery_cap != NULL ||
        s_home_battery_label != NULL || s_home_battery_charge_icon != NULL ||
        s_favorite_button != NULL || s_play_button_label != NULL ||
        s_radio_status_label != NULL || s_radio_list != NULL ||
        s_radio_back_button != NULL || s_radio_result_title != NULL ||
        s_load_more_button != NULL || s_load_previous_button != NULL ||
        s_settings_status_label != NULL || s_settings_info_label != NULL ||
        s_settings_list != NULL || s_timer_button_label != NULL ||
        s_screen_button_label != NULL || s_error_message != NULL ||
        s_portal_qr_code != NULL || s_portal_qr_hint != NULL ||
        s_portal_qr_manual_label != NULL ||
        s_portal_qr_toggle_label != NULL ||
        s_screen_brightness_choice_label != NULL ||
        s_screen_timeout_choice_label != NULL || s_screensaver_choice_label != NULL ||
        s_clock_settings_value_label != NULL) {
        s_view_handles = previous_handles;
        return false;
    }
    for (size_t i = 0; i < RADIO_TAB_COUNT; ++i) {
        if (s_radio_tab_buttons[i] != NULL) {
            s_view_handles = previous_handles;
            return false;
        }
    }
    for (size_t i = 0; i < UI_STATION_LIMIT; ++i) {
        if (s_station_row_contexts[i].in_use ||
            s_station_row_contexts[i].row != NULL) {
            s_view_handles = previous_handles;
            return false;
        }
    }
    s_view_handles = previous_handles;
    return true;
}

bool radio_ui_test_views_are_clear(void)
{
    return test_view_handles_are_clear(&s_ui_view_handles) &&
           test_view_handles_are_clear(&s_staging_view_handles) &&
           !radio_ui_screensaver_exists();
}

bool radio_ui_test_staging_is_clear(void)
{
    return test_view_handles_are_clear(&s_staging_view_handles);
}

esp_err_t radio_ui_test_rebuild_theme(void)
{
    s_requested_theme_id = (s_theme_id + 1) % RADIO_UI_THEME_COUNT;
    s_requested_accent_id = (s_accent_id + 1) % RADIO_UI_ACCENT_COUNT;
    return ui_theme_rebuild(false);
}

void radio_ui_test_load_page(unsigned page)
{
    if (page <= PAGE_SETTINGS && s_ui_view_handles.screens[page] != NULL) {
        lv_screen_load(s_ui_view_handles.screens[page]);
    }
}

void *radio_ui_test_active_screen(void)
{
    return lv_screen_active();
}

size_t radio_ui_test_view_root_count(void)
{
    return s_test_view_root_count;
}

int radio_ui_test_theme_id(void)
{
    return s_theme_id;
}

int radio_ui_test_accent_id(void)
{
    return s_accent_id;
}

uint32_t radio_ui_test_catalog_request_id(void)
{
    return s_catalog_request_id;
}

size_t radio_ui_test_station_count(void)
{
    return s_station_count;
}

void radio_ui_test_build_modal(unsigned modal_id)
{
    if (s_screens[PAGE_SETTINGS] == NULL) return;
    lv_screen_load(s_screens[PAGE_SETTINGS]);
    switch (modal_id) {
    case RADIO_UI_TEST_MODAL_APPEARANCE: show_appearance_settings(NULL); break;
    case RADIO_UI_TEST_MODAL_SCREEN: show_screen_settings(NULL); break;
    case RADIO_UI_TEST_MODAL_CLOCK: show_clock_settings(NULL); break;
    case RADIO_UI_TEST_MODAL_SLEEP_TIMER: show_sleep_timer(NULL); break;
    case RADIO_UI_TEST_MODAL_PORTAL_QR: (void)show_portal_qr(); break;
    default: break;
    }
}

void radio_ui_test_close_modal(void)
{
    close_overlay_event(NULL);
}

bool radio_ui_test_modal_exists(void)
{
    return s_modal_overlay != NULL;
}

uint32_t radio_ui_test_settings_child_count(void)
{
    return s_screens[PAGE_SETTINGS] != NULL
               ? lv_obj_get_child_count(s_screens[PAGE_SETTINGS]) : 0;
}

esp_err_t radio_ui_test_build_dynamic(unsigned builder_id)
{
    static const radio_station_t station = {
        .name = "UI fault fixture",
        .country = "Test",
        .codec = "MP3",
        .url = "https://example.invalid/test.mp3",
        .uuid = "ui-fault-fixture",
    };
    if (s_radio_list == NULL) return ESP_ERR_INVALID_STATE;
    switch (builder_id) {
    case RADIO_UI_TEST_DYNAMIC_STATION_ROW:
        return station_row_create(&station, 0) ? ESP_OK : ESP_ERR_NO_MEM;
    case RADIO_UI_TEST_DYNAMIC_EXPLORE_ROW:
        return explore_row_create("UI fault fixture", explore_option_event,
                                  -2, false);
    case RADIO_UI_TEST_DYNAMIC_EMPTY_STATE:
        return empty_saved_state_create("Sin radios", "Prueba de asignacion");
    case RADIO_UI_TEST_DYNAMIC_CATALOG_FAILURE:
        return catalog_show_failure(ESP_ERR_NO_MEM);
    case RADIO_UI_TEST_DYNAMIC_PREVIOUS_BUTTON:
        s_catalog_page_count = 1;
        s_catalog_page_offsets[0] = RADIO_CATALOG_PAGE_SIZE;
        return catalog_previous_button_create();
    case RADIO_UI_TEST_DYNAMIC_EXPLORE_OPTIONS:
        s_radio_tab = RADIO_TAB_EXPLORE;
        s_explore_category_open = false;
        return populate_explore_options();
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

uint32_t radio_ui_test_radio_list_child_count(void)
{
    return s_radio_list != NULL ? lv_obj_get_child_count(s_radio_list) : 0;
}

esp_err_t radio_ui_test_seed_stations(const radio_station_t *stations,
                                      size_t count)
{
    if (count > UI_STATION_LIMIT) return ESP_ERR_INVALID_ARG;
    if (count > 0 && stations == NULL) return ESP_ERR_INVALID_ARG;
    if (s_stations == NULL) {
        s_stations = heap_caps_calloc(UI_STATION_LIMIT, sizeof(*s_stations),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_stations == NULL) return ESP_ERR_NO_MEM;
        s_station_capacity = UI_STATION_LIMIT;
    }
    if (count > s_station_capacity) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < count; ++i) {
        s_stations[i] = stations[i];
    }
    s_station_count = count;
    /* The seeded rows are rendered from the Favorites tab; force that tab so
       the fixture does not depend on whatever tab a previous test selected. */
    s_radio_tab = RADIO_TAB_FAVORITES;
    s_showing_catalog_results = false;
    s_explore_category_open = false;
    return ESP_OK;
}

bool radio_ui_test_row_contexts_resolve(void)
{
    if (s_radio_list == NULL) return false;
    bool found = false;
    const uint32_t rows = lv_obj_get_child_count(s_radio_list);
    for (uint32_t r = 0; r < rows; ++r) {
        lv_obj_t *row = lv_obj_get_child(s_radio_list, r);
        const uint32_t events = lv_obj_get_event_count(row);
        for (uint32_t e = 0; e < events; ++e) {
            lv_event_dsc_t *dsc = lv_obj_get_event_dsc(row, e);
            if (dsc == NULL || lv_event_dsc_get_cb(dsc) != station_selected) {
                continue;
            }
            const size_t index =
                (size_t)(uintptr_t)lv_event_dsc_get_user_data(dsc);
            if (index >= UI_STATION_LIMIT) return false;
            const station_row_context_t *context = &s_station_row_contexts[index];
            if (!context->in_use || context->row != row) return false;
            found = true;
        }
    }
    return found;
}
#endif

void radio_ui_bind_services(void)
{
    radio_wifi_set_status_callback(wifi_status_callback);
    radio_catalog_set_callback(catalog_callback);
    radio_player_set_status_callback(player_status_callback);
    radio_station_store_set_error_callback(store_error_callback, NULL);
}

void radio_ui_refresh_volume(void)
{
    radio_ui_wake_display();
}
