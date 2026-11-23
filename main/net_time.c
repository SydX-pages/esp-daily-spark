/*
 * WiFi station + SNTP.
 *
 * Unlike the first version, the station link is not torn down after a sync:
 * the panel's footer shows the connection state and the weather module needs
 * the network. Time is still only fetched by SNTP (once when the link comes
 * up, then daily), so the radio is idle apart from beacons and the weather
 * request every half hour.
 *
 * Credentials are kept in NVS so the configuration page can change them
 * without a reflash. Kconfig is only the factory fallback.
 */

#include "net_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define WIFI_CONNECTED_BIT BIT0

/* Anything after 2020-09-13 means the clock has been set. */
#define TIME_VALID_THRESHOLD 1600000000L

/* Re-syncing daily is enough: the internal timer drifts by seconds per day. */
#define SYNC_INTERVAL_MS (24 * 60 * 60 * 1000)

/* Retry a first sync that never got an answer (e.g. the router had no
 * internet yet) at this cadence. */
#define SYNC_RETRY_MS (60 * 1000)

#define NVS_NS "wifi"

static const char *TAG = "net";

static EventGroupHandle_t s_events;
static esp_timer_handle_t s_retry_timer;
static esp_timer_handle_t s_fail_timer;
static bool s_stack_ready;
static bool s_wifi_on;
static bool s_sntp_started;
static volatile bool s_wifi_wanted;
static volatile bool s_connected;
static volatile bool s_synced;
static volatile int s_retries;
static TickType_t s_last_try;

/* When "no time yet" turns into "无法同步". Pushed back on every connect so a
 * slow router still gets a chance before the footer gives up. */
#define SYNC_FAIL_MS (30 * 1000)
static TickType_t s_sync_deadline;

static char s_ssid[33];
static char s_pass[65];
static char s_ip[16];
static volatile uint8_t s_last_reason;

static int s_sync_hour;
static int s_sync_min;

static void (*s_change_cb)(void);

static void notify_change(void)
{
    if (s_change_cb) {
        s_change_cb();
    }
}

/* Wake the UI when the grace period for the first sync runs out, so the
 * footer can switch from 正在同步 to 无法同步 without waiting for another
 * WiFi/NTP event. */
static void fail_timer_cb(void *arg)
{
    (void)arg;
    notify_change();
}

static void arm_fail_timer(void)
{
    if (!s_fail_timer || s_synced) {
        return;
    }
    esp_timer_stop(s_fail_timer);
    esp_timer_start_once(s_fail_timer, (uint64_t)SYNC_FAIL_MS * 1000 + 200000);
}

static void remember_sync_time(void)
{
    time_t now = time(NULL);
    struct tm local;

    localtime_r(&now, &local);
    s_sync_hour = local.tm_hour;
    s_sync_min = local.tm_min;
}

static void on_time_synced(struct timeval *tv)
{
    s_synced = true;
    if (s_fail_timer) {
        esp_timer_stop(s_fail_timer);
    }
    remember_sync_time();

    time_t now = tv->tv_sec;
    struct tm local;
    localtime_r(&now, &local);
    ESP_LOGI(TAG, "time synced: %04d-%02d-%02d %02d:%02d:%02d",
             local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
             local.tm_hour, local.tm_min, local.tm_sec);
    notify_change();
}

static void start_sntp(void)
{
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 1
    esp_sntp_setservername(1, "cn.pool.ntp.org");
#endif
    esp_sntp_set_time_sync_notification_cb(on_time_synced);
    esp_sntp_set_sync_interval(SYNC_INTERVAL_MS);
    esp_sntp_init();
    s_sntp_started = true;
    ESP_LOGI(TAG, "SNTP started");
}

static void retry_connect(void *arg)
{
    (void)arg;

    if (s_wifi_wanted && !s_connected) {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            ESP_LOGW(TAG, "reconnect failed: %s", esp_err_to_name(err));
        }
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_wifi_wanted) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
        bool was_connected = s_connected;

        xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
        s_connected = false;
        s_ip[0] = '\0';
        s_last_reason = (uint8_t)ev->reason;

        if (!s_wifi_wanted) {
            if (was_connected) {
                notify_change();
            }
            return;
        }

        s_retries++;
        if (s_retries == 1 || s_retries % 5 == 0) {
            ESP_LOGW(TAG, "disconnected (reason %d), retry %d", ev->reason,
                     s_retries);
        }
        if (was_connected) {
            notify_change();
        }
        /* Reconnect from a timer, never by sleeping here: this runs in the
         * system event task, which must stay responsive for IP events.
         * Back off after a few tries so a wrong SSID does not keep the radio
         * scanning every two seconds. */
        if (s_retry_timer) {
            int delay_ms = s_retries < 5 ? 2000
                           : s_retries < 15 ? 10000
                                            : 30000;
            esp_timer_stop(s_retry_timer);
            esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&ev->ip_info.ip));
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_retries = 0;
        s_last_reason = 0;
        s_connected = true;
        s_sync_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(SYNC_FAIL_MS);
        arm_fail_timer();
        if (s_retry_timer) {
            esp_timer_stop(s_retry_timer);
        }
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);

        if (!s_sntp_started) {
            start_sntp();
        } else if (!s_synced) {
            /* First sync still owed: ask again right away. */
            esp_sntp_restart();
            s_last_try = xTaskGetTickCount();
        }
        notify_change();
    }
}

