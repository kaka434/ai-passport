// main/app_portal.c
#include "app_portal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_net.h"
#include "app_settings.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "app_portal";

// 手机上的配网页面。属性与 JS 字符串一律用单引号,这样整段放进 C 字面量时
// 不需要任何转义;也刻意不引外部资源,设备热点没有外网。
static const char PAGE_HTML[] =
    "<!DOCTYPE html><html lang='zh-CN'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>比赛看板配网</title><style>"
    "body{font-family:-apple-system,system-ui,sans-serif;margin:0;padding:18px;"
    "background:#111;color:#eee}"
    "h1{font-size:18px;margin:0 0 4px}"
    "p.sub{color:#8a8a8a;font-size:13px;margin:0 0 14px}"
    "button.net{display:block;width:100%;text-align:left;padding:12px;margin:6px 0;"
    "border-radius:8px;border:1px solid #333;background:#1c1c1c;color:#eee;font-size:15px}"
    "button.net.on{border-color:#f0a020;background:#2a2113}"
    "input{width:100%;box-sizing:border-box;padding:12px;border-radius:8px;"
    "border:1px solid #333;background:#1c1c1c;color:#eee;font-size:15px;margin:8px 0}"
    "button.go{width:100%;padding:13px;border:0;border-radius:8px;background:#f0a020;"
    "color:#111;font-size:16px;font-weight:600}"
    "#m{margin-top:14px;font-size:14px;color:#f0a020;min-height:20px}"
    "</style></head><body>"
    "<h1>比赛看板 · Wi-Fi 配网</h1>"
    "<p class='sub'>点一个网络,输密码,然后点连接</p>"
    "<div id='list'><p class='sub'>正在扫描…</p></div>"
    "<input id='p' type='password' placeholder='密码(开放网络留空)' autocomplete='off'>"
    "<button class='go' onclick='go()'>连接</button>"
    "<div id='m'></div>"
    "<script>"
    "var ssid='';"
    "function scan(){fetch('/api/scan').then(function(r){return r.json()}).then(function(d){"
    "var box=document.getElementById('list');box.textContent='';"
    "if(!d.aps.length){box.textContent='没扫到网络,靠近路由器再试';return;}"
    "d.aps.forEach(function(a){var b=document.createElement('button');b.className='net';"
    "b.setAttribute('data-s',a.s);b.textContent=a.s+'  ('+a.r+' dBm)';box.appendChild(b);});"
    "}).catch(function(){document.getElementById('list').textContent='扫描失败,刷新页面重试';});}"
    "document.getElementById('list').addEventListener('click',function(e){"
    "var b=e.target.closest('.net');if(!b)return;ssid=b.getAttribute('data-s');"
    "var all=document.querySelectorAll('.net');"
    "for(var i=0;i<all.length;i++){all[i].classList.remove('on');}b.classList.add('on');});"
    "function go(){var m=document.getElementById('m');"
    "if(!ssid){m.textContent='先点一个网络';return;}m.textContent='正在连接…';"
    "var body='ssid='+encodeURIComponent(ssid)+'&pass='+"
    "encodeURIComponent(document.getElementById('p').value);"
    "fetch('/api/connect',{method:'POST',"
    "headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})"
    ".then(function(r){return r.json()}).then(function(d){m.textContent=d.msg||'已提交';poll();})"
    ".catch(function(){m.textContent='提交失败,再点一次';});}"
    "function poll(){fetch('/api/status').then(function(r){return r.json()}).then(function(d){"
    "document.getElementById('m').textContent=d.msg||'';"
    "if(d.state===2){setTimeout(poll,800);}}).catch(function(){});}"
    "scan();"
    "</script></body></html>";

// ---------------------------------------------------------------- 状态

static SemaphoreHandle_t s_lock;
static app_portal_snapshot_t s_snap;
static esp_netif_t *s_ap_netif;
static httpd_handle_t s_server;
static TaskHandle_t s_task;

// 一次扫描的结果与序列化缓冲。都是静态的:HTTP 任务的栈只有 4 KB。
static app_scan_result_t s_scan_tmp;
static char s_json[1100];

