#pragma once

#include <stdbool.h>

/* Small configuration page served over a SoftAP, so the sentence on the panel
 * can be replaced from a phone without reflashing.
 *
 * The AP is always on (SSID "Daily-Spark", open, http://192.168.4.1). The clock
 * also stays connected to the home network (the footer shows its state and the
 * weather uses it); the WiFi credentials can be changed from this page.
 *
 * `on_change` runs in the HTTP server task. It must not touch the panel; use it
 * to wake the main task, which redraws in its own context. */

typedef void (*web_portal_changed_cb_t)(void);

/* Must be called after net_time_start() (it relies on the WiFi driver and NVS
 * being initialised). Returns false when the AP or the HTTP server failed. */
bool web_portal_start(web_portal_changed_cb_t on_change);