/* ------------------------------------------------------------------------- */
/* Credentials                                                               */
/* ------------------------------------------------------------------------- */

static bool creds_load(void)
{
    nvs_handle_t h;
    size_t len = sizeof(s_ssid);

    s_ssid[0] = '\0';
    s_pass[0] = '\0';

    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "ssid", s_ssid, &len) == ESP_OK && s_ssid[0]) {
            len = sizeof(s_pass);
            if (nvs_get_str(h, "pass", s_pass, &len) != ESP_OK) {
                s_pass[0] = '\0';
            }
            nvs_close(h);
            return true;
        }
        nvs_close(h);
    }

    /* Factory fallback. */
    strlcpy(s_ssid, CONFIG_GIFT_WIFI_SSID, sizeof(s_ssid));
    strlcpy(s_pass, CONFIG_GIFT_WIFI_PASSWORD, sizeof(s_pass));
    return s_ssid[0] != '\0';
}

static bool creds_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);

    if (err != ESP_OK) {
        return false;
    }
    err = nvs_set_str(h, "ssid", s_ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "pass", s_pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}

static void apply_sta_config(void)
{
    wifi_config_t cfg = { 0 };

    strlcpy((char *)cfg.sta.ssid, s_ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, s_pass, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode =
        s_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sta config: %s", esp_err_to_name(err));
    }
}

/* ------------------------------------------------------------------------- */
/* Startup / connection                                                      */
/* ------------------------------------------------------------------------- */

bool net_time_start(void)
{
    /* Beijing time, no DST. */
    setenv("TZ", "CST-8", 1);
    tzset();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(err));
        return false;
    }

    s_events = xEventGroupCreate();

    const esp_timer_create_args_t retry_args = {
        .callback = retry_connect,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&retry_args, &s_retry_timer));

    const esp_timer_create_args_t fail_args = {
        .callback = fail_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "sync_fail",
    };
    ESP_ERROR_CHECK(esp_timer_create(&fail_args, &s_fail_timer));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    creds_load();
    apply_sta_config();

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    /* Until the first connect call, the status screen must say 正在同步 rather
     * than 无法同步. */
    s_sync_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(SYNC_FAIL_MS);

    s_stack_ready = true;
    ESP_LOGI(TAG, "wifi stack ready, ssid \"%s\"", s_ssid);
    return true;
}

void net_time_connect(void)
{
    if (!s_stack_ready) {
        return;
    }

    s_wifi_wanted = true;
    s_sync_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(SYNC_FAIL_MS);
    arm_fail_timer();

    if (!s_wifi_on) {
        esp_err_t err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
            ESP_LOGW(TAG, "wifi start failed: %s", esp_err_to_name(err));
            s_wifi_wanted = false;
            return;
        }
        s_wifi_on = true;

        /* 8.5 dBm keeps the current spikes and RF noise low without making
         * the cold-start scan miss the router. */
        esp_err_t tx_err = esp_wifi_set_max_tx_power(34);
        ESP_LOGI(TAG, "max tx power set: %s", esp_err_to_name(tx_err));
    } else {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            ESP_LOGW(TAG, "sta connect failed: %s", esp_err_to_name(err));
        }
    }
}

void net_time_tick(void)
{
    if (!s_connected || s_synced || !s_sntp_started) {
        return;
    }

    TickType_t now = xTaskGetTickCount();
    if ((now - s_last_try) < pdMS_TO_TICKS(SYNC_RETRY_MS)) {
        return;
    }
    s_last_try = now;
    ESP_LOGW(TAG, "no time yet, retrying SNTP");
    esp_sntp_restart();
}

bool net_time_is_synced(void)
{
    return s_synced;
}

bool net_time_clock_valid(void)
{
    return time(NULL) > TIME_VALID_THRESHOLD;
}

void net_time_status(net_time_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->connected = s_connected;
    out->configured = s_ssid[0] != '\0';
    strlcpy(out->ssid, s_ssid, sizeof(out->ssid));
    strlcpy(out->ip, s_connected ? s_ip : "", sizeof(out->ip));
    out->synced = s_synced;
    out->sync_hour = s_sync_hour;
    out->sync_min = s_sync_min;
    out->reason = s_last_reason;
    out->sync_failed = !s_synced &&
                       (int32_t)(xTaskGetTickCount() - s_sync_deadline) >= 0;
}

