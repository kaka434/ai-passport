#include "app_crest.h"

#include "app_net.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "app_crest";

// 原 URL 最多 94 字符(app_detail.c 的 APP_LOGO_LEN 减掉截断回退的那一个),
// 再加 "?imageView2/2/w/32" 的 18 字符。128 留了余量。
#define APP_CREST_URL_LEN 128

// 缩略图参数的由来说清楚:七牛的 imageView2 用斜杠分隔,w_32 那种下划线写法
// 不认。实测 w/32 出来 1~3.5 KB,120 个样本里 p99 是 3295、最大 3451,
// 所以 4096 有 19% 余量。真超出去了会被这里截断,解码时 IDAT 不完整 → 失败
// → 画占位块,不会越界。
#define APP_CREST_THUMB_SUFFIX "?imageView2/2/w/32"
#define APP_CREST_PNG_MAX 4096

// 队徽放在比分面板上,Referer 随便带一个站内的就认(实测不带也能下,
// 但带上更稳,和详情接口一个道理)。
#define APP_CREST_REFERER "https://www.dongqiudi.com/match"

typedef enum {
    CREST_EMPTY = 0,   // 没被请求过,或者被作废了
    CREST_WANTED,      // 在排队等下载
    CREST_READY,       // 解好了,img 可用
    CREST_FAILED,      // 下载或解码失败
} crest_state_t;

typedef struct {
    crest_state_t state;
    char url[APP_CREST_URL_LEN];
    app_png_image_t img;
} crest_slot_t;

static crest_slot_t s_slots[APP_CREST_SLOTS];
static uint32_t s_revision;

// 界面任务改状态、详情任务读状态,两边都会碰。C3 是单核,一个临界区够用;
// 用 portMUX 而不是互斥量是为了省掉一次初始化调用。
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void crest_wake_worker(void) {
    // 详情任务可能还没建(内存紧张时开机建不出来)。建失败的话队徽就永远
    // 不显示,但比分和赛况不受影响——这是可接受的降级。
    app_net_wake_detail();
}

void app_crest_request(int slot, const char *logo_url) {
    if (slot < 0 || slot >= APP_CREST_SLOTS) return;
    crest_slot_t *s = &s_slots[slot];

    if (logo_url == NULL || logo_url[0] == '\0') {
        taskENTER_CRITICAL(&s_lock);
        const bool was = (s->state != CREST_EMPTY);
        s->state = CREST_EMPTY;
        s->url[0] = '\0';
        if (was) s_revision++;
        taskEXIT_CRITICAL(&s_lock);
        return;
    }

    taskENTER_CRITICAL(&s_lock);
    bool restart = false;
    // 同一个 URL 已经有结果或者正在下:什么都不做。FAILED 是例外——再请求
    // 一次就重试,一次网络抖动不该让这场球整场没队徽。
    if (s->state == CREST_EMPTY || s->state == CREST_FAILED
        || strcmp(s->url, logo_url) != 0) {
        restart = true;
    }
    if (restart) {
        snprintf(s->url, sizeof(s->url), "%s", logo_url);
        s->state = CREST_WANTED;
        s_revision++;
    }
    taskEXIT_CRITICAL(&s_lock);

    if (restart) crest_wake_worker();
}

const app_png_image_t *app_crest_image(int slot) {
    if (slot < 0 || slot >= APP_CREST_SLOTS) return NULL;
    const crest_slot_t *s = &s_slots[slot];
    return (s->state == CREST_READY) ? &s->img : NULL;
}

uint32_t app_crest_revision(void) {
    taskENTER_CRITICAL(&s_lock);
    const uint32_t r = s_revision;
    taskEXIT_CRITICAL(&s_lock);
    return r;
}

bool app_crest_busy(void) {
    bool busy = false;
    taskENTER_CRITICAL(&s_lock);
    for (int i = 0; i < APP_CREST_SLOTS; i++) {
        if (s_slots[i].state == CREST_WANTED) busy = true;
    }
    taskEXIT_CRITICAL(&s_lock);
    return busy;
}

bool app_crest_pump(void) {
    // 先把要下的 URL 抄出来,再退出临界区去跑网络——握着临界区做 TLS 握手
    // 会把界面任务堵住几百毫秒。
    char url[APP_CREST_URL_LEN];
    int slot = -1;

    taskENTER_CRITICAL(&s_lock);
    for (int i = 0; i < APP_CREST_SLOTS; i++) {
        if (s_slots[i].state == CREST_WANTED) {
            slot = i;
            memcpy(url, s_slots[i].url, sizeof(url));
            break;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    if (slot < 0) return false;

    char full[APP_CREST_URL_LEN + 32];
    // 七牛/懂球帝图片 CDN 均支持纯 HTTP，改用 HTTP 彻底免除 TLS 握手及 0x4290 证书验签内存耗尽错误
    if (strncmp(url, "https://", 8) == 0) {
        snprintf(full, sizeof(full), "http://%s%s", url + 8, APP_CREST_THUMB_SUFFIX);
    } else {
        snprintf(full, sizeof(full), "%s%s", url, APP_CREST_THUMB_SUFFIX);
    }

    // 下载缓冲和解压缓冲都临时从堆上要,用完立刻还——这一条是硬要求。
    //
    // 两块加起来 8 KB,如果钉成静态 .bss,C3 的系统堆会永久少 8 KB,而系统堆
    // 正是 TLS 握手要用的那点余量:证书验签时 mbedtls_mpi_grow() 要 256~524
    // 字节的连续块,堆见底就返回 MBEDTLS_ERR_MPI_ALLOC_FAILED,表现为
    // "PK verify failed with error 0x4290"。上一轮把 LVGL 池从 40 KB 降到
    // 32 KB 才腾出来的余量,不能在这里又花回去。
    //
    // 固定尺寸、每次成对申请释放,分配器会一直复用同一块,碎片风险可以接受;
    // 真申请不到就画占位块,比分和赛况不受影响。
    //
    // 顺序也有讲究:下载缓冲先要、解压缓冲等下载完了再要。这样队徽那次 TLS
    // 握手身上只背着 4 KB,而不是 8 KB。
    bool ok = false;
    int n = -1;
    uint8_t *png = heap_caps_malloc(APP_CREST_PNG_MAX, MALLOC_CAP_8BIT);
    if (png != NULL) {
        n = app_net_fetch_binary(full, APP_CREST_REFERER, png, APP_CREST_PNG_MAX);
        if (n > 0) {
            uint8_t *scratch = heap_caps_malloc(APP_PNG_SCRATCH_SIZE, MALLOC_CAP_8BIT);
            if (scratch != NULL) {
                ok = app_png_decode(png, (size_t)n, APP_CREST_BG_RGB565, scratch,
                                    APP_PNG_SCRATCH_SIZE, &s_slots[slot].img);
                heap_caps_free(scratch);
            }
        }
        heap_caps_free(png);
    }

    taskENTER_CRITICAL(&s_lock);
    s_slots[slot].state = ok ? CREST_READY : CREST_FAILED;
    s_revision++;
    taskEXIT_CRITICAL(&s_lock);

    if (ok) {
        ESP_LOGI(TAG, "槽 %d 队徽就绪:%u 字节", slot, (unsigned)n);
    } else {
        ESP_LOGW(TAG, "槽 %d 队徽失败(%d 字节),画占位块", slot, n);
    }
    return true;
}
