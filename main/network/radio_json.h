#pragma once

#include <stddef.h>

#include "cJSON.h"

/* Parse exactly one JSON value from a bounded buffer. Trailing non-whitespace,
   including bytes after an embedded NUL, is rejected. */
cJSON *radio_json_parse_exact(const char *json, size_t length);
