#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "radio_station_store.h"
#include "radio_station_record_codec.h"
#include "radio_snapshot_store.h"
#include "radio_lock_diagnostics.h"
#include "radio_nvs_worker.h"
#include "radio_test_adapters.h"
#include "radio_task_supervisor.h"

#define STORE_NAMESPACE "radio_lib"
#define STORE_SCHEMA_VERSION_KEY "schema_ver"
#define STORE_SCHEMA_VERSION 2U
#define STORE_MAGIC RADIO_STATION_RECORD_MAGIC
#define STORE_VERSION RADIO_STATION_RECORD_VERSION
#define STORE_LEGACY_VERSION 1u
#define STORE_HEADER_SIZE RADIO_STATION_RECORD_HEADER_SIZE
#define STORE_SETTLE_MS 15000u
#define VIEW_SETTLE_MS 5000u
#define VOLUME_SETTLE_MS 2500u
#define PREFERENCE_SETTLE_MS 3000u
#define VOLUME_NAMESPACE "radio_cfg"
#define VOLUME_KEY "volume"
#define LAST_VIEW_KEY "last_view"
#define SLEEP_PRESET_KEY "sleep_preset"
#define SCREEN_BRIGHTNESS_KEY "lcd_brightness"
#define SCREEN_DIM_KEY "lcd_dim_after"
#define SCREEN_TIMEOUT_KEY "lcd_timeout"
#define UI_THEME_KEY "ui_theme"
#define UI_ACCENT_KEY "ui_accent"
#define SCREENSAVER_KEY "clock_style"
#define LAST_VIEW_MAX 2
#define UI_THEME_MAX 2
#define UI_ACCENT_MAX 2
#define SCREENSAVER_STYLE_MAX 2
#define PREFERENCE_SNAPSHOT_SIZE 28U
#define VOLUME_SNAPSHOT_SIZE 4U

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    radio_station_t stations[RADIO_FAVORITES_MAX];
} station_list_record_t;
_Static_assert(offsetof(station_list_record_t, stations) == STORE_HEADER_SIZE,
               "Station record header size changed");

static station_list_record_t s_favorites = {
    .magic = STORE_MAGIC, .version = STORE_VERSION,
};
static station_list_record_t s_recents = {
    .magic = STORE_MAGIC, .version = STORE_VERSION,
};
static station_list_record_t s_recents_confirmed = {
    .magic = STORE_MAGIC, .version = STORE_VERSION,
};
static station_list_record_t s_record_scratch;
static station_list_record_t s_current_record;
static radio_station_t s_current;
static radio_station_t s_current_confirmed;
static bool s_has_current;
static bool s_has_current_confirmed;
static bool s_current_dirty;
static bool s_recents_dirty;
static TickType_t s_station_changed_at;
static bool s_last_view_dirty;
static TickType_t s_last_view_changed_at;
static int s_last_view_confirmed;
static bool s_volume_dirty;
static TickType_t s_volume_changed_at;
static bool s_sleep_preset_dirty;
static bool s_screen_brightness_dirty;
static bool s_screen_timeout_dirty;
static bool s_ui_appearance_dirty;
static TickType_t s_preferences_changed_at;
static int s_volume = 55;
static int s_last_view;
static int s_sleep_preset;
static int s_screen_brightness = 70;
static int s_screen_timeout_seconds = 30;
static int s_ui_theme;
static int s_ui_accent;
static int s_screensaver_style;
static int s_volume_confirmed = 55;
static int s_sleep_preset_confirmed;
static int s_screen_brightness_confirmed = 70;
static int s_screen_timeout_confirmed = 30;
static int s_ui_theme_confirmed;
static int s_ui_accent_confirmed;
static int s_screensaver_style_confirmed;
static SemaphoreHandle_t s_store_lock;
static bool s_initialized;
static radio_station_store_error_cb_t s_error_callback;
static void *s_error_callback_context;
static const char *TAG = "radio_store";

static esp_err_t save_list_to_handle(nvs_handle_t nvs, const char *key,
                                     const station_list_record_t *record);
static esp_err_t save_list_snapshot(const char *key,
                                    const station_list_record_t *record);
static esp_err_t migrate_favorites_snapshot_job(void *context);
static esp_err_t migrate_recents_snapshot_job(void *context);
static esp_err_t migrate_current_snapshot_job(void *context);
static esp_err_t migrate_preferences_snapshot_job(void *context);
static esp_err_t migrate_volume_snapshot_job(void *context);
static void persistence_task(void *arg);

static void restore_cache_defaults(void)
{
    memset(&s_favorites, 0, sizeof(s_favorites));
    memset(&s_recents, 0, sizeof(s_recents));
    memset(&s_recents_confirmed, 0, sizeof(s_recents_confirmed));
    memset(&s_record_scratch, 0, sizeof(s_record_scratch));
    memset(&s_current_record, 0, sizeof(s_current_record));
    s_favorites.magic = STORE_MAGIC;
    s_favorites.version = STORE_VERSION;
    s_recents.magic = STORE_MAGIC;
    s_recents.version = STORE_VERSION;
    s_recents_confirmed.magic = STORE_MAGIC;
    s_recents_confirmed.version = STORE_VERSION;
    memset(&s_current, 0, sizeof(s_current));
    memset(&s_current_confirmed, 0, sizeof(s_current_confirmed));
    s_has_current = false;
    s_has_current_confirmed = false;
    s_current_dirty = false;
    s_recents_dirty = false;
    s_last_view_dirty = false;
    s_last_view_confirmed = 0;
    s_volume_dirty = false;
    s_sleep_preset_dirty = false;
    s_screen_brightness_dirty = false;
    s_screen_timeout_dirty = false;
    s_ui_appearance_dirty = false;
    s_volume = 55;
    s_volume_confirmed = 55;
    s_last_view = 0;
    s_sleep_preset = 0;
    s_sleep_preset_confirmed = 0;
    s_screen_brightness = 70;
    s_screen_brightness_confirmed = 70;
    s_screen_timeout_seconds = 30;
    s_screen_timeout_confirmed = 30;
    s_ui_theme = 0;
    s_ui_theme_confirmed = 0;
    s_ui_accent = 0;
    s_ui_accent_confirmed = 0;
    s_screensaver_style = 0;
    s_screensaver_style_confirmed = 0;
}

static bool station_strings_terminated(const radio_station_t *station)
{
    return station != NULL &&
           memchr(station->name, '\0', sizeof(station->name)) != NULL &&
           memchr(station->country, '\0', sizeof(station->country)) != NULL &&
           memchr(station->codec, '\0', sizeof(station->codec)) != NULL &&
           memchr(station->url, '\0', sizeof(station->url)) != NULL &&
           memchr(station->uuid, '\0', sizeof(station->uuid)) != NULL;
}

static bool station_record_valid(const radio_station_t *station)
{
    return station_strings_terminated(station) && station->name[0] != '\0' &&
           station->url[0] != '\0';
}

static bool same_station(const radio_station_t *left, const radio_station_t *right)
{
    if (left->uuid[0] != '\0' && right->uuid[0] != '\0') {
        return strcmp(left->uuid, right->uuid) == 0;
    }
    return strcmp(left->url, right->url) == 0;
}

static bool same_station_details(const radio_station_t *left,
                                 const radio_station_t *right)
{
    return strcmp(left->name, right->name) == 0 &&
           strcmp(left->country, right->country) == 0 &&
           strcmp(left->codec, right->codec) == 0 &&
           strcmp(left->url, right->url) == 0 &&
           strcmp(left->uuid, right->uuid) == 0;
}

static esp_err_t encode_field(uint8_t **cursor, size_t *remaining,
                              const char *source, size_t capacity)
{
    const size_t length = strnlen(source, capacity);
    if (length == capacity || length + 1 > *remaining) return ESP_ERR_INVALID_SIZE;
    memcpy(*cursor, source, length + 1);
    *cursor += length + 1;
    *remaining -= length + 1;
    return ESP_OK;
}

