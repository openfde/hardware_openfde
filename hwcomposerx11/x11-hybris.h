#pragma once

#include <stdint.h>
#include <stdbool.h>

struct display;
struct window;

/* 一层画面在送显时的完整描述，由 hwcomposer.cpp 从 hwc_layer_1_t 提取。
 * 本模块不依赖 hwcomposer 的类型。 */
struct x11_hybris_layer {
    /* ---- 层内容：dma-buf 零拷贝来源 ---- */
    int dmabuf_fd;      /* gralloc 的 dma-buf fd，-1 表示不可用（该层跳过） */
    uint64_t modifier;  /* DRM format modifier，0 = linear */
    uint64_t buffer_id; /* dma-buf 稳定标识（纹理缓存 key，见 hwcomposer 的
                         * dmabuf_id_from_fd），0 表示不缓存 */

    int buf_w;      /* 缓冲宽（像素） */
    int buf_h;      /* 缓冲高（像素） */
    int stride_px;  /* 行跨度，单位像素 */
    int hal_format; /* HAL_PIXEL_FORMAT_* */

    int src_x; /* 源裁剪，缓冲像素坐标 */
    int src_y;
    int src_w;
    int src_h;

    int dst_x; /* 目标矩形，窗口像素坐标 */
    int dst_y;
    int dst_w;
    int dst_h;

    uint32_t transform; /* HWC_TRANSFORM_* 位组合 */
    bool opaque;        /* 源不带有效 alpha（RGBX/RGB565 等） */
};

/* 本模块是否负责「层内容 -> 纹理 -> swap 上屏」。false 表示调用方应按
 * shm 上传 + XRender 合成处理（光标层永远走那条路）。
 * 首次调用做一次完整探测（dlopen 三个库 + 开 Display + 选 config + 建上下文），
 * 结果缓存；探测失败打印具体缺什么并返回 false。 */
bool x11_hybris_get_ready(struct display *display);

/* 把一层排进该窗口本帧的送显队列（只记账，不上屏）。失败返回 false。 */
bool x11_hybris_add_layer(struct window *window,
                           const struct x11_hybris_layer *layer);

/* 上屏：make current -> clear -> 逐层贴图 -> eglSwapBuffers。
 * 必须在所有 add_layer 之后、这一帧对窗口的其它 X 请求之前调用。 */
void x11_hybris_flush(struct window *window);

/* 释放某个窗口持有的 EGLSurface（窗口销毁时调用）。 */
void x11_hybris_release_window(struct window *window);

/* display 销毁时调用：销毁上下文、纹理，并关闭 glibc 侧的 X 连接。 */
void x11_hybris_display_destroy(struct display *display);
