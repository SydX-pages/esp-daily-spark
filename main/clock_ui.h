#pragma once

#include <stdbool.h>

/* Clock screen layout. Draws into the canvas (see canvas.h), so the same code
 * is used on the panel and in the host preview tool.
 *
 * Layout (400x300):
 *
 *   +----------------------------------------------+
 *   | 11月21日 星期六   02:14  [ANSI portrait]     |
 *   |                                              |
 *   | 　　今天也要好好吃饭呀。                      |
 *   | 慢一点也没关系，方向对就好。                  |
 *   | （句子从状态栏下方开始，前两行绕开右上角形象）|
 *   |----------------------------------------------|
 *   | 上次同步 16:25 | MyWiFi | Nanjing 多云 16°C  |
 *   +----------------------------------------------+
 */

/* Rectangle that contains the HH:MM digits plus a small margin. Only this
 * area is redrawn and refreshed when the minute changes. */
#define CLOCK_UI_TIME_X 170
#define CLOCK_UI_TIME_Y 2
#define CLOCK_UI_TIME_W 88
#define CLOCK_UI_TIME_H 42

/* Footer strip (hairline + status text). Redrawn on its own when WiFi, the
 * weather or the last sync time changes. */
#define CLOCK_UI_FOOTER_X 0
#define CLOCK_UI_FOOTER_Y 248
#define CLOCK_UI_FOOTER_W 400
#define CLOCK_UI_FOOTER_H 52

/* Longest single rendered line, in UTF-8 bytes (buffer size). */
#define QUOTE_LINE_MAX 160

/* Redraw only the time: clears the time rectangle and draws HH:MM. */
void clock_ui_draw_time(int hour, int minute);

/* Full screen: status line on top, ANSI art top right. Does not draw the
 * sentence or the footer, call clock_ui_message() and clock_ui_draw_status(). */
void clock_ui_draw(int hour, int minute, int month, int day, int weekday);

/* Frame for a clock that has no valid time yet: "--:--" and a small
 * "未获取时间" note instead of the date, plus the ANSI art. The sentence and
 * footer are drawn as usual, so the panel is usable without a network. */
void clock_ui_draw_waiting(void);

/* Daily sentence, flowing around the portrait. Top aligned below the status
 * bar, left aligned with a first-line indent. */
void clock_ui_message(const char *text);

/* Select which embedded ANSI portrait clock_ui_draw() uses. */
void clock_ui_set_art(int index);

typedef struct {
    bool synced;          /* NTP succeeded at least once this boot */
    bool sync_failed;     /* no answer within the grace period */
    int sync_hour;        /* local time of the last successful sync */
    int sync_min;
    bool wifi_connected;  /* station link is up, show the SSID */
    bool wifi_connecting; /* still trying, show 连接中 */
    const char *ssid;     /* joined network; may be short/empty */
    const char *weather;  /* "area desc 16°C", "无网络", ... */
    const char *weather_short; /* "area 16°C", used if the line is full */
} clock_ui_status_t;

/* Hairline plus one 16 px status line: sync time | WiFi | weather. The WiFi
 * slot shows only the SSID, 连接中 or 未连接; the weather slot starts with the
 * place name so the reading has a location. */
void clock_ui_draw_status(const clock_ui_status_t *status);
