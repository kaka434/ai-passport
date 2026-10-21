// tests/test_app_keyboard.c —— 屏上键盘的纯逻辑:光标、字符、退格、大小写、上限。
#include "app_keyboard.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_reset(void) {
    app_keyboard_t kb;
    memset(&kb, 0xFF, sizeof(kb));   // 脏数据也要被清干净
    app_keyboard_reset(&kb);
    assert(kb.len == 0);
    assert(kb.text[0] == '\0');
    assert(kb.cursor == 0);
    assert(!kb.upper);

    app_keyboard_reset(NULL);   // 不炸
}

static void test_cursor_wraps(void) {
    app_keyboard_t kb;
    app_keyboard_reset(&kb);

    // 上 = 前一个键,首端绕到末尾。
    app_keyboard_move(&kb, -1);
    assert(kb.cursor == APP_KB_KEYS - 1);

    // 下 = 下一个键,末端绕回开头。
    app_keyboard_move(&kb, 1);
    assert(kb.cursor == 0);

    for (int i = 0; i < APP_KB_KEYS; i++) app_keyboard_move(&kb, 1);
    assert(kb.cursor == 0);

    app_keyboard_move(&kb, 0);   // 原地不动
    assert(kb.cursor == 0);
    app_keyboard_move(NULL, 1);  // 不炸
}

static void test_type_and_backspace(void) {
    app_keyboard_t kb;
    app_keyboard_reset(&kb);

    // 第 0 个键是 'a'。
    assert(app_keyboard_press(&kb));
    assert(strcmp(kb.text, "a") == 0);
    assert(kb.len == 1);

    // 第 1 个键是 'b'。
    app_keyboard_move(&kb, 1);
    assert(app_keyboard_press(&kb));
    assert(strcmp(kb.text, "ab") == 0);

    // 退格是第 40 个键。
    while (kb.cursor != APP_KB_KEYS - 2) app_keyboard_move(&kb, 1);
    assert(app_keyboard_press(&kb));       // 删掉 'b'
    assert(strcmp(kb.text, "a") == 0);
    assert(app_keyboard_press(&kb));       // 删掉 'a'
    assert(kb.len == 0);
    assert(!app_keyboard_press(&kb));      // 空串再删:不变,返回 false
    assert(kb.len == 0);
}

static void test_shift(void) {
    app_keyboard_t kb;
    app_keyboard_reset(&kb);

    char label[8];
    assert(strcmp(app_keyboard_label(&kb, 0, label, sizeof(label)), "a") == 0);

    // 最后一个键是大小写切换:只改状态,不改文本。
    kb.cursor = APP_KB_KEYS - 1;
    assert(!app_keyboard_press(&kb));
    assert(kb.upper);
    assert(kb.len == 0);
    assert(strcmp(app_keyboard_label(&kb, 0, label, sizeof(label)), "A") == 0);

    // 数字键不受大小写影响。
    assert(strcmp(app_keyboard_label(&kb, 26, label, sizeof(label)), "1") == 0);

    kb.cursor = 0;
    assert(app_keyboard_press(&kb));
    assert(strcmp(kb.text, "A") == 0);
}

static void test_labels(void) {
    app_keyboard_t kb;
    app_keyboard_reset(&kb);

    char label[8];
    assert(strcmp(app_keyboard_label(&kb, 25, label, sizeof(label)), "z") == 0);
    assert(strcmp(app_keyboard_label(&kb, 26, label, sizeof(label)), "1") == 0);
    assert(strcmp(app_keyboard_label(&kb, 35, label, sizeof(label)), "0") == 0);
    assert(strcmp(app_keyboard_label(&kb, 36, label, sizeof(label)), ".") == 0);
    assert(strcmp(app_keyboard_label(&kb, 40, label, sizeof(label)), "删") == 0);
    assert(strcmp(app_keyboard_label(&kb, 41, label, sizeof(label)), "Aa") == 0);

    // 越界与空指针都要安全。
    assert(app_keyboard_label(&kb, -1, label, sizeof(label)) != NULL);
    assert(label[0] == '\0');
    assert(app_keyboard_label(&kb, APP_KB_KEYS, label, sizeof(label)) != NULL);
    assert(label[0] == '\0');
    app_keyboard_label(NULL, 0, label, sizeof(label));
    app_keyboard_label(&kb, 0, NULL, sizeof(label));
}

