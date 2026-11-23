#pragma once

#include <stdbool.h>
#include <stdint.h>

/* WiFi station + SNTP.
 *
 * The station link stays up while the clock is powered: the panel's status
 * line shows whether WiFi is connected and the weather needs the network.
 * The SoftAP configuration portal keeps running in the same driver (APSTA).
 *
 * Credentials live in NVS (namespace "wifi"); the Kconfig values are only the
 * fallback for a device that has never been configured. The web page can
 * replace them at runtime with net_time_set_wifi().
 *
 * Time is synced with SNTP once the link comes up, then once a day. A failed
 * first sync is retried every minute by net_time_tick(). */

/* Bring up the network stack (NVS, netif, event loop, WiFi driver). Does not
 * connect yet. Returns false only when the stack could not be brought up. */
bool net_time_start(void);

/* Start the station link in the background. Returns immediately; progress is
 * reported through the change callback and net_time_status(). */
void net_time_connect(void);

/* Retry a failed first time sync. Call once per minute from the main loop. */
void net_time_tick(void);

/* True after at least one successful NTP sync in this power cycle. */
bool net_time_is_synced(void);

/* True when the system clock looks valid (i.e. has been set at least once). */
bool net_time_clock_valid(void);

typedef struct {
    bool connected;      /* station link has an IP address */
    bool configured;     /* an SSID is known (NVS or Kconfig) */
    char ssid[33];       /* the SSID we try to join */
    char ip[16];         /* dotted quad while connected, "" otherwise */
    bool synced;         /* NTP succeeded at least once this boot */
    bool sync_failed;    /* no answer for a while: show 无法同步 */
    int sync_hour;       /* local time of the last successful sync */
    int sync_min;
    uint8_t reason;      /* last disconnect reason (0 while connected) */
} net_time_status_t;

void net_time_status(net_time_status_t *out);

/* One scanned 2.4 GHz network, strongest first. */
typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} net_time_ap_t;

/* Scan for nearby networks. Blocking (a couple of seconds) and only meant for
 * the configuration page. Returns the number of unique SSIDs, or -1 while the
 * radio is busy. */
int net_time_scan(net_time_ap_t *out, int max);

/* Save credentials (NVS) and reconnect. An empty password means an open
 * network; if the SSID is unchanged, an empty password keeps the stored one
 * so the page never has to echo the password back to the phone. */
bool net_time_set_wifi(const char *ssid, const char *pass);

/* Called from the WiFi/SNTP tasks when the status may have changed. Runs in
 * the system event task: keep it short (wake the main task). */
void net_time_set_change_cb(void (*cb)(void));
