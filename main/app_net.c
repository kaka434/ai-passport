// main/app_net.c
#include "app_net.h"

#include "app_json.h"

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "app_net";

// 懂球帝的比分列表接口。参数含义:
//   cmp_type=soccer  只要足球(接口还支持 basketball)
//   tab_type=all     全部比赛(直播中/未开始/已结束都在这一个列表里)
// 站点会校验 Referer,不带就返回 567 + 一段 HTML,所以下面必须补上。
//
// 注意这个接口的窗口是**UTC 日**,不是北京日:实测下午 14:00 请求,返回的
// 比赛从当天北京 07:30 才开始。北京 00:00-07:30 这一段它压根不返回,而欧冠/
// 欧联的凌晨场(00:45 / 03:00)正好全落在这一段里——这是"主页看不到凌晨
// 比赛"的根因。补这一段要用下面的赛程接口,见 fetch_today_extra()。
#define APP_FEED_URL \
    "https://www.dongqiudi.com/magicball/v1/list/match_list?language=zh-CN&cmp_type=soccer&tab_type=all"
// 赛程接口:start 起 24 小时,start 是 UTC 的 "YYYY-MM-DD HH:00:00"。
// tab_type=result 只给已完场,tab_type=fixture 只给未开始,**正在踢的那场
// 三个 tab 都不返回**(实测),所以它只能用来补历史,不能替掉 match_list。
#define APP_FEED_SCHEDULE_URL \
    "https://www.dongqiudi.com/magicball/v1/list/schedule_list" \
    "?language=zh-CN&tab_type=%s&cmp_type=soccer&start=%s"
#define APP_FEED_REFERER "https://www.dongqiudi.com/match"
#define APP_FEED_UA "Mozilla/5.0 (FoloToy-AI-Passport)"

// 响应是 1.6 MB,不可能整段缓冲。这里只留 4 KB 的读块,边收边喂给流式解析器,
// 内存占用与响应大小无关。C3 没有 PSRAM,堆要同时供 Wi-Fi 与 TLS。
#define APP_READ_CHUNK 4096
#define APP_WIFI_WAIT_MS 25000
#define APP_TIME_WAIT_MS 15000
#define APP_RETRY_MIN_MS 5000
#define APP_RETRY_MAX_MS 60000

// 拉取失败后的重试节奏:先隔 1.2 秒立刻再试一次(多数瞬时抖动这一下就过去了),
// 两次都失败才把下一次等待压到 30 秒——比睡满整个刷新间隔(60~300 秒)自愈得快,
// 又不会在真断网时把流量和电耗在死循环重试上。
#define APP_RETRY_ONCE_DELAY_MS 1200
#define APP_RETRY_SHORT_WAIT_MS 30000
// 今天没有白名单赛事时,最多往后找几天。七天覆盖一个国际比赛日间歇够用,
// 再多就是拿流量换一个"确实没有"的结论。
#define APP_NEXT_DAY_MAX 7
// 已经找到下一个比赛日之后,隔多久再确认一次(临期会补上新的场次)。
#define APP_NEXT_RECHECK_SECONDS (30 * 60)

// 今天"凌晨段"的补拉节流。
//
// 补一份 schedule_list 要 200 KB 上下,而凌晨场最晚 04:00 开球、06:00 前
// 一定踢完,过了这个点比分就定格了,当天再补也是同一份数据。所以 07:00
// 之后当天只补一次——正好把流量开销从"每次刷新"压到"每天一次"。
#define APP_EXTRA_SETTLE_HOUR 7

static SemaphoreHandle_t s_lock;
// 单独一把锁保护"射频只初始化一次":联网任务和扫描任务都会拉 Wi-Fi,
// 而 esp_wifi_init() 重复调用会直接报错。
static SemaphoreHandle_t s_wifi_lock;
static app_config_t s_config;
static app_snapshot_t s_snapshot;
static app_scan_result_t s_scan;
// 解析暂存区。一个 app_match_list_t 约 3.6 KB,放在任务栈上会压爆联网任务的栈
// (还要留位置给 TLS 握手),所以做成静态的。只有 net_task 会碰它。
//
// 未来赛程的探测也复用这一块:探测只在"今天一场白名单比赛都没有"时发生,那时
// 它本来就是空的,用完再 publish 进快照,快照本身就是缓存。以前另开一份
// s_next_parsed 存同样的东西,白白多占 3.6 KB。
static app_match_list_t s_parsed;
// 缓存的比赛日:0 = 没找到,1..7 = 未来第 N 天。
static int s_next_day;
static bool s_next_probed;          // 是否已经探过一轮(含"确实没有")
static int64_t s_next_day_start;    // 那一天的 CST 00:00(UTC 秒),用来判断缓存过期
static int64_t s_next_probe_at;     // 上次探测的时刻(UTC 秒)
// 用户选的日期:0 = 今天,-1 = 回看前一天,1..7 = 未来第 N 天。
// 只有 app_net_set_view_day() 会改,联网任务读它来决定这一轮拉哪一份。
static int8_t s_view_day;
// 非今天那份数据的缓存。s_parsed 只有一块,所以最多只能缓存一天:切到别的
// 日子就把前一天的数据覆盖掉了,这也是下面要靠 s_published_view 判断
// "缓存到底还能不能用"的原因。
static bool s_day_cache_valid;
static int64_t s_day_cache_key;     // 拉的是哪一天的 CST 00:00(UTC 秒)
// 今天"凌晨段"补过没有,补的是哪一天,以及补的时刻。见 APP_EXTRA_SETTLE_HOUR。
static bool s_today_extra_valid;
static int64_t s_today_extra_key;
static int64_t s_today_extra_at;
// 快照里实际装的是哪一天的比赛:0 / -1。
//
// 和 s_view_day 的区别很关键:用户按上下键的瞬间 s_view_day 就变了,但数据
// 要等拉回来才变。这里只在 publish() 里更新,用来判断"切回前日时缓存能不能
// 直接复用"——数据已经被今天那份覆盖过,就不能,必须重拉。
static int8_t s_published_view;
static char *s_read_buf;
// 扫描结果缓冲。约 1.2 KB,同样放静态区,不占任务栈。
static wifi_ap_record_t s_scan_records[APP_SCAN_MAX];