static void test_is_char(void) {
    assert(app_keyboard_is_char(0));
    assert(app_keyboard_is_char(39));
    assert(!app_keyboard_is_char(40));   // 退格
    assert(!app_keyboard_is_char(41));   // 大小写
    assert(!app_keyboard_is_char(-1));
    assert(!app_keyboard_is_char(APP_KB_KEYS));
}

static void test_text_cap(void) {
    app_keyboard_t kb;
    app_keyboard_reset(&kb);

    // 填满到上限:多按的那些必须被静默忽略,不能覆盖已有内容。
    for (int i = 0; i < APP_KB_TEXT_MAX; i++) {
        kb.cursor = 0;
        assert(app_keyboard_press(&kb));
    }
    assert(kb.len == APP_KB_TEXT_MAX);
    assert(kb.text[APP_KB_TEXT_MAX] == '\0');

    kb.cursor = 0;
    assert(!app_keyboard_press(&kb));
    assert(kb.len == APP_KB_TEXT_MAX);
    assert(strlen(kb.text) == APP_KB_TEXT_MAX);
}

static void test_clear_keeps_cursor(void) {
    app_keyboard_t kb;
    app_keyboard_reset(&kb);
    kb.cursor = 7;
    kb.upper = true;
    (void)app_keyboard_press(&kb);
    assert(kb.len == 1);

    app_keyboard_clear(&kb);
    assert(kb.len == 0);
    assert(kb.text[0] == '\0');
    // 重试同一个网络时,光标位置和大小写都该保留。
    assert(kb.cursor == 7);
    assert(kb.upper);
}

static void test_set_text_prefill(void) {
    app_keyboard_t kb;
    app_keyboard_reset(&kb);
    kb.cursor = 9;
    kb.upper = true;

    // 已存网络的密码预填:文本替换成传进来的,光标回到第一个键。
    app_keyboard_set_text(&kb, "abc123");
    assert(kb.len == 6);
    assert(strcmp(kb.text, "abc123") == 0);
    assert(kb.cursor == 0);
    assert(kb.upper);   // 大小写状态不动,和 clear 保持一致

    // 再预填一次:旧的必须被整串替换,不是追加。
    app_keyboard_set_text(&kb, "xy");
    assert(kb.len == 2);
    assert(strcmp(kb.text, "xy") == 0);

    // 空串 / NULL 等价于清空。
    app_keyboard_set_text(&kb, "");
    assert(kb.len == 0);
    app_keyboard_set_text(&kb, NULL);
    assert(kb.len == 0);
    assert(kb.text[0] == '\0');

    // 超长按上限截断,且保证结束符。
    char big[APP_KB_TEXT_MAX + 16];
    memset(big, 'k', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    app_keyboard_set_text(&kb, big);
    assert(kb.len == APP_KB_TEXT_MAX);
    assert(kb.text[APP_KB_TEXT_MAX] == '\0');

    // 预填之后还能继续编辑。
    kb.cursor = APP_KB_KEYS - 2;   // 退格
    assert(app_keyboard_press(&kb));
    assert(kb.len == APP_KB_TEXT_MAX - 1);

    app_keyboard_set_text(NULL, "x");   // 不炸
}

int main(void) {
    test_reset();
    test_cursor_wraps();
    test_type_and_backspace();
    test_shift();
    test_labels();
    test_is_char();
    test_text_cap();
    test_clear_keeps_cursor();
    test_set_text_prefill();
    puts("test_app_keyboard: OK");
    return 0;
}