static bool list_snapshot_identity(const char *key, const char **unit,
                                   uint32_t *kind)
{
    if (key == NULL || unit == NULL || kind == NULL) return false;
    if (strcmp(key, "favorites") == 0) {
        *unit = "favorites";
        *kind = RADIO_SNAPSHOT_KIND_FAVORITES;
        return true;
    }
    if (strcmp(key, "recents") == 0) {
        *unit = "recents";
        *kind = RADIO_SNAPSHOT_KIND_RECENTS;
        return true;
    }
    return false;
}

static esp_err_t load_list_snapshot(const char *key,
                                    station_list_record_t *record,
                                    bool *found)
{
    const char *unit = NULL;
    uint32_t kind = 0U;
    if (record == NULL || found == NULL ||
        !list_snapshot_identity(key, &unit, &kind)) {
        return ESP_ERR_INVALID_ARG;
    }
    *found = false;
    size_t length = sizeof(s_record_scratch);
    uint32_t generation = 0U;
    esp_err_t err = radio_snapshot_store_read(
        unit, kind,
        (uint8_t *)&s_record_scratch, sizeof(s_record_scratch), &length,
        &generation);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    memset(record, 0, sizeof(*record));
    record->magic = STORE_MAGIC;
    record->version = STORE_VERSION;
    err = radio_station_record_decode_v2(
        (const uint8_t *)&s_record_scratch, length, record->stations,
        RADIO_FAVORITES_MAX, &record->count);
    if (err != ESP_OK) {
        memset(record, 0, sizeof(*record));
        record->magic = STORE_MAGIC;
        record->version = STORE_VERSION;
        return ESP_ERR_INVALID_STATE;
    }
    *found = true;
    return ESP_OK;
}

static esp_err_t load_list(nvs_handle_t handle, const char *key,
                           station_list_record_t *record,
                           bool *needs_migration,
                           bool *needs_snapshot_migration)
{
    if (needs_snapshot_migration != NULL)
        *needs_snapshot_migration = false;
    if (strcmp(key, "favorites") == 0 || strcmp(key, "recents") == 0) {
        bool found = false;
        const esp_err_t snapshot_err = load_list_snapshot(key, record, &found);
        if (snapshot_err != ESP_OK) return snapshot_err;
        if (found) return ESP_OK;
    }

    if (handle == 0) return ESP_OK;
    size_t length = sizeof(s_record_scratch);
    memset(&s_record_scratch, 0, sizeof(s_record_scratch));
    esp_err_t err = nvs_get_blob(handle, key, &s_record_scratch, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (length < STORE_HEADER_SIZE || s_record_scratch.magic != STORE_MAGIC ||
        s_record_scratch.count > RADIO_FAVORITES_MAX) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_record_scratch.version == STORE_LEGACY_VERSION) {
        if (length != sizeof(s_record_scratch)) return ESP_ERR_INVALID_STATE;
        for (size_t i = 0; i < s_record_scratch.count; ++i) {
            if (!station_record_valid(&s_record_scratch.stations[i])) {
                return ESP_ERR_INVALID_STATE;
            }
        }
        *record = s_record_scratch;
        record->version = STORE_VERSION;
        if (needs_migration != NULL) *needs_migration = true;
        if (needs_snapshot_migration != NULL &&
            (strcmp(key, "favorites") == 0 ||
             strcmp(key, "recents") == 0)) {
            *needs_snapshot_migration = true;
        }
        return ESP_OK;
    }
    if (s_record_scratch.version != STORE_VERSION) return ESP_ERR_INVALID_STATE;
    memset(record, 0, sizeof(*record));
    record->magic = STORE_MAGIC;
    record->version = STORE_VERSION;
    esp_err_t decode_err = radio_station_record_decode_v2(
        (const uint8_t *)&s_record_scratch, length, record->stations,
        RADIO_FAVORITES_MAX, &record->count);
    if (decode_err != ESP_OK) {
        memset(record, 0, sizeof(*record));
        record->magic = STORE_MAGIC;
        record->version = STORE_VERSION;
        return ESP_ERR_INVALID_STATE;
    }
    if (needs_snapshot_migration != NULL &&
        (strcmp(key, "favorites") == 0 ||
         strcmp(key, "recents") == 0)) {
        *needs_snapshot_migration = true;
    }
    return ESP_OK;
}

static bool valid_sleep_preset(int minutes)
{
    return minutes == 0 || minutes == 15 || minutes == 30 || minutes == 60;
}

static bool valid_screen_brightness(int percent)
{
    return percent == 35 || percent == 70 || percent == 100;
}

static bool valid_screen_timeout(int seconds)
{
    return seconds == 0 || seconds == 5 || seconds == 15 || seconds == 30;
}

static bool valid_choice(int value, int maximum)
{
    return value >= 0 && value <= maximum;
}

static void write_u32_le(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8U);
    output[2] = (uint8_t)(value >> 16U);
    output[3] = (uint8_t)(value >> 24U);
}

static uint32_t read_u32_le(const uint8_t *input)
{
    return (uint32_t)input[0] |
           ((uint32_t)input[1] << 8U) |
           ((uint32_t)input[2] << 16U) |
           ((uint32_t)input[3] << 24U);
}

static esp_err_t load_current_station(nvs_handle_t nvs,
                                      bool *needs_snapshot_migration)
{
    if (needs_snapshot_migration != NULL) {
        *needs_snapshot_migration = false;
    }
    size_t snapshot_size = sizeof(s_record_scratch);
    uint32_t generation = 0U;
    esp_err_t err = radio_snapshot_store_read(
        "current", RADIO_SNAPSHOT_KIND_CURRENT_STATION,
        (uint8_t *)&s_record_scratch, sizeof(s_record_scratch),
        &snapshot_size, &generation);
    if (err == ESP_OK) {
        uint16_t count = 0U;
        err = radio_station_record_decode_v2(
            (const uint8_t *)&s_record_scratch, snapshot_size,
            s_current_record.stations, RADIO_FAVORITES_MAX, &count);
        if (err != ESP_OK || count != 1U ||
            !station_record_valid(&s_current_record.stations[0])) {
            return ESP_ERR_INVALID_STATE;
        }
        s_current = s_current_record.stations[0];
        s_has_current = true;
        return ESP_OK;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) return err;
    if (nvs == 0) return ESP_OK;

    size_t current_size = sizeof(s_current);
    err = nvs_get_blob(nvs, "current", &s_current, &current_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    if (current_size != sizeof(s_current) ||
        !station_record_valid(&s_current)) {
        memset(&s_current, 0, sizeof(s_current));
        return ESP_ERR_INVALID_STATE;
    }
    s_has_current = true;
    if (needs_snapshot_migration != NULL) {
        *needs_snapshot_migration = true;
    }
    return ESP_OK;
}

static esp_err_t load_station_state(nvs_handle_t nvs,
                                    bool *favorites_migration,
                                    bool *recents_migration,
                                    bool *favorites_snapshot_migration,
                                    bool *recents_snapshot_migration,
                                    bool *current_snapshot_migration)
{
    esp_err_t err = load_list(nvs, "favorites", &s_favorites,
                              favorites_migration,
                              favorites_snapshot_migration);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Lista de favoritas invalida; se conserva el resto de ajustes");
        if (favorites_snapshot_migration != NULL)
            *favorites_snapshot_migration = false;
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = load_list(nvs, "recents", &s_recents,
                        recents_migration,
                        recents_snapshot_migration);
        if (err == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Lista de recientes invalida; se conserva el resto de ajustes");
            if (recents_snapshot_migration != NULL) {
                *recents_snapshot_migration = false;
            }
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = load_current_station(nvs, current_snapshot_migration);
    }
    return err;
}

static esp_err_t load_screen_timeout(nvs_handle_t nvs,
                                     bool *needs_migration)
{
    int32_t timeout = 30;
    const esp_err_t err = nvs_get_i32(nvs, SCREEN_TIMEOUT_KEY, &timeout);
    if (err == ESP_OK) {
        if (valid_screen_timeout(timeout)) {
            s_screen_timeout_seconds = (int)timeout;
        } else if (timeout == 60) {
            s_screen_timeout_seconds = 30;
            if (needs_migration != NULL) *needs_migration = true;
        }
        return ESP_OK;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) return ESP_OK;

    int32_t legacy_minutes = 0;
    if (nvs_get_i32(nvs, SCREEN_DIM_KEY, &legacy_minutes) == ESP_OK &&
        (legacy_minutes == 1 || legacy_minutes == 5 || legacy_minutes == 10)) {
        /* Legacy dim timers become the new 30 s maximum timeout. */
        s_screen_timeout_seconds = 30;
        if (needs_migration != NULL) *needs_migration = true;
    }
    return ESP_OK;
}

static void load_ui_choice(nvs_handle_t nvs, const char *key, int *destination,
                           int maximum)
{
    int32_t value = 0;
    if (nvs_get_i32(nvs, key, &value) == ESP_OK && valid_choice(value, maximum)) {
        *destination = (int)value;
    }
}

static void encode_preferences_snapshot(uint8_t payload[PREFERENCE_SNAPSHOT_SIZE],
                                        int last_view, int sleep_preset,
                                        int brightness, int timeout,
                                        int theme, int accent, int saver)
{
    const int values[] = {
        last_view, sleep_preset, brightness, timeout, theme, accent, saver,
    };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        write_u32_le(payload + i * 4U, (uint32_t)(int32_t)values[i]);
    }
}

