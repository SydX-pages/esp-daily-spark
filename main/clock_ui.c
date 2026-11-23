#include "clock_ui.h"

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "ansi_art.h"
#include "canvas.h"
#include "font_bar.h"
#include "font_status.h"
#include "font_text.h"

#define SCREEN_W        400

/* Top status bar: small date on the left, larger time next to it, both in the
 * free area left of the thumbnail. */
#define BAR_DATE_X      38
#define BAR_DATE_Y      16
#define BAR_DATE_SCALE  1

#define BAR_TIME_X      174
#define BAR_TIME_Y      6
#define BAR_TIME_SCALE  2

/* ANSI portrait. One character cell is about twice as tall as it is wide, so
 * the bitmap is stretched vertically to restore its proportions. Several
 * portraits are embedded; clock_ui_set_art() picks the one drawn next. */
#define ART_X           292
#define ART_Y           8
#define ART_SCALE_Y     2
#define ART_GAP         10

static int s_art;

/* Sentence area: starts a little below the status bar and runs down to the
 * footer. Lines that sit next to the portrait use a narrower column, so the
 * sentence flows around it; once below the portrait the full width is used.
 * The body font is a 24 px bitmap font (33 px cell). */
#define MSG_X           16
#define MSG_TOP         58
#define MSG_W           (SCREEN_W - 2 * MSG_X)
#define MSG_COL_W       (ART_X - MSG_X - ART_GAP)
#define MSG_SCALE       1
#define MSG_LINE_H      37
#define MSG_MAX_LINES   5
#define MSG_LINE_TWEAK  0
#define MSG_EM          24
#define MSG_INDENT      (2 * MSG_EM)

/* Footer: hairline plus one 16 px line. The sentence keeps five lines so its
 * last row (y=241) stays clear of the hairline. The weather is right aligned,
 * the WiFi slot takes whatever is left and clips a long SSID. */
#define FOOTER_LINE_Y   250
#define FOOTER_TEXT_Y   260
#define FOOTER_LEFT     12
#define FOOTER_RIGHT    (SCREEN_W - 12)
#define FOOTER_GAP      8
#define FOOTER_MIN_WIFI_W 32

static const char *const WEEKDAY_CN[7] = {
    "日", "一", "二", "三", "四", "五", "六",
};

/* 1-bit bitmap, rows MSB first, 1 = ink. */
static void draw_bitmap(int x, int y, const uint8_t *rows, int row_bytes,
                        int w, int h, int scale_x, int scale_y)
{
    for (int r = 0; r < h; r++) {
        for (int b = 0; b < row_bytes; b++) {
            uint8_t value = rows[r * row_bytes + b];
            if (value == 0) {
                continue;
            }
            for (int bit = 0; bit < 8; bit++) {
                int px = b * 8 + bit;
                if (px >= w) {
                    break;
                }
                if (value & (0x80 >> bit)) {
                    canvas_fill_rect(x + px * scale_x, y + r * scale_y,
                                     scale_x, scale_y);
                }
            }
        }
    }
}

static void draw_art(int x, int y)
{
    const ansi_art_t *art = &ansi_arts[s_art];

    draw_bitmap(x, y, art->data, art->row_bytes,
                art->width, art->height, 1, ART_SCALE_Y);
}

void clock_ui_set_art(int index)
{
    int count = ANSI_ART_COUNT;

    index %= count;
    if (index < 0) {
        index += count;
    }
    s_art = index;
}

