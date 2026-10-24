#include "app_png.h"

#include <string.h>

// ------------------------------------------------------------------ CRC32

// PNG 每个块尾都带 CRC32。逐位算,不建 1 KB 的表——块都只有几百字节,
// 建表的开销比省下来的那点 CPU 大。
static uint32_t crc32_of(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ ((c & 1u) ? 0xEDB88320u : 0u);
        }
    }
    return c ^ 0xFFFFFFFFu;
}

// ------------------------------------------------------------- DEFLATE 流

#define APP_PNG_MAXBITS 15

// 位读取:zlib 的位是从字节最低位开始排的。
typedef struct {
    const uint8_t *p;
    size_t len;
    size_t pos;
    uint32_t bitbuf;   // 已取出的位,低位有效
    unsigned bitcnt;
    bool bad;          // 输入耗尽
} app_bits_t;

static void bits_init(app_bits_t *b, const uint8_t *p, size_t len) {
    b->p = p;
    b->len = len;
    b->pos = 0;
    b->bitbuf = 0;
    b->bitcnt = 0;
    b->bad = false;
}

static uint32_t bits_get(app_bits_t *b, unsigned need) {
    uint32_t val = b->bitbuf;
    while (b->bitcnt < need) {
        if (b->pos >= b->len) {
            b->bad = true;
            return 0;
        }
        val |= (uint32_t)b->p[b->pos++] << b->bitcnt;
        b->bitcnt += 8;
    }
    b->bitbuf = val >> need;
    b->bitcnt -= need;
    return val & ((1u << need) - 1u);
}

// 规范霍夫曼树:按码长把符号排成序,解码时一位一位比对。
// 这个写法来自 Mark Adler 的 puff.c(zlib 许可),自己照着思路重排了一遍。
typedef struct {
    short count[APP_PNG_MAXBITS + 1];
    short symbol[320];
} app_huff_t;

static bool huff_build(app_huff_t *h, const uint8_t *length, unsigned n) {
    for (unsigned len = 0; len <= APP_PNG_MAXBITS; len++) h->count[len] = 0;
    for (unsigned s = 0; s < n; s++) h->count[length[s]]++;
    // 全 0:这棵树一个码字都没有。距离树经常这样(整段没有匹配),
    // 不算错,等真用到它时解码会失败。
    if (h->count[0] == (short)n) return true;

    int left = 1;
    for (unsigned len = 1; len <= APP_PNG_MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return false;   // 码字分配超额,数据坏了
    }
    // left > 0 是"码没编满",zlib 允许,不拦。

    short offs[APP_PNG_MAXBITS + 2];
    offs[1] = 0;
    for (unsigned len = 1; len < APP_PNG_MAXBITS; len++) {
        offs[len + 1] = (short)(offs[len] + h->count[len]);
    }
    for (unsigned s = 0; s < n; s++) {
        if (length[s] != 0) h->symbol[offs[length[s]]++] = (short)s;
    }
    return true;
}

