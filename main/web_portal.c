#include "web_portal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "art.h"
#include "net_time.h"
#include "quotes.h"

static const char *TAG = "portal";

#define AP_SSID     "Daily-Spark"
#define HTTP_BODY   1024

/* Stringify QUOTE_CHARS_MAX so the HTML maxlength can never drift away from
 * the firmware-side limit. */
#define STR_(x) #x
#define STR(x)  STR_(x)

static web_portal_changed_cb_t s_on_change;

/* ------------------------------------------------------------------------- */
/* Page                                                                      */
/* ------------------------------------------------------------------------- */

static const char PAGE_HEAD[] =
    "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>日励 · Daily Spark</title><style>"
    "body{font-family:system-ui,-apple-system,\"PingFang SC\",sans-serif;"
    "background:#f4f4f0;color:#222;margin:0;padding:16px}"
    ".card{max-width:560px;margin:0 auto;background:#fff;border-radius:14px;"
    "padding:18px;box-shadow:0 2px 14px rgba(0,0,0,.07)}"
    "h1{font-size:20px;margin:0 0 10px}"
    "h2{font-size:14px;color:#666;margin:24px 0 8px;font-weight:600}"
    ".now{background:#161616;color:#fff;border-radius:12px;padding:13px 15px;"
    "font-size:17px;line-height:1.55}"
    ".now .tag{display:block;font-size:12px;opacity:.6;margin-bottom:4px;"
    "letter-spacing:.04em}"
    "textarea,input[type=text],input[type=password]{width:100%;font-size:16px;"
    "padding:10px 12px;border:1px solid #ddd;border-radius:10px;"
    "box-sizing:border-box;font-family:inherit;background:#fff}"
    "textarea{min-height:78px;line-height:1.5;margin-top:10px}"
    "button{font-size:15px;padding:9px 15px;border-radius:9px;border:0;"
    "background:#1f1f1f;color:#fff;margin-top:8px}"
    "button.ghost{background:#ececec;color:#333}"
    "button.small{font-size:13px;padding:7px 10px;margin:0 0 0 6px}"
    "form.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap;"
    "margin:0 0 8px;padding:8px;border-radius:12px;background:#fafaf7}"
    "form.row input[type=text]{flex:1 1 160px;font-size:15px;padding:8px 10px}"
    "form.row .ops{display:flex;gap:6px;margin-left:auto}"
    "form.row.cur{background:#eeeee6;box-shadow:inset 0 0 0 2px #1f1f1f}"
    "form.add{margin-bottom:12px}"
    ".msg{background:#e9f3e9;color:#20551f;padding:10px 12px;border-radius:10px;"
    "font-size:14px;margin:12px 0 0}"
    ".msg.warn{background:#fdf0e6;color:#8a4b12}"
    ".wifi-state{font-size:14px;line-height:1.55;background:#f6f6f1;"
    "border-radius:10px;padding:10px 12px;margin:10px 0 0;color:#333}"
    ".wifi-state.ok{background:#e9f3e9;color:#20551f;font-weight:600}"
    ".wifi-state.wait{background:#fdf0e6;color:#8a4b12}"
    "p.hint{color:#777;font-size:13px;margin:10px 0 0;line-height:1.6}"
    ".arts{display:flex;gap:8px;flex-wrap:wrap;margin:10px 0 0}"
    ".arts button{margin:0}"
    "button.cur{background:#fff;color:#111;box-shadow:inset 0 0 0 2px #1f1f1f}"
    "</style></head><body><div class=\"card\">";