static TaskHandle_t s_task;
static TaskHandle_t s_scan_task;
static TaskHandle_t s_detail_task;
static volatile bool s_sta_connected;
// 有没有凭证,决定断开后要不要自动重连。没配网时连了也是白连,只会刷日志。
static volatile bool s_has_credentials;
static bool s_wifi_started;
static bool s_sntp_started;
static bool s_started;
static char s_applied_ssid[APP_SSID_MAX + 1];
static char s_applied_pass[APP_PASS_MAX + 1];
// 拿到 IP 后要往 NVS 里记一条"连过的网络"。在事件里直接写 NVS 不合适
// (事件任务还要处理别的),所以只置标志,由联网任务落盘。
static volatile bool s_remember_pending;
static app_net_connected_cb_t s_connected_hook;

// 单场详情:一把 HTTP 锁 + 一份快照。锁的作用是让"列表刷新"和"详情拉取"
// 排队握手——两条 TLS 会话同时开,C3 的堆扛不住。
static SemaphoreHandle_t s_http_lock;
static app_detail_snapshot_t s_detail;
static char s_detail_req_id[APP_MATCH_ID_MAX + 1];
static volatile bool s_detail_pending;

static void lock(void) {
    if (s_lock != NULL) xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void) {
    if (s_lock != NULL) xSemaphoreGive(s_lock);
}

// Wi-Fi 的 ssid/password 是定长 uint8_t 数组,比配置里的 char[N+1] 少一个字节。
// 用 snprintf 会被 -Wformat-truncation 判成"可能截断"(实际确实会截,而那正是
// 我们想要的),所以这里显式按长度拷贝,截断规则一目了然。
static void copy_field(uint8_t *dst, size_t dst_len, const char *src) {
    if (dst_len == 0) return;
    size_t n = 0;
    while (n + 1 < dst_len && src[n] != '\0') n++;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

// 同上,给 char 缓冲用。扫描到的 SSID 来自驱动里的定长数组,编译器看不到
// 长度,直接 snprintf 会触发 -Wformat-truncation。
static void copy_text(char *dst, size_t cap, const char *src) {
    if (cap == 0) return;
    size_t n = 0;
    while (n + 1 < cap && src[n] != '\0') n++;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void set_error(const char *text) {
    lock();
    s_snapshot.state = APP_NET_ERROR;
    snprintf(s_snapshot.error, sizeof(s_snapshot.error), "%s", text != NULL ? text : "未知错误");
    s_snapshot.revision++;   // 界面靠 revision 才知道要不要重画
    unlock();
}

static void set_state(uint8_t state) {
    lock();
    if (s_snapshot.state != state) {
        s_snapshot.state = state;
        // 探测"下一个比赛日"要连下好几份响应,可能几十秒。不在这里加版本号,
        // 界面就会一直停在旧画面,看起来像死机。
        s_snapshot.revision++;
    }
    if (state != APP_NET_ERROR) s_snapshot.error[0] = '\0';
    unlock();
}

// ---------------------------------------------------------------- Wi-Fi 事件

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    (void)data;

    if (id == WIFI_EVENT_STA_START) {
        // 没凭证时不主动连:第一次开机就是这条路,连了只会一直报 201。
        if (s_has_credentials) esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
        // 不做指数退避:工作任务每秒检查一次连接状态,断线就重连,
        // 这样热点恢复后能自己回来,不需要用户按键。
        if (s_wifi_started && s_has_credentials) esp_wifi_connect();
        lock();
        s_snapshot.wifi_connected = false;
        s_snapshot.rssi = 0;
        s_snapshot.ip[0] = '\0';
        unlock();
        ESP_LOGW(TAG, "Wi-Fi 断开,准备重连");
    } else if (id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "Wi-Fi 已关联,等待 IP");
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    if (id != IP_EVENT_STA_GOT_IP) return;

    const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
    s_sta_connected = true;
    s_remember_pending = true;

    lock();
    s_snapshot.wifi_connected = true;
    snprintf(s_snapshot.ip, sizeof(s_snapshot.ip), IPSTR, IP2STR(&event->ip_info.ip));
    unlock();
    ESP_LOGI(TAG, "拿到 IP:" IPSTR, IP2STR(&event->ip_info.ip));
}

// 只做硬件初始化,不配凭证。这样"没配过网"的设备也能扫热点。
static esp_err_t wifi_hw_start(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 不自动擦除:应用自己的配置也在 NVS 里,擦掉等于丢用户的配网信息。
        ESP_LOGE(TAG, "NVS 需要重新格式化,请手动处理:%s", esp_err_to_name(err));
        return err;
    }
    if (err != ESP_OK) return err;

    err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    // 不用 esp_netif_create_default_wifi_sta():它在分配失败时会 abort,
    // 而这里希望把错误报给界面。
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_WIFI_STA();
    esp_netif_t *sta = esp_netif_new(&netif_cfg);
    if (sta == NULL) return ESP_ERR_NO_MEM;
    err = esp_netif_attach_wifi_station(sta);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_default_wifi_sta_handlers();
    if (err != ESP_OK) return err;

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) return err;
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event,
                                              NULL, NULL);
    if (err != ESP_OK) return err;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event,
                                              NULL, NULL);
    if (err != ESP_OK) return err;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;
    // 看板每秒最多刷一帧,射频不必全速;省一点电。
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    return esp_wifi_start();
}

// 幂等:谁先来谁初始化,后来的直接返回。失败不缓存,下次调用会重试。
static bool wifi_ensure_started(void) {
    if (s_wifi_started) return true;
    if (s_wifi_lock == NULL) return false;

    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    if (!s_wifi_started) {
        const esp_err_t err = wifi_hw_start();
        if (err == ESP_OK) {
            s_wifi_started = true;
        } else {
            ESP_LOGE(TAG, "Wi-Fi 启动失败:%s", esp_err_to_name(err));
        }
    }
    const bool ok = s_wifi_started;
    xSemaphoreGive(s_wifi_lock);
    return ok;
}