static esp_err_t decode_preferences_snapshot(const uint8_t *payload,
                                             size_t payload_size)
{
    if (payload == NULL || payload_size != PREFERENCE_SNAPSHOT_SIZE) {
        return ESP_ERR_INVALID_STATE;
    }
    const int values[] = {
        (int32_t)read_u32_le(payload),
        (int32_t)read_u32_le(payload + 4U),
        (int32_t)read_u32_le(payload + 8U),
        (int32_t)read_u32_le(payload + 12U),
        (int32_t)read_u32_le(payload + 16U),
        (int32_t)read_u32_le(payload + 20U),
        (int32_t)read_u32_le(payload + 24U),
    };
    if (!valid_choice(values[0], LAST_VIEW_MAX) ||
        !valid_sleep_preset(values[1]) ||
        !valid_screen_brightness(values[2]) ||
        !valid_screen_timeout(values[3]) ||
        !valid_choice(values[4], UI_THEME_MAX) ||
        !valid_choice(values[5], UI_ACCENT_MAX) ||
        !valid_choice(values[6], SCREENSAVER_STYLE_MAX)) {
        return ESP_ERR_INVALID_STATE;
    }
    s_last_view = values[0];
    s_sleep_preset = values[1];
    s_screen_brightness = values[2];
    s_screen_timeout_seconds = values[3];
    s_ui_theme = values[4];
    s_ui_accent = values[5];
    s_screensaver_style = values[6];
    return ESP_OK;
}

static esp_err_t load_preferences(nvs_handle_t nvs,
                                  bool *screen_timeout_migration,
                                  bool *preferences_snapshot_migration)
{
    uint8_t payload[PREFERENCE_SNAPSHOT_SIZE];
    size_t snapshot_size = sizeof(payload);
    uint32_t generation = 0U;
    esp_err_t err = radio_snapshot_store_read(
        "prefs", RADIO_SNAPSHOT_KIND_PREFERENCES, payload, sizeof(payload),
        &snapshot_size, &generation);
    if (err == ESP_OK) {
        err = decode_preferences_snapshot(payload, snapshot_size);
        if (err != ESP_OK) return err;
        s_sleep_preset_confirmed = s_sleep_preset;
        s_screen_brightness_confirmed = s_screen_brightness;
        s_screen_timeout_confirmed = s_screen_timeout_seconds;
        s_ui_theme_confirmed = s_ui_theme;
        s_ui_accent_confirmed = s_ui_accent;
        s_screensaver_style_confirmed = s_screensaver_style;
        return ESP_OK;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) return err;
    if (preferences_snapshot_migration != NULL) {
        *preferences_snapshot_migration = true;
    }
    if (nvs == 0) {
        s_sleep_preset_confirmed = s_sleep_preset;
        s_screen_brightness_confirmed = s_screen_brightness;
        s_screen_timeout_confirmed = s_screen_timeout_seconds;
        s_ui_theme_confirmed = s_ui_theme;
        s_ui_accent_confirmed = s_ui_accent;
        s_screensaver_style_confirmed = s_screensaver_style;
        return ESP_OK;
    }

    load_ui_choice(nvs, LAST_VIEW_KEY, &s_last_view, LAST_VIEW_MAX);

    int32_t sleep_preset = 0;
    if (nvs_get_i32(nvs, SLEEP_PRESET_KEY, &sleep_preset) == ESP_OK &&
        valid_sleep_preset(sleep_preset)) {
        s_sleep_preset = (int)sleep_preset;
    }
    int32_t brightness = 70;
    if (nvs_get_i32(nvs, SCREEN_BRIGHTNESS_KEY, &brightness) == ESP_OK &&
        valid_screen_brightness(brightness)) {
        s_screen_brightness = (int)brightness;
    }
    err = load_screen_timeout(nvs, screen_timeout_migration);
    if (err != ESP_OK) return err;
    load_ui_choice(nvs, UI_THEME_KEY, &s_ui_theme, UI_THEME_MAX);
    load_ui_choice(nvs, UI_ACCENT_KEY, &s_ui_accent, UI_ACCENT_MAX);
    load_ui_choice(nvs, SCREENSAVER_KEY, &s_screensaver_style,
                   SCREENSAVER_STYLE_MAX);
    s_sleep_preset_confirmed = s_sleep_preset;
    s_screen_brightness_confirmed = s_screen_brightness;
    s_screen_timeout_confirmed = s_screen_timeout_seconds;
    s_ui_theme_confirmed = s_ui_theme;
    s_ui_accent_confirmed = s_ui_accent;
    s_screensaver_style_confirmed = s_screensaver_style;
    return ESP_OK;
}

static esp_err_t load_volume(bool *needs_snapshot_migration)
{
    if (needs_snapshot_migration != NULL) {
        *needs_snapshot_migration = false;
    }
    uint8_t payload[VOLUME_SNAPSHOT_SIZE];
    size_t snapshot_size = sizeof(payload);
    uint32_t generation = 0U;
    esp_err_t err = radio_snapshot_store_read(
        "volume", RADIO_SNAPSHOT_KIND_VOLUME, payload, sizeof(payload),
        &snapshot_size, &generation);
    if (err == ESP_OK) {
        if (snapshot_size != sizeof(payload)) return ESP_ERR_INVALID_STATE;
        const int32_t volume = (int32_t)read_u32_le(payload);
        if (volume < 0 || volume > 100) return ESP_ERR_INVALID_STATE;
        s_volume = (int)volume;
        s_volume_confirmed = s_volume;
        return ESP_OK;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) return err;
    if (needs_snapshot_migration != NULL) {
        *needs_snapshot_migration = true;
    }

    nvs_handle_t nvs;
    err = nvs_open(VOLUME_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_volume_confirmed = s_volume;
        return ESP_OK;
    }
    if (err != ESP_OK) return err;
    int32_t volume = 55;
    if (nvs_get_i32(nvs, VOLUME_KEY, &volume) == ESP_OK &&
        volume >= 0 && volume <= 100) {
        s_volume = (int)volume;
    }
    s_volume_confirmed = s_volume;
    nvs_close(nvs);
    return ESP_OK;
}

