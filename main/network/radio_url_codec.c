#include "radio_url_codec.h"

#include <string.h>

static int hex_value(unsigned char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool radio_url_form_decode(char *dst, size_t dst_size, const char *src,
                           size_t src_length)
{
    if (dst == NULL || dst_size == 0U) return false;
    dst[0] = '\0';
    if (src == NULL) return false;

    size_t written = 0U;
    for (size_t read = 0U; read < src_length; ++read) {
        unsigned char decoded = (unsigned char)src[read];
        if (decoded == '\0') goto invalid;
        if (decoded == '+') {
            decoded = ' ';
        } else if (decoded == '%') {
            if (src_length - read < 3U) goto invalid;
            const int high = hex_value((unsigned char)src[read + 1U]);
            const int low = hex_value((unsigned char)src[read + 2U]);
            if (high < 0 || low < 0) goto invalid;
            decoded = (unsigned char)((high << 4) | low);
            if (decoded == '\0') goto invalid;
            read += 2U;
        }

        if (written >= dst_size - 1U) goto invalid;
        dst[written++] = (char)decoded;
    }
    dst[written] = '\0';
    return true;

invalid:
    memset(dst, 0, dst_size);
    return false;
}