static void lock(void) {
    if (s_lock != NULL) xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void) {
    if (s_lock != NULL) xSemaphoreGive(s_lock);
}

static void publish(uint8_t state, const char *ssid, const char *message) {
    lock();
    s_snap.state = state;
    if (ssid != NULL) {
        size_t n = 0;
        while (n + 1 < sizeof(s_snap.ssid) && ssid[n] != '\0') n++;
        memcpy(s_snap.ssid, ssid, n);
        s_snap.ssid[n] = '\0';
    }
    if (message != NULL) {
        size_t n = 0;
        while (n + 1 < sizeof(s_snap.message) && message[n] != '\0') n++;
        memcpy(s_snap.message, message, n);
        s_snap.message[n] = '\0';
    }
    s_snap.revision++;
    unlock();
}

// ---------------------------------------------------------------- 小工具

// SSID 里可能出现引号或反斜杠,直接拼进 JSON 会把页面打乱。
static size_t json_escape(const char *src, char *dst, size_t cap) {
    size_t n = 0;
    for (const char *p = src; *p != '\0' && n + 7 < cap; p++) {
        const unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            dst[n++] = '\\';
            dst[n++] = (char)c;
        } else if (c < 0x20) {
            n += (size_t)snprintf(dst + n, cap - n, "\\u%04x", c);
        } else {
            dst[n++] = (char)c;
        }
    }
    dst[n] = '\0';
    return n;
}

// URL 解码:表单里的 %XX 与 '+'。
static void url_decode(const char *src, char *dst, size_t cap) {
    size_t n = 0;
    for (const char *p = src; *p != '\0' && n + 1 < cap; p++) {
        if (*p == '+') {
            dst[n++] = ' ';
        } else if (*p == '%' && p[1] != '\0' && p[2] != '\0') {
            char hex[3] = { p[1], p[2], '\0' };
            dst[n++] = (char)strtol(hex, NULL, 16);
            p += 2;
        } else {
            dst[n++] = *p;
        }
    }
    dst[n] = '\0';
}

// 从 application/x-www-form-urlencoded 里取一个字段。
static bool form_field(const char *body, const char *key, char *out, size_t cap) {
    const size_t key_len = strlen(key);
    const char *p = body;
    while (*p != '\0') {
        const char *eq = strchr(p, '=');
        if (eq == NULL) break;
        const bool match = ((size_t)(eq - p) == key_len) && (strncmp(p, key, key_len) == 0);
        const char *end = strchr(eq + 1, '&');
        if (match) {
            char raw[APP_PASS_MAX * 3 + 8];
            size_t n = 0;
            for (const char *q = eq + 1; (end == NULL || q < end) && *q != '\0'; q++) {
                if (n + 1 >= sizeof(raw)) break;
                raw[n++] = *q;
            }
            raw[n] = '\0';
            url_decode(raw, out, cap);
            return true;
        }
        if (end == NULL) break;
        p = end + 1;
    }
    return false;
}

static esp_err_t reply_json(httpd_req_t *req, const char *json) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

// ---------------------------------------------------------------- HTTP 处理

