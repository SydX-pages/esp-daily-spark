/*
 * Desktop clock on a 4.2 inch e-Paper (400x300, SSD1683) with an ESP32-C3.
 *
 * Time keeping: the WiFi station connects at boot and stays up (the footer
 * shows its state and the weather needs it). SNTP sets the clock when the link
 * comes up, then once a day. The panel is usable immediately: it shows the
 * retained time, or a "syncing" screen until the first answer.
 *
 * Refreshing: the first frame and the daily frame use a full refresh. Between
 * them the HH:MM rectangle and the footer strip use the panel's partial
 * waveform, so the digits and the status change without flashing the whole
 * screen. A full refresh once a day clears the ghosting that partial updates
 * leave behind.
 */

#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "art.h"
#include "clock_ui.h"
#include "epd4in2.h"
#include "net_time.h"
#include "quotes.h"
#include "weather.h"
#include "web_portal.h"

static const char *TAG = "clock";

/* Main task, woken up by the web portal when a setting changed. */
static TaskHandle_t s_main_task;
static volatile bool s_portal_dirty;
static volatile bool s_status_dirty;

/* Revisions of what is on the panel right now. */
static uint32_t s_painted_rev;
static uint32_t s_painted_art_rev;

/* True once the frame on the panel shows a real time (not "--:--"). */
static bool s_painted_valid;
/* Set once the first NTP answer has been painted. */
static bool s_synced_seen;

/* Full refresh at least this often, to clear partial-update ghosting. The
 * daily new-day refresh already does one; this is only a fallback for the
 * case where the clock could not sync for a whole day. */
#define FULL_REFRESH_S  (24 * 60 * 60)

/* Let the HTTP response and the WiFi radio go quiet before a full refresh.
 * The refresh is synchronous and blocks the main task; if the radio is still
 * transmitting, the supply sags and the waveform comes out grey. */
#define PORTAL_SETTLE_MS 2000

/* Partial refresh cadence: exactly once per minute, when the displayed minute
 * changes. Nothing is refreshed in between. */

static void build_status(clock_ui_status_t *st)
{
    static char weather_full[64];
    static char weather_short[48];
    static char ssid_text[33];

    net_time_status_t net;
    weather_status_t weather;

    net_time_status(&net);
    weather_get(&weather);

    st->synced = net.synced;
    st->sync_failed = net.sync_failed;
    st->sync_hour = net.sync_hour;
    st->sync_min = net.sync_min;
    st->wifi_connected = net.connected;
    st->wifi_connecting = net.configured && !net.connected && net.reason == 0;

    /* `net` lives on this stack frame, so st->ssid must not point into it:
     * clock_ui_draw_status() runs after build_status() has returned, and the
     * freed stack slot was being reused by the footer's own text buffer. That
     * turned the SSID into garbage such as "B同步中". */
    strlcpy(ssid_text, net.ssid, sizeof(ssid_text));
    st->ssid = ssid_text;

    if (!net.connected) {
        strlcpy(weather_full, "无网络", sizeof(weather_full));
        strlcpy(weather_short, "无网络", sizeof(weather_short));
    } else if (weather.valid) {
        if (weather.area[0]) {
            /* wttr.in only reports the nearest town in English; keep it in
             * front of the condition so the temperature has a location. */
            snprintf(weather_full, sizeof(weather_full), "%s %s %d°C",
                     weather.area, weather.desc, weather.temp_c);
            snprintf(weather_short, sizeof(weather_short), "%s %d°C",
                     weather.area, weather.temp_c);
        } else {
            snprintf(weather_full, sizeof(weather_full), "%s %d°C",
                     weather.desc, weather.temp_c);
            strlcpy(weather_short, weather_full, sizeof(weather_short));
        }
    } else {
        strlcpy(weather_full, "天气 --", sizeof(weather_full));
        strlcpy(weather_short, "天气 --", sizeof(weather_short));
    }
    st->weather = weather_full;
    st->weather_short = weather_short;
}

static void render_full_screen(void)
{
    epd_clear();
    clock_ui_set_art(art_index());

    if (!net_time_clock_valid()) {
        ESP_LOGI(TAG, "frame: no valid time yet, showing placeholders");
        clock_ui_draw_waiting();
    } else {
        time_t now = time(NULL);
        struct tm local;
        localtime_r(&now, &local);

        ESP_LOGI(TAG, "frame: %04d-%02d-%02d %02d:%02d (wday %d)",
                 local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                 local.tm_hour, local.tm_min, local.tm_wday);

        clock_ui_draw(local.tm_hour, local.tm_min, local.tm_mon + 1,
                      local.tm_mday, local.tm_wday);
    }

    /* The sentence and the footer are shown in both cases, so an offline
     * clock is still usable and simply says 无网络 / 无法同步. */
    clock_ui_message(quote_text());

    clock_ui_status_t status;
    build_status(&status);
    clock_ui_draw_status(&status);
}