typedef struct {
    bool favorites;
    bool recents;
    bool screen_timeout;
    uint8_t schema_version;
} store_migration_t;

static esp_err_t migrate_store_namespace_job(void *context)
{
    const store_migration_t *migration = context;
    if (migration == NULL) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(STORE_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    if (migration->favorites) {
        err = save_list_to_handle(nvs, "favorites", &s_favorites);
    }
    if (err == ESP_OK && migration->recents) {
        err = save_list_to_handle(nvs, "recents", &s_recents);
    }
    if (err == ESP_OK && migration->screen_timeout) {
        err = nvs_set_i32(nvs, SCREEN_TIMEOUT_KEY,
                          s_screen_timeout_seconds);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, STORE_SCHEMA_VERSION_KEY,
                         migration->schema_version);
    }
    if (err == ESP_OK) err = RADIO_TEST_NVS_COMMIT(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t radio_station_store_init(void)
{
    if (s_initialized) return ESP_OK;
    restore_cache_defaults();
    esp_err_t err = radio_snapshot_store_init();
    if (err != ESP_OK) return err;
    err = radio_nvs_worker_init();
    if (err != ESP_OK) return err;
    s_store_lock = xSemaphoreCreateMutex();
    if (s_store_lock == NULL) return ESP_ERR_NO_MEM;
    nvs_handle_t nvs = 0;
    err = nvs_open(STORE_NAMESPACE, NVS_READONLY, &nvs);
    const bool namespace_missing = err == ESP_ERR_NVS_NOT_FOUND;
    if (namespace_missing) {
        err = ESP_OK;
    } else if (err != ESP_OK) {
        vSemaphoreDelete(s_store_lock);
        s_store_lock = NULL;
        return err;
    }

    uint8_t schema_version = 0;
    const esp_err_t version_err = namespace_missing
        ? ESP_ERR_NVS_NOT_FOUND
        : nvs_get_u8(nvs, STORE_SCHEMA_VERSION_KEY, &schema_version);
    if (version_err != ESP_OK && version_err != ESP_ERR_NVS_NOT_FOUND) {
        err = version_err;
    } else if (version_err == ESP_OK && schema_version > STORE_SCHEMA_VERSION) {
        err = ESP_ERR_NOT_SUPPORTED;
    }

    bool favorites_migration = false;
    bool recents_migration = false;
    bool screen_timeout_migration = false;
    bool favorites_snapshot_migration = false;
    bool recents_snapshot_migration = false;
    bool current_snapshot_migration = false;
    bool preferences_snapshot_migration = false;
    bool volume_snapshot_migration = false;
    if (err == ESP_OK) {
        err = load_station_state(nvs, &favorites_migration,
                                 &recents_migration,
                                 &favorites_snapshot_migration,
                                 &recents_snapshot_migration,
                                 &current_snapshot_migration);
    }
    if (err == ESP_OK) {
        err = load_preferences(nvs, &screen_timeout_migration,
                               &preferences_snapshot_migration);
    }
    if (err == ESP_OK) {
        err = load_volume(&volume_snapshot_migration);
    }

    const bool schema_migration = version_err == ESP_ERR_NVS_NOT_FOUND ||
                                  (version_err == ESP_OK &&
                                   schema_version < STORE_SCHEMA_VERSION);
    if (!namespace_missing) nvs_close(nvs);
    if (err == ESP_OK && favorites_snapshot_migration) {
        err = radio_nvs_worker_run_sync(migrate_favorites_snapshot_job,
                                        &s_favorites);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "No se pudo copiar favoritas a snapshot: %s",
                     esp_err_to_name(err));
        }
    }
    if (err == ESP_OK && recents_snapshot_migration) {
        err = radio_nvs_worker_run_sync(migrate_recents_snapshot_job,
                                        &s_recents);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "No se pudo copiar recientes a snapshot: %s",
                     esp_err_to_name(err));
        }
    }
    if (err == ESP_OK && current_snapshot_migration) {
        err = radio_nvs_worker_run_sync(migrate_current_snapshot_job,
                                        &s_current);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "No se pudo copiar emisora actual a snapshot: %s",
                     esp_err_to_name(err));
        }
    }
    if (err == ESP_OK && preferences_snapshot_migration) {
        err = radio_nvs_worker_run_sync(migrate_preferences_snapshot_job,
                                        NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "No se pudieron copiar preferencias a snapshot: %s",
                     esp_err_to_name(err));
        }
    }
    if (err == ESP_OK && volume_snapshot_migration) {
        err = radio_nvs_worker_run_sync(migrate_volume_snapshot_job, NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "No se pudo copiar volumen a snapshot: %s",
                     esp_err_to_name(err));
        }
    }
    if (err == ESP_OK && (schema_migration || favorites_migration ||
                          recents_migration || screen_timeout_migration)) {
        const store_migration_t migration = {
            .favorites = favorites_migration,
            .recents = recents_migration,
            .screen_timeout = screen_timeout_migration,
            .schema_version = STORE_SCHEMA_VERSION,
        };
        /* Snapshot copies land before updating even the legacy schema marker. */
        err = radio_nvs_worker_run_sync(migrate_store_namespace_job,
                                        (void *)&migration);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "No se pudo migrar namespace %s: %s",
                     STORE_NAMESPACE, esp_err_to_name(err));
        }
    }
    if (err == ESP_OK) {
        s_recents_confirmed = s_recents;
        s_current_confirmed = s_current;
        s_has_current_confirmed = s_has_current;
        s_last_view_confirmed = s_last_view;
        s_initialized = true;
        if (xTaskCreate(persistence_task, "radio_store", 4096, NULL, 2, NULL) != pdPASS) {
            s_initialized = false;
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err != ESP_OK) {
        vSemaphoreDelete(s_store_lock);
        s_store_lock = NULL;
    }
    return err;
}

static size_t copy_list(const station_list_record_t *record, radio_station_t *out,
                        size_t capacity)
{
    if (out == NULL || capacity == 0) {
        return record->count;
    }
    size_t count = record->count < capacity ? record->count : capacity;
    memcpy(out, record->stations, count * sizeof(*out));
    return count;
}

size_t radio_station_store_favorites(radio_station_t *out, size_t capacity)
{
    if (!s_initialized) return 0;
    xSemaphoreTake(s_store_lock, portMAX_DELAY);
    const size_t count = copy_list(&s_favorites, out, capacity);
    xSemaphoreGive(s_store_lock);
    return count;
}

size_t radio_station_store_recents(radio_station_t *out, size_t capacity)
{
    if (!s_initialized) return 0;
    xSemaphoreTake(s_store_lock, portMAX_DELAY);
    const size_t count = copy_list(&s_recents, out, capacity);
    xSemaphoreGive(s_store_lock);
    return count;
}

static bool is_favorite_locked(const radio_station_t *station)
{
    if (station == NULL) return false;
    for (size_t i = 0; i < s_favorites.count; ++i) {
        if (same_station(station, &s_favorites.stations[i])) return true;
    }
    return false;
}

static bool record_contains_station(const station_list_record_t *record,
                                    const radio_station_t *station)
{
    for (size_t i = 0; i < record->count; ++i) {
        if (same_station(station, &record->stations[i])) return true;
    }
    return false;
}

bool radio_station_store_is_favorite(const radio_station_t *station)
{
    if (!s_initialized || !station_record_valid(station)) return false;
    xSemaphoreTake(s_store_lock, portMAX_DELAY);
    const bool favorite = is_favorite_locked(station);
    xSemaphoreGive(s_store_lock);
    return favorite;
}

