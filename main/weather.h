#pragma once

#include <stdbool.h>

/* Current weather for the footer, fetched from wttr.in (the same source the
 * desktop's waybar script uses). wttr.in picks the location from the public IP,
 * so no coordinates have to be configured.
 *
 * A background task refreshes it every 30 minutes while WiFi is connected and
 * retries after 5 minutes on failure. The footer shows "无网络" when the
 * station link is down, so the old value is simply kept here. */

typedef struct {
    bool valid;        /* at least one successful fetch */
    int temp_c;
    char desc[16];     /* 晴 / 多云 / 阴 / 雨 / 雪 / 雷 / 雾 */
    char area[24];     /* nearest_area[0].areaName, e.g. "Nanjing" */
} weather_status_t;

/* Start the fetch task. `on_change` runs in that task when the values changed;
 * it should only wake the main task. */
void weather_init(void (*on_change)(void));

void weather_get(weather_status_t *out);
