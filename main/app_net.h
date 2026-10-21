// main/app_net.h
// 联网层:Wi-Fi STA、热点扫描、SNTP 校时、懂球帝比分拉取、快照发布。
//
// 线程模型:本模块有两个后台任务。联网任务负责连接/校时/HTTP,解析结果写进
// 受互斥量保护的一份快照;扫描任务只在用户进配网页时被唤醒,扫完就把热点列表
// 写进另一份快照。界面任务按 revision 变化去拷一份出来画。界面永远不直接碰
// 网络对象,网络任务也永远不碰 LVGL 对象。
#pragma once

#include "app_config.h"
#include "app_detail.h"
#include "app_model.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    APP_NET_OFF = 0,       // 未配网
    APP_NET_CONNECTING,    // 正在连 Wi-Fi
    APP_NET_SYNCING,       // 正在校时(TLS 证书校验需要正确时间)
    APP_NET_FETCHING,      // 正在拉数据
    APP_NET_READY,         // 数据就绪
    APP_NET_ERROR,         // 最近一次失败,会按退避重试
} app_net_state_t;

#define APP_NET_ERROR_LEN 64
#define APP_NET_IP_LEN 16

typedef struct {
    app_match_list_t matches;    // 直播中 + 接下来要打的比赛(已排好序)
    int64_t updated_at;          // 最近一次成功刷新的 UTC 秒
    uint32_t revision;           // 每次成功发布 +1,界面据此决定要不要重画
    uint8_t state;               // app_net_state_t
    // 这份列表是哪一天的:0 = 今天,1..7 = 未来第 N 天,-1 = 未来七天都没赛事。
    // 不是 0 的时候界面要在顶部显示"下一个比赛日还有几天"。
    int8_t day_offset;
    // 今天没有赛事、正在往后找下一个比赛日。探测要连下几份响应,可能几十秒,
    // 界面靠它说"正在查找",而不是让用户对着"暂无比赛"干等。
    bool probing_next_day;
    bool wifi_connected;
    int8_t rssi;
    char ssid[APP_SSID_MAX + 1];
    char ip[APP_NET_IP_LEN];
    char error[APP_NET_ERROR_LEN];
} app_snapshot_t;

// ---------------------------------------------------------------- 热点扫描
//
// 20 个上限是屏幕与内存的折中:配网页一次只显示 5 行,20 个够翻;每个条目
// 约 40 字节,整份结果不到 1 KB,可以整体拷给界面。

#define APP_SCAN_MAX 20

typedef struct {
    char ssid[APP_SSID_MAX + 1];
    int8_t rssi;
    uint8_t channel;
    bool secure;   // 扫描结果里是否加密(开放网络连的时候不用输密码)
} app_scan_ap_t;

typedef struct {
    app_scan_ap_t items[APP_SCAN_MAX];   // 按信号强度从强到弱
    size_t count;
    bool scanning;      // 扫描进行中
    bool done;          // 至少完成过一次扫描(用来区分"还没扫"和"扫完没结果")
    bool failed;        // 最近一次扫描失败(Wi-Fi 没起来或驱动报错),界面据此显示原因
    uint32_t revision;  // 开始扫、扫完各 +1
    // 扫描完成的时刻(esp_timer 微秒,不是墙钟)。开机预扫的结果可以复用,
    // 但要有保质期,否则用户换了地方进配网页会看到一份过期列表。
    int64_t done_us;
} app_scan_result_t;

// 启动联网任务。cfg 会被复制一份,调用方之后可以释放。
// 没有凭证时任务保持 APP_NET_OFF,不初始化射频,省电也省内存。
void app_net_start(const app_config_t *cfg);

// 应用新配置:刷新间隔立即生效;凭证变化会触发重新连接。
void app_net_apply_config(const app_config_t *cfg);

// 拷一份当前快照(线程安全)。out 约 3 KB,放在调用方的静态区。
void app_net_snapshot_copy(app_snapshot_t *out);

// 只读一个 32 位计数,界面先用它判断要不要拷整份快照,避免每秒都拷。
uint32_t app_net_revision(void);

// 叫醒工作任务立刻刷新,不等下一个周期。
void app_net_request_refresh(void);

// 确保射频已启动。配网门户要开热点,而"从没配过网"的设备此时 STA 还没起来。
bool app_net_wifi_ready(void);

// 当前是否已经连上热点并拿到 IP。只读一个布尔量,不用拷整份快照。
bool app_net_is_connected(void);

// 断开并清空内存中的凭证(持久化由 app_store 负责)。
void app_net_forget_credentials(void);

// 请求扫描一次附近热点。射频还没起来时扫描任务会先把 Wi-Fi 拉起来——
// 这正是"没配过网也能扫"的前提。重复调用等于重新扫一次。
// 扫描任务若在开机时没建成功(内存紧张),这里会再尝试创建一次。
void app_net_scan_start(void);

// 拷一份扫描结果(线程安全)。
void app_net_scan_copy(app_scan_result_t *out);

// 扫描结果的版本号,界面用它判断要不要重画列表。
uint32_t app_net_scan_revision(void);

// 成功连上热点(拿到 IP)时回调,参数是本次使用的 ssid / 密码。
// 回调在联网任务里执行,可以做 NVS 写;不要在里面碰 LVGL。
typedef void (*app_net_connected_cb_t)(const char *ssid, const char *pass);
void app_net_set_connected_hook(app_net_connected_cb_t cb);

// ---------------------------------------------------------------- 单场详情
//
// 详情是用户点开某一场时才拉的,单独一个任务、单独一把 HTTP 锁:它和列表
// 刷新不会同时握手,免得两条 TLS 会话把堆吃干净。响应 20~35 KB,整段收进
// 一块临时堆缓冲,解析完立刻释放。

#define APP_MATCH_ID_MAX 24

typedef enum {
    APP_DETAIL_IDLE = 0,   // 还没请求过
    APP_DETAIL_LOADING,    // 正在拉
    APP_DETAIL_READY,      // 数据可用
    APP_DETAIL_FAILED,     // 失败,error 里有原因
} app_detail_state_t;

typedef struct {
    char match_id[APP_MATCH_ID_MAX + 1];
    app_detail_t detail;
    uint8_t state;                // app_detail_state_t
    uint32_t revision;            // 每次状态变化 +1
    char error[APP_NET_ERROR_LEN];
} app_detail_snapshot_t;

// 请求拉取某一场的赛况。重复调用同一场等于重新拉一次(详情页按 OK 刷新)。
// 没联网时立刻失败,不会一直挂着"加载中"。
bool app_net_detail_request(const char *match_id);

// 拷一份详情快照(线程安全)。out 约 1 KB,放调用方的静态区。
void app_net_detail_copy(app_detail_snapshot_t *out);

// 详情版本号,界面用它判断要不要重画。
uint32_t app_net_detail_revision(void);
