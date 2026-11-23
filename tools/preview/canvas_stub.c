/* Host-side canvas backend for the layout preview tool (no ESP-IDF needed). */
#include <stdio.h>
#include <stdlib.h>

#include "canvas.h"
#include "clock_ui.h"

#define W 400
#define H 300

static unsigned char fb[H][W];

void canvas_fill_rect(int x, int y, int w, int h)
{
    for (int r = y; r < y + h; r++) {
        for (int c = x; c < x + w; c++) {
            if (r >= 0 && r < H && c >= 0 && c < W) {
                fb[r][c] = 1;
            }
        }
    }
}

void canvas_clear_rect(int x, int y, int w, int h)
{
    for (int r = y; r < y + h; r++) {
        for (int c = x; c < x + w; c++) {
            if (r >= 0 && r < H && c >= 0 && c < W) {
                fb[r][c] = 0;
            }
        }
    }
}

int main(int argc, char **argv)
{
    int hour = argc > 1 ? atoi(argv[1]) : 14;
    int minute = argc > 2 ? atoi(argv[2]) : 32;
    int month = argc > 3 ? atoi(argv[3]) : 11;
    int day = argc > 4 ? atoi(argv[4]) : 18;
    int weekday = argc > 5 ? atoi(argv[5]) : 2;
    int art = argc > 7 ? atoi(argv[7]) : 0;
    int mode = argc > 8 ? atoi(argv[8]) : 0;

    clock_ui_set_art(art);
    if (mode == 3) {
        clock_ui_draw_waiting();
    } else {
        clock_ui_draw(hour, minute, month, day, weekday);
    }
    clock_ui_message(argc > 6 ? argv[6] : "今天也要好好吃饭呀。");

    /* mode 0: online; mode 1: offline; mode 2: never synced; mode 3: no valid
     * time yet (placeholders on a full refresh); mode 4: WiFi connecting. */
    clock_ui_status_t status = {
        .synced = mode != 2 && mode != 3 && mode != 4,
        .sync_failed = mode == 2 || mode == 3,
        .sync_hour = 16,
        .sync_min = 25,
        .wifi_connected = mode == 0,
        .wifi_connecting = mode == 4,
        .ssid = "MyWiFi",
        .weather = mode == 0 ? "Nanjing 多云 16°C" : "无网络",
        .weather_short = mode == 0 ? "Nanjing 16°C" : "无网络",
    };
    clock_ui_draw_status(&status);

    printf("P4\n%d %d\n", W, H);
    for (int r = 0; r < H; r++) {
        for (int b = 0; b < W / 8; b++) {
            unsigned char value = 0;
            for (int i = 0; i < 8; i++) {
                if (fb[r][b * 8 + i]) {
                    value |= 0x80 >> i;
                }
            }
            putchar(value);
        }
    }
    return 0;
}