static const char PAGE_TAIL[] =
    "</div>"
    "<script>"
    "var st=document.getElementById('wifi-state');"
    "function paintWifi(j){if(!st)return;"
    "if(j.connected&&j.ssid){st.textContent=j.ssid;st.className='wifi-state ok';}"
    "else if(j.configured&&j.reason==0){st.textContent='连接中';"
    "st.className='wifi-state wait';}"
    "else{st.textContent='未连接';st.className='wifi-state';}}"
    "function refreshWifi(){fetch('/status',{cache:'no-store'})"
    ".then(function(r){return r.json();}).then(paintWifi)"
    ".catch(function(){if(st){st.textContent='未连接';"
    "st.className='wifi-state';}});}"
    "setInterval(refreshWifi,2000);refreshWifi();"
    "function scanWifi(){var box=document.getElementById('scan-list');"
    "box.textContent='正在扫描…';fetch('/scan',{cache:'no-store'})"
    ".then(function(r){return r.json();}).then(function(list){"
    "box.textContent='';"
    "if(!list.length){box.textContent='没有扫到网络，稍后再试';return;}"
    "list.forEach(function(ap){var b=document.createElement('button');"
    "b.type='button';b.className='ghost';"
    "b.textContent=ap.s+(ap.o?' (开放)':'')+' '+ap.r+'dBm';"
    "b.onclick=function(){document.getElementsByName('ssid')[0].value=ap.s;};"
    "box.appendChild(b);});"
    "}).catch(function(){box.textContent='扫描失败，稍后再试';});}"
    "</script></body></html>";

static const char ROW_FORM[] =
    "<form class=\"row%s\" method=\"post\" action=\"/pool\">"
    "<input type=\"hidden\" name=\"id\" value=\"%u\">"
    "<input type=\"text\" name=\"text\" maxlength=\"" STR(QUOTE_CHARS_MAX)
    "\" value=\"";

static const char ROW_END[] =
    "\"><span class=\"ops\">"
    "<button class=\"small\" name=\"op\" value=\"save\">保存</button>"
    "<button class=\"small\" name=\"op\" value=\"show\">显示</button>"
    "<button class=\"small\" name=\"op\" value=\"del\" "
    "onclick=\"return confirm('删除这一句？')\">删除</button>"
    "</span></form>";

static esp_err_t chunk(httpd_req_t *req, const char *text)
{
    return httpd_resp_send_chunk(req, text, text ? strlen(text) : 0);
}

static void html_escape(const char *in, char *out, size_t out_size)
{
    size_t o = 0;

    for (const char *p = in; *p && o + 8 < out_size; p++) {
        switch (*p) {
        case '<':
            memcpy(out + o, "&lt;", 4);
            o += 4;
            break;
        case '>':
            memcpy(out + o, "&gt;", 4);
            o += 4;
            break;
        case '&':
            memcpy(out + o, "&amp;", 5);
            o += 5;
            break;
        case '"':
            memcpy(out + o, "&quot;", 6);
            o += 6;
            break;
        default:
            out[o++] = *p;
            break;
        }
    }
    out[o] = '\0';
}