// 写入凭证并(重新)连接。调用前必须已经 wifi_ensure_started()。
static void wifi_apply_credentials(const app_config_t *cfg) {
    wifi_config_t wifi_cfg = { 0 };
    copy_field(wifi_cfg.sta.ssid, sizeof(wifi_cfg.sta.ssid), cfg->ssid);
    copy_field(wifi_cfg.sta.password, sizeof(wifi_cfg.sta.password), cfg->pass);
    // 开放网络(空密码)必须显式声明,否则 ESP-IDF 会当成 WPA2 去握手。
    wifi_cfg.sta.threshold.authmode = (cfg->pass[0] == '\0') ? WIFI_AUTH_OPEN
                                                             : WIFI_AUTH_WPA2_PSK;
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;

    (void)esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    copy_text(s_applied_ssid, sizeof(s_applied_ssid), cfg->ssid);
    copy_text(s_applied_pass, sizeof(s_applied_pass), cfg->pass);

    const bool had = cfg->ssid[0] != '\0';
    s_has_credentials = had;
    if (!had) return;

    s_sta_connected = false;
    // 没连上时 disconnect 会返回 ESP_ERR_WIFI_NOT_CONNECT,无所谓。
    (void)esp_wifi_disconnect();
    (void)esp_wifi_connect();
}

// ---------------------------------------------------------------- 校时

static bool time_is_valid(void) {
    // 证书校验要求真实时间;2023-11-14 之前的读数一律当作没校时。
    return time(NULL) > 1700000000;
}

static bool ensure_time_synced(void) {
    if (time_is_valid()) return true;

    if (!s_sntp_started) {
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "ntp.aliyun.com");
        esp_sntp_setservername(1, "cn.pool.ntp.org");
        esp_sntp_setservername(2, "pool.ntp.org");
        esp_sntp_init();
        s_sntp_started = true;
    }

    set_state(APP_NET_SYNCING);
    for (int waited = 0; waited < APP_TIME_WAIT_MS; waited += 500) {
        if (time_is_valid()) {
            ESP_LOGI(TAG, "时间已同步");
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return false;
}

// ---------------------------------------------------------------- 拉取

// 流式拉取一份比赛列表:边收边解析,不把响应攒在内存里。
// url 可以是"今天"的列表接口,也可以是某一天的赛程接口;target_day 交给
// 解析器决定留哪一天。append=true 时不清空 out,只往里面补完场——补凌晨段
// 用这个模式,两份响应的重叠部分靠 upsert 按 id 去重。
// 成功返回 ESP_OK;失败时调用方保留上一次的数据。
static esp_err_t fetch_stream(const char *url, app_match_list_t *out, int target_day,
                              int64_t now_utc, const char *what, bool append) {
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .user_agent = APP_FEED_UA,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) return ESP_ERR_NO_MEM;

    // 详情任务可能正在握手:排队,别让两条 TLS 同时占堆。
    if (s_http_lock != NULL) xSemaphoreTake(s_http_lock, portMAX_DELAY);

    // 站点会校验 Referer,不带就返回 567 + 一段 HTML 错误页。
    esp_http_client_set_header(client, "Referer", APP_FEED_REFERER);
    esp_http_client_set_header(client, "Accept", "application/json");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "打开连接失败:%s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        if (s_http_lock != NULL) xSemaphoreGive(s_http_lock);
        return err;
    }

    // 顺序不能反:open() 只负责建连 + 把请求发出去,响应是一个字节都没读的。
    // 状态码是 fetch_headers() 解析响应头时才填进 response 结构体的,在那之前
    // get_status_code() 返回结构体初始值 0。先读状态码再 fetch,等于拿 0 跟 200
    // 比,每一条响应都会被判成失败——日志里就是那句 "HTTP 0"。
    (void)esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "%s HTTP %d", what, status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (s_http_lock != NULL) xSemaphoreGive(s_http_lock);
        return ESP_FAIL;
    }

    app_feed_t feed;
    if (append) {
        app_feed_init_append(&feed, out, now_utc);
    } else {
        app_feed_init(&feed, out, now_utc);
        app_feed_set_day(&feed, target_day);
    }
    feed.target_day = target_day;

    size_t total = 0;
    for (;;) {
        const int n = esp_http_client_read(client, s_read_buf, APP_READ_CHUNK);
        if (n < 0) {
            ESP_LOGW(TAG, "%s 读响应失败", what);
            err = ESP_FAIL;
            break;
        }
        if (n == 0) break;
        app_feed_write(&feed, s_read_buf, (size_t)n);
        total += (size_t)n;
    }
    app_feed_finish(&feed);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (s_http_lock != NULL) xSemaphoreGive(s_http_lock);

    if (err != ESP_OK) return err;

    app_match_list_sort(out);
    // 把过滤计数一起打出来:如果"联赛不符"等于全部对象数,说明接口换了联赛名,
    // 一眼就能看出是白名单要改,而不是网络坏了。
    ESP_LOGI(TAG, "%s:收到 %u KB,解析 %u 场,保留 %u 场(直播 %u / 未开始 %u / 完场 %u)",
             what, (unsigned)(total / 1024), (unsigned)feed.objects_seen, (unsigned)out->count,
             (unsigned)feed.live_kept, (unsigned)feed.upcoming_kept,
             (unsigned)feed.finished_kept);
    ESP_LOGI(TAG, "%s 过滤掉:联赛不符 %u 场,非目标日 %u 场", what,
             (unsigned)feed.dropped_league, (unsigned)feed.dropped_day);
    return ESP_OK;
}

// 今天的列表。目标日固定是 0。
static esp_err_t fetch_matches(int64_t now_utc) {
    return fetch_stream(APP_FEED_URL, &s_parsed, 0, now_utc, "比分列表", false);
}

// 拼 schedule_list 的 URL。day 是相对"今天"的天数(-1 = 昨天,0 = 今天,
// 1..7 = 未来第 N 天),内部换算成"北京时间那天 00:00"对应的 UTC 时刻。
static void build_schedule_url(char *url, size_t cap, int64_t now_utc, int day,
                               const char *tab) {
    char start[32];
    app_schedule_start_text(app_cst_day_start(now_utc, day), start, sizeof(start));

    // start 里只有一个空格(日期与时间之间),换成 %20 最省事,不必上完整的
    // URL 编码器。
    char encoded[40];
    size_t n = 0;
    for (const char *p = start; *p != '\0' && n + 4 < sizeof(encoded); p++) {
        if (*p == ' ') {
            encoded[n++] = '%';
            encoded[n++] = '2';
            encoded[n++] = '0';
        } else {
            encoded[n++] = *p;
        }
    }
    encoded[n] = '\0';

    snprintf(url, cap, APP_FEED_SCHEDULE_URL, tab, encoded);
}

