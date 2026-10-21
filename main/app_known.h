// main/app_known.h
// "连接过的网络"历史。纯逻辑,持久化在 app_store 里。
//
// 需求来自实机使用:每次换热点都要重新输一遍密码不现实。所以把成功连上过的
// SSID + 密码记下来(最多 5 条,最近连的排最前),下次开机自动连第一条还在
// 覆盖范围内的;进配网页时这些网络直接列在最上面,选中即连,不用再输密码。
//
// 密码是明文存在 NVS 里的 —— 这块板没有 NVS 加密,和 app_config 里的
// ssid/pass 一样,请把设备当成"存着几个 Wi-Fi 密码"的物品。
#pragma once

#include "app_config.h"
#include "app_net.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_KNOWN_MAX 5

typedef struct {
    char ssid[APP_SSID_MAX + 1];
    char pass[APP_PASS_MAX + 1];
} app_known_net_t;

typedef struct {
    app_known_net_t items[APP_KNOWN_MAX];
    size_t count;
} app_known_list_t;

// 配网页一行。已保存的网络排在扫描结果前面。
#define APP_AP_ROW_MAX (APP_KNOWN_MAX + APP_SCAN_MAX)
typedef struct {
    char ssid[APP_SSID_MAX + 1];
    bool saved;    // 在历史里,选中后直接连
    bool secure;   // 扫描结果里是否加密;历史条目一律视为加密(空密码即开放网络)
    int8_t rssi;
} app_ap_row_t;

void app_known_reset(app_known_list_t *list);

// 找到返回下标,没有返回 -1。
int app_known_find(const app_known_list_t *list, const char *ssid);

// 记一条网络:已存在则更新密码并提到最前面;满了淘汰最旧的一条。
// 返回写入的下标(永远是 0),参数非法返回 -1。
int app_known_upsert(app_known_list_t *list, const char *ssid, const char *pass);

bool app_known_remove(app_known_list_t *list, const char *ssid);

// 取密码;不存在返回 NULL。密码可能为空字符串(开放网络)。
const char *app_known_password(const app_known_list_t *list, const char *ssid);

// 把"已保存"与"扫描到"合成配网页要显示的列表:
// 已保存的按历史顺序在前,其余按信号强度从强到弱,SSID 重复只留一条。
// 返回写入的行数(不超过 cap)。
size_t app_known_merge_rows(const app_known_list_t *known,
                            const app_scan_ap_t *scan, size_t scan_count,
                            app_ap_row_t *out, size_t cap);