/* Minimal JSON string escaping for SSIDs. */
static void json_escape(const char *in, char *out, size_t out_size)
{
    size_t o = 0;

    for (const char *p = in; *p && o + 3 < out_size; p++) {
        unsigned char c = (unsigned char)*p;

        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            out[o++] = ' ';
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

/* Feedback for the redirect after a POST: /?m=<code>. */
static const char *notice_for(const char *uri)
{
    const char *q = strstr(uri, "?m=");

    if (!q || q[3] == '\0') {
        return NULL;
    }
    switch (q[3]) {
    case 'a':
        return "<p class=\"msg\">已添加到句子库。</p>";
    case 's':
        return "<p class=\"msg\">已保存。</p>";
    case 'd':
        return "<p class=\"msg\">已删除。</p>";
    case 'v':
        return "<p class=\"msg\">已显示在屏幕上。</p>";
    case 'c':
        return "<p class=\"msg\">已临时显示。</p>";
    case 'r':
        return "<p class=\"msg\">已恢复每日一句。</p>";
    case 'z':
        return "<p class=\"msg\">已切换右上角形象。</p>";
    case 'w':
        return "<p class=\"msg\">WiFi 设置已保存，正在连接新网络…</p>";
    case 'n':
        return "<p class=\"msg warn\">WiFi 名称不能为空。</p>";
    case 'f':
        return "<p class=\"msg warn\">句子库满了（365 句）。</p>";
    case 'e':
        return "<p class=\"msg warn\">内容是空的。</p>";
    case 'x':
        return "<p class=\"msg warn\">没有找到这一句，或者写入失败，请刷新后再试。</p>";
    default:
        return NULL;
    }
}

static esp_err_t send_home(httpd_req_t *req)
{
    /* Static: the HTTP server handles one request at a time, and these buffers
     * grew with the sentence limit, so keep them off the (4 KB) task stack. */
    static char escaped[QUOTE_TEXT_MAX * 6];
    static char state_escaped[800];
    static char ssid_escaped[256];
    static char line[2000];

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    if (chunk(req, PAGE_HEAD) != ESP_OK) {
        return ESP_FAIL;
    }

    const char *tag = quote_is_custom()  ? "正在显示 · 临时一句话"
                      : quote_is_pinned() ? "正在显示 · 你指定的句子"
                                          : "正在显示 · 每日一句";
    html_escape(quote_text(), escaped, sizeof(escaped));
    snprintf(line, sizeof(line),
             "<h1>每日一句</h1><div class=\"now\"><span class=\"tag\">%s"
             "</span>%s</div>", tag, escaped);
    chunk(req, line);

    const char *notice = notice_for(req->uri);
    if (notice) {
        chunk(req, notice);
    }

    /* WiFi: credentials are stored in NVS by net_time_set_wifi(). The AP stays
     * up regardless, so a wrong password can always be corrected here. The
     * state line is refreshed by JS from /status; scanning lists only 2.4 GHz
     * networks, which is all the ESP32-C3 can join. */
    net_time_status_t wifi;
    char wifi_state[128];
    net_time_status(&wifi);
    if (wifi.connected && wifi.ssid[0]) {
        strlcpy(wifi_state, wifi.ssid, sizeof(wifi_state));
    } else if (wifi.configured && wifi.reason == 0) {
        strlcpy(wifi_state, "连接中", sizeof(wifi_state));
    } else {
        strlcpy(wifi_state, "未连接", sizeof(wifi_state));
    }
    html_escape(wifi_state, state_escaped, sizeof(state_escaped));
    html_escape(wifi.ssid, ssid_escaped, sizeof(ssid_escaped));

    snprintf(line, sizeof(line),
             "<h2>WiFi 连接</h2>"
             "<div class=\"wifi-state%s\" id=\"wifi-state\">%s</div>"
             "<p class=\"hint\">ESP32 只支持 2.4GHz 网络，名字里带 5G 的通常"
             "连不上。名称不变、密码留空时沿用原密码；新名称配空密码表示开放"
             "网络。AP 不会关闭，密码填错也能改回来。手机如果自动切回移动"
             "数据，页面会失联，可在 WiFi 设置里关掉「自动切换网络」。</p>"
             "<form method=\"post\" action=\"/wifi\">"
             "<input type=\"text\" name=\"ssid\" maxlength=\"32\" value=\"%s\" "
             "placeholder=\"WiFi 名称\">"
             "<input type=\"password\" name=\"pass\" maxlength=\"64\" "
             "placeholder=\"WiFi 密码（留空保持不变）\">"
             "<button type=\"submit\">保存并连接</button>"
             "<button class=\"ghost\" type=\"button\" onclick=\"scanWifi()\">"
             "扫描附近 WiFi</button></form>"
             "<div class=\"arts\" id=\"scan-list\"></div>",
             wifi.connected ? " ok"
                            : (wifi.configured && wifi.reason == 0) ? " wait"
                                                                   : "",
             state_escaped, ssid_escaped);
    chunk(req, line);

    /* One-off text: shown today, not stored in the pool. */
    html_escape(quote_is_custom() ? quote_text() : "", escaped, sizeof(escaped));
    snprintf(line, sizeof(line),
             "<h2>临时显示（明天自动恢复，不进入句子库）</h2>"
             "<form method=\"post\" action=\"/custom\">"
             "<textarea name=\"text\" maxlength=\"" STR(QUOTE_CHARS_MAX) "\" "
             "placeholder=\"写一句话\">%s</textarea>"
             "<button type=\"submit\">临时显示</button></form>"
             "<form method=\"post\" action=\"/auto\">"
             "<button class=\"ghost\" type=\"submit\">恢复每日一句</button>"
             "</form>", escaped);
    chunk(req, line);

    /* Thumbnail choice: daily random, or pinned to one picture. */
    snprintf(line, sizeof(line),
             "<h2>右上角形象 · %s</h2>"
             "<p class=\"hint\">「每日随机」每天 0 点换一张；选一张后会一直显示"
             "到你再改。</p>"
             "<form class=\"arts\" method=\"post\" action=\"/art\">"
             "<button name=\"op\" value=\"random\"%s>每日随机</button>",
             art_is_random() ? "每日随机" : "固定",
             art_is_random() ? " class=\"cur\"" : "");
    chunk(req, line);
    for (int i = 0; i < art_count(); i++) {
        bool cur = !art_is_random() && art_index() == i;

        snprintf(line, sizeof(line),
                 "<button name=\"op\" value=\"%d\"%s>%s</button>",
                 i, cur ? " class=\"cur\"" : "", art_name(i));
        chunk(req, line);
    }
    chunk(req, "</form>");

    snprintf(line, sizeof(line),
             "<h2>句子库 · %d 句 · 每句最多 %d 字</h2>"
             "<form class=\"row add\" method=\"post\" action=\"/pool\">"
             "<input type=\"text\" name=\"text\" maxlength=\""
             STR(QUOTE_CHARS_MAX) "\" "
             "placeholder=\"添加一句新的\">"
             "<button name=\"op\" value=\"add\" type=\"submit\">添加</button>"
             "</form>", quote_pool_count(), QUOTE_CHARS_MAX);
    chunk(req, line);

    uint16_t current = quote_current_id();
    for (int i = 0; i < quote_pool_count(); i++) {
        char text[QUOTE_TEXT_MAX];
        uint16_t id = quote_pool_get(i, text, sizeof(text));

        html_escape(text, escaped, sizeof(escaped));
        /* One send per row instead of three: a long pool otherwise turns into
         * hundreds of tiny sockets writes, and phones drop the AP halfway. */
        snprintf(line, sizeof(line), ROW_FORM,
                 id == current ? " cur" : "", (unsigned)id);
        size_t used = strlen(line);
        snprintf(line + used, sizeof(line) - used, "%s%s", escaped, ROW_END);
        chunk(req, line);
    }

    if (quote_pool_count() == 0) {
        chunk(req, "<p class=\"hint\">句子库是空的，先添加一句吧。</p>");
    }

    chunk(req, "<p class=\"hint\">「显示」把这一句钉在屏幕上，明天 0 点自动回到"
               "每日一句。编辑完点「保存」。</p>");
    chunk(req, PAGE_TAIL);
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ------------------------------------------------------------------------- */
/* Form handling                                                             */
/* ------------------------------------------------------------------------- */

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Extracts `field=` from an application/x-www-form-urlencoded body, decoding
 * '+' and %XX. Returns false when the field is absent. */
static bool form_field(const char *body, const char *field, char *out,
                       size_t out_size)
{
    size_t flen = strlen(field);
    const char *p = body;

    while (p && *p) {
        if (strncmp(p, field, flen) == 0 && p[flen] == '=') {
            p += flen + 1;
            size_t o = 0;
            while (*p && *p != '&' && o + 1 < out_size) {
                if (*p == '+') {
                    out[o++] = ' ';
                    p++;
                } else if (*p == '%' && hex_value(p[1]) >= 0 &&
                           hex_value(p[2]) >= 0) {
                    out[o++] = (char)(hex_value(p[1]) * 16 + hex_value(p[2]));
                    p += 3;
                } else {
                    out[o++] = *p++;
                }
            }
            out[o] = '\0';
            return true;
        }
        p = strchr(p, '&');
        if (p) {
            p++;
        }
    }
    return false;
}

/* Trims trailing CR/LF/spaces and cuts at a UTF-8 boundary. */
static void tidy_text(char *text, size_t max_bytes)
{
    size_t len = strlen(text);

    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r' ||
                       text[len - 1] == ' ' || text[len - 1] == '\t')) {
        text[--len] = '\0';
    }
    while (len > max_bytes) {
        len--;
        while (len > 0 && ((unsigned char)text[len] & 0xC0) == 0x80) {
            len--;
        }
    }
    text[len] = '\0';
}

static esp_err_t read_body(httpd_req_t *req, char *body, size_t size)
{
    int total = req->content_len;
    if (total <= 0 || total >= (int)size) {
        return ESP_ERR_INVALID_SIZE;
    }
    int got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, body + got, (size_t)(total - got));
        if (n <= 0) {
            return ESP_FAIL;
        }
        got += n;
    }
    body[got] = '\0';
    return ESP_OK;
}