// 未来第 day 天的赛程。接口要 UTC 的 "YYYY-MM-DD HH:00:00",空格必须编码。
static esp_err_t fetch_day(int64_t now_utc, int day) {
    char url[192];
    build_schedule_url(url, sizeof(url), now_utc, day, "fixture");
    return fetch_stream(url, &s_parsed, day, now_utc, "未来赛程", false);
}

// 今天的"凌晨段":北京 00:00 到 match_list 窗口起点(实测 07:30)之间那段。
//
// match_list 按 UTC 日切窗口,凌晨那批欧冠/欧联/西甲它压根不返回;赛程接口
// 按 start 起 24 小时给,start 用"今天 00:00 CST"就能拿到完整的一天。
// 两份响应的重叠区在白名单里基本是空的(白名单赛事最晚 04:00 开球、06:00
// 前结束),真重叠了也由 upsert 按 id 去重。
//
// 用 append 模式:match_list 那份已经把直播和未开始都拿到了,这里只补完场。
static esp_err_t fetch_today_extra(int64_t now_utc) {
    char url[192];
    build_schedule_url(url, sizeof(url), now_utc, 0, "result");
    return fetch_stream(url, &s_parsed, 0, now_utc, "今天凌晨段", true);
}

// 回看前一天:整份都是已完场,一份 result 就够(实测 200~450 KB)。
static esp_err_t fetch_prev_day(int64_t now_utc, int day) {
    char url[192];
    build_schedule_url(url, sizeof(url), now_utc, day, "result");
    return fetch_stream(url, &s_parsed, day, now_utc, "前日赛程", false);
}

// 翻到未来某一天。整份都是未开始,一份 fixture 就够。
static esp_err_t fetch_future_day(int64_t now_utc, int day) {
    char url[192];
    build_schedule_url(url, sizeof(url), now_utc, day, "fixture");
    return fetch_stream(url, &s_parsed, day, now_utc, "未来赛程", false);
}

// 拉取期间用户可能已经翻走了。这时手里这份属于旧视图,发布出去会把界面
// 从"正在拉取"打回旧日期的列表(真机上能明显看到按一下键又跳回去),而且
// 下一次纠正要等满一个刷新间隔。所以丢掉它,并叫醒自己立刻按新视图重拉。
static bool view_changed_during_fetch(int8_t view) {
    if (s_view_day == view) return false;
    ESP_LOGI(TAG, "拉取期间视图已切到第 %d 天,丢弃本次结果", (int)s_view_day);
    if (s_task != NULL) xTaskNotifyGive(s_task);
    return true;
}

// 今天要不要补凌晨段。见 APP_EXTRA_SETTLE_HOUR。
static bool need_today_extra(int64_t now_utc) {    if (!s_today_extra_valid) return true;

    // 跨天了:新的一天有新的凌晨场,要重新补。
    if (s_today_extra_key != app_cst_day_start(now_utc, 0)) return true;

    app_datetime_t now_dt;
    if (!app_datetime_from_utc(now_utc, &now_dt)) return true;
    // 07:00 之前凌晨场可能还在踢,每次都补。
    if (now_dt.hour < APP_EXTRA_SETTLE_HOUR) return true;

    // 已经过了定格时间。这里要判"定格之后补过一次没有",而不是"补过一次
    // 没有":设备从凌晨一直开着的话,06:00 那次补拉拿到的只是当时已完场的,
    // 05:30 开球那几场还没结束;等到 08:00 match_list 的窗口翻篇、凌晨段
    // 彻底消失,就必须再补一次,否则那几场就漏了。
    app_datetime_t at_dt;
    if (!app_datetime_from_utc(s_today_extra_at, &at_dt)) return true;
    return at_dt.hour < APP_EXTRA_SETTLE_HOUR;
}

// 今天没有白名单赛事时,按天往前找最近的一个比赛日。
// 一天一份响应(177 KB ~ 1.3 MB),找到就停;七天都没有就认了。
// 返回:>0 = 未来第 N 天,0 = 探过了确实没有,-1 = 请求全失败(网络问题)。
static int probe_next_day(int64_t now_utc) {
    bool any_ok = false;
    int fails = 0;

    for (int day = 1; day <= APP_NEXT_DAY_MAX; day++) {
        if (fetch_day(now_utc, day) != ESP_OK) {
            // 单次失败可能只是抖动,连着两次就是网络断了。再试下去每次要等
            // 20 秒超时,七次能把界面卡在"正在查找"上两分多钟,不如早点认输。
            if (++fails >= 2) {
                ESP_LOGW(TAG, "连续 %d 天拉取失败,放弃探测", fails);
                break;
            }
            continue;
        }
        fails = 0;
        any_ok = true;   // 至少有一份响应是好的,才能断言"没有赛事"

        if (s_parsed.count > 0) {
            ESP_LOGI(TAG, "下一个比赛日:%d 天后,共 %u 场", day,
                     (unsigned)s_parsed.count);
            return day;
        }
        ESP_LOGI(TAG, "%d 天后没有白名单赛事,继续往后找", day);
    }

    if (!any_ok) {
        ESP_LOGW(TAG, "未来 %d 天的赛程都没拉下来", APP_NEXT_DAY_MAX);
        return -1;
    }
    ESP_LOGI(TAG, "未来 %d 天都没有白名单赛事", APP_NEXT_DAY_MAX);
    return 0;
}

