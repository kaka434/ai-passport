// main/app_keyboard.h
// 屏上键盘的纯逻辑模型(不碰 LVGL,可在主机上测)。
//
// 这块设备只有上/下/确定三个键,没有左右,所以键盘不做二维光标:
// 6 列 × 7 行共 42 个键排成一张表,光标按**阅读顺序**在表里线性移动
// (下 = 下一个,上 = 上一个,首尾相接)。视觉上仍然是一张网格,只是
// 走到一行末尾再按下会"回车"到下一行开头,和文本光标的直觉一致。
//
//    a b c d e f
//    g h i j k l
//    m n o p q r
//    s t u v w x
//    y z 1 2 3 4
//    5 6 7 8 9 0
//    . - _ @ 删 ⇧
//
// 确定 = 按下当前键(字符/退格/大小写);长按确定不归这里管,由界面解释成
// "提交密码并连接"。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_KB_COLS 6
#define APP_KB_ROWS 7
#define APP_KB_KEYS (APP_KB_COLS * APP_KB_ROWS)

// 与 app_config.h 的 APP_PASS_MAX 对齐:WPA2 口令最长 63 字节。
#define APP_KB_TEXT_MAX 64

typedef struct {
    char text[APP_KB_TEXT_MAX + 1];
    size_t len;
    uint8_t cursor;   // 0 .. APP_KB_KEYS-1
    bool upper;       // 大小写状态,只影响 26 个字母键
} app_keyboard_t;

void app_keyboard_reset(app_keyboard_t *kb);

// 清空已输入内容,保留光标位置与大小写(重试同一个网络时用)。
void app_keyboard_clear(app_keyboard_t *kb);

// 预填文本(已存网络的密码),光标回到第一个键。超长按 APP_KB_TEXT_MAX 截断。
// 用户进密码页就能看到密码已经填好,长按确定即连,想改也还能改。
void app_keyboard_set_text(app_keyboard_t *kb, const char *text);

// 光标移动。delta 为 -1 / +1,越界时首尾相接。
void app_keyboard_move(app_keyboard_t *kb, int delta);

// 按下当前键。返回 true 表示文本发生了变化(退格删掉了字符或追加了字符),
// 大小写切换只改状态,返回 false。
bool app_keyboard_press(app_keyboard_t *kb);

// 第 index 个键上显示的文本。字符键返回字符本身(受大小写影响),
// 退格返回"删",大小写键返回"Aa"。返回 buf。
const char *app_keyboard_label(const app_keyboard_t *kb, int index, char *buf, size_t cap);

// 第 index 个键是不是字符键。界面用它区分普通键与功能键的配色。
bool app_keyboard_is_char(int index);
