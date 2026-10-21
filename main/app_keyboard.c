// main/app_keyboard.c
#include "app_keyboard.h"

#include <stdio.h>
#include <string.h>

// 42 个键的字符表:26 个字母 + 10 个数字 + 4 个符号 = 40,后面两个是功能键。
// 顺序即网格的阅读顺序(行优先),界面直接按 index 摆格子。
static const char KB_CHARS[] = "abcdefghijklmnopqrstuvwxyz1234567890.-_@";

#define KB_CHAR_COUNT ((int)(sizeof(KB_CHARS) - 1))   // 40
#define KB_BACKSPACE KB_CHAR_COUNT                    // 40
#define KB_SHIFT (KB_CHAR_COUNT + 1)                  // 41

// 编译期自检:字符数必须正好填满前 6 行(36)+ 第 7 行的前 4 格。
typedef char app_kb_layout_must_fill_grid[(KB_CHAR_COUNT == 40) ? 1 : -1];

void app_keyboard_reset(app_keyboard_t *kb) {
    if (kb == NULL) return;
    memset(kb, 0, sizeof(*kb));
    kb->cursor = 0;
    kb->upper = false;
}

void app_keyboard_clear(app_keyboard_t *kb) {
    if (kb == NULL) return;
    kb->text[0] = '\0';
    kb->len = 0;
}

void app_keyboard_set_text(app_keyboard_t *kb, const char *text) {
    if (kb == NULL) return;
    kb->text[0] = '\0';
    kb->len = 0;
    kb->cursor = 0;   // 预填之后从第一个键开始,和刚进密码页的手感一致
    if (text == NULL) return;

    size_t n = 0;
    while (n < APP_KB_TEXT_MAX && text[n] != '\0') {
        kb->text[n] = text[n];
        n++;
    }
    kb->text[n] = '\0';
    kb->len = n;
}

void app_keyboard_move(app_keyboard_t *kb, int delta) {
    if (kb == NULL || delta == 0) return;
    int next = (int)kb->cursor + delta;
    while (next < 0) next += APP_KB_KEYS;
    while (next >= APP_KB_KEYS) next -= APP_KB_KEYS;
    kb->cursor = (uint8_t)next;
}

bool app_keyboard_is_char(int index) {
    return index >= 0 && index < KB_CHAR_COUNT;
}

static char char_key(const app_keyboard_t *kb, int index) {
    const char c = KB_CHARS[index];
    if (!kb->upper) return c;
    if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
    return c;
}

bool app_keyboard_press(app_keyboard_t *kb) {
    if (kb == NULL) return false;

    if (kb->cursor < KB_CHAR_COUNT) {
        if (kb->len >= APP_KB_TEXT_MAX) return false;   // 满了就静默忽略,不覆盖已有内容
        kb->text[kb->len++] = char_key(kb, kb->cursor);
        kb->text[kb->len] = '\0';
        return true;
    }

    if (kb->cursor == KB_BACKSPACE) {
        if (kb->len == 0) return false;
        kb->len--;
        kb->text[kb->len] = '\0';
        return true;
    }

    // 大小写切换:不改变文本,但界面要重画。
    kb->upper = !kb->upper;
    return false;
}

const char *app_keyboard_label(const app_keyboard_t *kb, int index, char *buf, size_t cap) {
    if (buf == NULL || cap == 0) return buf;
    if (kb == NULL || index < 0 || index >= APP_KB_KEYS) {
        buf[0] = '\0';
        return buf;
    }

    if (index < KB_CHAR_COUNT) {
        buf[0] = char_key(kb, index);
        buf[1] = '\0';
    } else if (index == KB_BACKSPACE) {
        snprintf(buf, cap, "删");
    } else {
        snprintf(buf, cap, "Aa");
    }
    return buf;
}