/* Byte length of the UTF-8 sequence starting at s (1 for invalid bytes). */
static int utf8_len(const char *s)
{
    unsigned char c = (unsigned char)*s;

    if (c < 0x80) {
        return 1;
    }
    if ((c & 0xE0) == 0xC0) {
        return 2;
    }
    if ((c & 0xF0) == 0xE0) {
        return 3;
    }
    if ((c & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;
}

/* Width of one UTF-8 character on the panel. */
static int char_width(const char *s, int len)
{
    char buf[5];

    memcpy(buf, s, (size_t)len);
    buf[len] = '\0';
    return glyphfont_width(font_text_glyphs, FONT_TEXT_COUNT, buf, MSG_SCALE);
}

void clock_ui_draw_time(int hour, int minute)
{
    char clock[8];

    canvas_clear_rect(CLOCK_UI_TIME_X, CLOCK_UI_TIME_Y,
                      CLOCK_UI_TIME_W, CLOCK_UI_TIME_H);

    snprintf(clock, sizeof(clock), "%02d:%02d", hour, minute);
    glyphfont_draw(font_bar_glyphs, FONT_BAR_COUNT, font_bar_data,
                   BAR_TIME_X, BAR_TIME_Y, clock, BAR_TIME_SCALE);
}

/* Characters that must not start a line (Chinese typesetting). */
static bool no_line_start(const char *s)
{
    static const char *const BAD[] = {
        "，", "。", "、", "；", "：", "？", "！", "）", "】", "》", "」",
        "』", "”", "’", "…", "·",
    };

    int len = utf8_len(s);
    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        if ((int)strlen(BAD[i]) == len && memcmp(BAD[i], s, (size_t)len) == 0) {
            return true;
        }
    }
    return false;
}

/* Characters that end a natural phrase: a line may break right after them. */
static bool break_after(const char *s)
{
    static const char *const PUNCT[] = {
        "，", "。", "、", "；", "：", "！", "？", "…",
        ",", ".", ";", ":", "!", "?",
    };

    int len = utf8_len(s);
    for (size_t i = 0; i < sizeof(PUNCT) / sizeof(PUNCT[0]); i++) {
        if ((int)strlen(PUNCT[i]) == len && memcmp(PUNCT[i], s, (size_t)len) == 0) {
            return true;
        }
    }
    return false;
}

/* Width of the rest of the current paragraph (stops at an explicit newline). */
static int text_width_until_newline(const char *s)
{
    int width = 0;

    while (*s && *s != '\n') {
        int len = utf8_len(s);
        width += char_width(s, len);
        s += len;
    }
    return width;
}

void clock_ui_message(const char *text)
{
    static char line[QUOTE_LINE_MAX];

    const char *p = text;

    /* Measure first: line count decides how far the block reaches, and each
     * line's width depends on whether it sits next to the portrait. */
    const char *starts[MSG_MAX_LINES];
    int ends[MSG_MAX_LINES];
    int indents[MSG_MAX_LINES];
    int line_count = 0;

    int art_bottom = ART_Y + ansi_arts[s_art].height * ART_SCALE_Y;

    while (*p && line_count < MSG_MAX_LINES) {
        const char *start = p;
        const char *end = p;
        const char *last = p;
        const char *space = NULL;
        const char *punct = NULL;
        int punct_w = 0;
        int width = 0;

        int line_top = MSG_TOP + line_count * MSG_LINE_H;
        /* Keep clear of the portrait for every line that overlaps it. The +6
         * accounts for the top padding inside the glyph cell. */
        int col_w = (line_top + 6 < art_bottom) ? MSG_COL_W : MSG_W;

        int indent = (line_count == 0) ? MSG_INDENT : 0;
        /* Narrow first line: if the whole sentence only just misses with the
         * full indent, shrink the indent (whole characters) instead of
         * pushing one or two characters onto a second line. */
        if (indent > 0) {
            int total = text_width_until_newline(p);
            if (total <= col_w && total + indent > col_w) {
                indent = col_w - total;
                indent -= indent % MSG_EM;
                if (indent < 0) {
                    indent = 0;
                }
            }
        }
        int avail = col_w - indent;

        while (*end && *end != '\n') {
            int len = utf8_len(end);
            int w = char_width(end, len);
            if (width + w > avail) {
                break;
            }
            width += w;
            last = end;
            end += len;
            if (end[-1] == ' ') {
                space = end - 1;
            }
            if (break_after(end - len)) {
                punct = end;
                punct_w = width;
            }
        }

        /* Prefer a break after a comma/period (natural Chinese phrasing), but
         * only if that does not leave a very short first line. Then try a space
         * for Latin text. */
        if (*end != '\0' && *end != '\n') {
            if (punct && punct > start && punct_w * 5 >= avail * 3) {
                end = punct;
            } else if (space && space > start) {
                end = space;
            }
            /* Never leave closing punctuation alone at the start of a line:
             * pull the previous character down with it. */
            if (*end != '\0' && *end != '\n' && no_line_start(end) && last > start) {
                end = last;
            }
        }
        if (end == start) {
            end = start + utf8_len(start); /* never emit an empty line */
        }

        starts[line_count] = start;
        ends[line_count] = (int)(end - start);
        indents[line_count] = indent;
        line_count++;

        p = end;
        if (*p == '\n') {
            p++;
        }
        /* Skip the spaces a wrapped line left behind. */
        while (*p == ' ') {
            p++;
        }
    }

    int y0 = MSG_TOP;
    int glyph_h = font_text_glyphs[0].rows;

    for (int i = 0; i < line_count; i++) {
        int len = ends[i];
        if (len >= QUOTE_LINE_MAX) {
            len = QUOTE_LINE_MAX - 1;
        }
        memcpy(line, starts[i], (size_t)len);
        line[len] = '\0';

        int x = MSG_X + indents[i];
        int y = y0 + i * MSG_LINE_H + (MSG_LINE_H - glyph_h) / 2 +
                MSG_LINE_TWEAK / 2;

        glyphfont_draw(font_text_glyphs, FONT_TEXT_COUNT, font_text_data,
                       x, y, line, MSG_SCALE);
    }
}

void clock_ui_draw(int hour, int minute, int month, int day, int weekday)
{
    char date[32];

    snprintf(date, sizeof(date), "%d月%d日 星期%s", month, day,
             WEEKDAY_CN[weekday % 7]);
    glyphfont_draw(font_bar_glyphs, FONT_BAR_COUNT, font_bar_data,
                   BAR_DATE_X, BAR_DATE_Y, date, BAR_DATE_SCALE);

    clock_ui_draw_time(hour, minute);

    draw_art(ART_X, ART_Y);
}

void clock_ui_draw_waiting(void)
{
    /* No date is known yet, so the small line says why the clock is blank
     * instead of showing 1970-01-01. */
    glyphfont_draw(font_status_glyphs, FONT_STATUS_COUNT, font_status_data,
                   BAR_DATE_X, BAR_DATE_Y, "未获取时间", 1);

    canvas_clear_rect(CLOCK_UI_TIME_X, CLOCK_UI_TIME_Y,
                      CLOCK_UI_TIME_W, CLOCK_UI_TIME_H);
    glyphfont_draw(font_bar_glyphs, FONT_BAR_COUNT, font_bar_data,
                   BAR_TIME_X, BAR_TIME_Y, "--:--", BAR_TIME_SCALE);

    draw_art(ART_X, ART_Y);
}

/* ------------------------------------------------------------------------- */
/* Footer                                                                    */
/* ------------------------------------------------------------------------- */

static int status_width(const char *text)
{
    return glyphfont_width(font_status_glyphs, FONT_STATUS_COUNT, text, 1);
}

/* Draw at most max_w pixels of `text` (used for the SSID, which has no fixed
 * length). Returns the width actually drawn. */
static int draw_clipped(int x, int y, const char *text, int max_w)
{
    int used = 0;

    while (*text) {
        int len = utf8_len(text);
        char one[5];

        memcpy(one, text, (size_t)len);
        one[len] = '\0';

        int w = status_width(one);
        if (used + w > max_w) {
            break;
        }
        glyphfont_draw(font_status_glyphs, FONT_STATUS_COUNT, font_status_data,
                       x + used, y, one, 1);
        used += w;
        text += len;
    }
    return used;
}

void clock_ui_draw_status(const clock_ui_status_t *st)
{
    char buf[48];
    const char *weather;
    const char *wifi_text;
    int usable = FOOTER_RIGHT - FOOTER_LEFT;
    int sep_w = status_width("|");
    int x = FOOTER_LEFT;
    int y = FOOTER_TEXT_Y;
    int sync_w;
    int weather_w;
    int weather_x;
    int sep2_x;
    int wifi_max;

    canvas_clear_rect(CLOCK_UI_FOOTER_X, CLOCK_UI_FOOTER_Y,
                      CLOCK_UI_FOOTER_W, CLOCK_UI_FOOTER_H);
    canvas_fill_rect(FOOTER_LEFT, FOOTER_LINE_Y,
                     FOOTER_RIGHT - FOOTER_LEFT, 1);

    /* Left: when the last sync happened, or why there is none yet. */
    if (st->synced) {
        snprintf(buf, sizeof(buf), "上次同步 %02d:%02d", st->sync_hour,
                 st->sync_min);
    } else if (st->sync_failed) {
        strlcpy(buf, "无法同步", sizeof(buf));
    } else {
        strlcpy(buf, "同步中", sizeof(buf));
    }
    sync_w = status_width(buf);
    glyphfont_draw(font_status_glyphs, FONT_STATUS_COUNT, font_status_data,
                   x, y, buf, 1);
    x += sync_w + FOOTER_GAP;

    glyphfont_draw(font_status_glyphs, FONT_STATUS_COUNT, font_status_data,
                   x, y, "|", 1);
    x += sep_w + FOOTER_GAP;

    /* The weather is right aligned. If the full "area desc temp" text would
     * squeeze the WiFi slot too much, fall back to "area temp"; as a last
     * resort it is clipped instead of running off the panel. */
    weather = st->weather ? st->weather : "天气 --";
    weather_w = status_width(weather);
    int avail = usable - sync_w - 2 * (2 * FOOTER_GAP + sep_w);
    if (weather_w > avail - FOOTER_MIN_WIFI_W) {
        const char *shorter = st->weather_short;

        if (shorter && shorter[0] &&
            status_width(shorter) <= avail - FOOTER_MIN_WIFI_W) {
            weather = shorter;
            weather_w = status_width(shorter);
        } else {
            weather_w = avail - FOOTER_MIN_WIFI_W;
            if (weather_w < 0) {
                weather_w = 0;
            }
        }
    }

    weather_x = FOOTER_RIGHT - weather_w;
    sep2_x = weather_x - FOOTER_GAP - sep_w;
    wifi_max = sep2_x - FOOTER_GAP - x;
    if (wifi_max < 0) {
        wifi_max = 0;
    }

    /* Middle: the SSID once connected, 连接中 while trying, 未连接 otherwise. */
    if (st->wifi_connected && st->ssid && st->ssid[0]) {
        wifi_text = st->ssid;
    } else if (st->wifi_connecting) {
        wifi_text = "连接中";
    } else {
        wifi_text = "未连接";
    }
    draw_clipped(x, y, wifi_text, wifi_max);

    glyphfont_draw(font_status_glyphs, FONT_STATUS_COUNT, font_status_data,
                   sep2_x, y, "|", 1);
    draw_clipped(weather_x, y, weather, weather_w);
}