static esp_err_t encode_list_record(const station_list_record_t *record,
                                    size_t *encoded_size)
{
    if (record == NULL || encoded_size == NULL ||
        record->count > RADIO_FAVORITES_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    *encoded_size = 0U;
    uint8_t *const bytes = (uint8_t *)&s_record_scratch;
    uint8_t *cursor = bytes + STORE_HEADER_SIZE;
    size_t remaining = sizeof(s_record_scratch) - STORE_HEADER_SIZE;
    memcpy(bytes, record, STORE_HEADER_SIZE);
    for (size_t i = 0; i < record->count; ++i) {
        const radio_station_t *station = &record->stations[i];
        if (encode_field(&cursor, &remaining, station->name, sizeof(station->name)) != ESP_OK ||
            encode_field(&cursor, &remaining, station->country, sizeof(station->country)) != ESP_OK ||
            encode_field(&cursor, &remaining, station->codec, sizeof(station->codec)) != ESP_OK ||
            encode_field(&cursor, &remaining, station->url, sizeof(station->url)) != ESP_OK ||
            encode_field(&cursor, &remaining, station->uuid, sizeof(station->uuid)) != ESP_OK) {
            return ESP_ERR_INVALID_SIZE;
        }
    }
    *encoded_size = (size_t)(cursor - bytes);
    return ESP_OK;
}

static esp_err_t save_list_to_handle(nvs_handle_t nvs, const char *key,
                                     const station_list_record_t *record)
{
    size_t encoded_size = 0U;
    esp_err_t err = encode_list_record(record, &encoded_size);
    if (err != ESP_OK) return err;
    return nvs_set_blob(nvs, key, &s_record_scratch, encoded_size);
}

static esp_err_t write_list_snapshot(const char *unit, uint32_t kind,
                                     const station_list_record_t *record)
{
    if (unit == NULL || record == NULL) return ESP_ERR_INVALID_ARG;
    size_t encoded_size = 0U;
    esp_err_t err = encode_list_record(record, &encoded_size);
    if (err != ESP_OK) return err;

    radio_snapshot_store_write_report_t report = {0};
    err = radio_snapshot_store_write(
        unit, kind,
        (const uint8_t *)&s_record_scratch, encoded_size, &report);
    if (report.reconciled_after_error) {
        ESP_LOGW(TAG, "Snapshot %s reconciliado tras error NVS: %s",
                 unit,
                 esp_err_to_name(report.operation_error));
    }
    return err;
}

static esp_err_t save_list_snapshot(const char *key,
                                    const station_list_record_t *record)
{
    const char *unit = NULL;
    uint32_t kind = 0U;
    if (!list_snapshot_identity(key, &unit, &kind)) {
        return ESP_ERR_INVALID_ARG;
    }
    return write_list_snapshot(unit, kind, record);
}

static esp_err_t save_current_snapshot(const radio_station_t *station)
{
    if (station == NULL || !station_record_valid(station)) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&s_current_record, 0, sizeof(s_current_record));
    s_current_record.magic = STORE_MAGIC;
    s_current_record.version = STORE_VERSION;
    s_current_record.count = 1U;
    s_current_record.stations[0] = *station;
    return write_list_snapshot("current",
                               RADIO_SNAPSHOT_KIND_CURRENT_STATION,
                               &s_current_record);
}

static esp_err_t save_preferences_snapshot(
    int last_view, int sleep_preset, int brightness, int timeout, int theme,
    int accent, int screensaver_style)
{
    static uint8_t payload[PREFERENCE_SNAPSHOT_SIZE];
    encode_preferences_snapshot(payload, last_view, sleep_preset, brightness,
                                 timeout, theme, accent, screensaver_style);
    radio_snapshot_store_write_report_t report = {0};
    const esp_err_t err = radio_snapshot_store_write(
        "prefs", RADIO_SNAPSHOT_KIND_PREFERENCES, payload, sizeof(payload),
        &report);
    if (report.reconciled_after_error) {
        ESP_LOGW(TAG, "Preferencias reconciliadas tras error NVS: %s",
                 esp_err_to_name(report.operation_error));
    }
    return err;
}

static esp_err_t save_volume_snapshot(int volume)
{
    static uint8_t payload[VOLUME_SNAPSHOT_SIZE];
    write_u32_le(payload, (uint32_t)(int32_t)volume);
    radio_snapshot_store_write_report_t report = {0};
    const esp_err_t err = radio_snapshot_store_write(
        "volume", RADIO_SNAPSHOT_KIND_VOLUME, payload, sizeof(payload),
        &report);
    if (report.reconciled_after_error) {
        ESP_LOGW(TAG, "Volumen reconciliado tras error NVS: %s",
                 esp_err_to_name(report.operation_error));
    }
    return err;
}

static esp_err_t migrate_favorites_snapshot_job(void *context)
{
    if (context == NULL) return ESP_ERR_INVALID_ARG;
    return save_list_snapshot("favorites", context);
}

static esp_err_t migrate_recents_snapshot_job(void *context)
{
    if (context == NULL) return ESP_ERR_INVALID_ARG;
    return save_list_snapshot("recents", context);
}

static esp_err_t migrate_current_snapshot_job(void *context)
{
    return save_current_snapshot(context);
}

static esp_err_t migrate_preferences_snapshot_job(void *context)
{
    (void)context;
    return save_preferences_snapshot(
        s_last_view, s_sleep_preset, s_screen_brightness,
        s_screen_timeout_seconds, s_ui_theme, s_ui_accent,
        s_screensaver_style);
}

static esp_err_t migrate_volume_snapshot_job(void *context)
{
    (void)context;
    return save_volume_snapshot(s_volume);
}

