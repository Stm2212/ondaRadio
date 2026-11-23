#pragma once

#include <stddef.h>

/* Convert bounded NUL-terminated UTF-8 to the glyph subset in bundled fonts.
   Returns bytes written and always terminates a non-empty destination. */
size_t radio_ui_text_format_for_font(char *display, size_t display_size,
                                     const char *text);
