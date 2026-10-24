// main/app_png.h —— 队徽缩略图的极简 PNG 解码。
//
// 懂球帝的队徽原图是 150×150,直接解要 90 KB,C3 上没有 PSRAM,下不动。
// CDN(七牛)支持 ?imageView2/2/w/32 把图缩到 32×32,实测整段 1~3.3 KB,
// 于是"自己解一张小图"变得可行。
//
// 这里不追求通用 PNG,只认实际会遇到的形态:32×32 以内、位深 8、不隔行,
// 色彩类型 0/2/3/4/6(灰度 / RGB / 调色板 / 灰度+Alpha / RGBA)。抽样 45 个
// 队徽,五类里出现了三类——色彩类型由 CDN 的缩放引擎按图片内容自己挑,
// 认不全就会有一批队徽变成占位块。
//
// 之所以不挂 lodepng:它解一张 32×32 的图要 4 KB RGBA 输出加近 3 KB 霍夫曼
// 树,都从堆上来;而这里除了调用方给的出图缓冲,只有一个 4 KB 的静态解压缓冲,
// 全程不碰堆。反正要支持的输入窄到只有一种尺寸,自己写比重整份解码器便宜,
// 也躲开了"往 LVGL 组件内部借符号"这种耦合。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 只接受这么大的图。请求里带的是 ?imageView2/2/w/32,CDN 正常就给 32×32;
// 万一它不理这个参数把原图返回来,这里直接拒掉,而不是解到缓冲区外面去。
#define APP_PNG_MAX_SIDE 32

typedef struct {
    uint16_t w;
    uint16_t h;
    // RGB565、行主序,位序和 lv_color_to_u16() 一致(红在高 5 位),
    // 可以直接塞进 lv_image_dsc_t 的 LV_COLOR_FORMAT_RGB565。
    uint16_t pixels[APP_PNG_MAX_SIDE * APP_PNG_MAX_SIDE];
} app_png_image_t;

// 解压缓冲要多大。反过滤前的原始扫描线是"每行 1 字节过滤类型 + w×通道数",
// 最宽的是 32 位 RGBA,也就是 32 × (1 + 32×4) = 4128 字节。
//
// 这块缓冲由调用方提供,本模块一个字节都不申请。原因很实在:C3 没有 PSRAM,
// 静态 .bss 是直接从系统堆里扣的,而系统堆正是 TLS 握手要用的那点余量——
// 把 4 KB 钉在 .bss 里,等于让每一次握手都少 4 KB,那就是 0x4290(证书验签时
// mbedtls_mpi_grow 分不到内存)的成因。放调用方手里,它就只在真正解码的那几
// 毫秒里活着。
#define APP_PNG_SCRATCH_SIZE (APP_PNG_MAX_SIDE * (1 + APP_PNG_MAX_SIDE * 4))

// 解码一段 PNG。bg_rgb565 是底色,用来填透明像素——队徽四角是透明的,
// 不填的话会在深色面板上留下一圈黑边。半透明像素按 alpha 往底色上混,
// 边缘的抗锯齿才不会出一圈硬锯齿。
//
// scratch 至少要有 APP_PNG_SCRATCH_SIZE 字节,不满足直接返回 false。
// 尺寸/格式不合口径、数据被截断、CRC 不对,同样返回 false;此时 out 内容
// 未定义,不要用。
bool app_png_decode(const uint8_t *png, size_t len, uint16_t bg_rgb565,
                    uint8_t *scratch, size_t scratch_len,
                    app_png_image_t *out);