static esp_err_t save_list(const char *key, const station_list_record_t *record)
{
    if (key != NULL &&
        (strcmp(key, "favorites") == 0 || strcmp(key, "recents") == 0)) {
        return save_list_snapshot(key, record);
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(STORE_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = save_list_to_handle(nvs, key, record);
    if (err == ESP_OK) err = RADIO_TEST_NVS_COMMIT(nvs);
    nvs_close(nvs);
    return err;
}

typedef enum {
    STORE_MUTATION_TOGGLE_FAVORITE = 0,
    STORE_MUTATION_MOVE_FAVORITE,
} store_mutation_kind_t;

typedef struct {
    store_mutation_kind_t kind;
    radio_station_t station;
    char uuid[RADIO_STATION_UUID_MAX];
    int direction;
    bool is_favorite;
    radio_station_store_result_cb_t callback;
    void *callback_context;
} store_mutation_job_t;

static esp_err_t store_mutation_execute(void *context)
{
    store_mutation_job_t *job = context;
    if (job == NULL || !s_initialized) return ESP_ERR_INVALID_STATE;
    esp_err_t err = ESP_OK;
    /* NVS jobs are serialized; keep this 4+ KiB record off the worker stack. */
    static station_list_record_t candidate;
    (void)radio_mutex_take_audited(s_store_lock, portMAX_DELAY, TAG,
                                   "store", __func__);
    candidate = s_favorites;
    xSemaphoreGive(s_store_lock);

    if (job->kind == STORE_MUTATION_TOGGLE_FAVORITE) {
        const bool originally_favorite =
            record_contains_station(&candidate, &job->station);
        size_t index = 0;
        while (index < candidate.count &&
               !same_station(&job->station, &candidate.stations[index])) {
            ++index;
        }
        if (index < candidate.count) {
            for (size_t i = index + 1; i < candidate.count; ++i) {
                candidate.stations[i - 1] = candidate.stations[i];
            }
            --candidate.count;
            memset(&candidate.stations[candidate.count], 0,
                   sizeof(candidate.stations[0]));
        } else if (candidate.count >= RADIO_FAVORITES_MAX) {
            err = ESP_ERR_NO_MEM;
        } else {
            candidate.stations[candidate.count++] = job->station;
        }
        if (err == ESP_OK) {
            err = save_list("favorites", &candidate);
        }
        job->is_favorite = err == ESP_OK
                               ? record_contains_station(&candidate,
                                                         &job->station)
                               : originally_favorite;
    } else if (job->kind == STORE_MUTATION_MOVE_FAVORITE) {
        size_t index = 0;
        while (index < candidate.count &&
               strcmp(job->uuid, candidate.stations[index].uuid) != 0) {
            ++index;
        }
        if (index == candidate.count) {
            err = ESP_ERR_NOT_FOUND;
        } else {
            const int target = (int)index + job->direction;
            if (target < 0 || target >= (int)candidate.count) {
                err = ESP_ERR_INVALID_STATE;
            } else {
                const radio_station_t moving = candidate.stations[index];
                candidate.stations[index] = candidate.stations[target];
                candidate.stations[target] = moving;
                err = save_list("favorites", &candidate);
            }
        }
    } else {
        err = ESP_ERR_INVALID_ARG;
    }
    if (err == ESP_OK) {
        /* NVS commit is complete; publish the new cache in a short critical
           section so UI readers never wait behind flash I/O. Mutations are
           serialized by the NVS worker. */
        (void)radio_mutex_take_audited(s_store_lock, portMAX_DELAY, TAG,
                                       "store", __func__);
        s_favorites = candidate;
        xSemaphoreGive(s_store_lock);
    }
    return err;
}

static void store_mutation_complete(esp_err_t result, void *context)
{
    store_mutation_job_t *job = context;
    if (job == NULL) return;
    if (job->callback != NULL) {
        job->callback(result, job->is_favorite, job->callback_context);
    }
    free(job);
}

static esp_err_t store_mutation_submit_async(store_mutation_job_t *job)
{
    const esp_err_t err = radio_nvs_worker_submit(
        store_mutation_execute, job, store_mutation_complete, job, 0);
    if (err != ESP_OK) free(job);
    return err;
}

typedef struct {
    bool current_dirty;
    bool recents_dirty;
    bool last_view_dirty;
    bool volume_dirty;
    bool sleep_preset_dirty;
    bool screen_brightness_dirty;
    bool screen_timeout_dirty;
    bool ui_appearance_dirty;
    bool has_current;
    radio_station_t current;
    station_list_record_t recents;
    int last_view;
    int volume;
    int sleep_preset;
    int screen_brightness;
    int screen_timeout_seconds;
    int ui_theme;
    int ui_accent;
    int screensaver_style;
    bool job_ran;
    esp_err_t current_result;
    esp_err_t recents_result;
    esp_err_t preferences_result;
    esp_err_t volume_result;
} store_flush_snapshot_t;

typedef struct {
    bool stations;
    bool volume;
    bool preferences;
} store_flush_request_t;

static esp_err_t persist_snapshot_job(void *context)
{
    store_flush_snapshot_t *snapshot = context;
    if (snapshot == NULL) return ESP_ERR_INVALID_ARG;
    snapshot->job_ran = true;
    snapshot->current_result = ESP_OK;
    snapshot->recents_result = ESP_OK;
    snapshot->preferences_result = ESP_OK;
    snapshot->volume_result = ESP_OK;

    if (snapshot->current_dirty && snapshot->has_current) {
        snapshot->current_result = save_current_snapshot(&snapshot->current);
    }
    if (snapshot->recents_dirty) {
        snapshot->recents_result =
            save_list_snapshot("recents", &snapshot->recents);
    }

    const bool preferences_dirty = snapshot->last_view_dirty ||
                                   snapshot->sleep_preset_dirty ||
                                   snapshot->screen_brightness_dirty ||
                                   snapshot->screen_timeout_dirty ||
                                   snapshot->ui_appearance_dirty;
    if (preferences_dirty) {
        snapshot->preferences_result = save_preferences_snapshot(
            snapshot->last_view, snapshot->sleep_preset,
            snapshot->screen_brightness, snapshot->screen_timeout_seconds,
            snapshot->ui_theme, snapshot->ui_accent,
            snapshot->screensaver_style);
    }
    if (snapshot->volume_dirty) {
        snapshot->volume_result = save_volume_snapshot(snapshot->volume);
    }

    if (snapshot->current_result != ESP_OK) return snapshot->current_result;
    if (snapshot->recents_result != ESP_OK) return snapshot->recents_result;
    if (snapshot->preferences_result != ESP_OK)
        return snapshot->preferences_result;
    return snapshot->volume_result;
}

static esp_err_t flush_store_worker_job(void *context)
{
    const store_flush_request_t *request = context;
    if (request == NULL) return ESP_ERR_INVALID_ARG;
    /* This record contains station arrays larger than the 4 KiB worker stack.
       The single NVS worker serializes access to this scratch snapshot. */
    static store_flush_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    (void)radio_mutex_take_audited(s_store_lock, portMAX_DELAY, TAG,
                                   "store", __func__);
    snapshot.current_dirty = request->stations && s_current_dirty;
    snapshot.recents_dirty = request->stations && s_recents_dirty;
    snapshot.last_view_dirty = request->stations && s_last_view_dirty;
    snapshot.volume_dirty = request->volume && s_volume_dirty;
    snapshot.sleep_preset_dirty = request->preferences && s_sleep_preset_dirty;
    snapshot.screen_brightness_dirty = request->preferences && s_screen_brightness_dirty;
    snapshot.screen_timeout_dirty = request->preferences && s_screen_timeout_dirty;
    snapshot.ui_appearance_dirty = request->preferences && s_ui_appearance_dirty;
    snapshot.has_current = s_has_current;
    snapshot.current = s_current;
    snapshot.recents = s_recents;
    snapshot.last_view = s_last_view;
    snapshot.volume = s_volume;
    snapshot.sleep_preset = s_sleep_preset;
    snapshot.screen_brightness = s_screen_brightness;
    snapshot.screen_timeout_seconds = s_screen_timeout_seconds;
    snapshot.ui_theme = s_ui_theme;
    snapshot.ui_accent = s_ui_accent;
    snapshot.screensaver_style = s_screensaver_style;
    const bool any_work = snapshot.current_dirty || snapshot.recents_dirty ||
                          snapshot.last_view_dirty || snapshot.volume_dirty ||
                          snapshot.sleep_preset_dirty ||
                          snapshot.screen_brightness_dirty ||
                          snapshot.screen_timeout_dirty ||
                          snapshot.ui_appearance_dirty;
    xSemaphoreGive(s_store_lock);
    if (!any_work) return ESP_OK;

    const esp_err_t err = persist_snapshot_job(&snapshot);
    radio_station_store_error_cb_t error_callback = NULL;
    void *error_callback_context = NULL;
    (void)radio_mutex_take_audited(s_store_lock, portMAX_DELAY, TAG,
                                   "store", __func__);
    if (snapshot.current_dirty) {
        if (snapshot.job_ran && snapshot.current_result == ESP_OK) {
            s_current_confirmed = snapshot.current;
            s_has_current_confirmed = snapshot.has_current;
            if (s_current_dirty && s_has_current == snapshot.has_current &&
                memcmp(&s_current, &snapshot.current, sizeof(s_current)) == 0) {
                s_current_dirty = false;
            }
        } else if (s_current_dirty &&
                   s_has_current == snapshot.has_current &&
                   memcmp(&s_current, &snapshot.current,
                          sizeof(s_current)) == 0) {
            s_current = s_current_confirmed;
            s_has_current = s_has_current_confirmed;
            s_current_dirty = false;
        }
    }
    if (snapshot.recents_dirty) {
        if (snapshot.job_ran && snapshot.recents_result == ESP_OK) {
            s_recents_confirmed = snapshot.recents;
            if (s_recents_dirty &&
                memcmp(&s_recents, &snapshot.recents, sizeof(s_recents)) == 0) {
                s_recents_dirty = false;
            }
        } else if (s_recents_dirty &&
                   memcmp(&s_recents, &snapshot.recents,
                          sizeof(s_recents)) == 0) {
            s_recents = s_recents_confirmed;
            s_recents_dirty = false;
        }
    }

    const bool preferences_dirty = snapshot.last_view_dirty ||
                                   snapshot.sleep_preset_dirty ||
                                   snapshot.screen_brightness_dirty ||
                                   snapshot.screen_timeout_dirty ||
                                   snapshot.ui_appearance_dirty;
    if (preferences_dirty && snapshot.job_ran &&
        snapshot.preferences_result == ESP_OK) {
        if (snapshot.last_view_dirty) {
            s_last_view_confirmed = snapshot.last_view;
            if (s_last_view_dirty && s_last_view == snapshot.last_view) {
                s_last_view_dirty = false;
            }
        }
        if (snapshot.sleep_preset_dirty) {
            s_sleep_preset_confirmed = snapshot.sleep_preset;
            if (s_sleep_preset_dirty &&
                s_sleep_preset == snapshot.sleep_preset) {
                s_sleep_preset_dirty = false;
            }
        }
        if (snapshot.screen_brightness_dirty) {
            s_screen_brightness_confirmed = snapshot.screen_brightness;
            if (s_screen_brightness_dirty &&
                s_screen_brightness == snapshot.screen_brightness) {
                s_screen_brightness_dirty = false;
            }
        }
        if (snapshot.screen_timeout_dirty) {
            s_screen_timeout_confirmed = snapshot.screen_timeout_seconds;
            if (s_screen_timeout_dirty &&
                s_screen_timeout_seconds == snapshot.screen_timeout_seconds) {
                s_screen_timeout_dirty = false;
            }
        }
        if (snapshot.ui_appearance_dirty) {
            s_ui_theme_confirmed = snapshot.ui_theme;
            s_ui_accent_confirmed = snapshot.ui_accent;
            s_screensaver_style_confirmed = snapshot.screensaver_style;
            if (s_ui_appearance_dirty && s_ui_theme == snapshot.ui_theme &&
                s_ui_accent == snapshot.ui_accent &&
                s_screensaver_style == snapshot.screensaver_style) {
                s_ui_appearance_dirty = false;
            }
        }
    } else if (preferences_dirty) {
        if (snapshot.last_view_dirty && s_last_view_dirty &&
            s_last_view == snapshot.last_view) {
            s_last_view = s_last_view_confirmed;
            s_last_view_dirty = false;
        }
        if (snapshot.sleep_preset_dirty && s_sleep_preset_dirty &&
            s_sleep_preset == snapshot.sleep_preset) {
            s_sleep_preset = s_sleep_preset_confirmed;
            s_sleep_preset_dirty = false;
        }
        if (snapshot.screen_brightness_dirty && s_screen_brightness_dirty &&
            s_screen_brightness == snapshot.screen_brightness) {
            s_screen_brightness = s_screen_brightness_confirmed;
            s_screen_brightness_dirty = false;
        }
        if (snapshot.screen_timeout_dirty && s_screen_timeout_dirty &&
            s_screen_timeout_seconds == snapshot.screen_timeout_seconds) {
            s_screen_timeout_seconds = s_screen_timeout_confirmed;
            s_screen_timeout_dirty = false;
        }
        if (snapshot.ui_appearance_dirty && s_ui_appearance_dirty &&
            s_ui_theme == snapshot.ui_theme && s_ui_accent == snapshot.ui_accent &&
            s_screensaver_style == snapshot.screensaver_style) {
            s_ui_theme = s_ui_theme_confirmed;
            s_ui_accent = s_ui_accent_confirmed;
            s_screensaver_style = s_screensaver_style_confirmed;
            s_ui_appearance_dirty = false;
        }
    }
    if (snapshot.volume_dirty && snapshot.job_ran &&
        snapshot.volume_result == ESP_OK) {
        s_volume_confirmed = snapshot.volume;
        if (s_volume_dirty && s_volume == snapshot.volume) {
            s_volume_dirty = false;
        }
    } else if (snapshot.volume_dirty && s_volume_dirty &&
               s_volume == snapshot.volume) {
        s_volume = s_volume_confirmed;
        s_volume_dirty = false;
    }
    if (err != ESP_OK) {
        const TickType_t now = xTaskGetTickCount();
        if (request->stations) s_station_changed_at = now;
        if (request->stations) s_last_view_changed_at = now;
        if (request->volume) s_volume_changed_at = now;
        if (request->preferences) s_preferences_changed_at = now;
    }
    if (err != ESP_OK) {
        error_callback = s_error_callback;
        error_callback_context = s_error_callback_context;
    }
    xSemaphoreGive(s_store_lock);
    if (error_callback != NULL) error_callback(err, error_callback_context);
    return err;
}

static esp_err_t flush_store_groups(bool stations, bool volume,
                                    bool preferences)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    const store_flush_request_t request = {
        .stations = stations,
        .volume = volume,
        .preferences = preferences,
    };
    /* The worker serializes flush/mutation jobs; no store lock is held while
       this caller waits for the NVS queue. */
    return radio_nvs_worker_run_sync(flush_store_worker_job,
                                     (void *)&request);
}

static void persistence_task(void *arg)
{
    (void)arg;
    radio_task_supervisor_register(RADIO_SUPERVISED_TASK_STORE);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        xSemaphoreTake(s_store_lock, portMAX_DELAY);
        const TickType_t now = xTaskGetTickCount();
        const bool flush_stations =
            (((s_current_dirty || s_recents_dirty) &&
              now - s_station_changed_at >= pdMS_TO_TICKS(STORE_SETTLE_MS)) ||
             (s_last_view_dirty &&
              now - s_last_view_changed_at >= pdMS_TO_TICKS(VIEW_SETTLE_MS)));
        const bool flush_volume = s_volume_dirty &&
            now - s_volume_changed_at >= pdMS_TO_TICKS(VOLUME_SETTLE_MS);
        const bool flush_preferences =
            (s_sleep_preset_dirty || s_screen_brightness_dirty ||
             s_screen_timeout_dirty || s_ui_appearance_dirty) &&
            now - s_preferences_changed_at >= pdMS_TO_TICKS(PREFERENCE_SETTLE_MS);
        xSemaphoreGive(s_store_lock);

        if (flush_stations || flush_volume || flush_preferences) {
            const esp_err_t err = flush_store_groups(flush_stations,
                                                     flush_volume,
                                                     flush_preferences);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "No se pudieron guardar cambios NVS: %s",
                         esp_err_to_name(err));
            }
        }
        radio_task_supervisor_progress(RADIO_SUPERVISED_TASK_STORE);
    }
}

