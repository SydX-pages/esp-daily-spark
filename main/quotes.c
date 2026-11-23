/* Editable pool of daily sentences, persisted in NVS.
 *
 * Storage is one packed blob:
 *
 *   magic u16 | version u16 | count u16 | next_id u16
 *   per entry: id u16 | last_shown u32 | len u16 | text[len]
 *
 * Packing keeps the blob small (only the text that exists is stored), which
 * matters because NVS rewrites the whole value on every edit. */

#include "quotes.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

static const char *TAG = "quotes";

/* Seeded into NVS the first time the clock boots (or after the pool is
 * removed entirely from a very old firmware). */
static const char *const DEFAULT_POOL[] = {
    "今天也要好好吃饭呀。",
    "慢一点也没关系，方向对就好。",
    "你比昨天更靠近想去的地方。",
    "累了就休息，别硬撑。",
    "做难而正确的事。",
    "你已经做得很好了。",
    "一点一点来，也很快。",
    "记得抬头看看天空。",
    "保持好奇，保持温柔。",
    "认真生活的人最幸运。",
    "所有的努力都算数。",
    "别急，花会慢慢开。",
    "今天的你值得被喜欢。",
    "世界很大，慢慢逛。",
    "事缓则圆，人缓则安。",
    "允许自己偶尔停下来。",
    "你正在成为更好的人。",
    "每一小步都值得庆祝。",
    "生活是自己的，尽兴就好。",
    "心情不好就去晒晒太阳。",
    "用力生活，轻轻放下。",
    "把今天过成喜欢的样子。",
    "慢慢来，比较快。",
    "你只需要比昨天好一点。",
    "保持热爱，奔赴山海。",
    "雨天也有雨天的好。",
    "别怕，你比自己想的更强。",
    "好好睡觉，明天继续。",
    "万事胜意，来日可期。",
    "我一直在你身边。",
};

#define DEFAULT_COUNT ((int)(sizeof(DEFAULT_POOL) / sizeof(DEFAULT_POOL[0])))

#define NVS_NS        "clock"
#define POOL_MAGIC    0x5041u /* "AP" */
#define POOL_VERSION  1u

/* Bumped when the meaning of the stored state changes. Version 1 was the
 * index/bitmap rotation of the first firmware; version 2 is the id/date model
 * below. Old keys are ignored, the cycle simply starts over once. */
#define STATE_VERSION 2u

#define NO_ID         0u
#define NO_DAY        0xFFFFu

enum { MODE_AUTO = 0, MODE_PIN = 1, MODE_CUSTOM = 2 };

typedef struct {
    uint16_t id;
    uint32_t last; /* days since epoch of the last display, 0 = never shown */
    char text[QUOTE_TEXT_MAX];
} item_t;

static item_t s_pool[QUOTE_POOL_MAX];
static int s_count;
static uint16_t s_next_id = 1;

static uint16_t s_auto_id;
static uint16_t s_pin_id;
static uint16_t s_day = NO_DAY;
static uint16_t s_pin_day = NO_DAY;
static uint16_t s_custom_day = NO_DAY;
static uint8_t s_mode = MODE_AUTO;
static char s_custom[QUOTE_TEXT_MAX];

/* Copy of what quote_text() returned last time, used to bump the revision only
 * on a real change. */
static char s_view[QUOTE_TEXT_MAX];
static uint32_t s_rev;
static bool s_ready;

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static uint16_t get16(const uint8_t **p)
{
    uint16_t v = (uint16_t)((*p)[0] | ((uint16_t)(*p)[1] << 8));
    *p += 2;
    return v;
}

static uint32_t get32(const uint8_t **p)
{
    uint32_t v = (uint32_t)(*p)[0] | ((uint32_t)(*p)[1] << 8) |
                 ((uint32_t)(*p)[2] << 16) | ((uint32_t)(*p)[3] << 24);
    *p += 4;
    return v;
}

static void put16(uint8_t **p, uint16_t v)
{
    (*p)[0] = (uint8_t)(v & 0xFF);
    (*p)[1] = (uint8_t)(v >> 8);
    *p += 2;
}

static void put32(uint8_t **p, uint32_t v)
{
    (*p)[0] = (uint8_t)(v & 0xFF);
    (*p)[1] = (uint8_t)((v >> 8) & 0xFF);
    (*p)[2] = (uint8_t)((v >> 16) & 0xFF);
    (*p)[3] = (uint8_t)((v >> 24) & 0xFF);
    *p += 4;
}

