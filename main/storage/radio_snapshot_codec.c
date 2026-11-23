#include "radio_snapshot_codec.h"

#include <limits.h>
#include <string.h>

#define SNAPSHOT_VERSION 1U

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8U));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) |
           ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

static void put_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8U);
}

static void put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8U);
    p[2] = (uint8_t)(value >> 16U);
    p[3] = (uint8_t)(value >> 24U);
}

static uint32_t crc_update(uint32_t crc, const uint8_t *bytes, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8U; ++bit)
            crc = (crc >> 1U) ^ (UINT32_C(0xedb88320) & (0U - (crc & 1U)));
    }
    return crc;
}

static uint32_t record_crc(const uint8_t *record, size_t payload_size)
{
    uint32_t crc = crc_update(UINT32_MAX, record, 20U);
    return ~crc_update(crc, record + RADIO_SNAPSHOT_HEADER_SIZE, payload_size);
}

radio_snapshot_result_t radio_snapshot_encode(
    uint8_t *output, size_t capacity, uint32_t kind, uint32_t generation,
    const uint8_t *payload, size_t payload_size, size_t *written)
{
    if (written != NULL) *written = 0U;
    if (output == NULL || written == NULL || kind == 0U || generation == 0U ||
        (payload == NULL && payload_size != 0U) || payload_size > UINT32_MAX)
        return RADIO_SNAPSHOT_INVALID;
    if (capacity < RADIO_SNAPSHOT_HEADER_SIZE ||
        payload_size > capacity - RADIO_SNAPSHOT_HEADER_SIZE)
        return RADIO_SNAPSHOT_NO_SPACE;

    memcpy(output, "ONDS", 4U);
    put_u16(output + 4U, SNAPSHOT_VERSION);
    put_u16(output + 6U, RADIO_SNAPSHOT_HEADER_SIZE);
    put_u32(output + 8U, generation);
    put_u32(output + 12U, (uint32_t)payload_size);
    put_u32(output + 16U, kind);
    if (payload_size != 0U)
        memcpy(output + RADIO_SNAPSHOT_HEADER_SIZE, payload, payload_size);
    put_u32(output + 20U, record_crc(output, payload_size));
    *written = RADIO_SNAPSHOT_HEADER_SIZE + payload_size;
    return RADIO_SNAPSHOT_OK;
}

radio_snapshot_result_t radio_snapshot_decode(
    const uint8_t *record, size_t length, uint32_t expected_kind,
    radio_snapshot_view_t *view)
{
    if (view != NULL) memset(view, 0, sizeof(*view));
    if (record == NULL || view == NULL || expected_kind == 0U || length < 6U)
        return RADIO_SNAPSHOT_INVALID;
    if (memcmp(record, "ONDS", 4U) != 0) return RADIO_SNAPSHOT_INVALID;
    /* Never silently select an older slot when another uses a newer format. */
    if (get_u16(record + 4U) != SNAPSHOT_VERSION) return RADIO_SNAPSHOT_UNSUPPORTED;
    if (length < RADIO_SNAPSHOT_HEADER_SIZE) return RADIO_SNAPSHOT_INVALID;
    if (get_u16(record + 6U) != RADIO_SNAPSHOT_HEADER_SIZE ||
        get_u32(record + 8U) == 0U || get_u32(record + 16U) != expected_kind)
        return RADIO_SNAPSHOT_INVALID;

    const size_t payload_size = get_u32(record + 12U);
    if (payload_size != length - RADIO_SNAPSHOT_HEADER_SIZE ||
        get_u32(record + 20U) != record_crc(record, payload_size))
        return RADIO_SNAPSHOT_INVALID;
    view->generation = get_u32(record + 8U);
    view->payload = record + RADIO_SNAPSHOT_HEADER_SIZE;
    view->payload_size = payload_size;
    return RADIO_SNAPSHOT_OK;
}

radio_snapshot_result_t radio_snapshot_select(
    const uint8_t *first, size_t first_size,
    const uint8_t *second, size_t second_size, uint32_t expected_kind,
    unsigned *selected)
{
    if (selected == NULL || expected_kind == 0U) return RADIO_SNAPSHOT_INVALID;
    *selected = 0U;
    radio_snapshot_view_t a, b;
    const radio_snapshot_result_t result_a =
        radio_snapshot_decode(first, first_size, expected_kind, &a);
    const radio_snapshot_result_t result_b =
        radio_snapshot_decode(second, second_size, expected_kind, &b);
    if (result_a == RADIO_SNAPSHOT_UNSUPPORTED || result_b == RADIO_SNAPSHOT_UNSUPPORTED)
        return RADIO_SNAPSHOT_UNSUPPORTED;
    const bool valid_a = result_a == RADIO_SNAPSHOT_OK;
    const bool valid_b = result_b == RADIO_SNAPSHOT_OK;
    if (!valid_a && !valid_b) return RADIO_SNAPSHOT_NO_VALID_SLOT;
    if (!valid_a) { *selected = 1U; return RADIO_SNAPSHOT_OK; }
    if (!valid_b) return RADIO_SNAPSHOT_OK;
    const uint32_t difference = a.generation - b.generation;
    if (difference == 0U) {
        if (first_size != second_size || memcmp(first, second, first_size) != 0)
            return RADIO_SNAPSHOT_AMBIGUOUS;
        return RADIO_SNAPSHOT_OK;
    }
    if (difference == UINT32_C(0x80000000)) return RADIO_SNAPSHOT_AMBIGUOUS;
    if (difference > UINT32_C(0x80000000)) *selected = 1U;
    return RADIO_SNAPSHOT_OK;
}
