/*
 * Weather footer: wttr.in's j1 JSON, trimmed down to condition + temperature.
 *
 * The JSON is about 40 KB but the fields we need all sit in the first kilobyte
 * (current_condition comes first, nearest_area right after it), so only the
 * first 4 KB of the response are read.
 */

#include "weather.h"

#include <stdlib.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "net_time.h"

#define URL             "http://wttr.in/?format=j1"
#define READ_MAX        4096
#define REFRESH_OK_MS   (30 * 60 * 1000)
#define REFRESH_ERR_MS  (5 * 60 * 1000)
#define RETRY_LINK_MS   (10 * 1000)

static const char *TAG = "weather";

static weather_status_t s_weather;
static void (*s_change_cb)(void);
static char s_buf[READ_MAX];

/* Find `"key": <number>` in the JSON fragment and store it. */
static bool json_int(const char *buf, const char *key, int *out)
{
    const char *p = strstr(buf, key);

    if (!p) {
        return false;
    }
    p += strlen(key);
    while (*p && *p != ':') {
        p++;
    }
    if (!*p) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '"') {
        p++;
    }

    char *end = NULL;
    long value = strtol(p, &end, 10);
    if (end == p) {
        return false;
    }
    *out = (int)value;
    return true;
}

/* Find `"key": [ { "value": "..." } ]` and copy the first value. */
static bool json_name(const char *buf, const char *key, char *out,
                      size_t out_size)
{
    const char *p = strstr(buf, key);
    if (!p) {
        return false;
    }
    p = strstr(p, "\"value\"");
    if (!p) {
        return false;
    }
    p += 7;
    p = strchr(p, ':');
    if (!p) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;

    size_t o = 0;
    while (*p && *p != '"' && o + 1 < out_size) {
        out[o++] = *p++;
    }
    out[o] = '\0';
    return o > 0;
}

/* wttr.in uses the same WWO condition codes as the waybar script. */
static const char *condition_text(int code)
{
    switch (code) {
    case 113:
        return "晴";
    case 116:
        return "多云";
    case 119:
    case 122:
        return "阴";
    case 143:
    case 149:
    case 248:
    case 260:
        return "雾";
    case 200:
    case 386:
    case 389:
    case 392:
        return "雷";
    case 227:
    case 230:
    case 329:
    case 332:
    case 335:
    case 338:
    case 368:
    case 371:
        return "雪";
    case 176:
    case 179:
    case 182:
    case 185:
    case 281:
    case 284:
    case 293:
    case 296:
    case 299:
    case 302:
    case 305:
    case 308:
    case 311:
    case 314:
    case 317:
    case 320:
    case 323:
    case 326:
    case 350:
    case 353:
    case 356:
    case 359:
    case 362:
    case 365:
    case 374:
    case 377:
    case 395:
        return "雨";
    default:
        return "多云";
    }
}

static bool fetch(void)
{
    esp_http_client_config_t cfg = {
        .url = URL,
        .timeout_ms = 20000,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .user_agent = "DailySpark/1.0",
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return false;
    }

    bool ok = false;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);

        int total = 0;
        while (total < READ_MAX - 1) {
            int n = esp_http_client_read(client, s_buf + total,
                                         READ_MAX - 1 - total);
            if (n <= 0) {
                break;
            }
            total += n;
        }
        s_buf[total] = '\0';
        esp_http_client_close(client);

        int code;
        int temp;
        if (total > 0 && json_int(s_buf, "\"weatherCode\"", &code) &&
            json_int(s_buf, "\"temp_C\"", &temp)) {
            weather_status_t next = {
                .valid = true,
                .temp_c = temp,
            };
            strlcpy(next.desc, condition_text(code), sizeof(next.desc));
            json_name(s_buf, "\"areaName\"", next.area, sizeof(next.area));

            bool changed = !s_weather.valid ||
                           s_weather.temp_c != next.temp_c ||
                           strcmp(s_weather.desc, next.desc) != 0 ||
                           strcmp(s_weather.area, next.area) != 0;
            s_weather = next;
            if (changed && s_change_cb) {
                s_change_cb();
            }
            ESP_LOGI(TAG, "weather: %s %s %d C (code %d)", next.area,
                     next.desc, next.temp_c, code);
            ok = true;
        } else {
            ESP_LOGW(TAG, "could not parse the weather response (%d bytes)",
                     total);
        }
    } else {
        ESP_LOGW(TAG, "weather request failed");
        esp_http_client_close(client);
    }

    esp_http_client_cleanup(client);
    return ok;
}

static void weather_task(void *arg)
{
    (void)arg;
    TickType_t last_ok = 0;
    bool need = true;

    /* Give the boot full refresh (and the first NTP exchange) the radio to
     * itself: a burst of HTTP traffic during the waveform greys the panel. */
    vTaskDelay(pdMS_TO_TICKS(15000));

    for (;;) {
        net_time_status_t net;
        net_time_status(&net);

        if (!net.connected) {
            need = true;
            vTaskDelay(pdMS_TO_TICKS(RETRY_LINK_MS));
            continue;
        }

        TickType_t now = xTaskGetTickCount();
        bool due = need || last_ok == 0 ||
                   (now - last_ok) >= pdMS_TO_TICKS(REFRESH_OK_MS);
        if (!due) {
            vTaskDelay(pdMS_TO_TICKS(30 * 1000));
            continue;
        }

        if (fetch()) {
            last_ok = xTaskGetTickCount();
            need = false;
            vTaskDelay(pdMS_TO_TICKS(REFRESH_OK_MS));
        } else {
            need = true;
            vTaskDelay(pdMS_TO_TICKS(REFRESH_ERR_MS));
        }
    }
}

void weather_init(void (*on_change)(void))
{
    s_change_cb = on_change;
    xTaskCreate(weather_task, "weather", 5120, NULL, 3, NULL);
}

void weather_get(weather_status_t *out)
{
    *out = s_weather;
}
