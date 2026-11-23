#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RADIO_SNAPSHOT_HEADER_SIZE 24U

typedef enum {
    RADIO_SNAPSHOT_OK = 0,
    RADIO_SNAPSHOT_INVALID,
    RADIO_SNAPSHOT_NO_SPACE,
    RADIO_SNAPSHOT_NO_VALID_SLOT,
    RADIO_SNAPSHOT_AMBIGUOUS,
    RADIO_SNAPSHOT_UNSUPPORTED,
} radio_snapshot_result_t;

typedef struct {
    uint32_t generation;
    const uint8_t *payload;
    size_t payload_size;
} radio_snapshot_view_t;

/* Output and payload must not overlap. Kind and generation must be nonzero. */
radio_snapshot_result_t radio_snapshot_encode(
    uint8_t *output, size_t capacity, uint32_t kind, uint32_t generation,
    const uint8_t *payload, size_t payload_size, size_t *written);

/* The returned payload points into record and is valid only while it is alive. */
radio_snapshot_result_t radio_snapshot_decode(
    const uint8_t *record, size_t length, uint32_t expected_kind,
    radio_snapshot_view_t *view);

/* Selects a valid newer record. Equal generations are accepted only when the
   complete records are identical; serial numbers half a range apart are
   ambiguous. An absent or malformed slot is not selected; an unknown format
   version blocks selection. I/O errors must be handled by the caller and must
   not be passed off as an absent slot. */
radio_snapshot_result_t radio_snapshot_select(
    const uint8_t *first, size_t first_size,
    const uint8_t *second, size_t second_size, uint32_t expected_kind,
    unsigned *selected);