// 把一份结果发布成快照。day_offset:0 = 今天,1..7 = 未来第 N 天,
// -1 = 已经找过但未来七天都没有赛事。
//
// view_day 显式传进来,不在这里读 s_view_day:拉取期间用户可能已经翻走了,
// 读全局变量会把"旧数据 + 新日期"一起发布出去。
static void publish(const app_match_list_t *matches, int8_t day_offset, int8_t view_day,
                    const app_config_t *cfg) {
    wifi_ap_record_t ap = { 0 };
    const int8_t rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;

    size_t count = 0;
    lock();
    if (matches != NULL) s_snapshot.matches = *matches;
    else app_match_list_reset(&s_snapshot.matches);
    s_snapshot.day_offset = day_offset;
    s_snapshot.view_day = view_day;
    s_published_view = view_day;
    s_snapshot.probing_next_day = false;
    s_snapshot.updated_at = (int64_t)time(NULL);
    s_snapshot.rssi = rssi;
    s_snapshot.revision++;
    s_snapshot.state = APP_NET_READY;
    s_snapshot.error[0] = '\0';
    snprintf(s_snapshot.ssid, sizeof(s_snapshot.ssid), "%s", cfg->ssid);
    count = s_snapshot.matches.count;
    unlock();

    if (day_offset > 0) {
        ESP_LOGI(TAG, "刷新完成:%u 场比赛(%d 天后)", (unsigned)count, day_offset);
    } else if (day_offset < 0) {
        ESP_LOGI(TAG, "刷新完成:未来 %d 天没有白名单赛事", APP_NEXT_DAY_MAX);
    } else {
        ESP_LOGI(TAG, "刷新完成:%u 场比赛", (unsigned)count);
    }
}

// 返回 false 表示"这次拉取失败了"(网络/TLS/上游),调用方可以重试;
// 返回 true 表示拿到了结论——包括"今天确实没有白名单赛事"这种正常结果。
static bool refresh_once(const app_config_t *cfg) {
    const int64_t now = (int64_t)time(NULL);
    // 一开始就把视图定下来:拉取要好几秒,期间用户可能又翻了页,
    // 那样这一轮的结果就该按"翻页之后"的视图丢弃,而不是错位发布。
    const int8_t view = s_view_day;

    // ---- 用户翻到了别的日子(回看前一天 / 看未来第 N 天) ----
    // 这些日子的数据基本是定格的,一天只拉一次。已经拉过就直接返回:上一次
    // publish 的结果还在快照里,状态也还是 READY,连重画都不需要。
    //
    // 注意这个判断要在 set_state(FETCHING) 之前:放在后面的话,每 120 秒
    // 的自动刷新都会先把界面刷成"正在拉取"再改回 READY,白闪一下。
    if (view != 0) {
        // s_parsed 接下来要装别的日子,今天那份(含凌晨段)就没了。切回今天
        // 时必须重新补一次,否则凌晨那批比赛会凭空消失。
        s_today_extra_valid = false;

        const int64_t key = app_cst_day_start(now, view);
        // 缓存能直接用的前提有两条:同一天拉过,而且快照里装的**就是**这一
        // 天。少了后一条,来回翻页时会把别的日子的列表当成这一天发布出去。
        if (s_day_cache_valid && s_day_cache_key == key && s_published_view == view) {
            return true;
        }

        set_state(APP_NET_FETCHING);
        const esp_err_t err = (view < 0) ? fetch_prev_day(now, view)
                                         : fetch_future_day(now, view);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "第 %d 天拉取失败:空闲堆 %u B,最大连续块 %u B", (int)view,
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
            set_error(view < 0 ? "拉取前日赛程失败" : "拉取未来赛程失败");
            return false;
        }

        s_day_cache_valid = true;
        s_day_cache_key = key;
        // 拉取期间翻走了就别发布:这份是旧视图的。
        if (view_changed_during_fetch(view)) return true;
        // day_offset 保持 0:翻页不是"今天没球往后找到的那一天",界面靠
        // view_day 判断标题写什么,两者不能混用。
        publish(&s_parsed, 0, view, cfg);
        return true;
    }

    // ---- 今天 ----
    set_state(APP_NET_FETCHING);
    if (fetch_matches(now) != ESP_OK) {
        // 拉取失败不清空已有数据,界面继续显示上一次的结果。
        //
        // 把堆的现状一起打出来:TLS 握手要一块 16 KB 的入缓冲,如果这里
        // "最大连续块"只有几 KB,那失败是内存碎片而不是网络,两种原因的
        // 处理方式完全不同(前者要等详情任务结束,后者重试就好)。
        ESP_LOGW(TAG, "拉取失败:空闲堆 %u B,最大连续块 %u B",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        set_error("拉取比分失败");
        return false;
    }

    // 补今天的凌晨段。失败不算致命:match_list 那份已经把直播和未开始都
    // 拿到了,少的只是凌晨已完场的那几场,不值得让整次刷新算失败。
    if (need_today_extra(now)) {
        if (fetch_today_extra(now) == ESP_OK) {
            s_today_extra_valid = true;
            s_today_extra_key = app_cst_day_start(now, 0);
            s_today_extra_at = now;
        } else {
            ESP_LOGW(TAG, "凌晨段补拉失败,本次只显示比分列表的内容");
        }
    }

    // 拉取期间翻走了就别往下走:这份是旧视图的,连"下一个比赛日"探测也
    // 不该按旧视图的结论去跑。
    if (view_changed_during_fetch(view)) return true;

    // 今天有球:正常显示,顺便把"下一个比赛日"的缓存作废——明天它就不准了。
    if (s_parsed.count > 0) {
        s_next_day = 0;
        s_next_probed = false;
        s_next_day_start = 0;
        publish(&s_parsed, 0, 0, cfg);
        return true;
    }

    // 今天没有白名单赛事:显示最近的那个比赛日。
    // 缓存还在有效期内就不重复下载——每次刷新都探一遍会把流量吃光。
    // "七天都没有"也是结论,同样受节流约束,否则每两分钟就重下一遍。
    //
    // 命中时什么都不用做:上一次 publish 已经把结果写进快照,而今天的解析结果
    // (空列表)只落在 s_parsed 里,碰不到快照。这里唯一要补的是把开头
    // set_state(FETCHING) 的状态改回 READY,否则界面会一直停在"正在拉取"。
    const bool cache_ok = s_next_probed && s_next_day_start > now
                          && (now - s_next_probe_at) < APP_NEXT_RECHECK_SECONDS;
    if (cache_ok) {
        lock();
        s_snapshot.state = APP_NET_READY;
        s_snapshot.revision++;
        unlock();
        return true;
    }

    // 探测要连下好几份响应,先把状态改成"正在拉取",界面才不会像卡住。
    set_state(APP_NET_FETCHING);
    lock();
    s_snapshot.probing_next_day = true;
    s_snapshot.revision++;
    unlock();

    s_next_probe_at = now;
    const int day = probe_next_day(now);
    if (day < 0) {
        // 一份都没拉下来:是网络问题,不是"没有赛事"。保留旧数据。
        // 返回 false 让调用方重试,理由和拉取失败一样。
        ESP_LOGW(TAG, "未来赛程探测失败:空闲堆 %u B,最大连续块 %u B",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        set_error("拉取未来赛程失败");
        return false;
    }

    s_next_day = day;
    s_next_probed = true;
    // "没找到"时把参考点放在第七天,让节流同样生效。
    s_next_day_start = app_cst_day_start(now, day > 0 ? day : APP_NEXT_DAY_MAX);

    if (day > 0) publish(&s_parsed, (int8_t)day, 0, cfg);
    else publish(NULL, -1, 0, cfg);
    return true;
}

// ---------------------------------------------------------------- 热点扫描

// 过滤掉隐藏热点(SSID 为空)与同名重复项,按信号从强到弱排序。
// 输出到 s_scan_records 之外的一块,由调用方持锁写进 s_scan。
static size_t scan_collect(app_scan_ap_t *out, size_t cap) {
    uint16_t ap_count = 0;
    if (esp_wifi_scan_get_ap_num(&ap_count) != ESP_OK || ap_count == 0) return 0;

    uint16_t want = ap_count;
    if (want > APP_SCAN_MAX) want = APP_SCAN_MAX;
    if (esp_wifi_scan_get_ap_records(&want, s_scan_records) != ESP_OK) return 0;

    size_t count = 0;
    for (uint16_t i = 0; i < want && count < cap; i++) {
        const char *ssid = (const char *)s_scan_records[i].ssid;
        if (ssid[0] == '\0') continue;

        size_t found = count;
        for (size_t j = 0; j < count; j++) {
            if (strcmp(out[j].ssid, ssid) == 0) {
                found = j;
                break;
            }
        }
        if (found < count) {
            // 同一热点有多个 BSSID:留信号最强的那条。
            if (s_scan_records[i].rssi > out[found].rssi) {
                out[found].rssi = s_scan_records[i].rssi;
            }
            continue;
        }

        app_scan_ap_t *ap = &out[count++];
        copy_text(ap->ssid, sizeof(ap->ssid), ssid);
        ap->rssi = s_scan_records[i].rssi;
        ap->channel = s_scan_records[i].primary;
        ap->secure = s_scan_records[i].authmode != WIFI_AUTH_OPEN;
    }

    // 插入排序:最多 20 条,比调 qsort 省事也省栈。
    for (size_t i = 1; i < count; i++) {
        const app_scan_ap_t key = out[i];
        size_t j = i;
        while (j > 0 && out[j - 1].rssi < key.rssi) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = key;
    }
    return count;
}

static void scan_task(void *arg) {
    (void)arg;

    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (!wifi_ensure_started()) {
            lock();
            s_scan.scanning = false;
            s_scan.done = true;
            s_scan.failed = true;   // 界面要能说出"为什么没有网络"
            s_scan.count = 0;
            s_scan.revision++;
            unlock();
            ESP_LOGW(TAG, "扫描跳过:Wi-Fi 没起来");
            continue;
        }

        lock();
        s_scan.scanning = true;
        s_scan.revision++;      // 界面立刻切成"正在扫描"
        unlock();

        // 阻塞式扫描:本任务就是为它存在的,占 3~5 秒没关系。
        wifi_scan_config_t scan_cfg = { 0 };
        scan_cfg.show_hidden = false;
        esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "扫描失败:%s", esp_err_to_name(err));
        }

        // 扫描缓冲区在静态区,这里先算完再一次性写进快照,避免持锁时间长。
        app_scan_ap_t fresh[APP_SCAN_MAX];
        const size_t count = (err == ESP_OK) ? scan_collect(fresh, APP_SCAN_MAX) : 0;

        lock();
        s_scan.count = count;
        if (count > 0) memcpy(s_scan.items, fresh, sizeof(fresh[0]) * count);
        s_scan.scanning = false;
        s_scan.done = true;
        s_scan.failed = (err != ESP_OK);
        s_scan.done_us = esp_timer_get_time();
        s_scan.revision++;
        unlock();

        ESP_LOGI(TAG, "扫描完成:%u 个热点", (unsigned)count);
    }
}

