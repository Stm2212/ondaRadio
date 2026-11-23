#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Decode an explicitly bounded form/query source slice. Invalid
   escapes, encoded NUL, or output truncation fail closed and clear dst. */
bool radio_url_form_decode(char *dst, size_t dst_size, const char *src,
                           size_t src_length);
