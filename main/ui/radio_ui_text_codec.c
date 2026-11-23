#include "radio_ui_text_codec.h"

#include <stdbool.h>
#include <stdint.h>

static bool is_continuation(unsigned char value)
{
    return (value & 0xC0U) == 0x80U;
}

/* Return the canonical UTF-8 sequence length at p, or zero when invalid or
   incomplete. Each look-ahead is guarded by the preceding non-NUL byte. */
static size_t decode_sequence(const unsigned char *p, uint32_t *codepoint)
{
    if (p[0] < 0x80U) {
        *codepoint = p[0];
        return 1U;
    }
    if (p[0] >= 0xC2U && p[0] <= 0xDFU) {
        if (p[1] == '\0' || !is_continuation(p[1])) return 0U;
        *codepoint = ((uint32_t)(p[0] & 0x1FU) << 6) |
                     (uint32_t)(p[1] & 0x3FU);
        return 2U;
    }
    if (p[0] >= 0xE0U && p[0] <= 0xEFU) {
        if (p[1] == '\0' || p[2] == '\0' || !is_continuation(p[2])) {
            return 0U;
        }
        if ((p[0] == 0xE0U && p[1] < 0xA0U) ||
             (p[0] == 0xEDU && p[1] >= 0xA0U) || !is_continuation(p[1])) {
            return 0U;
        }
        *codepoint = ((uint32_t)(p[0] & 0x0FU) << 12) |
                     ((uint32_t)(p[1] & 0x3FU) << 6) |
                     (uint32_t)(p[2] & 0x3FU);
        return 3U;
    }
    if (p[0] >= 0xF0U && p[0] <= 0xF4U) {
        if (p[1] == '\0' || p[2] == '\0' || p[3] == '\0' ||
            !is_continuation(p[2]) || !is_continuation(p[3])) {
            return 0U;
        }
        if ((p[0] == 0xF0U && p[1] < 0x90U) ||
            (p[0] == 0xF4U && p[1] > 0x8FU) || !is_continuation(p[1])) {
            return 0U;
        }
        *codepoint = ((uint32_t)(p[0] & 0x07U) << 18) |
                     ((uint32_t)(p[1] & 0x3FU) << 12) |
                     ((uint32_t)(p[2] & 0x3FU) << 6) |
                     (uint32_t)(p[3] & 0x3FU);
        return 4U;
    }
    return 0U;
}

static bool glyph_is_supported(uint32_t codepoint)
{
    return (codepoint >= 0x00A0U && codepoint <= 0x017FU) ||
           (codepoint >= 0x0370U && codepoint <= 0x052FU) ||
           (codepoint >= 0x2000U && codepoint <= 0x206FU) ||
           (codepoint >= 0x20A0U && codepoint <= 0x20CFU) ||
           (codepoint >= 0xF000U && codepoint <= 0xF0A3U);
}

size_t radio_ui_text_format_for_font(char *display, size_t display_size,
                                     const char *text)
{
    if (display == NULL || display_size == 0U) return 0U;

    size_t used = 0U;
    const unsigned char *p = (const unsigned char *)(text != NULL ? text : "");
    while (*p != '\0' && used < display_size - 1U) {
        uint32_t codepoint = 0U;
        const size_t sequence_length = decode_sequence(p, &codepoint);
        if (sequence_length == 0U) {
            display[used++] = '?';
            ++p;
            continue;
        }

        if (codepoint < 0x80U || glyph_is_supported(codepoint)) {
            if (display_size - used <= sequence_length) break;
            for (size_t i = 0U; i < sequence_length; ++i) {
                display[used++] = (char)p[i];
            }
        } else {
            display[used++] = '?';
        }
        p += sequence_length;
    }
    display[used] = '\0';
    return used;
}
