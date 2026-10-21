// tests/test_app_known.c —— "连过的网络"历史与配网列表合并。
#include "app_known.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_reset_and_find(void) {
    app_known_list_t list;
    memset(&list, 0xFF, sizeof(list));
    app_known_reset(&list);
    assert(list.count == 0);
    assert(app_known_find(&list, "home") < 0);
    assert(app_known_password(&list, "home") == NULL);

    app_known_reset(NULL);   // 不炸
    assert(app_known_find(NULL, "home") < 0);
    assert(app_known_password(NULL, "home") == NULL);
}

static void test_upsert_orders_by_recency(void) {
    app_known_list_t list;
    app_known_reset(&list);

    assert(app_known_upsert(&list, "home", "11111111") == 0);
    assert(app_known_upsert(&list, "office", "22222222") == 0);
    assert(list.count == 2);
    // 最近连的排最前。
    assert(strcmp(list.items[0].ssid, "office") == 0);
    assert(strcmp(list.items[1].ssid, "home") == 0);

    // 重连旧的:提到最前,数量不变。
    assert(app_known_upsert(&list, "home", "33333333") == 0);
    assert(list.count == 2);
    assert(strcmp(list.items[0].ssid, "home") == 0);
    assert(strcmp(list.items[0].pass, "33333333") == 0);
    assert(strcmp(list.items[1].ssid, "office") == 0);
}

static void test_upsert_evicts_oldest(void) {
    app_known_list_t list;
    app_known_reset(&list);

    char ssid[16];
    for (int i = 0; i < APP_KNOWN_MAX; i++) {
        snprintf(ssid, sizeof(ssid), "net%d", i);
        assert(app_known_upsert(&list, ssid, "pass") == 0);
    }
    assert(list.count == APP_KNOWN_MAX);
    assert(strcmp(list.items[0].ssid, "net4") == 0);
    assert(strcmp(list.items[APP_KNOWN_MAX - 1].ssid, "net0") == 0);

    // 再记一个新的:最旧的一条(net0)被挤掉。
    assert(app_known_upsert(&list, "net9", "pass") == 0);
    assert(list.count == APP_KNOWN_MAX);
    assert(strcmp(list.items[0].ssid, "net9") == 0);
    assert(app_known_find(&list, "net0") < 0);
    assert(app_known_find(&list, "net1") >= 0);
}

static void test_upsert_rejects_bad_input(void) {
    app_known_list_t list;
    app_known_reset(&list);

    assert(app_known_upsert(&list, "", "pass") < 0);
    assert(app_known_upsert(&list, NULL, "pass") < 0);
    assert(app_known_upsert(&list, "home", NULL) < 0);
    assert(list.count == 0);

    // 超长 SSID 拒绝,而不是悄悄截断成一个连不上的名字。
    char long_ssid[APP_SSID_MAX + 8];
    memset(long_ssid, 'x', sizeof(long_ssid) - 1);
    long_ssid[sizeof(long_ssid) - 1] = '\0';
    assert(app_known_upsert(&list, long_ssid, "pass") < 0);
    assert(list.count == 0);
}

static void test_remove(void) {
    app_known_list_t list;
    app_known_reset(&list);
    (void)app_known_upsert(&list, "a", "1");
    (void)app_known_upsert(&list, "b", "2");
    (void)app_known_upsert(&list, "c", "3");   // 顺序:c b a

    assert(app_known_remove(&list, "b"));
    assert(list.count == 2);
    assert(strcmp(list.items[0].ssid, "c") == 0);
    assert(strcmp(list.items[1].ssid, "a") == 0);

    assert(!app_known_remove(&list, "b"));     // 已经没了
    assert(!app_known_remove(NULL, "b"));
    assert(list.count == 2);

    // 删掉最后一个:count 归零,残留数据要清掉。
    assert(app_known_remove(&list, "a"));
    assert(list.count == 1);
    assert(list.items[1].ssid[0] == '\0');
}

static void test_password_lookup(void) {
    app_known_list_t list;
    app_known_reset(&list);

    // 开放网络:历史里密码是空串,不是 NULL。
    (void)app_known_upsert(&list, "open", "");
    const char *pass = app_known_password(&list, "open");
    assert(pass != NULL);
    assert(pass[0] == '\0');
    assert(app_known_password(&list, "missing") == NULL);
}

static void test_merge_rows_saved_first_then_signal(void) {
    app_known_list_t known;
    app_known_reset(&known);
    (void)app_known_upsert(&known, "home", "11111111");   // 加密
    (void)app_known_upsert(&known, "cafe", "");           // 开放

    const app_scan_ap_t scan[] = {
        { .ssid = "neighbor", .rssi = -40, .channel = 6, .secure = true },
        { .ssid = "home", .rssi = -50, .channel = 1, .secure = true },
        { .ssid = "free", .rssi = -60, .channel = 11, .secure = false },
    };

    app_ap_row_t rows[APP_AP_ROW_MAX];
    const size_t count = app_known_merge_rows(&known, scan,
                                              sizeof(scan) / sizeof(scan[0]),
                                              rows, APP_AP_ROW_MAX);
    // 历史 2 条 + 扫描新增 2 条("home" 重复,只留一条)。
    assert(count == 4);

    assert(strcmp(rows[0].ssid, "cafe") == 0);    // 最近连的排最前
    assert(rows[0].saved);
    assert(!rows[0].secure);                      // 空密码 = 开放网络

    assert(strcmp(rows[1].ssid, "home") == 0);
    assert(rows[1].saved);
    assert(rows[1].secure);
    assert(rows[1].rssi == -50);                  // 扫到了就补上信号

    assert(strcmp(rows[2].ssid, "neighbor") == 0);
    assert(!rows[2].saved);
    assert(rows[2].secure);
    assert(rows[2].rssi == -40);

    assert(strcmp(rows[3].ssid, "free") == 0);
    assert(!rows[3].saved);
    assert(!rows[3].secure);
}

static void test_merge_rows_limits_and_empty(void) {
    app_known_list_t known;
    app_known_reset(&known);

    app_ap_row_t rows[3];
    assert(app_known_merge_rows(&known, NULL, 0, rows, 3) == 0);
    assert(app_known_merge_rows(NULL, NULL, 0, NULL, 3) == 0);
    assert(app_known_merge_rows(&known, NULL, 0, rows, 0) == 0);

    // 扫描结果里有隐藏热点(SSID 为空),必须跳过。
    const app_scan_ap_t scan[] = {
        { .ssid = "", .rssi = -30, .channel = 1, .secure = true },
        { .ssid = "a", .rssi = -50, .channel = 1, .secure = true },
        { .ssid = "b", .rssi = -60, .channel = 1, .secure = true },
        { .ssid = "c", .rssi = -70, .channel = 1, .secure = true },
    };
    const size_t count = app_known_merge_rows(&known, scan, 4, rows, 3);
    assert(count == 3);                       // cap 生效
    assert(strcmp(rows[0].ssid, "a") == 0);
    assert(strcmp(rows[2].ssid, "c") == 0);
}

int main(void) {
    test_reset_and_find();
    test_upsert_orders_by_recency();
    test_upsert_evicts_oldest();
    test_upsert_rejects_bad_input();
    test_remove();
    test_password_lookup();
    test_merge_rows_saved_first_then_signal();
    test_merge_rows_limits_and_empty();
    puts("test_app_known: OK");
    return 0;
}
