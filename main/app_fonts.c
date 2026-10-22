// main/app_fonts.c
#include "app_fonts.h"

#include <string.h>

// 三套小字号共用同一份字符清单,所以用 15px 做覆盖判定即可代表 12/19px。
static const lv_font_t *const COVERAGE_REF = &app_font_15;

static int utf8_decode(const unsigned char *p, uint32_t *cp) {
    if (p[0] < 0x80) {
        *cp = p[0];
        return 1;
    }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(p[0] & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
        return 2;
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6)
            | (uint32_t)(p[2] & 0x3F);
        return 3;
    }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80
        && (p[3] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12)
            | ((uint32_t)(p[2] & 0x3F) << 6) | (uint32_t)(p[3] & 0x3F);
        return 4;
    }
    *cp = 0xFFFD;
    return 1;
}

// 零宽/格式字符:排版用的,本身没有字形。懂球帝的球员名里实测出现过 U+200B,
// 按"缺字"处理会在名字中间冒出一个孤零零的 '?',所以直接丢掉。
static bool is_invisible(uint32_t cp) {
    return (cp >= 0x200B && cp <= 0x200F)   // 零宽空格 / 方向标记
           || cp == 0x00AD                  // 软连字符
           || cp == 0xFEFF;                 // BOM
}

const char *app_fonts_sanitize(const char *src, char *dst, size_t cap) {
    if (dst == NULL || cap == 0) return src;
    if (src == NULL) {
        dst[0] = '\0';
        return dst;
    }

    size_t out = 0;
    const unsigned char *p = (const unsigned char *)src;

    while (*p != '\0') {
        uint32_t cp = 0;
        const int len = utf8_decode(p, &cp);
        if (len <= 0) break;

        if (is_invisible(cp)) {
            p += len;
            continue;
        }

        bool covered = false;
        if (cp >= 0x20 && cp < 0x7F) {
            covered = true;   // ASCII 可打印字符一定在子集里
        } else if (cp == '\n' || cp == '\r' || cp == '\t') {
            cp = ' ';
            covered = true;
        } else {
            lv_font_glyph_dsc_t dsc;
            memset(&dsc, 0, sizeof(dsc));
            covered = lv_font_get_glyph_dsc(COVERAGE_REF, &dsc, cp, 0) && !dsc.is_placeholder;
        }

        if (!covered) {
            if (out + 1 >= cap) break;
            dst[out++] = '?';
        } else {
            if (out + (size_t)len >= cap) break;
            memcpy(dst + out, p, (size_t)len);
            out += (size_t)len;
        }
        p += len;
    }

    dst[out] = '\0';
    return dst;
}