// 扫描任务按需创建:开机时内存紧张没建成功的话,用户第一次进配网页还有一次
// 机会。否则配网页会永远停在"没有发现网络",看起来就像按键坏了。
static bool scan_task_spawn(void) {
    if (s_scan_task != NULL) return true;
    if (s_wifi_lock == NULL) return false;   // 联网层还没初始化

    if (xTaskCreate(scan_task, "app_scan", 4096, NULL, 3, &s_scan_task) != pdPASS) {
        s_scan_task = NULL;
        ESP_LOGW(TAG, "扫描任务创建失败,配网页无法扫描");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- 单场详情

// 赛况接口。Referer 必须带,否则站点直接回 403。
#define APP_DETAIL_URL "https://api.dongdianqiu.com/data/overview/match/"
#define APP_DETAIL_REFERER "https://topic.dongdianqiu.com/"
// 正文不落内存:解析器按需从连接里读(见 app_detail_parse_stream)。实测详情
// 响应 6~37 KB,而运行时空闲堆只有 45 KB、还要留 20 KB 给 TLS,整段缓冲在
// C3 上不可能成立。

// 解析器的取数回调:直接从 TLS 连接里读,读多少解析多少。
static int detail_read(void *ctx, char *buf, size_t cap) {
    return esp_http_client_read((esp_http_client_handle_t)ctx, buf, (int)cap);
}

static void detail_publish(uint8_t state, const app_detail_t *detail, const char *error) {
    lock();
    s_detail.state = state;
    if (detail != NULL) s_detail.detail = *detail;
    if (error != NULL) copy_text(s_detail.error, sizeof(s_detail.error), error);
    else s_detail.error[0] = '\0';
    s_detail.revision++;
    unlock();
}

// 拉一场赛况并解析。返回 true 表示快照已经更新成 READY。
static bool detail_fetch(const char *match_id) {
    if (!s_sta_connected) {
        detail_publish(APP_DETAIL_FAILED, NULL, "未连接网络");
        return false;
    }
    if (!ensure_time_synced()) {
        detail_publish(APP_DETAIL_FAILED, NULL, "时间未同步");
        return false;
    }

    char url[128];
    snprintf(url, sizeof(url), "%s%s", APP_DETAIL_URL, match_id);

    // 同一时刻只允许一个联网请求,免得两条 TLS 一起抢堆。
    if (s_http_lock != NULL) xSemaphoreTake(s_http_lock, portMAX_DELAY);

    bool ok = false;
    const char *why = "拉取失败";
    app_detail_t parsed;
    app_detail_reset(&parsed);

    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .user_agent = APP_FEED_UA,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        why = "初始化失败";
    } else {
        esp_http_client_set_header(client, "Referer", APP_DETAIL_REFERER);
        esp_http_client_set_header(client, "Accept", "application/json");

        if (esp_http_client_open(client, 0) != ESP_OK) {
            why = "打开连接失败";
        } else {
            (void)esp_http_client_fetch_headers(client);
            const int status = esp_http_client_get_status_code(client);
            if (status != 200) {
                ESP_LOGW(TAG, "详情 HTTP %d", status);
                why = "接口返回错误";
            } else if (app_detail_parse_stream(detail_read, client, &parsed)) {
                ok = true;
                ESP_LOGI(TAG, "详情 %s:进球 %u,统计 %u,阵型[%s]/[%s]",
                         match_id, (unsigned)parsed.goal_count, (unsigned)parsed.stat_count,
                         parsed.formation_home, parsed.formation_away);
            } else {
                why = "解析失败";
                ESP_LOGW(TAG, "详情解析失败(%s)", match_id);
            }
            esp_http_client_close(client);
        }
        esp_http_client_cleanup(client);
    }

    if (s_http_lock != NULL) xSemaphoreGive(s_http_lock);

    detail_publish(ok ? APP_DETAIL_READY : APP_DETAIL_FAILED, ok ? &parsed : NULL,
                   ok ? NULL : why);
    return ok;
}

static void detail_task(void *arg) {
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_detail_pending) continue;
        s_detail_pending = false;

        char id[APP_MATCH_ID_MAX + 1];
        lock();
        copy_text(id, sizeof(id), s_detail_req_id);
        unlock();
        if (id[0] == '\0') continue;

        (void)detail_fetch(id);
    }
}

