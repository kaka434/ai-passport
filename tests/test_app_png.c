// 队徽 PNG 解码器的主机测试。
//
// 期望值来自 tools/gen_crest_fixtures.py:那边用 PIL 独立解一遍 PNG,再按
// app_png.h 写明的换算公式算 RGB565。所以这里对上的不只是"两次跑出一样",
// 而是"我写的 inflate/反过滤/调色板查找 和 PIL 的结果一致"——inflate 是
// 手写的,错一个位就会在全图 1024 个像素上炸开,checksum 一碰就发现。
#include "crest_fixtures.h"

#include "app_png.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// 解码用的临时缓冲。固件里它是从堆上临时要的(见 app_png.h 的说明),主机
// 测试直接给一块静态的就行——这里关心的只是解码结果,不关心它放在哪。
static uint8_t s_scratch[APP_PNG_SCRATCH_SIZE];

static uint32_t fnv1a(const uint16_t *p, size_t n) {
    uint32_t h = 0x811C9DC5u;
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n * 2u; i++) {
        h ^= b[i];
        h *= 0x01000193u;
    }
    return h;
}

static void test_fixtures(void) {
    for (unsigned i = 0; i < crest_fixture_count; i++) {
        const crest_fixture_t *f = &crest_fixtures[i];
        app_png_image_t img;

        // 先确认上一轮的脏数据不会冒充成结果:fixture 之间不 reset,
        // 靠的是每次成功解码都会把 w/h 写掉。
        memset(&img, 0xA5, sizeof(img));

        if (!app_png_decode(f->png, f->png_len, CREST_TEST_BG, s_scratch, sizeof(s_scratch), &img)) {
            printf("FAIL %s: 解码失败\n", f->name);
            assert(false);
        }
        if (img.w != f->w || img.h != f->h) {
            printf("FAIL %s: 尺寸 %ux%u,期望 %ux%u\n", f->name,
                   img.w, img.h, f->w, f->h);
            assert(false);
        }

        size_t n = (size_t)img.w * img.h;
        uint32_t sum = fnv1a(img.pixels, n);
        if (sum != f->checksum) {
            printf("FAIL %s: checksum 0x%08X,期望 0x%08X\n", f->name, sum, f->checksum);
            assert(false);
        }
        if (f->pixels != NULL) {
            for (size_t k = 0; k < n; k++) {
                if (img.pixels[k] != f->pixels[k]) {
                    printf("FAIL %s: 像素[%zu] 0x%04X,期望 0x%04X\n", f->name, k,
                           img.pixels[k], f->pixels[k]);
                    assert(false);
                }
            }
        }
    }
    printf("fixtures: %u 个全部对上 PIL 的期望值\n", crest_fixture_count);
}

static void test_rejects(void) {
    for (unsigned i = 0; i < crest_reject_count; i++) {
        const crest_reject_t *r = &crest_rejects[i];
        app_png_image_t img;
        if (app_png_decode(r->png, r->png_len, CREST_TEST_BG, s_scratch, sizeof(s_scratch), &img)) {
            printf("FAIL %s: 本该拒掉却解开了\n", r->name);
            assert(false);
        }
    }
    printf("rejects: %u 个全部拒掉\n", crest_reject_count);
}

// 空指针/零长度这种调用方手滑的情况,不能崩。
static void test_bad_args(void) {
    app_png_image_t img;
    assert(!app_png_decode(NULL, 0, CREST_TEST_BG, s_scratch, sizeof(s_scratch), &img));
    assert(!app_png_decode((const uint8_t *)"x", 1, CREST_TEST_BG, s_scratch,
                           sizeof(s_scratch), &img));
    assert(!app_png_decode(crest_fixtures[0].png, 0, CREST_TEST_BG, s_scratch, sizeof(s_scratch), &img));

    static const uint8_t junk[64] = { 0 };
    assert(!app_png_decode(junk, sizeof(junk), CREST_TEST_BG, s_scratch,
                           sizeof(s_scratch), &img));

    // 解压缓冲不够大就得直接拒,不能先解到一半再说——调用方(固件里是队徽
    // 任务从堆上临时要的)要是把尺寸算错了,这里得挡住,而不是写越界。
    assert(!app_png_decode(crest_fixtures[0].png, crest_fixtures[0].png_len,
                           CREST_TEST_BG, s_scratch, APP_PNG_SCRATCH_SIZE - 1, &img));
    printf("bad args: OK\n");
}

// 底色只影响透明像素。不透明的图换底色必须一个像素都不变,带 alpha 的图
// 必须有一部分变、但不能全变——否则说明混色写反了,把不透明像素也搅进去,
// 或者压根没接上底色。按名字找夹具,别用下标:生成器一改顺序就全错位。
static const crest_fixture_t *find(const char *name) {
    for (unsigned i = 0; i < crest_fixture_count; i++) {
        if (strcmp(crest_fixtures[i].name, name) == 0) return &crest_fixtures[i];
    }
    return NULL;
}

static size_t diff_count(const crest_fixture_t *f, uint16_t bg) {
    app_png_image_t base;
    assert(app_png_decode(f->png, f->png_len, CREST_TEST_BG, s_scratch, sizeof(s_scratch), &base));
    app_png_image_t other;
    assert(app_png_decode(f->png, f->png_len, bg, s_scratch, sizeof(s_scratch), &other));

    size_t changed = 0;
    for (size_t k = 0; k < (size_t)base.w * base.h; k++) {
        if (base.pixels[k] != other.pixels[k]) changed++;
    }
    return changed;
}

static void test_bg_only_touches_transparent(void) {
    // 不透明的 RGB:底色怎么换都该一模一样。
    const crest_fixture_t *opaque = find("synth_filters");
    assert(opaque != NULL);
    if (diff_count(opaque, 0xF81Fu) != 0) {   // 一抹红
        printf("FAIL: 不透明的图随底色变了,混色漏进了 alpha=255 的像素\n");
        assert(false);
    }

    // 带 alpha 的调色板图:tRNS 里既有全透明也有不透明,该有一部分像素
    // 跟着底色走,另一部分纹丝不动。(不用 synth_rgba——它的 alpha 是
    // (x+y)*4%256,几乎没有等于 255 的像素,那就没有"不动"的那一半了。)
    const crest_fixture_t *alpha = find("synth_palette");
    assert(alpha != NULL);
    size_t changed = diff_count(alpha, 0xF81Fu);
    if (changed == 0) {
        printf("FAIL: 带 alpha 的图换底色一个像素都没变,底色没接上?\n");
        assert(false);
    }
    if (changed == (size_t)alpha->w * alpha->h) {
        printf("FAIL: 整张图都随底色变了,混色把不透明像素也搅进去了\n");
        assert(false);
    }

    // 真实队徽也是带透明的:换底色同样该只动一部分。
    const crest_fixture_t *real = find("real_ct6_0");
    if (real != NULL) {
        size_t rc = diff_count(real, 0xF81Fu);
        if (rc == 0 || rc == (size_t)real->w * real->h) {
            printf("FAIL: 真实队徽的透明像素行为不对(%zu / %zu)\n", rc,
                   (size_t)real->w * real->h);
            assert(false);
        }
        printf("bg blend: 真实队徽 %zu / %zu 个像素随底色变\n", rc,
               (size_t)real->w * real->h);
    }
}

int main(void) {
    test_bad_args();
    test_fixtures();
    test_rejects();
    test_bg_only_touches_transparent();
    printf("test_app_png: OK\n");
    return 0;
}
