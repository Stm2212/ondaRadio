#include "radio_station_record_codec.h"

#include <string.h>

static esp_err_t decode_field(const uint8_t **cursor, size_t *remaining,
                              char *destination, size_t capacity)
{
    if (*remaining == 0U) return ESP_ERR_INVALID_SIZE;
    const uint8_t *end = memchr(*cursor, '\0', *remaining);
    if (end == NULL || (size_t)(end - *cursor) >= capacity) {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t field_size = (size_t)(end - *cursor) + 1U;
    memcpy(destination, *cursor, field_size);
    *cursor += field_size;
    *remaining -= field_size;
    return ESP_OK;
}

static void clear_decoded_stations(radio_station_t *stations, size_t count)
{
    if (count > 0U) memset(stations, 0, count * sizeof(*stations));
}

esp_err_t radio_station_record_decode_v2(
    const uint8_t *blob, size_t length, radio_station_t *stations,
    size_t station_capacity, uint16_t *station_count)
{
    if (station_count != NULL) *station_count = 0U;
    if (blob == NULL || stations == NULL || station_count == NULL ||
        station_capacity == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (length < RADIO_STATION_RECORD_HEADER_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t magic = 0U;
    uint16_t version = 0U;
    uint16_t count = 0U;
    memcpy(&magic, blob, sizeof(magic));
    memcpy(&version, blob + sizeof(magic), sizeof(version));
    memcpy(&count, blob + sizeof(magic) + sizeof(version), sizeof(count));
    if (magic != RADIO_STATION_RECORD_MAGIC ||
        version != RADIO_STATION_RECORD_VERSION) {
        return ESP_ERR_INVALID_STATE;
    }
    if (count > station_capacity) return ESP_ERR_INVALID_SIZE;

    const uint8_t *cursor = blob + RADIO_STATION_RECORD_HEADER_SIZE;
    size_t remaining = length - RADIO_STATION_RECORD_HEADER_SIZE;
    for (size_t i = 0; i < count; ++i) {
        radio_station_t decoded = {0};
        if (decode_field(&cursor, &remaining, decoded.name,
                         sizeof(decoded.name)) != ESP_OK ||
            decode_field(&cursor, &remaining, decoded.country,
                         sizeof(decoded.country)) != ESP_OK ||
            decode_field(&cursor, &remaining, decoded.codec,
                         sizeof(decoded.codec)) != ESP_OK ||
            decode_field(&cursor, &remaining, decoded.url,
                         sizeof(decoded.url)) != ESP_OK ||
            decode_field(&cursor, &remaining, decoded.uuid,
                         sizeof(decoded.uuid)) != ESP_OK ||
            decoded.name[0] == '\0' || decoded.url[0] == '\0') {
            clear_decoded_stations(stations, i);
            return ESP_ERR_INVALID_STATE;
        }
        stations[i] = decoded;
    }
    if (remaining != 0U) {
        clear_decoded_stations(stations, count);
        return ESP_ERR_INVALID_STATE;
    }

    *station_count = count;
    return ESP_OK;
}