static esp_err_t redirect_home(httpd_req_t *req, const char *query)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", query ? query : "/");
    return httpd_resp_send(req, NULL, 0);
}

static void changed(void)
{
    if (s_on_change) {
        s_on_change();
    }
}

static esp_err_t handle_pool(httpd_req_t *req)
{
    static char body[HTTP_BODY];
    char text[QUOTE_TEXT_MAX];
    char op[8];
    char idbuf[8];
    uint16_t id = 0;
    const char *result = "/?m=x";

    if (read_body(req, body, sizeof(body)) != ESP_OK ||
        !form_field(body, "op", op, sizeof(op))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
        return ESP_OK;
    }
    if (form_field(body, "id", idbuf, sizeof(idbuf))) {
        id = (uint16_t)atoi(idbuf);
    }

    if (strcmp(op, "add") == 0) {
        if (!form_field(body, "text", text, sizeof(text))) {
            result = "/?m=e";
        } else {
            tidy_text(text, QUOTE_TEXT_MAX - 1);
            if (text[0] == '\0') {
                result = "/?m=e";
            } else if (quote_pool_count() >= QUOTE_POOL_MAX) {
                result = "/?m=f";
            } else if (quote_pool_add(text) == 0) {
                result = "/?m=x";
            } else {
                result = "/?m=a";
            }
        }
    } else if (strcmp(op, "save") == 0) {
        if (!form_field(body, "text", text, sizeof(text))) {
            result = "/?m=e";
        } else {
            tidy_text(text, QUOTE_TEXT_MAX - 1);
            result = text[0] == '\0' ? "/?m=e"
                     : quote_pool_set(id, text) ? "/?m=s"
                                                : "/?m=x";
        }
    } else if (strcmp(op, "show") == 0) {
        /* The row is an input, so "show" also stores what is typed in it;
         * otherwise a quick edit followed by "show" would lose the edit. */
        if (form_field(body, "text", text, sizeof(text))) {
            tidy_text(text, QUOTE_TEXT_MAX - 1);
            if (text[0] != '\0') {
                quote_pool_set(id, text);
            }
        }
        result = quote_show(id) ? "/?m=v" : "/?m=x";
    } else if (strcmp(op, "del") == 0) {
        result = quote_pool_delete(id) ? "/?m=d" : "/?m=x";
    }

    /* Send the response first: the panel refresh is synchronous in the main
     * task, and if WiFi is still transmitting while the waveform runs the
     * supply sags and the refresh comes out grey. */
    esp_err_t err = redirect_home(req, result);
    if (err == ESP_OK) {
        changed();
    }
    return err;
}

