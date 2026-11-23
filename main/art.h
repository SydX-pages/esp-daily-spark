#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Right-top ANSI thumbnail selection.
 *
 * The default mode is RANDOM: a new picture is picked once per day, when the
 * date changes. From the web page the user can also pin one picture; that
 * choice is stored in NVS and survives a reboot until "每日随机" is chosen
 * again. */

void art_init(void);

/* Index currently on the panel (0..art_count()-1). */
int art_index(void);

/* Number of embedded pictures and its display name. */
int art_count(void);
const char *art_name(int index);

/* True while the daily random rotation is active. */
bool art_is_random(void);

/* Bumped whenever the visible picture changes. */
uint32_t art_revision(void);

/* Called every tick: picks the random picture of a new day. Returns true when
 * the picture changed (a full refresh is then needed). Does nothing while a
 * picture is pinned. */
bool art_update_day(void);

/* Web page actions. Return true when the picture on the panel should change. */
bool art_set_random(void);
bool art_set_fixed(int index);