static esp_err_t h_page(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

// 扫描是异步的(扫描任务在后台跑),这里等它出结果再回 JSON。
// 单用户场景,占住这个请求 6 秒无所谓。
static esp_err_t h_scan(httpd_req_t *req) {
    const uint32_t before = app_net_scan_revision();
    app_net_scan_start();

    for (int i = 0; i < 70; i++) {
        app_net_scan_copy(&s_scan_tmp);
        if (app_net_scan_revision() != before && !s_scan_tmp.scanning && s_scan_tmp.done) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    size_t n = (size_t)snprintf(s_json, sizeof(s_json), "{\"aps\":[");
    for (size_t i = 0; i < s_scan_tmp.count && n + 96 < sizeof(s_json); i++) {
        char name[APP_SSID_MAX * 6 + 8];
        json_escape(s_scan_tmp.items[i].ssid, name, sizeof(name));
        n += (size_t)snprintf(s_json + n, sizeof(s_json) - n,
                              "%s{\"s\":\"%s\",\"r\":%d}",
                              i == 0 ? "" : ",", name, (int)s_scan_tmp.items[i].rssi);
    }
    snprintf(s_json + n, sizeof(s_json) - n, "]}");
    ESP_LOGI(TAG, "门户扫描:%u 个热点", (unsigned)s_scan_tmp.count);
    return reply_json(req, s_json);
}

static esp_err_t h_connect(httpd_req_t *req) {
    char body[256];
    const int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) {
        return reply_json(req, "{\"ok\":false,\"msg\":\"请求为空\"}");
    }
    body[n] = '\0';

    char ssid[APP_SSID_MAX + 1];
    char pass[APP_PASS_MAX + 1];
    ssid[0] = '\0';
    pass[0] = '\0';
    if (!form_field(body, "ssid", ssid, sizeof(ssid))) {
        return reply_json(req, "{\"ok\":false,\"msg\":\"没有收到网络名\"}");
    }
    (void)form_field(body, "pass", pass, sizeof(pass));

    if (!app_config_validate_credentials(ssid, pass)) {
        ESP_LOGW(TAG, "门户提交的凭证不合法:%s", ssid);
        return reply_json(req, "{\"ok\":false,\"msg\":\"网络名或密码不合法\"}");
    }

    // 和屏上配网走同一条路:落盘 + 通知联网任务重连。
    app_config_t cfg;
    app_settings_get(&cfg);
    size_t len = strlen(ssid);
    memcpy(cfg.ssid, ssid, len + 1);
    len = strlen(pass);
    memcpy(cfg.pass, pass, len + 1);
    app_settings_apply(&cfg, true);

    publish(APP_PORTAL_CONNECTING, ssid, "正在连接…");
    ESP_LOGI(TAG, "门户收到凭证:%s", ssid);
    return reply_json(req, "{\"ok\":true,\"msg\":\"正在连接,稍等几秒…\"}");
}

static esp_err_t h_status(httpd_req_t *req) {
    app_portal_snapshot_t snap;
    app_portal_snapshot_copy(&snap);
    char msg[APP_PORTAL_MSG_LEN * 6 + 8];
    json_escape(snap.message, msg, sizeof(msg));
    snprintf(s_json, sizeof(s_json), "{\"state\":%u,\"msg\":\"%s\"}",
             (unsigned)snap.state, msg);
    return reply_json(req, s_json);
}

// ---------------------------------------------------------------- 守护任务

// 只做三件事:等连接结果、超时报错、成功后自动关热点省电。
static void portal_task(void *arg) {
    (void)arg;
    uint8_t seen = APP_PORTAL_IDLE;
    int ticks = 0;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));

        uint8_t state;
        lock();
        state = s_snap.state;
        unlock();

        if (state != seen) {
            seen = state;
            ticks = 0;
        } else {
            ticks++;
        }

        if (state == APP_PORTAL_CONNECTING) {
            if (app_net_is_connected()) {
                app_portal_snapshot_t snap;
                app_portal_snapshot_copy(&snap);
                publish(APP_PORTAL_OK, NULL, "配网成功,手机可以断开");
                ESP_LOGI(TAG, "门户配网成功:%s", snap.ssid);
            } else if (ticks >= 40) {   // 20 秒还没连上
                publish(APP_PORTAL_FAILED, NULL, "连不上,密码可能不对");
                ESP_LOGW(TAG, "门户配网超时");
            }
        } else if (state == APP_PORTAL_OK && ticks >= 16) {
            // 页面已经看到"成功"了,热点再开 8 秒就收工,省电也省得被邻居扫到。
            app_portal_stop();
            publish(APP_PORTAL_IDLE, NULL, "配网完成");
        }
    }
}

// ---------------------------------------------------------------- 启停

