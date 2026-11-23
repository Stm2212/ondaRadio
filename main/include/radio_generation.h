#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Generation zero is reserved for an uninitialized/invalid event. */
static inline uint32_t radio_generation_next(uint32_t current)
{
    ++current;
    if (current == 0U) ++current;
    return current;
}

/* Serial-number ordering; callers must not compare generations >2^31 apart. */
static inline bool radio_generation_is_newer(uint32_t candidate,
                                              uint32_t current)
{
    return candidate != 0U && candidate != current &&
           (uint32_t)(candidate - current) < UINT32_C(0x80000000);
}

static inline bool radio_generation_is_current(uint32_t candidate,
                                                uint32_t current)
{
    return candidate != 0U && candidate == current;
}
