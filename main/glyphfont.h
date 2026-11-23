#pragma once

#include <stdint.h>

/* One glyph of a generated 1-bit bitmap font (see tools/gen_glyphfont.py).
 * Rows are stored MSB first, 1 = ink, and each glyph starts at `offset` in the
 * font's data array. */
typedef struct {
    uint32_t cp;        /* Unicode code point */
    uint32_t offset;    /* byte offset into the data array */
    uint8_t width;      /* bitmap width in pixels */
    uint8_t advance;    /* horizontal pen advance in pixels */
    uint8_t row_bytes;  /* width rounded up to whole bytes */
    uint8_t rows;       /* bitmap height (same for every glyph of a font) */
} glyph_t;

/* Width of a UTF-8 string in pixels at the given integer scale. */
int glyphfont_width(const glyph_t *table, int count, const char *text, int scale);

/* Draw a UTF-8 string with its top left corner at (x, y). Characters missing
 * from the subset are skipped (they still advance the pen). */
void glyphfont_draw(const glyph_t *table, int count, const uint8_t *data,
                    int x, int y, const char *text, int scale);
