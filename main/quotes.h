#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The pool of daily sentences.
 *
 * The pool lives in NVS so the web page can create, edit and delete entries
 * without reflashing. On the very first boot it is seeded from the built-in
 * list in quotes.c. Each entry remembers the day it was last shown, and the
 * daily pick always takes the one that has been waiting longest; that gives a
 * random permutation of the whole pool without a separate "used" bitmap, and
 * it keeps working when entries are added or removed.
 *
 * What is on the panel is one of three things:
 *   - the automatic sentence of the day (the default),
 *   - a pool sentence pinned from the page ("show this one"), valid for today,
 *   - a one-off text typed on the page, valid for today.
 * Both overrides fall back to the automatic sentence at the next day change. */

/* Buffer size for one sentence, in UTF-8 bytes including the terminator.
 * The web page limits input to QUOTE_CHARS_MAX characters (60 CJK characters
 * are 180 bytes), and the panel layout fits about 80 characters in six lines,
 * so 192 bytes is enough for anything the page accepts. */
#define QUOTE_TEXT_MAX 192

/* Longest sentence accepted by the web page. Kept in sync with maxlength in
 * web_portal.c and with QUOTE_TEXT_MAX above. */
#define QUOTE_CHARS_MAX 60

/* Hard limit of entries in the pool. */
#define QUOTE_POOL_MAX 365

void quote_init(void);

/* The sentence that belongs on the panel right now. Never NULL. */
const char *quote_text(void);

/* Incremented whenever quote_text() changes. The main task compares this with
 * the revision it last painted and only refreshes the panel when they differ,
 * so editing a sentence that is not on screen does not flash the display. */
uint32_t quote_revision(void);

/* Advance the rotation to `yday` (day of year). Call at boot after the clock is
 * valid and on every day change. Returns true when the visible text changed. */
bool quote_update_day(int yday);

/* --- display mode -------------------------------------------------------- */

bool quote_is_custom(void); /* one-off text is on the panel */
bool quote_is_pinned(void); /* a pool sentence was pinned on the panel */

/* Show `text` instead of the pool until the next day change. */
void quote_set_custom(const char *text);

/* Go back to the automatic sentence of the day. */
void quote_clear_custom(void);

/* --- pool editing -------------------------------------------------------- */

int quote_pool_count(void);

/* Copy entry `index` (0..count-1) into `text`. Returns its id, or 0 when the
 * index is out of range. */
uint16_t quote_pool_get(int index, char *text, size_t size);

/* Append a sentence. Returns its id, or 0 when the pool is full or the text is
 * empty. */
uint16_t quote_pool_add(const char *text);

/* Replace the text of an entry. Returns false when the id is unknown. */
bool quote_pool_set(uint16_t id, const char *text);

/* Remove an entry. Returns false when the id is unknown. */
bool quote_pool_delete(uint16_t id);

/* Pin a pool sentence on the panel until the next day change. */
bool quote_show(uint16_t id);

/* Id of the pool sentence currently displayed, or 0 for one-off text / empty
 * pool. Used by the web page to highlight the row. */
uint16_t quote_current_id(void);