static void refresh_full(void)
{
    /* Capture the revisions before drawing: if the web page changes the
     * sentence while the panel is refreshing, the stored revisions stay
     * behind and the next wake paints the new text. */
    uint32_t rev = quote_revision();
    uint32_t arev = art_revision();
    render_full_screen();

    esp_err_t err = epd_update();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "full refresh failed: %s", esp_err_to_name(err));
    }
    s_painted_rev = rev;
    s_painted_art_rev = arev;
}

/* Full refresh triggered by the web page. The redirect is already on the
 * wire, but the phone's follow-up requests and the radio's own traffic can
 * still overlap the waveform; a full refresh that runs during that window
 * comes out slightly grey (the panel is fine, the radio just sags the rail).
 * Wait a moment, then run the complete black/white cycle twice: the second
 * pass always lands with the link idle and leaves a clean white background.
 * If the sentence changes again during the first pass, stop there and let the
 * main loop paint the newest text. */
static void refresh_full_from_portal(void)
{
    xTaskNotifyWait(0, 0xFFFFFFFF, NULL, pdMS_TO_TICKS(PORTAL_SETTLE_MS));
    /* Both passes must show the same frame: art and sentence were chosen
     * before this call, and refresh_full() only reads them. */
    refresh_full();
    if (quote_revision() != s_painted_rev ||
        art_revision() != s_painted_art_rev) {
        return;
    }
    refresh_full();
}

/* Draws only the time rectangle and refreshes it with the partial waveform.
 * Called when the displayed minute changes. */
static esp_err_t refresh_time(int hour, int minute)
{
    clock_ui_draw_time(hour, minute);
    return epd_update_region(CLOCK_UI_TIME_X, CLOCK_UI_TIME_Y,
                             CLOCK_UI_TIME_W, CLOCK_UI_TIME_H);
}

/* Same, for the footer strip: sync time, WiFi state and weather. */
static esp_err_t refresh_status(void)
{
    clock_ui_status_t status;

    build_status(&status);
    clock_ui_draw_status(&status);
    return epd_update_region(CLOCK_UI_FOOTER_X, CLOCK_UI_FOOTER_Y,
                             CLOCK_UI_FOOTER_W, CLOCK_UI_FOOTER_H);
}

/* Runs in the HTTP server task. The panel is only touched by the main task, so
 * this just raises a flag and wakes it up. */
static void on_portal_change(void)
{
    s_portal_dirty = true;
    if (s_main_task) {
        xTaskNotifyGive(s_main_task);
    }
}

/* Runs in the net_time / weather tasks. Same idea: only wake the main task,
 * which owns the panel. */