static esp_err_t handle_custom(httpd_req_t *req)
{
    static char body[HTTP_BODY];
    char text[QUOTE_TEXT_MAX];

    if (read_body(req, body, sizeof(body)) != ESP_OK ||
        !form_field(body, "text", text, sizeof(text))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
        return ESP_OK;
    }

    tidy_text(text, QUOTE_TEXT_MAX - 1);
    if (text[0] == '\0') {
        return redirect_home(req, "/?m=e");
    }

    quote_set_custom(text);
    esp_err_t err = redirect_home(req, "/?m=c");
    if (err == ESP_OK) {
        changed();
    }
    return err;
}

static esp_err_t handle_auto(httpd_req_t *req)
{
    quote_clear_custom();
    esp_err_t err = redirect_home(req, "/?m=r");
    if (err == ESP_OK) {
        changed();
    }
    return err;
}

static esp_err_t handle_art(httpd_req_t *req)
{
    static char body[HTTP_BODY];
    char op[8];
    bool ok = false;

    if (read_body(req, body, sizeof(body)) != ESP_OK ||
        !form_field(body, "op", op, sizeof(op))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
        return ESP_OK;
    }

    if (strcmp(op, "random") == 0) {
        ok = art_set_random();
    } else if (op[0] >= '0' && op[0] <= '9') {
        ok = art_set_fixed(atoi(op));
    }

    esp_err_t err = redirect_home(req, ok ? "/?m=z" : "/?m=x");
    if (err == ESP_OK) {
        changed();
    }
    return err;
}

