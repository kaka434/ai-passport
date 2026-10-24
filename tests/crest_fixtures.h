// 本文件由 tools/gen_crest_fixtures.py 生成,不要手改。
// 队徽 PNG 解码器的测试数据:真实 CDN 缩略图 + 覆盖各形态的合成图。
#pragma once

#include <stdint.h>

typedef struct {
    const char *name;
    const uint8_t *png;
    unsigned png_len;
    uint16_t w;
    uint16_t h;
    uint32_t checksum;      // FNV-1a over the expected RGB565 bytes
    const uint16_t *pixels; // expected NULL → 只校验尺寸与 checksum
    unsigned pixel_count;
} crest_fixture_t;

extern const crest_fixture_t crest_fixtures[];
extern const unsigned crest_fixture_count;

typedef struct {
    const char *name;
    const uint8_t *png;
    unsigned png_len;
} crest_reject_t;

extern const crest_reject_t crest_rejects[];
extern const unsigned crest_reject_count;

#define CREST_TEST_BG 0x1143