static void on_status_change(void)
{
    s_status_dirty = true;
    if (s_main_task) {
        xTaskNotifyGive(s_main_task);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "e-paper clock starting");

    /* Beijing time, no DST. Must be set before the first localtime_r(). */
    setenv("TZ", "CST-8", 1);
    tzset();

    s_main_task = xTaskGetCurrentTaskHandle();

    ESP_ERROR_CHECK(epd_init());

    if (!net_time_start()) {
        ESP_LOGE(TAG, "network stack init failed, clock will not sync");
    }

    /* NVS is ready after net_time_start(), so the sentence rotation and the
     * chosen ANSI picture can be loaded before the first frame is drawn. */
    quote_init();
    art_init();

    bool had_time = net_time_clock_valid();
    ESP_LOGI(TAG, "retained clock %s", had_time ? "available" : "not available");

    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);

    /* Pick today's sentence and picture before the first frame, so the boot
     * screen already matches the day even when the clock was retained. */
    if (had_time) {
        quote_update_day(local.tm_yday);
        art_update_day();
    }

    /* First frame: the retained clock, or "--:--" / 未获取时间 when this is the
     * first boot without a network. It is a full refresh, which also becomes
     * the base image that the partial updates compare against. */
    refresh_full();
    s_painted_valid = had_time;

    /* The configuration page is available from now on, even while the clock is
     * still waiting for its first time sync. */
    if (!web_portal_start(on_portal_change)) {
        ESP_LOGW(TAG, "configuration portal not available");
    }

    /* Join the home network in the background. Nothing below blocks on it: the
     * footer and the screen update through on_status_change(). */
    net_time_set_change_cb(on_status_change);
    weather_init(on_status_change);
    net_time_connect();

    time_t last_full = now;
    int last_yday = local.tm_yday;
    int last_min = local.tm_min;

    for (;;) {
        /* Sleep until the next minute boundary or a notification, whichever
         * comes first. If a refresh took long enough to cross a minute
         * boundary, process that minute right away instead of sleeping.
         * Without a valid time there is no display minute to wait for, so
         * always sleep rather than spin on the 1970 clock. */
        now = time(NULL);
        localtime_r(&now, &local);
        bool clock_valid = net_time_clock_valid();
        if (!clock_valid || local.tm_min == last_min) {
            int to_minute = 60 - (int)(now % 60);
            xTaskNotifyWait(0, 0xFFFFFFFF, NULL,
                            pdMS_TO_TICKS(to_minute * 1000));
            now = time(NULL);
            localtime_r(&now, &local);
            clock_valid = net_time_clock_valid();
        }

        /* Retry the first time sync while the router has no internet yet. */
        net_time_tick();

        if (s_portal_dirty) {
            s_portal_dirty = false;
            bool quote_changed = quote_revision() != s_painted_rev;
            bool art_changed = art_revision() != s_painted_art_rev;
            if (quote_changed || art_changed) {
                ESP_LOGI(TAG, "web page changed the %s",
                         quote_changed && art_changed ? "sentence and picture"
                         : quote_changed               ? "sentence"
                                                       : "picture");
                refresh_full_from_portal();
                last_full = now;
                last_yday = local.tm_yday;
                last_min = local.tm_min;
                continue;
            }
            /* Only the pool or the WiFi settings were edited: the frame is
             * unchanged, the footer follows through the status callback. */
            ESP_LOGI(TAG, "portal change did not alter the frame");
        }

        if (s_status_dirty) {
            s_status_dirty = false;
            bool valid = net_time_clock_valid();

            if (valid && !s_painted_valid) {
                /* First sync: replace the "--:--" placeholders with the real
                 * date and time. Let the NTP/HTTP traffic settle first: a
                 * full refresh that overlaps radio activity comes out grey. */
                ESP_LOGI(TAG, "first time sync done, drawing the clock");
                vTaskDelay(pdMS_TO_TICKS(PORTAL_SETTLE_MS));
                quote_update_day(local.tm_yday);
                art_update_day();
                refresh_full();
                s_painted_valid = true;
                s_synced_seen = net_time_is_synced();
                last_yday = local.tm_yday;
                last_min = local.tm_min;
                last_full = now;
                continue;
            }

            /* A sync correction can move the minute; repaint it once. */
            if (valid && net_time_is_synced() && !s_synced_seen) {
                s_synced_seen = true;
                refresh_time(local.tm_hour, local.tm_min);
                last_min = local.tm_min;
            }
            refresh_status();
            continue;
        }

        bool new_day = (local.tm_yday != last_yday);
        bool new_sentence = quote_update_day(local.tm_yday);
        bool new_art = art_update_day();
        bool need_full = new_day || new_sentence || new_art ||
                         (now - last_full >= FULL_REFRESH_S);
        if (need_full) {
            ESP_LOGI(TAG, "full refresh (%s)",
                     new_day        ? "new day"
                     : new_sentence ? "new sentence"
                     : new_art      ? "new picture"
                                    : "ghosting");
            refresh_full();
            last_yday = local.tm_yday;
            last_full = now;
            last_min = local.tm_min;
            continue;
        }

        /* Only refresh when a real time is on screen and the displayed minute
         * really changed. Without a valid time the clock is stuck at 1970 and
         * there is nothing to redraw. */
        if (net_time_clock_valid() && local.tm_min != last_min) {
            last_min = local.tm_min;
            esp_err_t err = refresh_time(local.tm_hour, local.tm_min);
            if (err != ESP_OK) {
                /* The controller got confused: reset it and repaint
                 * everything. */
                ESP_LOGW(TAG, "time refresh failed, re-initialising the panel");
                if (epd_init() == ESP_OK) {
                    refresh_full();
                    time_t fixed = time(NULL);
                    localtime_r(&fixed, &local);
                    last_yday = local.tm_yday;
                    last_min = local.tm_min;
                    last_full = fixed;
                }
            }
        }
    }
}
