# UI font extensions

These LVGL 9 font subsets add common Latin-1/Latin Extended-A, Greek, Cyrillic,
punctuation and currency glyphs to the built-in Montserrat fonts at 12, 14, 16
and 18 px. Latin glyphs come from Montserrat Medium; Greek and Cyrillic glyphs
come from Noto Sans. ASCII and the existing private-use UI icons fall back to
the matching built-in Montserrat font. Unsupported Unicode code points are
replaced by `?` by `radio_ui_text_codec`.

The generated glyph bitmaps use LVGL's built-in RLE compression. Enable
`CONFIG_LV_USE_FONT_COMPRESSED` in both firmware and UI-test configurations;
without it, LVGL can find accented glyphs but cannot draw their bitmaps.

The subsets were generated with `lv_font_conv` 1.5.3, 4 bpp, no kerning, and the
ranges encoded in each source file's generator comment. Montserrat Medium is
the variable Montserrat font from Google Fonts, instantiated at weight 500 with
FontTools. Noto Sans supplies Greek and Cyrillic glyphs. Their SIL Open Font Licenses 1.1
are included in [OFL.txt](OFL.txt) and [OFL-Noto.txt](OFL-Noto.txt).

To regenerate, obtain
[`Montserrat[wght].ttf`](https://github.com/google/fonts/blob/main/ofl/montserrat/Montserrat%5Bwght%5D.ttf),
instantiate `wght=500` as `Montserrat-Medium.ttf`, then run the matching
`lv_font_conv` command shown in each generated C file. Keep the resulting font
assets under the respective SIL Open Font Licenses.
