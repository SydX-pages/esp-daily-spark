#include "glyphfont.h"

#include <stddef.h>

#include "canvas.h"

static uint32_t utf8_next(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t cp;
    int extra;

    if (s[0] < 0x80) {
        *p += 1;
        return s[0];
    }
    if ((s[0] & 0xE0) == 0xC0) {
        cp = s[0] & 0x1F;
        extra = 1;
    } else if ((s[0] & 0xF0) == 0xE0) {
        cp = s[0] & 0x0F;
        extra = 2;
    } else if ((s[0] & 0xF8) == 0xF0) {
        cp = s[0] & 0x07;
        extra = 3;
    } else {
        *p += 1;            /* invalid byte, skip it */
        return 0xFFFD;
    }

    for (int i = 1; i <= extra; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *p += 1;
            return 0xFFFD;
        }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *p += extra + 1;
    return cp;
}

/* The generated tables are sorted by code point. */
static const glyph_t *find_glyph(const glyph_t *table, int count, uint32_t cp)
{
    int lo = 0;
    int hi = count - 1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (table[mid].cp == cp) {
            return &table[mid];
        }
        if (table[mid].cp < cp) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

int glyphfont_width(const glyph_t *table, int count, const char *text, int scale)
{
    int width = 0;

    while (*text) {
        uint32_t cp = utf8_next(&text);
        const glyph_t *g = find_glyph(table, count, cp);
        /* Missing glyph: keep the cell width of the script, so a character
         * outside the subset does not shift the rest of the line. */
        width += (g ? g->advance : (cp >= 0x2E80 ? 16 : 8)) * scale;
    }
    return width;
}

void glyphfont_draw(const glyph_t *table, int count, const uint8_t *data,
                    int x, int y, const char *text, int scale)
{
    while (*text) {
        uint32_t cp = utf8_next(&text);
        const glyph_t *g = find_glyph(table, count, cp);

        if (!g) {
            x += (cp >= 0x2E80 ? 16 : 8) * scale;
            continue;
        }

        const uint8_t *rows = data + g->offset;
        for (int r = 0; r < g->rows; r++) {
            for (int b = 0; b < g->row_bytes; b++) {
                uint8_t value = rows[r * g->row_bytes + b];
                if (value == 0) {
                    continue;
                }
                for (int bit = 0; bit < 8; bit++) {
                    int px = b * 8 + bit;
                    if (px >= g->width) {
                        break;
                    }
                    if (value & (0x80 >> bit)) {
                        canvas_fill_rect(x + px * scale, y + r * scale,
                                         scale, scale);
                    }
                }
            }
        }
        x += g->advance * scale;
    }
}
