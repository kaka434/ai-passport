// main/app_known.c
#include "app_known.h"

#include <string.h>

// 与 app_net.c 里的 copy_text 同义:定长拷贝,超长截断,总是留结束符。
static void copy_text(char *dst, size_t cap, const char *src) {
    if (cap == 0) return;
    size_t n = 0;
    while (n + 1 < cap && src[n] != '\0') n++;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void app_known_reset(app_known_list_t *list) {
    if (list == NULL) return;
    memset(list, 0, sizeof(*list));
}

int app_known_find(const app_known_list_t *list, const char *ssid) {
    if (list == NULL || ssid == NULL || ssid[0] == '\0') return -1;
    for (size_t i = 0; i < list->count && i < APP_KNOWN_MAX; i++) {
        // SSID 区分大小写,和路由器保持一致。
        if (strcmp(list->items[i].ssid, ssid) == 0) return (int)i;
    }
    return -1;
}

int app_known_upsert(app_known_list_t *list, const char *ssid, const char *pass) {
    if (list == NULL || ssid == NULL || pass == NULL) return -1;
    const size_t ssid_len = strlen(ssid);
    if (ssid_len == 0 || ssid_len > APP_SSID_MAX) return -1;
    if (strlen(pass) > APP_PASS_MAX) return -1;
    if (list->count > APP_KNOWN_MAX) list->count = APP_KNOWN_MAX;

    const int existing = app_known_find(list, ssid);
    if (existing > 0) {
        // 已经在历史里但不是第一条:把它前面的整体后移一格,自己顶到最前。
        memmove(&list->items[1], &list->items[0],
                sizeof(list->items[0]) * (size_t)existing);
    } else if (existing < 0) {
        // 新网络:腾出第 0 格;已经满了就把最旧的一条(末尾)挤掉。
        size_t keep = list->count;
        if (keep >= APP_KNOWN_MAX) keep = APP_KNOWN_MAX - 1;
        if (keep > 0) {
            memmove(&list->items[1], &list->items[0], sizeof(list->items[0]) * keep);
        }
        if (list->count < APP_KNOWN_MAX) list->count++;
    }

    copy_text(list->items[0].ssid, sizeof(list->items[0].ssid), ssid);
    copy_text(list->items[0].pass, sizeof(list->items[0].pass), pass);
    return 0;
}

bool app_known_remove(app_known_list_t *list, const char *ssid) {
    if (list == NULL || ssid == NULL) return false;
    const int index = app_known_find(list, ssid);
    if (index < 0) return false;

    const size_t tail = list->count - (size_t)index - 1;
    if (tail > 0) {
        memmove(&list->items[index], &list->items[index + 1],
                sizeof(list->items[0]) * tail);
    }
    list->count--;
    memset(&list->items[list->count], 0, sizeof(list->items[0]));
    return true;
}

const char *app_known_password(const app_known_list_t *list, const char *ssid) {
    const int index = app_known_find(list, ssid);
    return index < 0 ? NULL : list->items[index].pass;
}

size_t app_known_merge_rows(const app_known_list_t *known, const app_scan_ap_t *scan,
                            size_t scan_count, app_ap_row_t *out, size_t cap) {
    if (out == NULL || cap == 0) return 0;

    size_t count = 0;

    // 先放历史:用户最可能选的就是上次连过的那个,固定在最上面。
    if (known != NULL) {
        const size_t known_count = known->count > APP_KNOWN_MAX ? APP_KNOWN_MAX : known->count;
        for (size_t i = 0; i < known_count && count < cap; i++) {
            const app_known_net_t *net = &known->items[i];
            if (net->ssid[0] == '\0') continue;
            app_ap_row_t *row = &out[count++];
            copy_text(row->ssid, sizeof(row->ssid), net->ssid);
            row->saved = true;
            row->secure = net->pass[0] != '\0';
            row->rssi = 0;   // 还没扫到,不知道信号
        }
    }

    // 再补扫描结果。扫描表已按信号排序,所以这里不需要再排。
    if (scan != NULL) {
        for (size_t i = 0; i < scan_count && count < cap; i++) {
            const app_scan_ap_t *ap = &scan[i];
            if (ap->ssid[0] == '\0') continue;

            size_t dup = count;
            for (size_t j = 0; j < count; j++) {
                if (strcmp(out[j].ssid, ap->ssid) == 0) {
                    dup = j;
                    break;
                }
            }
            if (dup < count) {
                // 历史里的网络这次也扫到了:补上信号强度与加密状态。
                out[dup].rssi = ap->rssi;
                out[dup].secure = ap->secure;
                continue;
            }

            app_ap_row_t *row = &out[count++];
            copy_text(row->ssid, sizeof(row->ssid), ap->ssid);
            row->saved = false;
            row->secure = ap->secure;
            row->rssi = ap->rssi;
        }
    }

    return count;
}