esp_err_t radio_station_store_flush(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    return flush_store_groups(true, true, true);
}

void radio_station_store_set_error_callback(
    radio_station_store_error_cb_t callback, void *context)
{
    if (!s_initialized || s_store_lock == NULL) {
        s_error_callback = callback;
        s_error_callback_context = context;
        return;
    }
    xSemaphoreTake(s_store_lock, portMAX_DELAY);
    s_error_callback = callback;
    s_error_callback_context = context;
    xSemaphoreGive(s_store_lock);
}

static int read_cached_int(const int *value, int fallback)
{
    if (!s_initialized) return fallback;
    xSemaphoreTake(s_store_lock, portMAX_DELAY);
    const int result = *value;
    xSemaphoreGive(s_store_lock);
    return result;
}

static esp_err_t set_cached_preference(int *value, bool *dirty, int next)
{
    xSemaphoreTake(s_store_lock, portMAX_DELAY);
    if (*value != next) {
        *value = next;
        *dirty = true;
        s_preferences_changed_at = xTaskGetTickCount();
    }
    xSemaphoreGive(s_store_lock);
    return ESP_OK;
}

int radio_station_store_volume(void)
{
    return read_cached_int(&s_volume, 55);
}

esp_err_t radio_station_store_set_volume(int volume)
{
    if (!s_initialized || volume < 0 || volume > 100) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_store_lock, portMAX_DELAY);
    if (s_volume != volume) {
        s_volume = volume;
        s_volume_dirty = true;
        s_volume_changed_at = xTaskGetTickCount();
    }
    xSemaphoreGive(s_store_lock);
    return ESP_OK;
}