// 返回符号,<0 表示解不出来。
static int huff_decode(app_bits_t *b, const app_huff_t *h) {
    int code = 0, first = 0, index = 0;
    for (unsigned len = 1; len <= APP_PNG_MAXBITS; len++) {
        code |= (int)bits_get(b, 1);
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const uint16_t LEN_BASE[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t LEN_EXTRA[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t DIST_BASE[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
    12289, 16385, 24577
};
static const uint8_t DIST_EXTRA[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
static const uint8_t CL_ORDER[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

// 动态霍夫曼块:先从码长流里解出文字/长度树和距离树的码长。
static bool dyn_block(app_bits_t *b, app_huff_t *lit, app_huff_t *dist) {
    unsigned nlen = (unsigned)bits_get(b, 5) + 257u;
    unsigned ndist = (unsigned)bits_get(b, 5) + 1u;
    unsigned ncode = (unsigned)bits_get(b, 4) + 4u;
    if (b->bad) return false;
    if (nlen > 286u || ndist > 30u) return false;

    uint8_t cl_len[19];
    memset(cl_len, 0, sizeof(cl_len));
    for (unsigned i = 0; i < ncode; i++) cl_len[CL_ORDER[i]] = (uint8_t)bits_get(b, 3);
    app_huff_t cl;
    if (!huff_build(&cl, cl_len, 19)) return false;

    uint8_t lengths[320];
    memset(lengths, 0, sizeof(lengths));
    unsigned idx = 0;
    while (idx < nlen + ndist) {
        int sym = huff_decode(b, &cl);
        if (sym < 0 || b->bad) return false;
        if (sym < 16) {
            lengths[idx++] = (uint8_t)sym;
            continue;
        }
        unsigned rep = 0;
        uint8_t val = 0;
        if (sym == 16) {
            if (idx == 0) return false;   // 重复上一个,但没有上一个
            val = lengths[idx - 1];
            rep = 3u + (unsigned)bits_get(b, 2);
        } else if (sym == 17) {
            rep = 3u + (unsigned)bits_get(b, 3);
        } else {
            rep = 11u + (unsigned)bits_get(b, 7);
        }
        if (b->bad) return false;
        if (idx + rep > nlen + ndist) return false;
        while (rep-- > 0u) lengths[idx++] = val;
    }
    if (lengths[256] == 0) return false;   // 没有结束码,解不完

    if (!huff_build(lit, lengths, nlen)) return false;
    if (!huff_build(dist, lengths + nlen, ndist)) return false;
    return true;
}

static bool fixed_block(app_huff_t *lit, app_huff_t *dist) {
    uint8_t lengths[288];
    for (unsigned i = 0; i < 144; i++) lengths[i] = 8;
    for (unsigned i = 144; i < 256; i++) lengths[i] = 9;
    for (unsigned i = 256; i < 280; i++) lengths[i] = 7;
    for (unsigned i = 280; i < 288; i++) lengths[i] = 8;
    if (!huff_build(lit, lengths, 288)) return false;

    uint8_t d[30];
    for (unsigned i = 0; i < 30; i++) d[i] = 5;
    if (!huff_build(dist, d, 30)) return false;
    return true;
}

// 把一个 zlib 流解到 out。返回字节数,<0 = 出错。
// 输出是按顺序产生的,匹配串的回指一定落在已经写过的区间里,所以不需要
// 滑动窗口,直接往 out 里找就行。
static int inflate_zlib(const uint8_t *in, size_t in_len, uint8_t *out, size_t cap) {
    if (in_len < 2u) return -1;
    if ((in[0] & 0x0Fu) != 8u) return -1;               // CM 必须是 8
    if ((((unsigned)in[0] << 8) | in[1]) % 31u != 0) return -1;   // 头校验
    // FDICT 在 FLG(第二个字节)的 bit 5,不在 CMF 上。CMF 高 4 位是 CINFO,
    // 常见值 7(0x78)自己就带着 bit 5,查错了会把几乎所有正常 PNG 全拒掉。
    if ((in[1] & 0x20u) != 0) return -1;                // 带预置字典,不认

    app_bits_t b;
    bits_init(&b, in + 2, in_len - 2u);

    size_t n = 0;
    for (;;) {
        unsigned last = bits_get(&b, 1);
        unsigned type = bits_get(&b, 2);
        if (b.bad) return -1;

        if (type == 0) {
            // 存贮块:丢掉当前字节剩下的位,读 LEN/NLEN 后原样拷。
            b.bitbuf = 0;
            b.bitcnt = 0;
            if (b.pos + 4u > b.len) return -1;
            unsigned len = (unsigned)b.p[b.pos] | ((unsigned)b.p[b.pos + 1] << 8);
            unsigned nlen = (unsigned)b.p[b.pos + 2] | ((unsigned)b.p[b.pos + 3] << 8);
            b.pos += 4;
            if ((len ^ 0xFFFFu) != nlen) return -1;
            if (n + len > cap || b.pos + len > b.len) return -1;
            memcpy(out + n, b.p + b.pos, len);
            b.pos += len;
            n += len;
        } else if (type == 1 || type == 2) {
            app_huff_t lit, dist;
            if (type == 1) {
                if (!fixed_block(&lit, &dist)) return false;
            } else {
                if (!dyn_block(&b, &lit, &dist)) return false;
            }

            for (;;) {
                int sym = huff_decode(&b, &lit);
                if (sym < 0 || b.bad) return -1;
                if (sym < 256) {
                    if (n >= cap) return -1;
                    out[n++] = (uint8_t)sym;
                    continue;
                }
                if (sym == 256) break;   // 块结束
                sym -= 257;
                if (sym >= 29) return -1;
                unsigned len = LEN_BASE[sym] + (unsigned)bits_get(&b, LEN_EXTRA[sym]);
                int dsym = huff_decode(&b, &dist);
                if (dsym < 0 || dsym >= 30 || b.bad) return -1;
                unsigned d = DIST_BASE[dsym] + (unsigned)bits_get(&b, DIST_EXTRA[dsym]);
                if (d > n) return -1;          // 回指还没产生过的数据
                if (n + len > cap) return -1;
                for (unsigned i = 0; i < len; i++) {
                    out[n] = out[n - d];
                    n++;
                }
            }
        } else {
            return -1;   // BTYPE=3 保留
        }
        if (last != 0u) break;
    }
    return (int)n;
}

// ------------------------------------------------------------------ PNG 层

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// 每种色彩类型每个像素占几个字节。调色板记 1:那一个字节是索引,
// 真正查表在最后一步。
static unsigned channels_of(uint8_t ctype) {
    switch (ctype) {
        case 0: return 1;   // 灰度
        case 2: return 3;   // RGB
        case 3: return 1;   // 调色板索引
        case 4: return 2;   // 灰度 + Alpha
        case 6: return 4;   // RGBA
        default: return 0;
    }
}

// RGB565 → 8 位分量,用来和调色板/Alpha 做混合。
static void split565(uint16_t c, unsigned *r, unsigned *g, unsigned *b) {
    unsigned r5 = (c >> 11) & 0x1Fu;
    unsigned g6 = (c >> 5) & 0x3Fu;
    unsigned b5 = c & 0x1Fu;
    // 把高位复制到低位补满 8 位,0x1F → 0xFF,比乘 255/31 便宜也够准。
    *r = (r5 << 3) | (r5 >> 2);
    *g = (g6 << 2) | (g6 >> 4);
    *b = (b5 << 3) | (b5 >> 2);
}

static uint16_t pack565(unsigned r, unsigned g, unsigned b) {
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

// 把解压后的扫描线还原成像素。逐行原地做:第 y 行用的上一行已经是还原过的。
// 注意循环走的是**字节**,不是像素:RGB 一个像素 3 字节、RGBA 4 字节,
// 用像素下标去索引字节只会处理到前三分之一,而且 filter 0 的行是直通的、
// 看不出来——第一行看着永远是对的。
static bool unfilter(uint8_t *raw, uint32_t w, uint32_t h, size_t stride, unsigned bpp) {
    size_t row_bytes = (size_t)w * bpp;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * stride;
        uint8_t filter = row[0];
        uint8_t *cur = row + 1;
        const uint8_t *prev = (y > 0) ? raw + (size_t)(y - 1) * stride + 1 : NULL;
        if (filter > 4) return false;

        for (size_t i = 0; i < row_bytes; i++) {
            int a = (i >= bpp) ? cur[i - bpp] : 0;            // 左
            int b = prev ? prev[i] : 0;                       // 上
            int c = (i >= bpp && prev) ? prev[i - bpp] : 0;   // 左上
            int v = cur[i];
            switch (filter) {
                case 0: break;
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) / 2; break;
                default: {
                    int p = a + b - c;
                    int pa = p > a ? p - a : a - p;
                    int pb = p > b ? p - b : b - p;
                    int pc = p > c ? p - c : c - p;
                    int pred = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                    v += pred;
                    break;
                }
            }
            cur[i] = (uint8_t)v;
        }
    }
    return true;
}

bool app_png_decode(const uint8_t *png, size_t len, uint16_t bg_rgb565,
                    uint8_t *scratch, size_t scratch_len,
                    app_png_image_t *out) {
    static const uint8_t SIG[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };

    if (png == NULL || out == NULL) return false;
    if (scratch == NULL || scratch_len < APP_PNG_SCRATCH_SIZE) return false;
    if (len < 8u + 25u) return false;   // 签名 + 一个 IHDR 块
    if (memcmp(png, SIG, 8) != 0) return false;

    uint32_t w = 0, h = 0;
    uint8_t depth = 0, ctype = 0, interlace = 0;
    uint8_t pal[256][3];
    uint8_t alpha[256];
    unsigned pal_n = 0;
    memset(alpha, 0xFF, sizeof(alpha));   // 没给 tRNS 就是全不透明

    bool seen_ihdr = false, seen_iend = false;
    size_t idat_at = 0, idat_len = 0;

    size_t pos = 8;
    while (pos + 8u <= len) {
        uint32_t clen = rd32(png + pos);
        const uint8_t *type = png + pos + 4;
        const uint8_t *data = png + pos + 8;
        if (clen > len || pos + 12u + clen > len) return false;
        if (rd32(data + clen) != crc32_of(type, 4u + clen)) return false;

        if (memcmp(type, "IHDR", 4) == 0) {
            if (clen != 13u || seen_ihdr) return false;
            w = rd32(data);
            h = rd32(data + 4);
            depth = data[8];
            ctype = data[9];
            interlace = data[12];
            seen_ihdr = true;
        } else if (memcmp(type, "PLTE", 4) == 0) {
            if (clen == 0u || clen % 3u != 0u || clen / 3u > 256u) return false;
            memcpy(pal, data, clen);
            pal_n = clen / 3u;
        } else if (memcmp(type, "tRNS", 4) == 0) {
            // 调色板的 tRNS 可以短于调色板,没列到的项按不透明算。
            if (clen > 256u) return false;
            memcpy(alpha, data, clen);
        } else if (memcmp(type, "IDAT", 4) == 0) {
            // 规范允许把 IDAT 拆成多块,但只在压缩数据攒够 8192 字节时才拆。
            // 这里支持的图解压后最多 4128 字节(32×32 RGBA),压缩后更小,
            // 实测 60 个真实缩略图全是单块——结构上就拆不起来。
            // 多块的数据在文件里并不连续(中间夹着块头),不拼起来就是一堆
            // 乱码,所以宁可拒掉画占位块,也不冒解出错数据的风险。
            if (idat_len != 0u) return false;
            idat_at = pos + 8u;
            idat_len = clen;
        } else if (memcmp(type, "IEND", 4) == 0) {
            seen_iend = true;
        }
        pos += 12u + clen;
    }

    if (!seen_ihdr || !seen_iend) return false;
    if (idat_len == 0u) return false;
    if (depth != 8u || interlace != 0u) return false;
    if (w == 0u || h == 0u || w > APP_PNG_MAX_SIDE || h > APP_PNG_MAX_SIDE) return false;
    if (ctype == 3u && pal_n == 0u) return false;

    unsigned ch = channels_of(ctype);
    if (ch == 0u) return false;
    size_t stride = (size_t)w * ch + 1u;
    if (stride * h > scratch_len) return false;

    int got = inflate_zlib(png + idat_at, idat_len, scratch, stride * h);
    if (got < 0 || (size_t)got != stride * h) return false;   // 解出来的量和 IHDR 对不上
    if (!unfilter(scratch, w, h, stride, ch)) return false;

    unsigned bg_r, bg_g, bg_b;
    split565(bg_rgb565, &bg_r, &bg_g, &bg_b);

    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *cur = scratch + (size_t)y * stride + 1u;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *px = cur + (size_t)x * ch;
            unsigned r, g, b, a = 255u;
            switch (ctype) {
                case 0:   // 灰度
                    r = g = b = px[0];
                    break;
                case 2:   // RGB
                    r = px[0]; g = px[1]; b = px[2];
                    break;
                case 3: { // 调色板索引
                    unsigned i = px[0];
                    if (i >= pal_n) return false;
                    r = pal[i][0]; g = pal[i][1]; b = pal[i][2];
                    a = alpha[i];
                    break;
                }
                case 4:   // 灰度 + Alpha
                    r = g = b = px[0];
                    a = px[1];
                    break;
                default:  // RGBA
                    r = px[0]; g = px[1]; b = px[2]; a = px[3];
                    break;
            }
            if (a != 255u) {
                // 往底色上混。队徽边缘是抗锯齿的,直接当不透明会出一圈硬锯齿。
                r = (r * a + bg_r * (255u - a)) / 255u;
                g = (g * a + bg_g * (255u - a)) / 255u;
                b = (b * a + bg_b * (255u - a)) / 255u;
            }
            out->pixels[(size_t)y * w + x] = pack565(r, g, b);
        }
    }

    out->w = (uint16_t)w;
    out->h = (uint16_t)h;
    return true;
}