// 详情任务同样按需创建:内存紧张时开机建不出来,第一次点开比赛还有机会。
static bool detail_task_spawn(void) {
    if (s_detail_task != NULL) return true;
    if (s_http_lock == NULL) return false;

    // 8 KB:解析器的 1 KB 窗口在栈上,而且这条任务同时要跑 mbedtls 的解密。
    if (xTaskCreate(detail_task, "app_detail", 8192, NULL, 3, &s_detail_task) != pdPASS) {
        s_detail_task = NULL;
        ESP_LOGW(TAG, "详情任务创建失败,点开比赛将无法加载");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- 工作任务

static bool wait_for_link(uint32_t timeout_ms) {
    for (uint32_t waited = 0; waited < timeout_ms; waited += 250) {
        if (s_sta_connected) return true;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    return s_sta_connected;
}

static uint32_t backoff(uint32_t ms) {
    return (ms * 2 > APP_RETRY_MAX_MS) ? APP_RETRY_MAX_MS : ms * 2;
}

static void net_task(void *arg) {
    (void)arg;
    uint32_t retry_ms = APP_RETRY_MIN_MS;

    for (;;) {
        app_config_t cfg;
        lock();
        cfg = s_config;
        unlock();

        if (!app_config_has_credentials(&cfg)) {
            set_state(APP_NET_OFF);
            // 没配网时不做任何射频动作,等配置变化(任务通知)再醒。
            // 注意射频可能已经被"配网页扫描"拉起来了,但这里不连接、不拉数据。
            s_has_credentials = false;
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
            continue;
        }

        if (!wifi_ensure_started()) {
            set_state(APP_NET_CONNECTING);
            set_error("Wi-Fi 启动失败");
            vTaskDelay(pdMS_TO_TICKS(retry_ms));
            retry_ms = backoff(retry_ms);
            continue;
        }

        if (strcmp(s_applied_ssid, cfg.ssid) != 0 || strcmp(s_applied_pass, cfg.pass) != 0) {
            // 换了热点或改了密码:更新配置后重连,不清空已有数据。
            wifi_apply_credentials(&cfg);
        }

        if (!s_sta_connected) set_state(APP_NET_CONNECTING);
        if (!wait_for_link(APP_WIFI_WAIT_MS)) {
            set_error("连接 Wi-Fi 超时");
            (void)esp_wifi_connect();
            vTaskDelay(pdMS_TO_TICKS(retry_ms));
            retry_ms = backoff(retry_ms);
            continue;
        }

        // 连上了:把这次的凭证记进"连过的网络",下次进配网页可以直接选。
        if (s_remember_pending) {
            s_remember_pending = false;
            if (s_connected_hook != NULL) s_connected_hook(cfg.ssid, cfg.pass);
        }

        if (!ensure_time_synced()) {
            set_error("时间同步失败,无法校验证书");
            vTaskDelay(pdMS_TO_TICKS(retry_ms));
            retry_ms = backoff(retry_ms);
            continue;
        }

        if (!refresh_once(&cfg)) {
            // 一次失败多半是瞬时的:TLS 握手超时、AP 侧丢包、上游偶发 5xx。
            // 隔 1.2 秒立刻重试一次;还不行就把下一次等待压到 30 秒。
            //
            // 原来的写法是失败后直接睡满整个刷新间隔(60~300 秒),于是一次
            // 抖动就让看板停更好几分钟——这正是"偶尔 TLS 会失败"最难受的
            // 地方:错的不是那一次握手,而是失败之后没有第二次机会。
            vTaskDelay(pdMS_TO_TICKS(APP_RETRY_ONCE_DELAY_MS));
            if (!refresh_once(&cfg)) {
                ESP_LOGW(TAG, "%d 秒后重试", APP_RETRY_SHORT_WAIT_MS / 1000);
                (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(APP_RETRY_SHORT_WAIT_MS));
                continue;
            }
        }
        retry_ms = APP_RETRY_MIN_MS;

        // 睡到下一个刷新点,期间有手动刷新就提前醒。
        const uint32_t wait_ms = (uint32_t)app_config_clamp_refresh(cfg.refresh_seconds) * 1000u;
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }
}

// ---------------------------------------------------------------- 对外接口

void app_net_start(const app_config_t *cfg) {
    if (s_started) {
        app_net_apply_config(cfg);
        return;
    }

    s_lock = xSemaphoreCreateMutex();
    s_wifi_lock = xSemaphoreCreateMutex();
    s_http_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL || s_wifi_lock == NULL || s_http_lock == NULL) {
        ESP_LOGE(TAG, "创建互斥量失败,联网不可用");
        return;
    }

    s_read_buf = malloc(APP_READ_CHUNK);
    if (s_read_buf == NULL) {
        ESP_LOGE(TAG, "读缓冲分配失败(%d 字节),联网不可用", APP_READ_CHUNK);
        return;
    }

    app_config_defaults(&s_config);
    if (cfg != NULL) s_config = *cfg;
    s_snapshot.state = app_config_has_credentials(&s_config) ? APP_NET_CONNECTING : APP_NET_OFF;

    // 8 KB:留给 TLS 握手(mbedtls 在同一个任务里跑,证书包校验很吃栈)。
    // 解析用的暂存区在静态区,不占栈。
    if (xTaskCreate(net_task, "app_net", 8192, NULL, 4, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "联网任务创建失败");
        free(s_read_buf);
        s_read_buf = NULL;
        return;
    }
    // 扫描任务只在用户进配网页时被唤醒,优先级低于联网任务。
    (void)scan_task_spawn();
    // 详情任务同理:第一次点开比赛时还会再试一次创建。
    (void)detail_task_spawn();

    // 还没配过网就先把热点扫出来放着:这种设备开机后第一件事就是配网,
    // 提前扫好能让"设置 → Wi-Fi 配网"一进去就有列表,不用干等 3~5 秒。
    // 已经配过网的设备不预扫——它开机就在连路由器,射频本来就要开,但没必
    // 要多耗一次扫描。
    if (!app_config_has_credentials(&s_config)) {
        app_net_scan_start();
    }

    s_started = true;
}

void app_net_apply_config(const app_config_t *cfg) {
    if (cfg == NULL || s_lock == NULL) return;
    lock();
    s_config = *cfg;
    unlock();
    if (s_task != NULL) xTaskNotifyGive(s_task);
}

void app_net_snapshot_copy(app_snapshot_t *out) {
    if (out == NULL) return;
    lock();
    *out = s_snapshot;
    unlock();
}

uint32_t app_net_revision(void) {
    // 32 位对齐读取本身就是原子的;这里只在界面侧做"要不要重画"的判断,
    // 真正的一致性由随后的 app_net_snapshot_copy() 保证。
    return s_snapshot.revision;
}

void app_net_request_refresh(void) {
    if (s_task != NULL) xTaskNotifyGive(s_task);
}

void app_net_set_view_day(int8_t day) {
    if (s_lock == NULL) return;
    if (day < APP_VIEW_DAY_MIN) day = APP_VIEW_DAY_MIN;
    if (day > APP_VIEW_DAY_MAX) day = APP_VIEW_DAY_MAX;
    const int8_t want = day;

    lock();
    if (s_view_day == want) {
        unlock();
        return;
    }
    s_view_day = want;
    // 快照里的 view_day 立刻改掉,不等拉取完成:界面要马上把页头换成
    // "回看 MM-DD"并显示"正在拉取",否则按一下键屏幕毫无反应。
    s_snapshot.view_day = want;
    // 同时把状态压成"正在拉取",并清掉旧列表:那份是另一天的,留着会和新
    // 页头对不上(标题写"回看 10-21"、底下挂的却是今天的比赛)。
    s_snapshot.state = APP_NET_FETCHING;
    s_snapshot.day_offset = 0;
    app_match_list_reset(&s_snapshot.matches);
    s_snapshot.revision++;
    unlock();

    if (s_task != NULL) xTaskNotifyGive(s_task);
}

int8_t app_net_view_day(void) {
    // int8_t 的对齐读取本身是原子的,这里只是给串口命令看个状态。
    return s_view_day;
}

bool app_net_wifi_ready(void) {
    return wifi_ensure_started();
}

bool app_net_is_connected(void) {
    if (s_lock == NULL) return false;
    lock();
    const bool ok = s_snapshot.wifi_connected;
    unlock();
    return ok;
}

void app_net_forget_credentials(void) {
    if (s_lock == NULL) return;
    lock();
    memset(s_config.ssid, 0, sizeof(s_config.ssid));
    memset(s_config.pass, 0, sizeof(s_config.pass));
    unlock();
    if (s_task != NULL) xTaskNotifyGive(s_task);
}

// ---------------------------------------------------------------- 详情接口

bool app_net_detail_request(const char *match_id) {
    if (match_id == NULL || match_id[0] == '\0' || s_lock == NULL) return false;
    if (!detail_task_spawn()) return false;

    lock();
    copy_text(s_detail_req_id, sizeof(s_detail_req_id), match_id);
    copy_text(s_detail.match_id, sizeof(s_detail.match_id), match_id);
    s_detail.state = APP_DETAIL_LOADING;
    s_detail.error[0] = '\0';
    s_detail.revision++;   // 界面立刻切成"加载中",不用等第一次读网络
    unlock();

    s_detail_pending = true;
    xTaskNotifyGive(s_detail_task);
    return true;
}

void app_net_detail_copy(app_detail_snapshot_t *out) {
    if (out == NULL) return;
    lock();
    *out = s_detail;
    unlock();
}

uint32_t app_net_detail_revision(void) {
    return s_detail.revision;
}

void app_net_scan_start(void) {
    if (!scan_task_spawn()) return;
    // 扫描任务可能正忙:通知会累积,扫完立刻再扫一次,正是用户连按的期望。
    xTaskNotifyGive(s_scan_task);
}

void app_net_scan_copy(app_scan_result_t *out) {
    if (out == NULL) return;
    lock();
    *out = s_scan;
    unlock();
}

uint32_t app_net_scan_revision(void) {
    return s_scan.revision;
}

void app_net_set_connected_hook(app_net_connected_cb_t cb) {
    s_connected_hook = cb;
}