/* Monotonic day counter. 0 while the clock is not set yet. */
static uint32_t day_number(void)
{
    time_t now = time(NULL);
    return now > 1600000000L ? (uint32_t)(now / 86400) : 0;
}

static uint16_t local_yday(void)
{
    time_t now = time(NULL);
    struct tm local;

    if (now < 1600000000L) {
        return NO_DAY;
    }
    localtime_r(&now, &local);
    return (uint16_t)local.tm_yday;
}

static int find_item(uint16_t id)
{
    if (id == NO_ID) {
        return -1;
    }
    for (int i = 0; i < s_count; i++) {
        if (s_pool[i].id == id) {
            return i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------------- */
/* What is on the panel                                                      */
/* ------------------------------------------------------------------------- */

const char *quote_text(void)
{
    int i = -1;

    if (s_mode == MODE_CUSTOM && s_custom[0] != '\0') {
        return s_custom;
    }
    if (s_mode == MODE_PIN) {
        i = find_item(s_pin_id);
    }
    if (i < 0) {
        i = find_item(s_auto_id);
    }
    if (i < 0 && s_count > 0) {
        i = 0; /* fallback: nothing valid is remembered */
    }
    if (i < 0) {
        return "在网页里添加一句吧。";
    }
    return s_pool[i].text;
}

/* Bump the revision only when the visible text really changed. */
static void view_refresh(void)
{
    const char *text = quote_text();

    if (strcmp(text, s_view) != 0) {
        strlcpy(s_view, text, sizeof(s_view));
        s_rev++;
    }
}

uint32_t quote_revision(void)
{
    return s_rev;
}

bool quote_is_custom(void)
{
    return s_mode == MODE_CUSTOM && s_custom[0] != '\0';
}

bool quote_is_pinned(void)
{
    return s_mode == MODE_PIN && find_item(s_pin_id) >= 0;
}

uint16_t quote_current_id(void)
{
    if (quote_is_pinned()) {
        return s_pin_id;
    }
    if (s_mode == MODE_AUTO && find_item(s_auto_id) >= 0) {
        return s_auto_id;
    }
    return NO_ID;
}

/* ------------------------------------------------------------------------- */
/* Persistence                                                               */
/* ------------------------------------------------------------------------- */

static bool pool_parse(const uint8_t *buf, size_t len)
{
    const uint8_t *p = buf;
    const uint8_t *end = buf + len;

    if (len < 8) {
        return false;
    }
    if (get16(&p) != POOL_MAGIC || get16(&p) > POOL_VERSION) {
        return false;
    }

    int count = get16(&p);
    uint16_t next = get16(&p);
    if (count < 0 || count > QUOTE_POOL_MAX) {
        return false;
    }

    for (int i = 0; i < count; i++) {
        if ((size_t)(end - p) < 8) {
            return false;
        }
        uint16_t id = get16(&p);
        uint32_t last = get32(&p);
        int n = get16(&p);
        if (n <= 0 || n >= QUOTE_TEXT_MAX || (size_t)(end - p) < (size_t)n) {
            return false;
        }
        memcpy(s_pool[i].text, p, (size_t)n);
        s_pool[i].text[n] = '\0';
        s_pool[i].id = id != NO_ID ? id : (uint16_t)(i + 1);
        s_pool[i].last = last;
        p += n;
    }

    s_count = count;
    s_next_id = next != NO_ID ? next : 1;
    for (int i = 0; i < s_count; i++) {
        if (s_pool[i].id >= s_next_id) {
            s_next_id = (uint16_t)(s_pool[i].id + 1);
        }
    }
    if (s_next_id == NO_ID) {
        s_next_id = 1;
    }
    return true;
}

static bool pool_load(nvs_handle_t h)
{
    size_t len = 0;

    if (nvs_get_blob(h, "q_pool", NULL, &len) != ESP_OK || len < 8) {
        return false;
    }

    uint8_t *buf = malloc(len);
    if (!buf) {
        return false;
    }
    bool ok = nvs_get_blob(h, "q_pool", buf, &len) == ESP_OK &&
              pool_parse(buf, len);
    free(buf);
    return ok;
}

static esp_err_t state_save(void)
{
    size_t len = 8;

    for (int i = 0; i < s_count; i++) {
        len += 8 + strlen(s_pool[i].text);
    }

    uint8_t *buf = malloc(len);
    if (!buf) {
        ESP_LOGE(TAG, "no memory for the pool (%u bytes)", (unsigned)len);
        return ESP_ERR_NO_MEM;
    }

    uint8_t *p = buf;
    put16(&p, POOL_MAGIC);
    put16(&p, POOL_VERSION);
    put16(&p, (uint16_t)s_count);
    put16(&p, s_next_id);
    for (int i = 0; i < s_count; i++) {
        size_t n = strlen(s_pool[i].text);
        put16(&p, s_pool[i].id);
        put32(&p, s_pool[i].last);
        put16(&p, (uint16_t)n);
        memcpy(p, s_pool[i].text, n);
        p += n;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "q_pool", buf, len);
        if (err == ESP_OK) {
            err = nvs_set_u16(h, "q_auto", s_auto_id);
        }
        if (err == ESP_OK) {
            err = nvs_set_u16(h, "q_pin", s_pin_id);
        }
        if (err == ESP_OK) {
            err = nvs_set_u16(h, "q_day", s_day);
        }
        if (err == ESP_OK) {
            err = nvs_set_u16(h, "q_pday", s_pin_day);
        }
        if (err == ESP_OK) {
            err = nvs_set_u16(h, "q_cday", s_custom_day);
        }
        if (err == ESP_OK) {
            err = nvs_set_u8(h, "q_mode", s_mode);
        }
        if (err == ESP_OK) {
            err = nvs_set_str(h, "q_text", s_custom);
        }
        if (err == ESP_OK) {
            err = nvs_set_u8(h, "q_ver", STATE_VERSION);
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }

    free(buf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cannot save the pool: %s", esp_err_to_name(err));
    }
    return err;
}

static void seed_defaults(void)
{
    int count = DEFAULT_COUNT < QUOTE_POOL_MAX ? DEFAULT_COUNT : QUOTE_POOL_MAX;

    for (int i = 0; i < count; i++) {
        s_pool[i].id = (uint16_t)(i + 1);
        s_pool[i].last = 0;
        strlcpy(s_pool[i].text, DEFAULT_POOL[i], sizeof(s_pool[i].text));
    }
    s_count = count;
    s_next_id = (uint16_t)(count + 1);
}

void quote_init(void)
{
    nvs_handle_t h;
    bool dirty = false;

    memset(s_pool, 0, sizeof(s_pool));
    s_count = 0;
    s_auto_id = NO_ID;
    s_pin_id = NO_ID;
    s_mode = MODE_AUTO;
    s_custom[0] = '\0';
    s_view[0] = '\0';

    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (!pool_load(h)) {
            ESP_LOGI(TAG, "no stored pool, seeding %d sentences", DEFAULT_COUNT);
            seed_defaults();
            dirty = true;
        }

        if (nvs_get_u16(h, "q_auto", &s_auto_id) != ESP_OK) {
            s_auto_id = NO_ID;
        }
        if (nvs_get_u16(h, "q_pin", &s_pin_id) != ESP_OK) {
            s_pin_id = NO_ID;
        }
        if (nvs_get_u16(h, "q_day", &s_day) != ESP_OK) {
            s_day = NO_DAY;
        }
        if (nvs_get_u16(h, "q_pday", &s_pin_day) != ESP_OK) {
            s_pin_day = NO_DAY;
        }
        if (nvs_get_u16(h, "q_cday", &s_custom_day) != ESP_OK) {
            s_custom_day = NO_DAY;
        }
        if (nvs_get_u8(h, "q_mode", &s_mode) != ESP_OK || s_mode > MODE_CUSTOM) {
            s_mode = MODE_AUTO;
        }

        uint8_t version = 0;
        if (nvs_get_u8(h, "q_ver", &version) != ESP_OK ||
            version < STATE_VERSION) {
            /* The old firmware stored a pool index where this one stores a
             * sentence id, so the saved day cannot be trusted. Start clean. */
            ESP_LOGI(TAG, "sentence state upgraded, starting a new cycle");
            s_day = NO_DAY;
            s_auto_id = NO_ID;
            s_pin_id = NO_ID;
            s_mode = MODE_AUTO;
            s_pin_day = NO_DAY;
            s_custom_day = NO_DAY;
            dirty = true;
        }
        size_t len = sizeof(s_custom);
        if (nvs_get_str(h, "q_text", s_custom, &len) != ESP_OK) {
            s_custom[0] = '\0';
        }
        nvs_close(h);
    } else {
        seed_defaults();
    }

    /* An id that no longer exists (deleted before the reboot) must not leave
     * the panel blank. */
    if (find_item(s_auto_id) < 0) {
        s_auto_id = s_count > 0 ? s_pool[0].id : NO_ID;
        dirty = true;
    }
    if (s_mode == MODE_PIN && find_item(s_pin_id) < 0) {
        s_mode = MODE_AUTO;
        dirty = true;
    }
    if (s_mode == MODE_CUSTOM && s_custom[0] == '\0') {
        s_mode = MODE_AUTO;
        dirty = true;
    }

    s_ready = true;
    view_refresh();
    if (dirty) {
        state_save();
    }

    ESP_LOGI(TAG, "pool ready: %d sentences, showing id %u, mode %u",
             s_count, (unsigned)s_auto_id, (unsigned)s_mode);
}

/* ------------------------------------------------------------------------- */
/* Rotation                                                                  */
/* ------------------------------------------------------------------------- */

/* The entry that has been waiting longest. Never-shown entries (last == 0) win
 * first; ties are broken randomly, which makes the initial order random. Once
 * every entry was shown, the oldest one starts the next round. */
static int pick_auto(void)
{
    uint32_t best = UINT32_MAX;
    int ties = 0;

    if (s_count == 0) {
        return -1;
    }

    for (int i = 0; i < s_count; i++) {
        uint32_t value = s_pool[i].last;
        if (value < best) {
            best = value;
            ties = 1;
        } else if (value == best) {
            ties++;
        }
    }

    int pick = (int)(esp_random() % (uint32_t)ties);
    for (int i = 0; i < s_count; i++) {
        if (s_pool[i].last != best) {
            continue;
        }
        if (pick-- == 0) {
            return i;
        }
    }
    return 0;
}

bool quote_update_day(int yday)
{
    if (!s_ready) {
        return false;
    }

    uint16_t day = (uint16_t)(yday & 0x7FFF);
    bool dirty = false;
    bool changed = false;

    /* Both overrides only last for the day they were set on. */
    if (s_mode == MODE_CUSTOM && s_custom_day != day) {
        ESP_LOGI(TAG, "one-off text expired");
        s_mode = MODE_AUTO;
        s_custom_day = NO_DAY;
        dirty = true;
        changed = true;
    }
    if (s_mode == MODE_PIN && s_pin_day != day) {
        ESP_LOGI(TAG, "pinned sentence expired");
        s_mode = MODE_AUTO;
        s_pin_day = NO_DAY;
        dirty = true;
        changed = true;
    }

    if (s_day != day || find_item(s_auto_id) < 0) {
        int i = pick_auto();
        s_auto_id = i >= 0 ? s_pool[i].id : NO_ID;
        if (i >= 0) {
            s_pool[i].last = day_number();
        }
        s_day = day;
        dirty = true;
        changed = true;
        ESP_LOGI(TAG, "daily sentence: id %u of %d", (unsigned)s_auto_id,
                 s_count);
    }

    if (dirty) {
        state_save();
    }
    if (changed) {
        view_refresh();
    }
    return changed;
}

/* ------------------------------------------------------------------------- */
/* Display overrides                                                         */
/* ------------------------------------------------------------------------- */

void quote_set_custom(const char *text)
{
    if (!s_ready || !text) {
        return;
    }
    strlcpy(s_custom, text, sizeof(s_custom));
    s_mode = MODE_CUSTOM;
    s_custom_day = local_yday();
    if (s_custom_day == NO_DAY) {
        s_custom_day = s_day; /* before the first sync: expiring is enough */
    }
    state_save();
    view_refresh();
    ESP_LOGI(TAG, "one-off text stored (%u bytes)",
             (unsigned)strlen(s_custom));
}

void quote_clear_custom(void)
{
    if (!s_ready) {
        return;
    }
    s_mode = MODE_AUTO;
    s_custom_day = NO_DAY;
    state_save();
    view_refresh();
    ESP_LOGI(TAG, "back to the automatic sentence");
}

bool quote_show(uint16_t id)
{
    int i = find_item(id);

    if (!s_ready || i < 0) {
        return false;
    }

    uint8_t old_mode = s_mode;
    uint16_t old_pin = s_pin_id;
    uint16_t old_pin_day = s_pin_day;
    uint32_t old_last = s_pool[i].last;

    s_mode = MODE_PIN;
    s_pin_id = id;
    s_pin_day = local_yday();
    if (s_pin_day == NO_DAY) {
        s_pin_day = s_day;
    }
    /* It was really shown today, so it goes to the back of the queue. */
    s_pool[i].last = day_number();

    if (state_save() != ESP_OK) {
        s_mode = old_mode;
        s_pin_id = old_pin;
        s_pin_day = old_pin_day;
        s_pool[i].last = old_last;
        return false;
    }

    view_refresh();
    ESP_LOGI(TAG, "pinned sentence id %u", (unsigned)id);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Pool editing                                                              */
/* ------------------------------------------------------------------------- */

int quote_pool_count(void)
{
    return s_count;
}

uint16_t quote_pool_get(int index, char *text, size_t size)
{
    if (index < 0 || index >= s_count || !text || size == 0) {
        return NO_ID;
    }
    strlcpy(text, s_pool[index].text, size);
    return s_pool[index].id;
}

uint16_t quote_pool_add(const char *text)
{
    if (!s_ready || !text || text[0] == '\0' || s_count >= QUOTE_POOL_MAX) {
        return NO_ID;
    }

    uint16_t old_auto = s_auto_id;
    int index = s_count;
    item_t *item = &s_pool[s_count++];
    item->id = s_next_id++;
    item->last = 0; /* never shown: it is picked soon */
    strlcpy(item->text, text, sizeof(item->text));

    if (s_auto_id == NO_ID) {
        s_auto_id = item->id; /* the pool was empty, show something */
    }

    if (state_save() != ESP_OK) {
        /* Do not let RAM and NVS disagree: the next boot would lose it. */
        s_count = index;
        s_next_id = item->id;
        s_auto_id = old_auto;
        memset(item, 0, sizeof(*item));
        return NO_ID;
    }

    view_refresh();
    ESP_LOGI(TAG, "added sentence id %u (%d in the pool)", (unsigned)item->id,
             s_count);
    return item->id;
}

bool quote_pool_set(uint16_t id, const char *text)
{
    int i = find_item(id);

    char old[QUOTE_TEXT_MAX];

    if (!s_ready || i < 0 || !text || text[0] == '\0') {
        return false;
    }
    strlcpy(old, s_pool[i].text, sizeof(old));
    strlcpy(s_pool[i].text, text, sizeof(s_pool[i].text));

    if (state_save() != ESP_OK) {
        strlcpy(s_pool[i].text, old, sizeof(s_pool[i].text));
        return false;
    }

    view_refresh();
    return true;
}

bool quote_pool_delete(uint16_t id)
{
    int i = find_item(id);

    if (!s_ready || i < 0) {
        return false;
    }

    /* Keep enough state to undo the change when the flash write fails. */
    item_t removed = s_pool[i];
    uint16_t old_auto = s_auto_id;
    uint8_t old_mode = s_mode;
    uint16_t old_pin = s_pin_id;
    uint16_t old_pin_day = s_pin_day;

    for (int k = i; k + 1 < s_count; k++) {
        s_pool[k] = s_pool[k + 1];
    }
    s_count--;

    if (s_mode == MODE_PIN && s_pin_id == id) {
        s_mode = MODE_AUTO;
        s_pin_day = NO_DAY;
    }
    if (s_auto_id == id) {
        int next = pick_auto();
        s_auto_id = next >= 0 ? s_pool[next].id : NO_ID;
    }

    if (state_save() != ESP_OK) {
        for (int k = s_count; k > i; k--) {
            s_pool[k] = s_pool[k - 1];
        }
        s_pool[i] = removed;
        s_count++;
        s_auto_id = old_auto;
        s_mode = old_mode;
        s_pin_id = old_pin;
        s_pin_day = old_pin_day;
        return false;
    }

    view_refresh();
    ESP_LOGI(TAG, "deleted sentence id %u (%d left)", (unsigned)id, s_count);
    return true;
}
