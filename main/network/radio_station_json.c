#include "radio_station_json.h"

#include <string.h>

#include "cJSON.h"
#include "radio_json.h"

static bool copy_string_field(const cJSON *object, const char *key,
                              char *destination, size_t capacity,
                              bool required)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (item == NULL) return !required;
    if (!cJSON_IsString(item) || item->valuestring == NULL) return false;

    const size_t length = strnlen(item->valuestring, capacity);
    if (length >= capacity) return false;
    memcpy(destination, item->valuestring, length + 1U);
    return true;
}

esp_err_t radio_station_json_decode(const char *json, size_t length,
                                    radio_station_t *station)
{
    if (station != NULL) memset(station, 0, sizeof(*station));
    if (json == NULL || station == NULL || length == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = radio_json_parse_exact(json, length);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const bool valid =
        copy_string_field(root, "name", station->name,
                          sizeof(station->name), true) &&
        copy_string_field(root, "country", station->country,
                          sizeof(station->country), false) &&
        copy_string_field(root, "codec", station->codec,
                          sizeof(station->codec), false) &&
        copy_string_field(root, "uuid", station->uuid,
                          sizeof(station->uuid), false) &&
        copy_string_field(root, "url", station->url,
                          sizeof(station->url), true) &&
        station->name[0] != '\0' && station->url[0] != '\0';
    cJSON_Delete(root);
    if (!valid) {
        memset(station, 0, sizeof(*station));
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}