esp_err_t radio_station_store_toggle_favorite(const radio_station_t *station,
                                               bool *is_favorite)
{
    if (!s_initialized || !station_record_valid(station))
        return ESP_ERR_INVALID_ARG;
    store_mutation_job_t job = {
        .kind = STORE_MUTATION_TOGGLE_FAVORITE,
        .station = *station,
    };
    const esp_err_t err = radio_nvs_worker_run_sync(store_mutation_execute,
                                                    &job);
    if (is_favorite != NULL) *is_favorite = job.is_favorite;
    return err;
}

esp_err_t radio_station_store_toggle_favorite_async(
    const radio_station_t *station, radio_station_store_result_cb_t callback,
    void *context)
{
    if (!s_initialized || !station_record_valid(station) || callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    store_mutation_job_t *job = calloc(1, sizeof(*job));
    if (job == NULL) return ESP_ERR_NO_MEM;
    job->kind = STORE_MUTATION_TOGGLE_FAVORITE;
    job->station = *station;
    job->callback = callback;
    job->callback_context = context;
    return store_mutation_submit_async(job);
}

esp_err_t radio_station_store_move_favorite(const char *uuid, int direction)
{
    if (!s_initialized || uuid == NULL ||
        (direction != -1 && direction != 1) ||
        strnlen(uuid, RADIO_STATION_UUID_MAX) == 0 ||
        strnlen(uuid, RADIO_STATION_UUID_MAX) >= RADIO_STATION_UUID_MAX)
        return ESP_ERR_INVALID_ARG;
    store_mutation_job_t job = {
        .kind = STORE_MUTATION_MOVE_FAVORITE,
        .direction = direction,
    };
    snprintf(job.uuid, sizeof(job.uuid), "%s", uuid);
    return radio_nvs_worker_run_sync(store_mutation_execute, &job);
}

esp_err_t radio_station_store_move_favorite_async(
    const char *uuid, int direction,
    radio_station_store_result_cb_t callback, void *context)
{
    if (!s_initialized || uuid == NULL || callback == NULL ||
        (direction != -1 && direction != 1) ||
        strnlen(uuid, RADIO_STATION_UUID_MAX) == 0 ||
        strnlen(uuid, RADIO_STATION_UUID_MAX) >= RADIO_STATION_UUID_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    store_mutation_job_t *job = calloc(1, sizeof(*job));
    if (job == NULL) return ESP_ERR_NO_MEM;
    job->kind = STORE_MUTATION_MOVE_FAVORITE;
    job->direction = direction;
    snprintf(job->uuid, sizeof(job->uuid), "%s", uuid);
    job->callback = callback;
    job->callback_context = context;
    return store_mutation_submit_async(job);
}

esp_err_t radio_station_store_add_recent(const radio_station_t *station)
{
    if (!s_initialized || !station_record_valid(station))
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_store_lock, portMAX_DELAY);
    if (s_recents.count > 0 && same_station(station, &s_recents.stations[0]) &&
        same_station_details(station, &s_recents.stations[0])) {
        xSemaphoreGive(s_store_lock);
        return ESP_OK;
    }
    size_t found = s_recents.count;
    for (size_t i = 0; i < s_recents.count; ++i) {
        if (same_station(station, &s_recents.stations[i])) { found = i; break; }
    }
    if (found < s_recents.count) {
        for (size_t i = found; i > 0; --i) s_recents.stations[i] = s_recents.stations[i - 1];
    } else {
        if (s_recents.count < RADIO_RECENTS_MAX) ++s_recents.count;
        for (size_t i = s_recents.count - 1; i > 0; --i)
            s_recents.stations[i] = s_recents.stations[i - 1];
    }
    s_recents.stations[0] = *station;
    s_recents.magic = STORE_MAGIC;
    s_recents.version = STORE_VERSION;
    s_recents_dirty = true;
    s_station_changed_at = xTaskGetTickCount();
    xSemaphoreGive(s_store_lock);
    return ESP_OK;
}

esp_err_t radio_station_store_set_current(const radio_station_t *station)
{
    if (!s_initialized || !station_record_valid(station))
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_store_lock, portMAX_DELAY);
    if (!s_has_current || !same_station_details(&s_current, station)) {
        s_current = *station;
        s_has_current = true;
        s_current_dirty = true;
        s_station_changed_at = xTaskGetTickCount();
    }
    xSemaphoreGive(s_store_lock);
    return ESP_OK;
}

bool radio_station_store_get_current(radio_station_t *station)
{
    if (!s_initialized || station == NULL) return false;
    xSemaphoreTake(s_store_lock, portMAX_DELAY);
    if (!s_has_current) { xSemaphoreGive(s_store_lock); return false; }
    *station = s_current;
    xSemaphoreGive(s_store_lock);
    return true;
}

int radio_station_store_last_view(void)
{
    return read_cached_int(&s_last_view, 0);
}

esp_err_t radio_station_store_set_last_view(int view)
{
    if (!s_initialized || !valid_choice(view, LAST_VIEW_MAX)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_store_lock, portMAX_DELAY);
    if (view == s_last_view) { xSemaphoreGive(s_store_lock); return ESP_OK; }
    s_last_view = view;
    s_last_view_dirty = true;
    s_last_view_changed_at = xTaskGetTickCount();
    xSemaphoreGive(s_store_lock);
    return ESP_OK;
}

int radio_station_store_sleep_preset(void)
{
    return read_cached_int(&s_sleep_preset, 0);
}

esp_err_t radio_station_store_set_sleep_preset(int minutes)
{
    if (!s_initialized || !valid_sleep_preset(minutes)) return ESP_ERR_INVALID_ARG;
    return set_cached_preference(&s_sleep_preset, &s_sleep_preset_dirty, minutes);
}

int radio_station_store_screen_brightness(void)
{
    return read_cached_int(&s_screen_brightness, 70);
}

esp_err_t radio_station_store_set_screen_brightness(int percent)
{
    if (!s_initialized || !valid_screen_brightness(percent)) return ESP_ERR_INVALID_ARG;
    return set_cached_preference(&s_screen_brightness, &s_screen_brightness_dirty, percent);
}

int radio_station_store_screen_timeout_seconds(void)
{
    return read_cached_int(&s_screen_timeout_seconds, 30);
}

esp_err_t radio_station_store_set_screen_timeout_seconds(int seconds)
{
    if (!s_initialized || !valid_screen_timeout(seconds))
        return ESP_ERR_INVALID_ARG;
    return set_cached_preference(&s_screen_timeout_seconds, &s_screen_timeout_dirty, seconds);
}

int radio_station_store_ui_theme(void)
{
    return read_cached_int(&s_ui_theme, 0);
}

int radio_station_store_ui_accent(void)
{
    return read_cached_int(&s_ui_accent, 0);
}

int radio_station_store_screensaver_style(void)
{
    return read_cached_int(&s_screensaver_style, 0);
}

esp_err_t radio_station_store_set_ui_appearance(int theme, int accent,
                                                  int screensaver_style)
{
    if (!s_initialized || !valid_choice(theme, UI_THEME_MAX) ||
        !valid_choice(accent, UI_ACCENT_MAX) ||
        !valid_choice(screensaver_style, SCREENSAVER_STYLE_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_store_lock, portMAX_DELAY);
    if (theme == s_ui_theme && accent == s_ui_accent &&
        screensaver_style == s_screensaver_style) {
        xSemaphoreGive(s_store_lock);
        return ESP_OK;
    }
    s_ui_theme = theme;
    s_ui_accent = accent;
    s_screensaver_style = screensaver_style;
    s_ui_appearance_dirty = true;
    s_preferences_changed_at = xTaskGetTickCount();
    xSemaphoreGive(s_store_lock);
    return ESP_OK;
}