static bool start_ap(void) {
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_WIFI_AP();
    s_ap_netif = esp_netif_new(&netif_cfg);
    if (s_ap_netif == NULL) return false;
    if (esp_netif_attach_wifi_ap(s_ap_netif) != ESP_OK) return false;
    if (esp_wifi_set_default_wifi_ap_handlers() != ESP_OK) return false;

    wifi_config_t ap = { 0 };
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", APP_PORTAL_AP_SSID);
    ap.ap.ssid_len = (uint8_t)strlen(APP_PORTAL_AP_SSID);
    snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s", APP_PORTAL_AP_PASS);
    ap.ap.channel = 6;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.pmf_cfg.required = false;

    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK) return false;
    if (esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK) return false;
    return true;
}

static void stop_ap(void) {
    if (s_ap_netif != NULL) {
        // 顺序不能反:先切回纯 STA 让驱动把 AP 接口停掉,再摘掉默认 AP 事件
        // 处理器,最后才能销毁 netif。以前直接 destroy,留下的默认处理器还指着
        // 已释放的 netif,下一次 AP 事件(比如又开了配网页)就是一次 use-after-free。
        (void)esp_wifi_set_mode(WIFI_MODE_STA);
        (void)esp_wifi_clear_default_wifi_driver_and_handlers(s_ap_netif);
        esp_netif_destroy(s_ap_netif);
        s_ap_netif = NULL;
    }
}

static bool start_server(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 2;   // 配网页面只有一个手机,给 2 个足够
    cfg.stack_size = 5120;      // 扫描端点会在这里等 6 秒,栈给宽一点
    cfg.lru_purge_enable = true;
    cfg.server_port = 80;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        s_server = NULL;
        return false;
    }

    const httpd_uri_t page = { .uri = "/", .method = HTTP_GET, .handler = h_page };
    const httpd_uri_t scan = { .uri = "/api/scan", .method = HTTP_GET, .handler = h_scan };
    const httpd_uri_t conn = { .uri = "/api/connect", .method = HTTP_POST, .handler = h_connect };
    const httpd_uri_t stat = { .uri = "/api/status", .method = HTTP_GET, .handler = h_status };
    (void)httpd_register_uri_handler(s_server, &page);
    (void)httpd_register_uri_handler(s_server, &scan);
    (void)httpd_register_uri_handler(s_server, &conn);
    (void)httpd_register_uri_handler(s_server, &stat);
    return true;
}

bool app_portal_start(void) {
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) return false;
    }
    if (s_task == NULL) {
        if (xTaskCreate(portal_task, "app_portal", 3072, NULL, 3, &s_task) != pdPASS) {
            s_task = NULL;
            ESP_LOGW(TAG, "门户任务创建失败");
            return false;
        }
    }

    if (!app_net_wifi_ready()) {
        publish(APP_PORTAL_FAILED, NULL, "Wi-Fi 没启动");
        return false;
    }

    if (s_ap_netif == NULL && !start_ap()) {
        ESP_LOGW(TAG, "热点启动失败");
        stop_ap();
        publish(APP_PORTAL_FAILED, NULL, "热点启动失败");
        return false;
    }
    if (s_server == NULL && !start_server()) {
        ESP_LOGW(TAG, "网页服务启动失败");
        stop_ap();
        publish(APP_PORTAL_FAILED, NULL, "网页服务启动失败");
        return false;
    }

    publish(APP_PORTAL_WAITING, NULL, "等手机连接");
    ESP_LOGI(TAG, "配网热点已开:%s / %s", APP_PORTAL_AP_SSID, APP_PORTAL_AP_IP);
    return true;
}

void app_portal_stop(void) {
    if (s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    stop_ap();
    publish(APP_PORTAL_IDLE, NULL, "");
}

bool app_portal_active(void) {
    return s_ap_netif != NULL;
}

void app_portal_snapshot_copy(app_portal_snapshot_t *out) {
    if (out == NULL || s_lock == NULL) return;
    lock();
    *out = s_snap;
    unlock();
}

uint32_t app_portal_revision(void) {
    if (s_lock == NULL) return 0;
    lock();
    const uint32_t rev = s_snap.revision;
    unlock();
    return rev;
}