static esp_err_t handle_wifi(httpd_req_t *req)
{
    static char body[HTTP_BODY];
    char ssid[33];
    char pass[65];

    if (read_body(req, body, sizeof(body)) != ESP_OK ||
        !form_field(body, "ssid", ssid, sizeof(ssid))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
        return ESP_OK;
    }

    tidy_text(ssid, sizeof(ssid) - 1);
    pass[0] = '\0';
    if (form_field(body, "pass", pass, sizeof(pass))) {
        tidy_text(pass, sizeof(pass) - 1);
    }

    if (ssid[0] == '\0') {
        return redirect_home(req, "/?m=n");
    }
    if (strlen(ssid) > 32 || strlen(pass) > 64) {
        return redirect_home(req, "/?m=x");
    }

    /* Send the redirect first. Switching networks stops the station link and
     * starts a scan, and doing that before the phone has received the response
     * makes the browser hang on a dead socket. */
    esp_err_t err = redirect_home(req, "/?m=w");
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(600));
        net_time_set_wifi(ssid, pass);
    }
    return err;
}

/* Polled by the page every two seconds: shows what the station is doing. */
static esp_err_t handle_status(httpd_req_t *req)
{
    static char buf[384];
    static char ssid[96];
    net_time_status_t st;

    net_time_status(&st);
    json_escape(st.ssid, ssid, sizeof(ssid));
    snprintf(buf, sizeof(buf),
             "{\"configured\":%s,\"connected\":%s,\"ssid\":\"%s\","
             "\"ip\":\"%s\",\"synced\":%s,\"sy\":%d,\"sm\":%d,"
             "\"reason\":%u,\"failed\":%s}",
             st.configured ? "true" : "false",
             st.connected ? "true" : "false", ssid,
             st.connected ? st.ip : "",
             st.synced ? "true" : "false", st.sync_hour, st.sync_min,
             (unsigned)st.reason, st.sync_failed ? "true" : "false");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

/* One blocking scan, triggered by the page's 扫描 button. */
static esp_err_t handle_scan(httpd_req_t *req)
{
    static net_time_ap_t aps[16];
    static char buf[4096];
    int n = net_time_scan(aps, 16);
    int o = 0;

    if (n < 0) {
        n = 0;
    }

    buf[o++] = '[';
    for (int i = 0; i < n; i++) {
        char ssid[200];

        if (sizeof(buf) - (size_t)o < 300) {
            break;
        }
        json_escape(aps[i].ssid, ssid, sizeof(ssid));
        o += snprintf(buf + o, sizeof(buf) - (size_t)o,
                      "%s{\"s\":\"%s\",\"r\":%d,\"o\":%d}",
                      i ? "," : "", ssid, aps[i].rssi,
                      aps[i].secure ? 0 : 1);
    }
    buf[o++] = ']';
    buf[o] = '\0';

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_root(httpd_req_t *req)
{
    return send_home(req);
}

static esp_err_t handle_404(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

/* ------------------------------------------------------------------------- */
/* Startup                                                                   */
/* ------------------------------------------------------------------------- */

static void portal_wifi_event(void *arg, esp_event_base_t base, int32_t id,
                              void *data)
{
    (void)arg;
    (void)base;
    (void)data;

    switch (id) {
    case WIFI_EVENT_AP_START:
        ESP_LOGI(TAG, "SoftAP up, SSID \"%s\", open http://192.168.4.1", AP_SSID);
        break;
    case WIFI_EVENT_AP_STOP:
        ESP_LOGW(TAG, "SoftAP stopped");
        break;
    case WIFI_EVENT_AP_STACONNECTED:
        ESP_LOGI(TAG, "phone connected to the portal");
        break;
    case WIFI_EVENT_AP_STADISCONNECTED:
        ESP_LOGI(TAG, "phone left the portal");
        break;
    default:
        break;
    }
}

bool web_portal_start(web_portal_changed_cb_t on_change)
{
    s_on_change = on_change;

    esp_netif_create_default_wifi_ap();

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set APSTA mode: %s", esp_err_to_name(err));
        return false;
    }

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.ap.ssid, AP_SSID, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = (uint8_t)strlen(AP_SSID);
    cfg.ap.channel = 1;
    cfg.ap.max_connection = 2;
    cfg.ap.authmode = WIFI_AUTH_OPEN;

    err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AP config: %s", esp_err_to_name(err));
        return false;
    }

    httpd_config_t cfg_http = HTTPD_DEFAULT_CONFIG();
    cfg_http.stack_size = 6144;
    cfg_http.lru_purge_enable = true;
    cfg_http.max_uri_handlers = 8;

    httpd_handle_t server = NULL;
    err = httpd_start(&server, &cfg_http);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http server: %s", esp_err_to_name(err));
        return false;
    }

    static const httpd_uri_t uri_root = {
        .uri = "/", .method = HTTP_GET, .handler = handle_root,
    };
    static const httpd_uri_t uri_pool = {
        .uri = "/pool", .method = HTTP_POST, .handler = handle_pool,
    };
    static const httpd_uri_t uri_custom = {
        .uri = "/custom", .method = HTTP_POST, .handler = handle_custom,
    };
    static const httpd_uri_t uri_auto = {
        .uri = "/auto", .method = HTTP_POST, .handler = handle_auto,
    };
    static const httpd_uri_t uri_art = {
        .uri = "/art", .method = HTTP_POST, .handler = handle_art,
    };
    static const httpd_uri_t uri_wifi = {
        .uri = "/wifi", .method = HTTP_POST, .handler = handle_wifi,
    };
    static const httpd_uri_t uri_status = {
        .uri = "/status", .method = HTTP_GET, .handler = handle_status,
    };
    static const httpd_uri_t uri_scan = {
        .uri = "/scan", .method = HTTP_GET, .handler = handle_scan,
    };
    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_pool);
    httpd_register_uri_handler(server, &uri_custom);
    httpd_register_uri_handler(server, &uri_auto);
    httpd_register_uri_handler(server, &uri_art);
    httpd_register_uri_handler(server, &uri_wifi);
    httpd_register_uri_handler(server, &uri_status);
    httpd_register_uri_handler(server, &uri_scan);
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, handle_404);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        portal_wifi_event, NULL, NULL);

    ESP_LOGI(TAG, "portal ready: connect to \"%s\", open http://192.168.4.1",
             AP_SSID);
    return true;
}