bool net_time_set_wifi(const char *ssid, const char *pass)
{
    if (!s_stack_ready || !ssid || !ssid[0]) {
        return false;
    }
    if (strlen(ssid) > 32 || strlen(pass ? pass : "") > 64) {
        return false;
    }

    bool same_ssid = strcmp(ssid, s_ssid) == 0;

    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    if (pass && pass[0]) {
        strlcpy(s_pass, pass, sizeof(s_pass));
    } else if (!same_ssid) {
        /* New network with an empty password field: treat it as open. */
        s_pass[0] = '\0';
    }
    /* Same SSID + empty password: keep the stored password. */

    if (!creds_save()) {
        ESP_LOGW(TAG, "could not save WiFi credentials");
    }

    ESP_LOGI(TAG, "WiFi changed to \"%s\"%s", s_ssid,
             s_pass[0] ? "" : " (open)");

    /* Fresh attempt: the page shows "正在连接" until the first real failure. */
    s_last_reason = 0;
    s_retries = 0;
    s_sync_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(SYNC_FAIL_MS);
    s_ip[0] = '\0';
    arm_fail_timer();

    if (!s_wifi_on) {
        net_time_connect();
        return true;
    }

    /* Drop the old link, then join the new network. s_wifi_wanted is cleared
     * while disconnecting so the event handler does not reconnect to the old
     * configuration in between. */
    s_wifi_wanted = false;
    if (s_retry_timer) {
        esp_timer_stop(s_retry_timer);
    }
    esp_wifi_disconnect();
    apply_sta_config();
    s_wifi_wanted = true;
    esp_wifi_connect();
    return true;
}

void net_time_set_change_cb(void (*cb)(void))
{
    s_change_cb = cb;
}

int net_time_scan(net_time_ap_t *out, int max)
{
    static wifi_ap_record_t recs[24];
    bool wanted;
    bool had_link;
    esp_err_t err;

    if (!s_stack_ready || !out || max <= 0) {
        return -1;
    }

    /* A scan cannot run while a connect attempt is in flight, which is exactly
     * the state an offline clock is in (it keeps retrying the saved SSID).
     * Pause the retry loop, cancel the attempt, scan, then resume. */
    wanted = s_wifi_wanted;
    had_link = s_connected;
    s_wifi_wanted = false;
    if (s_retry_timer) {
        esp_timer_stop(s_retry_timer);
    }
    if (!had_link) {
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    wifi_scan_config_t cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    /* Short dwell: the SoftAP shares the radio, so the phone's page must not
     * wait too long while the station hops across channels. */
    cfg.scan_time.active.min = 50;
    cfg.scan_time.active.max = 120;

    err = esp_wifi_scan_start(&cfg, true);
    for (int attempt = 0; err != ESP_OK && attempt < 2; attempt++) {
        /* A connect attempt cancelled a moment ago may still hold the STA
         * state machine; retry briefly instead of reporting an empty list. */
        vTaskDelay(pdMS_TO_TICKS(250));
        err = esp_wifi_scan_start(&cfg, true);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
    }

    uint16_t total = 0;
    uint16_t got = 0;
    int count = 0;

    if (err == ESP_OK) {
        esp_wifi_scan_get_ap_num(&total);
        got = total > 24 ? 24 : total;
        if (got > 0 && esp_wifi_scan_get_ap_records(&got, recs) != ESP_OK) {
            got = 0;
        }
        if (got < total) {
            esp_wifi_clear_ap_list();
        }
    }

    for (int i = 0; i < got && count < max; i++) {
        const char *ssid = (const char *)recs[i].ssid;

        if (ssid[0] == '\0') {
            continue; /* hidden network */
        }

        int j = 0;
        for (; j < count; j++) {
            if (strcmp(out[j].ssid, ssid) == 0) {
                if (recs[i].rssi > out[j].rssi) {
                    out[j].rssi = recs[i].rssi;
                }
                break;
            }
        }
        if (j < count) {
            continue; /* already listed (mesh/repeater) */
        }

        strlcpy(out[count].ssid, ssid, sizeof(out[count].ssid));
        out[count].rssi = recs[i].rssi;
        out[count].secure = recs[i].authmode != WIFI_AUTH_OPEN;
        count++;
    }

    /* Strongest first. */
    for (int i = 1; i < count; i++) {
        net_time_ap_t tmp = out[i];
        int j = i - 1;

        while (j >= 0 && out[j].rssi < tmp.rssi) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = tmp;
    }

    /* Resume the link exactly where the main loop left it. */
    s_wifi_wanted = wanted;
    if (wanted && !s_connected && s_retry_timer) {
        esp_timer_start_once(s_retry_timer, 1500 * 1000);
    }
    return err == ESP_OK ? count : -1;
}
