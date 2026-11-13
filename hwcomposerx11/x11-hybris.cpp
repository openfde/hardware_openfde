#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <map>
#include <mutex>
#include <vector>

#include <cutils/properties.h>
#include <log/log.h>

#include <EGL/egl.h>   /* 只取常量；EGL 函数全部经 dlsym 调用，不产生链接依赖 */
#include <EGL/eglext.h>
#include <GLES3/gl3.h> /* 只取类型/macro；GL 函数同样走 dlsym */

#include <drm_fourcc.h>
#include <xcb/xcb.h>

#include "x11-hybris.h"
#include "x11-hwc.h"

#ifndef EGL_PLATFORM_X11_KHR
#define EGL_PLATFORM_X11_KHR 0x31D5
#endif
#ifndef EGL_PLATFORM_X11_SCREEN_KHR
#define EGL_PLATFORM_X11_SCREEN_KHR 0x31D6
#endif
#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#endif
#ifndef EGL_LINUX_DRM_FOURCC_EXT
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#endif
#ifndef EGL_DMA_BUF_PLANE0_FD_EXT
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#endif
#ifndef EGL_DMA_BUF_PLANE0_OFFSET_EXT
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#endif
#ifndef EGL_DMA_BUF_PLANE0_PITCH_EXT
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#endif
#ifndef EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
#endif
#ifndef EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444
#endif

namespace {

/* ==========================================================================
 * 0. 宿主库函数表
 *
 * 全部走函数指针：既不会和 bionic 的 libX11 / libEGL 符号混在一起，也不需要
 * 在 Android.bp 里链接任何 glibc 库。
 * ========================================================================== */

struct host_api {
    void *lx11 = nullptr;
    void *legl = nullptr;
    void *lgles = nullptr;

    /* libX11（glibc） */
    void *(*XOpenDisplay)(const char *name) = nullptr;
    int (*XCloseDisplay)(void *dpy) = nullptr;
    int (*XSync)(void *dpy, int discard) = nullptr;

    /* libEGL（glibc）。EGLDisplay/EGLConfig/EGLSurface 统一用 void* */
    void *(*GetPlatformDisplayEXT)(unsigned int platform, void *native_dpy,
                                   const int32_t *attribs) = nullptr;
    void *(*GetDisplay)(void *native_dpy) = nullptr;
    unsigned int (*Initialize)(void *dpy, int32_t *major, int32_t *minor) = nullptr;
    unsigned int (*Terminate)(void *dpy) = nullptr;
    const char *(*QueryString)(void *dpy, int32_t name) = nullptr;
    unsigned int (*GetConfigs)(void *dpy, void **cfgs, int32_t cfg_size,
                               int32_t *num) = nullptr;
    unsigned int (*GetConfigAttrib)(void *dpy, void *cfg, int32_t attr,
                                    int32_t *value) = nullptr;
    void *(*CreateWindowSurface)(void *dpy, void *cfg, unsigned long win,
                                 const int32_t *attribs) = nullptr;
    unsigned int (*DestroySurface)(void *dpy, void *surf) = nullptr;
    void *(*CreateContext)(void *dpy, void *cfg, void *share,
                           const int32_t *attribs) = nullptr;
    unsigned int (*DestroyContext)(void *dpy, void *ctx) = nullptr;
    unsigned int (*MakeCurrent)(void *dpy, void *draw, void *read, void *ctx) = nullptr;
    unsigned int (*SwapBuffers)(void *dpy, void *surf) = nullptr;
    int32_t (*GetError)() = nullptr;
    unsigned int (*BindAPI)(unsigned int api) = nullptr;
    void *(*GetProcAddress)(const char *name) = nullptr;

    /* 扩展（eglGetProcAddress 取）：dma-buf 导入 */
    void *(*CreateImageKHR)(void *dpy, void *ctx, unsigned int target,
                            void *buffer, const int32_t *attribs) = nullptr;
    unsigned int (*DestroyImageKHR)(void *dpy, void *img) = nullptr;

    bool loaded = false;
    bool ok = false;
};

host_api g_api;

/* GLES 入口（从宿主 libGLESv2 dlsym；扩展走 eglGetProcAddress） */
struct host_gl {
    void (*GenTextures)(int32_t, unsigned *) = nullptr;
    void (*DeleteTextures)(int32_t, const unsigned *) = nullptr;
    void (*BindTexture)(unsigned, unsigned) = nullptr;
    void (*ActiveTexture)(unsigned) = nullptr;
    void (*TexParameteri)(unsigned, unsigned, int32_t) = nullptr;
    unsigned (*CreateShader)(unsigned) = nullptr;
    void (*ShaderSource)(unsigned, int32_t, const char *const *, const int32_t *) = nullptr;
    void (*CompileShader)(unsigned) = nullptr;
    void (*GetShaderiv)(unsigned, unsigned, int32_t *) = nullptr;
    void (*GetShaderInfoLog)(unsigned, int32_t, int32_t *, char *) = nullptr;
    unsigned (*CreateProgram)() = nullptr;
    void (*AttachShader)(unsigned, unsigned) = nullptr;
    void (*LinkProgram)(unsigned) = nullptr;
    void (*GetProgramiv)(unsigned, unsigned, int32_t *) = nullptr;
    void (*GetProgramInfoLog)(unsigned, int32_t, int32_t *, char *) = nullptr;
    int32_t (*GetAttribLocation)(unsigned, const char *) = nullptr;
    int32_t (*GetUniformLocation)(unsigned, const char *) = nullptr;
    void (*UseProgram)(unsigned) = nullptr;
    void (*Uniform1i)(int32_t, int32_t) = nullptr;
    void (*Uniform1f)(int32_t, float) = nullptr;
    void (*EnableVertexAttribArray)(unsigned) = nullptr;
    void (*DisableVertexAttribArray)(unsigned) = nullptr;
    void (*VertexAttribPointer)(unsigned, int32_t, unsigned, unsigned char,
                                int32_t, const void *) = nullptr;
    void (*DrawArrays)(unsigned, int32_t, int32_t) = nullptr;
    void (*Viewport)(int32_t, int32_t, int32_t, int32_t) = nullptr;
    void (*ClearColor)(float, float, float, float) = nullptr;
    void (*Clear)(unsigned) = nullptr;
    void (*Enable)(unsigned) = nullptr;
    void (*Disable)(unsigned) = nullptr;
    void (*BlendFunc)(unsigned, unsigned) = nullptr;
    void (*DeleteShader)(unsigned) = nullptr;
    void (*DeleteProgram)(unsigned) = nullptr;
    void (*EGLImageTargetTexture2DOES)(unsigned, void *) = nullptr;

    bool ok = false;
};

host_gl g_gl;

/* ==========================================================================
 * 1. 状态
 * ========================================================================== */

struct tex_slot {
    bool used = false;
    uint64_t id = 0;
    int w = 0;
    int h = 0;
    int hal_format = 0;
    uint64_t modifier = 0;
    void *img = nullptr;
    unsigned tex = 0;
    unsigned long seq = 0;
};

struct dead_obj {
    void *img = nullptr;
    unsigned tex = 0;
    unsigned long seq = 0;
};

constexpr int kMaxTexCache = 16;
constexpr int kMaxDead = 64;

/* 一层待绘制（x11_hybris_layer 的内部副本，入队时拷贝，不再依赖调用方内存） */
struct host_layer {
    int fd = -1;
    uint64_t id = 0;
    uint64_t modifier = 0;
    int buf_w = 0, buf_h = 0;
    int stride_px = 0;
    int hal_format = 0;
    int src_x = 0, src_y = 0, src_w = 0, src_h = 0;
    int dst_x = 0, dst_y = 0, dst_w = 0, dst_h = 0;
    uint32_t transform = 0;
    bool opaque = false;
};

struct host_state {
    bool tried = false;
    bool ok = false;

    void *dpy = nullptr;  /* glibc 的 Display*，只在 glibc 域内使用 */
    void *edpy = nullptr; /* EGLDisplay */
    void *cfg = nullptr;
    void *ctx = nullptr;
    /* 当前 EGLConfig 是按哪个 window visual 挑的（见 reinit_gl_for_window） */
    int cfg_visual = 0;

    void *cur_surf = nullptr;
    uint32_t cur_win = 0;
    /* 当前把上面那个 surface 绑成 current 的线程。EGL 的 current 是线程局部的，
     * 只记 surface 不记线程会在跨线程时误判「已经 current 过」。 */
    pid_t cur_tid = 0;

    unsigned prog = 0;
    int a_pos = -1, a_uv = -1, u_tex = -1, u_swizzle = -1, u_force_opaque = -1;

    /* 不透明底（见 kBaseVert/kBaseFrag，照 x11-egl-sphere 的第 0 层） */
    unsigned base_prog = 0;
    int base_a_pos = -1;

    tex_slot tex[kMaxTexCache];
    dead_obj dead[kMaxDead];
    int dead_n = 0;

    unsigned long frame_seq = 0;
};

struct host_win {
    std::vector<host_layer> pending;
    void *surf = nullptr;
    int w = 0;
    int h = 0;
    /* eglSwapBuffers 失败后重建 surface 的次数（自愈，最多 3 次防死循环） */
    int rebuild = 0;
    /* 是否已经成功 swap 过一帧（只用来打一行「真上屏了」的日志） */
    bool swapped_once = false;
};

/* 当前线程 id（bionic 的 gettid() 版本较新，统一走 syscall 保证可用） */
pid_t current_tid()
{
    return (pid_t)syscall(__NR_gettid);
}

std::mutex g_lock;
std::map<struct display *, host_state *> g_states;
std::map<struct window *, host_win *> g_wins;

/* ==========================================================================
 * 2. 加载宿主库
 * ========================================================================== */

 #define GLIBC_X11_LIB_NAME "libX11.so.6"
 #define GLIBC_EGL_LIB_NAME "libEGL.so.1"
 #define GLIBC_GLESC2_LIB_NAME "libGLESv2.so.2"

bool load_host_libs(void)
{
    if (g_api.loaded)
        return g_api.ok;
    g_api.loaded = true;

    /*
     * 顺序很重要：先 libX11，再 libEGL。
     *
     * glibc libEGL 的 NEEDED 里有 libX11.so.6；先加载它，loader 会按 soname
     * 复用同一份，于是 eglGetPlatformDisplay 内部用的 libX11 与我们 dlsym 到的
     * 是同一个实例 —— 这是「不跨 libc」能成立的前提。
     *
     * RTLD_LOCAL 必须保留：bionic 侧也有一份 libX11，不能让 glibc 的符号进入
     * 全局作用域，否则 XRender 那批老调用会被劫持到 glibc 版本。
     */
    g_api.lx11 = dlopen(GLIBC_X11_LIB_NAME, RTLD_NOW | RTLD_LOCAL);
    if (!g_api.lx11) {
        ALOGE("load_host_libs: dlopen(%s) fail: %s", GLIBC_X11_LIB_NAME, dlerror());
        return false;
    }
    g_api.legl = dlopen(GLIBC_EGL_LIB_NAME, RTLD_NOW | RTLD_LOCAL);
    if (!g_api.legl) {
        ALOGE("load_host_libs: dlopen(%s) fail: %s", GLIBC_EGL_LIB_NAME, dlerror());
        return false;
    }
    g_api.lgles = dlopen(GLIBC_GLESC2_LIB_NAME, RTLD_NOW | RTLD_LOCAL);
    if (!g_api.lgles) {
        ALOGE("load_host_libs: dlopen(%s) fail: %s", GLIBC_GLESC2_LIB_NAME, dlerror());
        return false;
    }

#define LOAD_X11(field, sym)                                        \
    do {                                                            \
        g_api.field = (decltype(g_api.field))dlsym(g_api.lx11, sym);  \
        if (!g_api.field) {                                         \
            ALOGE("load_host_libs: libX11 cant found %s", sym);                      \
            return false;                                           \
        }                                                           \
    } while (0)

    LOAD_X11(XOpenDisplay, "XOpenDisplay");
    LOAD_X11(XCloseDisplay, "XCloseDisplay");
    LOAD_X11(XSync, "XSync");

#define LOAD_EGL(field, sym)                                        \
    do {                                                            \
        g_api.field = (decltype(g_api.field))dlsym(g_api.legl, sym);  \
        if (!g_api.field) {                                         \
            ALOGE("x11_hybris: libEGL cant found %s", sym);                      \
            return false;                                           \
        }                                                           \
    } while (0)

    LOAD_EGL(GetDisplay, "eglGetDisplay");
    LOAD_EGL(Initialize, "eglInitialize");
    LOAD_EGL(Terminate, "eglTerminate");
    LOAD_EGL(QueryString, "eglQueryString");
    LOAD_EGL(GetConfigs, "eglGetConfigs");
    LOAD_EGL(GetConfigAttrib, "eglGetConfigAttrib");
    LOAD_EGL(CreateWindowSurface, "eglCreateWindowSurface");
    LOAD_EGL(DestroySurface, "eglDestroySurface");
    LOAD_EGL(CreateContext, "eglCreateContext");
    LOAD_EGL(DestroyContext, "eglDestroyContext");
    LOAD_EGL(MakeCurrent, "eglMakeCurrent");
    LOAD_EGL(SwapBuffers, "eglSwapBuffers");
    LOAD_EGL(GetError, "eglGetError");
    LOAD_EGL(BindAPI, "eglBindAPI");
    LOAD_EGL(GetProcAddress, "eglGetProcAddress");

#undef LOAD_EGL
#undef LOAD_X11

    /* 可选入口：没有时退回 eglGetDisplay */
    g_api.GetPlatformDisplayEXT = (decltype(g_api.GetPlatformDisplayEXT))dlsym(
        g_api.legl, "eglGetPlatformDisplayEXT");
    if (!g_api.GetPlatformDisplayEXT) {
        g_api.GetPlatformDisplayEXT =
            (decltype(g_api.GetPlatformDisplayEXT))g_api.GetProcAddress(
                "eglGetPlatformDisplayEXT");
    }

#define LOAD_GL(field, sym)                                         \
    do {                                                             \
        g_gl.field = (decltype(g_gl.field))dlsym(g_api.lgles, sym);   \
        if (!g_gl.field) {                                           \
            ALOGE("x11_hybris: libGLESv2 cant found %s", sym);                    \
            return false;                                            \
        }                                                            \
    } while (0)

    LOAD_GL(GenTextures, "glGenTextures");
    LOAD_GL(DeleteTextures, "glDeleteTextures");
    LOAD_GL(BindTexture, "glBindTexture");
    LOAD_GL(ActiveTexture, "glActiveTexture");
    LOAD_GL(TexParameteri, "glTexParameteri");
    LOAD_GL(CreateShader, "glCreateShader");
    LOAD_GL(ShaderSource, "glShaderSource");
    LOAD_GL(CompileShader, "glCompileShader");
    LOAD_GL(GetShaderiv, "glGetShaderiv");
    LOAD_GL(GetShaderInfoLog, "glGetShaderInfoLog");
    LOAD_GL(CreateProgram, "glCreateProgram");
    LOAD_GL(AttachShader, "glAttachShader");
    LOAD_GL(LinkProgram, "glLinkProgram");
    LOAD_GL(GetProgramiv, "glGetProgramiv");
    LOAD_GL(GetProgramInfoLog, "glGetProgramInfoLog");
    LOAD_GL(GetAttribLocation, "glGetAttribLocation");
    LOAD_GL(GetUniformLocation, "glGetUniformLocation");
    LOAD_GL(UseProgram, "glUseProgram");
    LOAD_GL(Uniform1i, "glUniform1i");
    LOAD_GL(Uniform1f, "glUniform1f");
    LOAD_GL(EnableVertexAttribArray, "glEnableVertexAttribArray");
    LOAD_GL(DisableVertexAttribArray, "glDisableVertexAttribArray");
    LOAD_GL(VertexAttribPointer, "glVertexAttribPointer");
    LOAD_GL(DrawArrays, "glDrawArrays");
    LOAD_GL(Viewport, "glViewport");
    LOAD_GL(ClearColor, "glClearColor");
    LOAD_GL(Clear, "glClear");
    LOAD_GL(Enable, "glEnable");
    LOAD_GL(Disable, "glDisable");
    LOAD_GL(BlendFunc, "glBlendFunc");
    LOAD_GL(DeleteShader, "glDeleteShader");
    LOAD_GL(DeleteProgram, "glDeleteProgram");

#undef LOAD_GL

    g_api.CreateImageKHR = (decltype(g_api.CreateImageKHR))g_api.GetProcAddress(
        "eglCreateImageKHR");
    g_api.DestroyImageKHR = (decltype(g_api.DestroyImageKHR))g_api.GetProcAddress(
        "eglDestroyImageKHR");
    if (!g_api.CreateImageKHR || !g_api.DestroyImageKHR) {
        ALOGE("x11_hybris: cant found eglCreateImageKHR / eglDestroyImageKHR");
        return false;
    }

    g_gl.EGLImageTargetTexture2DOES =
        (decltype(g_gl.EGLImageTargetTexture2DOES))g_api.GetProcAddress(
            "glEGLImageTargetTexture2DOES");
    if (!g_gl.EGLImageTargetTexture2DOES) {
        ALOGE("x11_hybris: cant found glEGLImageTargetTexture2DOES");
        return false;
    }

    g_gl.ok = true;
    g_api.ok = true;
    ALOGI("x11_hybris: load success (%s / %s / %s)", GLIBC_X11_LIB_NAME, GLIBC_EGL_LIB_NAME, GLIBC_GLESC2_LIB_NAME);
    return true;
}

/* ==========================================================================
 * 3. 工具
 * ========================================================================== */

const char *egl_str(int32_t err)
{
    switch (err) {
    case 0x3000: return "EGL_SUCCESS";
    case 0x3001: return "EGL_NOT_INITIALIZED";
    case 0x3002: return "EGL_BAD_ACCESS";
    case 0x3003: return "EGL_BAD_ALLOC";
    case 0x3004: return "EGL_BAD_ATTRIBUTE";
    case 0x3005: return "EGL_BAD_CONFIG";
    case 0x3006: return "EGL_BAD_CONTEXT";
    case 0x3007: return "EGL_BAD_CURRENT_SURFACE";
    case 0x3008: return "EGL_BAD_DISPLAY";
    case 0x3009: return "EGL_BAD_MATCH";
    case 0x300A: return "EGL_BAD_NATIVE_PIXMAP";
    case 0x300B: return "EGL_BAD_NATIVE_WINDOW";
    case 0x300C: return "EGL_BAD_PARAMETER";
    case 0x300D: return "EGL_BAD_SURFACE";
    case 0x300E: return "EGL_CONTEXT_LOST";
    default: return "EGL_?";
    }
}

/* HAL_PIXEL_FORMAT_* -> DRM fourcc（eglCreateImageKHR 的 EGL_LINUX_DRM_FOURCC_EXT） */
uint32_t hal_to_fourcc(int hal)
{
    switch (hal) {
    case 1: return DRM_FORMAT_ABGR8888;      /* RGBA_8888 */
    case 2: return DRM_FORMAT_XBGR8888;      /* RGBX_8888 */
    case 3: return DRM_FORMAT_BGR888;        /* RGB_888 */
    case 4: return DRM_FORMAT_RGB565;        /* RGB_565 */
    case 5: return DRM_FORMAT_ARGB8888;      /* BGRA_8888 */
    case 0x16: return DRM_FORMAT_ABGR16161616F; /* RGBA_FP16 */
    case 0x2b: return DRM_FORMAT_ABGR2101010;   /* RGBA_1010102 */
    default: return 0;
    }
}

int bytes_per_pixel(int hal)
{
    switch (hal) {
    case 3: return 3;    /* RGB_888 */
    case 4: return 2;    /* RGB_565 */
    case 0x16: return 8; /* RGBA_FP16 */
    default: return 4;
    }
}

/* ==========================================================================
 * 4. 着色器
 * ========================================================================== */

const char *kVert =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

const char *kFrag =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform float u_swizzle;\n"
    "uniform float u_force_opaque;\n"
    "void main() {\n"
    "    vec4 c = texture2D(u_tex, v_uv);\n"
    "    if (u_swizzle > 0.5)\n"
    "        c = vec4(c.b, c.g, c.r, c.a);\n"
    "    if (u_force_opaque > 0.5)\n"
    "        c.a = 1.0;\n"
    "    gl_FragColor = c;\n"
    "}\n";

/*
 * 「不透明底」—— 每帧第一件事，铺满整个窗口。
 *
 * 不会漏出任何 Linux 窗口。三个要点：
 *   1. 混合显式关掉（GL_BLEND off），片元的 rgb 和 a 都硬编码，**alpha 恒为 1.0**；
 *   2. 它走的是正常的光栅 + 片元输出路径，不是 glClear —— NVIDIA 上 glClear 写
 *      alpha 并不可靠（驱动可能只把 RGB 落进 hybrised buffer），而我们的 X 窗口
 *      是 32 位 ARGB visual，合成器按**每个像素的 alpha** 混合。只要某个像素
 *      alpha 不是 255，底下的 Linux 桌面 / 面板 / 其它 Linux 窗口就会从那一块
 *      透出来 —— 看起来就是"面板还在但点不到"、"切回 FDE 有窗口残影"。
 *   3. 它画在所有 layer 之前，所以后面无论怎么画、怎么漏 alpha，最坏也只是
 *      "颜色不对"，绝不会是"看得见桌面"。
 */
const char *kBaseVert =
    "attribute vec2 a_pos;\n"
    "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

const char *kBaseFrag =
    "precision mediump float;\n"
    "void main() { gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0); }\n";

unsigned compile_shader(unsigned type, const char *src)
{
    unsigned sh = g_gl.CreateShader(type);
    if (!sh)
        return 0;
    g_gl.ShaderSource(sh, 1, &src, nullptr);
    g_gl.CompileShader(sh);

    int32_t ok = 0;
    g_gl.GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        g_gl.GetShaderInfoLog(sh, sizeof(log), nullptr, log);
        ALOGE("x11_hybris: compile_shader fail: %s", log);
        g_gl.DeleteShader(sh);
        return 0;
    }
    return sh;
}

bool build_program(host_state *st)
{
    unsigned vs = compile_shader(GL_VERTEX_SHADER, kVert);
    unsigned fs = compile_shader(GL_FRAGMENT_SHADER, kFrag);
    if (!vs || !fs)
        return false;

    unsigned prog = g_gl.CreateProgram();
    g_gl.AttachShader(prog, vs);
    g_gl.AttachShader(prog, fs);
    g_gl.LinkProgram(prog);
    g_gl.DeleteShader(vs);
    g_gl.DeleteShader(fs);

    int32_t ok = 0;
    g_gl.GetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        g_gl.GetProgramInfoLog(prog, sizeof(log), nullptr, log);
        ALOGE("x11_hybris: GetProgramiv fail: %s", log);
        return false;
    }

    st->prog = prog;
    st->a_pos = g_gl.GetAttribLocation(prog, "a_pos");
    st->a_uv = g_gl.GetAttribLocation(prog, "a_uv");
    st->u_tex = g_gl.GetUniformLocation(prog, "u_tex");
    st->u_swizzle = g_gl.GetUniformLocation(prog, "u_swizzle");
    st->u_force_opaque = g_gl.GetUniformLocation(prog, "u_force_opaque");

    /* 不透明底的单色程序：和上面那套完全独立，最坏情况只是这层没画出来，
     * 不能让它的失败连带影响正常上屏。 */
    unsigned bvs = compile_shader(GL_VERTEX_SHADER, kBaseVert);
    unsigned bfs = compile_shader(GL_FRAGMENT_SHADER, kBaseFrag);
    if (bvs && bfs) {
        unsigned bp = g_gl.CreateProgram();
        g_gl.AttachShader(bp, bvs);
        g_gl.AttachShader(bp, bfs);
        g_gl.LinkProgram(bp);
        g_gl.DeleteShader(bvs);
        g_gl.DeleteShader(bfs);
        int32_t ok2 = 0;
        g_gl.GetProgramiv(bp, GL_LINK_STATUS, &ok2);
        if (ok2) {
            st->base_prog = bp;
            st->base_a_pos = g_gl.GetAttribLocation(bp, "a_pos");
        } else {
            g_gl.DeleteProgram(bp);
        }
    }

    return true;
}

/*
 * 第 0 层：铺满整窗的不透明底。
 *
 * 混合关闭、深度测试不参与、片元 alpha 硬编码 1.0 —— 画完这一层，整帧每个
 * 像素的 alpha 就一定是 1。后面所有 layer 画在它上面，最坏也只是颜色不对，
 * 不可能再让桌面/面板透出来。
 */
static const float kBaseQuad[] = {
    -1.0f, -1.0f,
     1.0f, -1.0f,
    -1.0f,  1.0f,
     1.0f,  1.0f,
};

static void draw_base(host_state *st)
{
    if (!st || !st->base_prog)
        return;

    g_gl.Disable(GL_BLEND);
    g_gl.UseProgram(st->base_prog);
    if (st->base_a_pos >= 0) {
        g_gl.EnableVertexAttribArray((unsigned)st->base_a_pos);
        g_gl.VertexAttribPointer((unsigned)st->base_a_pos, 2, GL_FLOAT, GL_FALSE,
                                 0, kBaseQuad);
    }
    g_gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (st->base_a_pos >= 0)
        g_gl.DisableVertexAttribArray((unsigned)st->base_a_pos);
}

/* ==========================================================================
 * 5. 初始化
 * ========================================================================== */

/*
 * 按窗口 visual 选 config。X11 下 config 的 EGL_NATIVE_VISUAL_ID 必须等于窗口
 * visual，否则 eglCreateWindowSurface 只会返回 EGL_BAD_MATCH(0x3009)。
 */
void *pick_config(struct display *display, host_state *st)
{
    int32_t total = 0;
    if (!g_api.GetConfigs(st->edpy, nullptr, 0, &total) || total <= 0) {
        const int32_t e = g_api.GetError();
        ALOGE("x11_hybris: eglGetConfigs fail 0x%x(%s)", e, egl_str(e));
        return nullptr;
    }

    void *cfgs[256];
    if (total > (int32_t)(sizeof(cfgs) / sizeof(cfgs[0])))
        total = (int32_t)(sizeof(cfgs) / sizeof(cfgs[0]));
    int32_t n = 0;
    if (!g_api.GetConfigs(st->edpy, cfgs, total, &n) || n <= 0)
        return nullptr;

    /*
     * 关键：必须按**窗口实际使用的 visual** 匹配 config。
     * 单窗口全屏时窗口是不透明的 24 位 visual（display->window_visualid），
     * 而 display->visualid 仍然是 ARGB32 —— 拿它去匹配会选到 0x23 的 config，
     * eglCreateWindowSurface 只会返回 EGL_BAD_MATCH。
     */
    const int want = (int)(display->window_visualid ? display->window_visualid
                                                    : display->visualid);
    /* 窗口带 alpha（ARGB32，subsurface / 多窗口模式）时优先挑带 alpha 的 config；
     * 不透明窗口则正好相反 —— 挑 alpha=0 的 config 才能和窗口 visual 对齐。 */
    const bool want_alpha = display->window_depth == 0 || display->window_depth >= 32;

    void *best = nullptr;
    int best_score = -1;
    int matched = 0;

    for (int32_t i = 0; i < n; i++) {
        int32_t surface_type = 0, visual = 0, renderable = 0, color_type = 0;
        int32_t alpha = 0, caveat = 0;

        if (!g_api.GetConfigAttrib(st->edpy, cfgs[i], EGL_SURFACE_TYPE, &surface_type) ||
            !(surface_type & EGL_WINDOW_BIT))
            continue;
        if (!g_api.GetConfigAttrib(st->edpy, cfgs[i], EGL_RENDERABLE_TYPE, &renderable) ||
            !(renderable & EGL_OPENGL_ES2_BIT))
            continue;
        if (!g_api.GetConfigAttrib(st->edpy, cfgs[i], EGL_NATIVE_VISUAL_ID, &visual))
            continue;
        if (visual != want)
            continue;

        matched++;
        if (!g_api.GetConfigAttrib(st->edpy, cfgs[i], EGL_COLOR_BUFFER_TYPE, &color_type) ||
            color_type != EGL_RGB_BUFFER)
            continue;

        int score = 0;
        g_api.GetConfigAttrib(st->edpy, cfgs[i], EGL_ALPHA_SIZE, &alpha);
        if (want_alpha ? (alpha == 8) : (alpha == 0))
            score += 4;
        g_api.GetConfigAttrib(st->edpy, cfgs[i], EGL_CONFIG_CAVEAT, &caveat);
        if (caveat == EGL_NONE)
            score += 2;
        if (score > best_score) {
            best_score = score;
            best = cfgs[i];
        }
    }

    if (!best) {
        ALOGE("x11_hybris: cant found native visual EGLConfig: 0x%03x (%d/%d)"
              " —— eglCreateWindowSurface 只会返回 EGL_BAD_MATCH",
              (unsigned)want, n, matched);
        return nullptr;
    }

    int32_t alpha = -1, cbuf = -1;
    g_api.GetConfigAttrib(st->edpy, best, EGL_ALPHA_SIZE, &alpha);
    g_api.GetConfigAttrib(st->edpy, best, EGL_BUFFER_SIZE, &cbuf);
    st->cfg_visual = want;
    return best;
}

/*
 * 给窗口挑一个「不透明」的 visual —— 这个 visual 必须同时满足：
 *   1. X 里真实存在，depth >= 24，TrueColor（没有 alpha 通道）；
 *   2. 存在一个 EGLConfig 的 EGL_NATIVE_VISUAL_ID 正好等于它，且该 config
 *      EGL_ALPHA_SIZE == 0、EGL_SURFACE_TYPE 带 WINDOW_BIT。
 *
 * 为什么要从 EGLConfig 反推，而不是直接用 X 的 DefaultVisual：
 * 实测 DefaultVisual 是 0x21，而 NVIDIA 的 EGL 只给 0x024 / 0x04f / 0x07a…
 * 这些 visual 出 config —— 直接拿 0x21 建窗，eglCreateWindowSurface 只有
 * EGL_BAD_MATCH。所以这里以 **EGLConfig 列表为准**，挑一个两边都认的 visual。
 *
 * 结果写进 display->window_visualid / window_depth，create_window 会用它建窗，
 * 于是窗口天然不带 alpha：合成器没有 alpha 可混，Linux 桌面/面板不可能透出来。
 */
static bool pick_opaque_window_visual(struct display *display, host_state *st)
{
    int32_t total = 0;
    if (!g_api.GetConfigs(st->edpy, nullptr, 0, &total) || total <= 0)
        return false;

    void *cfgs[256];
    if (total > (int32_t)(sizeof(cfgs) / sizeof(cfgs[0])))
        total = (int32_t)(sizeof(cfgs) / sizeof(cfgs[0]));
    int32_t n = 0;
    if (!g_api.GetConfigs(st->edpy, cfgs, total, &n) || n <= 0)
        return false;

    int best_visual = 0, best_depth = 0, best_score = -1;
    for (int32_t i = 0; i < n; i++) {
        int32_t stype = 0, rt = 0, ctype = 0, alpha = 0, visual = 0, buf = 0;
        if (!g_api.GetConfigAttrib(st->edpy, cfgs[i], EGL_SURFACE_TYPE, &stype) ||
            !(stype & EGL_WINDOW_BIT))
            continue;
        if (!g_api.GetConfigAttrib(st->edpy, cfgs[i], EGL_RENDERABLE_TYPE, &rt) ||
            !(rt & EGL_OPENGL_ES2_BIT))
            continue;
        if (!g_api.GetConfigAttrib(st->edpy, cfgs[i], EGL_COLOR_BUFFER_TYPE, &ctype) ||
            ctype != EGL_RGB_BUFFER)
            continue;
        /* 只要不带 alpha 的 config —— 这是"窗口不透明"的唯一前提 */
        if (!g_api.GetConfigAttrib(st->edpy, cfgs[i], EGL_ALPHA_SIZE, &alpha) || alpha != 0)
            continue;
        if (!g_api.GetConfigAttrib(st->edpy, cfgs[i], EGL_NATIVE_VISUAL_ID, &visual) ||
            !visual)
            continue;
        g_api.GetConfigAttrib(st->edpy, cfgs[i], EGL_BUFFER_SIZE, &buf);
        if (buf < 24)
            continue;

        XVisualInfo vt;
        memset(&vt, 0, sizeof(vt));
        vt.visualid = (VisualID)visual;
        int nvi = 0;
        XVisualInfo *vi = XGetVisualInfo(display->x11display, VisualIDMask, &vt, &nvi);
        if (!vi)
            continue; /* EGL 报的 visual 在 X 里不存在，跳过 */
        int vdepth = vi->depth;
        int vclass = vi->c_class;
        XFree(vi);
        if (vdepth < 24 || vclass != TrueColor)
            continue;

        int score = (buf >= 32 ? 2 : 0) + (vdepth == 24 ? 2 : 0);
        if (score > best_score) {
            best_score = score;
            best_visual = visual;
            best_depth = vdepth;
        }
    }

    if (!best_visual) {
        ALOGW("x11_hybris: cant found best_visual");
        return false;
    }

    display->window_visualid = (xcb_visualid_t)best_visual;
    display->window_depth = best_depth;
    return true;
}

bool init_state(struct display *display, host_state *st)
{
    if (!load_host_libs())
        return false;

    /*
     * 关键一步：Display* 必须由 glibc 的 libX11 自己开。
     * 传 bionic 的 display->x11display 会立刻踩跨 libc 的 _XReply assert。
     */
    const char *name = getenv("DISPLAY");
    st->dpy = g_api.XOpenDisplay(name && name[0] ? name : nullptr);
    if (!st->dpy) {
        ALOGE("x11_hybris: glibc XOpenDisplay(%s) fail", name ? name : "(null)");
        return false;
    }

    const int32_t screen = display->screen_default_nbr;
    if (g_api.GetPlatformDisplayEXT) {
        const int32_t attrs[] = {EGL_PLATFORM_X11_SCREEN_KHR, screen, EGL_NONE};
        st->edpy = g_api.GetPlatformDisplayEXT(EGL_PLATFORM_X11_KHR, st->dpy, attrs);
        if (st->edpy) {
            ALOGI("x11_hybris: display = eglGetPlatformDisplayEXT(X11, screen=%d)", screen);
        } else {
            const int32_t e = g_api.GetError();
            ALOGW("x11_hybris: eglGetPlatformDisplayEXT(X11) fail 0x%x(%s)",
                  e, egl_str(e));
        }
    }
    if (!st->edpy)
        st->edpy = g_api.GetDisplay(st->dpy);
    if (!st->edpy) {
        const int32_t e = g_api.GetError();
        ALOGE("x11_hybris: eglGetDisplay fail 0x%x(%s)", e, egl_str(e));
        return false;
    }

    int32_t major = 0, minor = 0;
    if (!g_api.Initialize(st->edpy, &major, &minor)) {
        const int32_t e = g_api.GetError();
        ALOGE("x11_hybris: eglInitialize fail 0x%x(%s)", e, egl_str(e));
        return false;
    }

    const char *exts = g_api.QueryString(st->edpy, EGL_EXTENSIONS);
    const bool has_dmabuf = exts && strstr(exts, "EGL_EXT_image_dma_buf_import");
    const bool has_mod = exts && strstr(exts, "EGL_EXT_image_dma_buf_import_modifiers");
    if (!has_dmabuf) {
        ALOGE("x11_hybris: cant found EGL_EXT_image_dma_buf_import");
        return false;
    }

    /*
     * 建窗口之前，先把「用哪个 visual 建窗」定下来。
     *
     * 顺序很重要：init_state 在 create_window **之前**跑，所以这里定下来的
     * visual 会被 x11-hwc 的 create_window 直接采用；否则那边只能退回
     * DefaultVisual（实测 0x21），而 NVIDIA 的 EGL 未必有 0x21 的 config，
     * 结果就是 eglCreateWindowSurface 一路 EGL_BAD_MATCH。
     */
    if (display->window_visualid == 0 && !display->multi_windows) {
        pick_opaque_window_visual(display, st);
    }

    void *cfg = pick_config(display, st);
    if (!cfg)
        return false;

    if (!g_api.BindAPI(EGL_OPENGL_ES_API)) {
        ALOGE("x11_hybris: eglBindAPI(ES) fail");
        return false;
    }
    const int32_t ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    void *ctx = g_api.CreateContext(st->edpy, cfg, nullptr, ctx_attrs);
    if (!ctx) {
        const int32_t e = g_api.GetError();
        ALOGE("x11_hybris: eglCreateContext fail 0x%x(%s)", e, egl_str(e));
        return false;
    }

    st->cfg = cfg;
    st->ctx = ctx;
    return true;
}

/* ==========================================================================
 * 6. 窗口 surface
 * ========================================================================== */

/* 窗口尺寸：优先问 X server（XID 是全局资源，glibc 连接也能查） */
void window_geometry(struct display *display, struct window *window, int *w, int *h)
{
    *w = display->width > 0 ? display->width : 1;
    *h = display->height > 0 ? display->height : 1;

    xcb_connection_t *conn = display->xcbconnection;
    if (conn && window->xcbwindow) {
        xcb_get_geometry_reply_t *rep = xcb_get_geometry_reply(
            conn, xcb_get_geometry(conn, window->xcbwindow), nullptr);
        if (rep) {
            if (rep->width && rep->height) {
                *w = rep->width;
                *h = rep->height;
            }
            free(rep);
        }
    }
}

/*
 * 窗口 visual 和当前 EGLConfig 不一致时，重建 config + context。
 *
 * 触发场景：init_state 发生在 create_window 之前，那时只能按「已经定下来的
 * window visual」挑 config；一旦窗口最终的 visual 变了（切多窗口模式、host 挑的
 * visual 被 create_window 覆盖等），eglCreateWindowSurface 只会给
 * EGL_BAD_MATCH。这里按窗口的真实 visual 重新挑，并重建 GL 侧对象。
 *
 * 只在启动期发生一次：此时 program 还没建、纹理缓存还是空的，作废代价为零。
 */
static bool reinit_gl_for_window(struct display *display, host_state *st)
{
    int32_t want = (int32_t)display->window_visualid;
    if (!want || want == st->cfg_visual)
        return false;

    g_api.MakeCurrent(st->edpy, nullptr, nullptr, nullptr);

    void *cfg = pick_config(display, st);
    if (!cfg)
        return false;

    const int32_t ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    void *ctx = g_api.CreateContext(st->edpy, cfg, nullptr, ctx_attrs);
    if (!ctx) {
        const int32_t e = g_api.GetError();
        ALOGE("x11_hybris: re-eglCreateContext fail 0x%x(%s)", e, egl_str(e));
        return false;
    }

    st->cfg = cfg;
    st->ctx = ctx;

    /* 旧 context 里的 program / 纹理全部作废（启动期一次性，不回收） */
    st->prog = 0;
    st->base_prog = 0;
    st->a_pos = st->a_uv = st->u_tex = st->u_swizzle = st->u_force_opaque = -1;
    st->base_a_pos = -1;
    for (int i = 0; i < kMaxTexCache; i++) {
        st->tex[i].used = false;
        st->tex[i].tex = 0;
        st->tex[i].img = nullptr;
    }
    st->dead_n = 0;
    return true;
}

bool ensure_surface(struct window *window, host_win *ws, host_state *st)
{
    struct display *display = window->display;

    if (!window->xcbwindow) {
        return false;
    }

    /*
     * 每次都重新 make current，不做「同一窗口连续帧就跳过」的缓存。
     *
     * EGL 的 current context / surface 是**线程局部**状态，而 hwc_set 由
     * SurfaceFlinger 经 hwbinder 调进来，同一个窗口的相邻两帧完全可能落在不同
     * 线程上。实测就踩了这个坑：线程 258 建好 surface 并 current、swap 正常，
     * 下一帧落到线程 257 时旧代码看到「cur_win 没变」直接返回 true，跳过
     * MakeCurrent —— 该线程上其实没有 current context，GL 调用全部无效，
     * eglSwapBuffers 报 0x300d EGL_BAD_SURFACE，画面永远黑。
     *
     * 代价只有每帧一次 eglMakeCurrent（驱动里是几十微秒量级），换来的是与
     * SurfaceFlinger 线程池调度无关的正确性。
     */
    if (!ws->surf) {
        /* 建窗前把刚才攒下的 X 请求推出去，XID 必须已经存在于 server */
        xcb_flush(display->xcbconnection);

        int32_t e = EGL_SUCCESS;
        ws->surf = g_api.CreateWindowSurface(
            st->edpy, st->cfg, (unsigned long)window->xcbwindow, nullptr);
        if (!ws->surf) {
            e = g_api.GetError();
            /* config 与窗口 visual 不符时自愈一次：按窗口真实 visual 重挑 config */
            if (e == EGL_BAD_MATCH && reinit_gl_for_window(display, st)) {
                ws->surf = g_api.CreateWindowSurface(st->edpy, st->cfg,
                                                     (unsigned long)window->xcbwindow,
                                                     nullptr);
                if (ws->surf)
                    ALOGI("x11_hybris: eglCreateWindowSurface success, window_visualid:0x%03x",
                          (unsigned)display->window_visualid);
                else
                    e = g_api.GetError();
            }
        }
        if (!ws->surf) {
            ALOGE("x11_hybris: eglCreateWindowSurface(0x%08x, visual=0x%03x) fail "
                  "0x%x(%s)%s",
                  window->xcbwindow,
                  (unsigned)(display->window_visualid ? display->window_visualid
                                                      : display->visualid),
                  e, egl_str(e),
                  e == EGL_BAD_MATCH ? " —— config not match visual" : "");
            return false;
        }
        window_geometry(display, window, &ws->w, &ws->h);
    }

    if (!g_api.MakeCurrent(st->edpy, ws->surf, ws->surf, st->ctx)) {
        int32_t e = g_api.GetError();
        ALOGE("x11_hybris: eglMakeCurrent fail 0x%x(%s) (tid=%d win=0x%08x)", e,
              egl_str(e), (int)current_tid(), window->xcbwindow);
        return false;
    }
    st->cur_surf = ws->surf;
    st->cur_win = (uint32_t)window->xcbwindow;
    st->cur_tid = current_tid();

    if (!st->prog && !build_program(st))
        return false;

    return true;
}

/* ==========================================================================
 * 7. 纹理缓存
 * ========================================================================== */

void *import_dmabuf(host_state *st, const host_layer *L)
{
    const uint32_t fourcc = hal_to_fourcc(L->hal_format);
    if (!fourcc) {
        static int warned = 0;
        if (warned++ < 8)
            ALOGW("x11_hybris: cant support HAL format 0x%x", L->hal_format);
        return nullptr;
    }

    int32_t attrs[24];
    int n = 0;
    attrs[n++] = EGL_WIDTH;
    attrs[n++] = L->buf_w;
    attrs[n++] = EGL_HEIGHT;
    attrs[n++] = L->buf_h;
    attrs[n++] = EGL_LINUX_DRM_FOURCC_EXT;
    attrs[n++] = (int32_t)fourcc;
    attrs[n++] = EGL_DMA_BUF_PLANE0_FD_EXT;
    attrs[n++] = L->fd;
    attrs[n++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
    attrs[n++] = 0;
    attrs[n++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
    attrs[n++] = L->stride_px * bytes_per_pixel(L->hal_format);
    attrs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
    attrs[n++] = (int32_t)(L->modifier & 0xffffffffu);
    attrs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
    attrs[n++] = (int32_t)(L->modifier >> 32);
    attrs[n++] = EGL_NONE;

    void *img = g_api.CreateImageKHR
                    ? g_api.CreateImageKHR(st->edpy, nullptr, EGL_LINUX_DMA_BUF_EXT,
                                           nullptr, attrs)
                    : nullptr;
    if (!img) {
        static int warned = 0;
        if (warned++ < 8) {
            const int32_t e = g_api.GetError();
            ALOGE("x11_hybris: eglCreateImageKHR fail 0x%x(%s) (fmt=0x%x %dx%d stride=%d "
                  "modifier=0x%llx)",
                  e, egl_str(e), fourcc, L->buf_w, L->buf_h, L->stride_px,
                  (unsigned long long)L->modifier);
        }
    }
    return img;
}

void retire(host_state *st, tex_slot *e)
{
    if (!e->used)
        return;

    if (st->dead_n < kMaxDead) {
        st->dead[st->dead_n].img = e->img;
        st->dead[st->dead_n].tex = e->tex;
        st->dead[st->dead_n].seq = st->frame_seq + 2;
        st->dead_n++;
    } else {
        if (e->tex)
            g_gl.DeleteTextures(1, &e->tex);
        if (e->img && g_api.DestroyImageKHR)
            g_api.DestroyImageKHR(st->edpy, e->img);
    }
    e->used = false;
    e->tex = 0;
    e->img = nullptr;
}

void reap(host_state *st)
{
    int w = 0;
    for (int i = 0; i < st->dead_n; i++) {
        if (st->dead[i].seq > st->frame_seq) {
            st->dead[w++] = st->dead[i];
            continue;
        }
        if (st->dead[i].tex)
            g_gl.DeleteTextures(1, &st->dead[i].tex);
        if (st->dead[i].img && g_api.DestroyImageKHR)
            g_api.DestroyImageKHR(st->edpy, st->dead[i].img);
    }
    st->dead_n = w;
}

bool bind_texture(host_state *st, const host_layer *L)
{
    tex_slot *free_slot = nullptr;
    tex_slot *victim = nullptr;

    if (L->id) {
        for (int i = 0; i < kMaxTexCache; i++) {
            tex_slot *e = &st->tex[i];
            if (!e->used) {
                if (!free_slot)
                    free_slot = e;
                continue;
            }
            if (e->id == L->id) {
                if (e->w != L->buf_w || e->h != L->buf_h ||
                    e->hal_format != L->hal_format || e->modifier != L->modifier) {
                    retire(st, e);
                    if (!free_slot)
                        free_slot = e;
                    continue;
                }
                e->seq = st->frame_seq;
                g_gl.BindTexture(GL_TEXTURE_2D, e->tex);
                return true;
            }
            if (!victim || e->seq < victim->seq)
                victim = e;
        }
    }

    tex_slot *e = free_slot;
    if (!e) {
        if (!victim)
            return false;
        retire(st, victim);
        e = victim;
    }

    void *img = import_dmabuf(st, L);
    if (!img)
        return false;

    unsigned tex = 0;
    g_gl.GenTextures(1, &tex);
    if (!tex) {
        if (g_api.DestroyImageKHR)
            g_api.DestroyImageKHR(st->edpy, img);
        return false;
    }
    g_gl.BindTexture(GL_TEXTURE_2D, tex);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g_gl.EGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);

    *e = tex_slot{};
    e->used = true;
    e->id = L->id;
    e->w = L->buf_w;
    e->h = L->buf_h;
    e->hal_format = L->hal_format;
    e->modifier = L->modifier;
    e->img = img;
    e->tex = tex;
    e->seq = st->frame_seq;
    return true;
}
/* ==========================================================================
 * 8. 绘制
 * ========================================================================== */

/* HWC_TRANSFORM_* 位组合 -> 目标第 i 个角取源第 m[i] 个角（与 XRender 路径同一套角定义） */
const int kCornerMap[8][4] = {
    {0, 1, 2, 3}, /* 0x0 NONE */
    {1, 0, 3, 2}, /* 0x1 FLIP_H */
    {3, 2, 1, 0}, /* 0x2 FLIP_V */
    {2, 3, 0, 1}, /* 0x3 ROT_180 */
    {3, 0, 1, 2}, /* 0x4 ROT_90 (CW) */
    {0, 3, 2, 1}, /* 0x5 FLIP_H_ROT_90 */
    {1, 2, 3, 0}, /* 0x6 FLIP_V_ROT_90 */
    {1, 2, 3, 0}, /* 0x7 ROT_270 */
};

void draw_layer(host_state *st, const host_layer *L, int W, int H)
{
    if (!bind_texture(st, L))
        return;

    /*
     * 纹理坐标：EGLImage（dma-buf）的 t=0 对应缓冲第 0 行，即图像顶部 ——
     * 与 XRender 路径「pixmap 第 0 行贴到窗口顶部」一致，所以 t 不翻；
     * 竖直方向只由顶点 NDC 的 y 决定（GL 的 y 向上）。
     */
    const float s0 = (float)L->src_x / (float)L->buf_w;
    const float s1 = (float)(L->src_x + L->src_w) / (float)L->buf_w;
    const float t0 = (float)L->src_y / (float)L->buf_h;
    const float t1 = (float)(L->src_y + L->src_h) / (float)L->buf_h;
    const float cs[4] = {s0, s1, s1, s0};
    const float ct[4] = {t0, t0, t1, t1};

    const int *m = kCornerMap[L->transform & 0x7];
    const int dx[4] = {L->dst_x, L->dst_x + L->dst_w, L->dst_x + L->dst_w, L->dst_x};
    const int dy[4] = {L->dst_y, L->dst_y, L->dst_y + L->dst_h, L->dst_y + L->dst_h};

    float verts[4 * 4];
    for (int i = 0; i < 4; i++) {
        verts[i * 4 + 0] = 2.0f * (float)dx[i] / (float)W - 1.0f;
        verts[i * 4 + 1] = 1.0f - 2.0f * (float)dy[i] / (float)H;
        verts[i * 4 + 2] = cs[m[i]];
        verts[i * 4 + 3] = ct[m[i]];
    }

    g_gl.UseProgram(st->prog);
    g_gl.ActiveTexture(GL_TEXTURE0);
    if (st->u_tex >= 0)
        g_gl.Uniform1i(st->u_tex, 0);
    if (st->u_swizzle >= 0)
        g_gl.Uniform1f(st->u_swizzle, 0.0f);
    /*
     * 不透明层的片元**直接输出 alpha=1**。
     *
     * 这一步不能省：不透明层是「关混合」画的，dst 的每个通道（含 alpha）都被
     * 纹理值原样覆盖。Android 的缓冲区经常是 RGBA8888 但第 4 个字节没意义
     * （0 或垃圾值），一旦 alpha 变成 0，合成器就把底下的 Linux 桌面/面板混出来
     * —— 这正是"面板还在显示但点不到"、"切回 FDE 有窗口残影"的直接来源。
     * 只靠最后 clear 一遍 alpha 不够稳（NVIDIA 上 glClear 写 alpha 并不可靠），
     * 所以在源头就把它钉成 1。
     */
    if (st->u_force_opaque >= 0)
        g_gl.Uniform1f(st->u_force_opaque, L->opaque ? 1.0f : 0.0f);

    if (st->a_pos >= 0) {
        g_gl.EnableVertexAttribArray((unsigned)st->a_pos);
        g_gl.VertexAttribPointer((unsigned)st->a_pos, 2, GL_FLOAT, GL_FALSE,
                                 (int32_t)(4 * sizeof(float)), verts);
    }
    if (st->a_uv >= 0) {
        g_gl.EnableVertexAttribArray((unsigned)st->a_uv);
        g_gl.VertexAttribPointer((unsigned)st->a_uv, 2, GL_FLOAT, GL_FALSE,
                                 (int32_t)(4 * sizeof(float)), verts + 2);
    }

    if (L->opaque) {
        g_gl.Disable(GL_BLEND);
    } else {
        g_gl.Enable(GL_BLEND);
        /* 层内容是预乘 alpha，等价于 XRender 的 PictOpOver */
        g_gl.BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }

    g_gl.DrawArrays(GL_TRIANGLE_FAN, 0, 4);

    if (st->a_pos >= 0)
        g_gl.DisableVertexAttribArray((unsigned)st->a_pos);
    if (st->a_uv >= 0)
        g_gl.DisableVertexAttribArray((unsigned)st->a_uv);
}

/* ==========================================================================
 * 9. 状态管理
 * ========================================================================== */

/* 调用方必须持 g_lock */
host_state *state_of(struct display *display, bool create)
{
    auto it = g_states.find(display);
    if (it != g_states.end())
        return it->second;
    if (!create)
        return nullptr;
    host_state *st = new host_state();
    g_states[display] = st;
    return st;
}

/* 首次调用做完整探测；结果缓存。调用方必须持 g_lock */
host_state *get_ready(struct display *display)
{
    host_state *st = state_of(display, true);
    if (!st)
        return nullptr;
    if (!st->tried) {
        st->tried = true;
        st->ok = init_state(display, st);
    }
    return st->ok ? st : nullptr;
}

}  // namespace

/* ==========================================================================
 * 10. 对外接口（声明见 x11-egl.h）
 * ========================================================================== */

bool x11_hybris_get_ready(struct display *display)
{
    if (!display || !display->xcbconnection)
        return false;

    std::lock_guard<std::mutex> lock(g_lock);
    return get_ready(display) != nullptr;
}

bool x11_hybris_add_layer(struct window *window,
                           const struct x11_hybris_layer *layer)
{
    if (!window || !window->display || !layer)
        return false;
    if (layer->dmabuf_fd < 0) {
        return false;
    }
    if (layer->buf_w <= 0 || layer->buf_h <= 0 || layer->src_w <= 0 ||
        layer->src_h <= 0 || layer->dst_w <= 0 || layer->dst_h <= 0)
        return false;

    std::lock_guard<std::mutex> lock(g_lock);
    if (!get_ready(window->display))
        return false;

    host_win *ws = nullptr;
    auto it = g_wins.find(window);
    if (it != g_wins.end()) {
        ws = it->second;
    } else {
        ws = new host_win();
        g_wins[window] = ws;
    }

    host_layer L;
    L.fd = layer->dmabuf_fd;
    L.id = layer->buffer_id;
    L.modifier = layer->modifier;
    L.buf_w = layer->buf_w;
    L.buf_h = layer->buf_h;
    L.stride_px = layer->stride_px > 0 ? layer->stride_px : layer->buf_w;
    L.hal_format = layer->hal_format;
    L.src_x = layer->src_x;
    L.src_y = layer->src_y;
    L.src_w = layer->src_w;
    L.src_h = layer->src_h;
    L.dst_x = layer->dst_x;
    L.dst_y = layer->dst_y;
    L.dst_w = layer->dst_w;
    L.dst_h = layer->dst_h;
    L.transform = layer->transform;
    L.opaque = layer->opaque;

    ws->pending.push_back(L);
    return true;
}

void x11_hybris_flush(struct window *window)
{
    if (!window || !window->display)
        return;

    std::lock_guard<std::mutex> lock(g_lock);

    auto it = g_wins.find(window);
    if (it == g_wins.end() || !it->second)
        return;
    host_win *ws = it->second;
    if (ws->pending.empty())
        return;

    host_state *st = get_ready(window->display);
    if (!st) {
        ws->pending.clear();
        return;
    }
    if (!ensure_surface(window, ws, st)) {
        ws->pending.clear();
        return;
    }

    st->frame_seq++;
    reap(st);

    /*
     * 窗口尺寸可能被 WM 改（进出全屏、切工作区、面板重新占位）。surface 的
     * 尺寸是建 surface 那一刻定下来的，不跟着变的话 viewport 与顶点 NDC 换算
     * 都按旧尺寸算，画面会被拉伸或挤到一角。每秒核对一次（60 帧），变了就
     * 重建 surface，本帧丢弃。
     */
    if (ws->surf && (st->frame_seq % 60) == 0) {
        int nw = 0, nh = 0;
        window_geometry(window->display, window, &nw, &nh);
        if (nw != ws->w || nh != ws->h) {
            ALOGI("x11_hybris: window:0x%08x size:%dx%d -> %dx%d, create surface",
                  window->xcbwindow, ws->w, ws->h, nw, nh);
            g_api.MakeCurrent(st->edpy, nullptr, nullptr, nullptr);
            st->cur_surf = nullptr;
            st->cur_win = 0;
            st->cur_tid = 0;
            g_api.DestroySurface(st->edpy, ws->surf);
            ws->surf = nullptr;
            ws->w = nw;
            ws->h = nh;
            ws->pending.clear();
            return;
        }
    }

    g_gl.Viewport(0, 0, ws->w, ws->h);
    g_gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    g_gl.Clear(GL_COLOR_BUFFER_BIT);

    /* 第 0 层：不透明底（照 tools/x11-egl-sphere 的流程）。
     * 必须在所有 layer 之前画，保证整帧 alpha 恒为 1。 */
    draw_base(st);

    for (size_t i = 0; i < ws->pending.size(); i++)
        draw_layer(st, &ws->pending[i], ws->w, ws->h);
    ws->pending.clear();

    if (!g_api.SwapBuffers(st->edpy, ws->surf)) {
        int32_t e = g_api.GetError();
        /*
         * 自愈：窗口被 WM 重新 map / 改尺寸 / 换 visual 之后，旧的 EGLSurface
         * 可能已经和 server 侧脱钩。销毁它，下一帧 ensure_surface 会重建。
         * 限次数，避免在真的渲染不了的情况下每帧重建 surface。
         */
        if (ws->rebuild < 3) {
            ws->rebuild++;
            g_api.MakeCurrent(st->edpy, nullptr, nullptr, nullptr);
            st->cur_surf = nullptr;
            st->cur_win = 0;
            st->cur_tid = 0;
            g_api.DestroySurface(st->edpy, ws->surf);
            ws->surf = nullptr;
            return;
        }
    } else {
        /* 能 swap 说明 surface 是好的：自愈计数清零，下次真出问题还能重建 */
        ws->rebuild = 0;
        if (!ws->swapped_once) {
            ws->swapped_once = true;
        }
    }

    /*
     * 解绑。两个理由：
     *   1) surface 不能同时 current 在两个线程上，下一个线程 make current 同一
     *      surface 会拿到 EGL_BAD_ACCESS；
     *   2) 让「下一帧可能落在别的线程」这件事天然成立，不依赖 tid 缓存判断。
     */
    g_api.MakeCurrent(st->edpy, nullptr, nullptr, nullptr);
    st->cur_surf = nullptr;
    st->cur_win = 0;
    st->cur_tid = 0;
}

void x11_hybris_release_window(struct window *window)
{
    if (!window)
        return;

    std::lock_guard<std::mutex> lock(g_lock);

    auto it = g_wins.find(window);
    if (it == g_wins.end())
        return;
    host_win *ws = it->second;
    if (!ws)
        return;

    host_state *st = state_of(window->display, false);
    if (ws->surf && st && st->edpy) {
        if (st->cur_win == (uint32_t)window->xcbwindow) {
            g_api.MakeCurrent(st->edpy, nullptr, nullptr, nullptr);
            st->cur_win = 0;
            st->cur_surf = nullptr;
        }
        g_api.DestroySurface(st->edpy, ws->surf);
        ws->surf = nullptr;
    }

    delete ws;
    g_wins.erase(it);
}

/*
 * display 销毁：上下文/纹理/EGLImage 全部回收，最后关掉 glibc 侧那条 X 连接。
 * 必须在 bionic 的 xcb_disconnect / XCloseDisplay 之前调用（头文件已约定）。
 */
void x11_hybris_display_destroy(struct display *display)
{
    if (!display)
        return;

    std::lock_guard<std::mutex> lock(g_lock);

    auto it = g_states.find(display);
    if (it == g_states.end())
        return;
    host_state *st = it->second;
    g_states.erase(it);
    if (!st)
        return;

    if (st->edpy) {
        /* 无 surface 的 current 也能删对象 */
        g_api.MakeCurrent(st->edpy, nullptr, nullptr, st->ctx);

        for (int i = 0; i < kMaxTexCache; i++) {
            if (!st->tex[i].used)
                continue;
            if (st->tex[i].tex)
                g_gl.DeleteTextures(1, &st->tex[i].tex);
            if (st->tex[i].img && g_api.DestroyImageKHR)
                g_api.DestroyImageKHR(st->edpy, st->tex[i].img);
        }
        for (int i = 0; i < st->dead_n; i++) {
            if (st->dead[i].tex)
                g_gl.DeleteTextures(1, &st->dead[i].tex);
            if (st->dead[i].img && g_api.DestroyImageKHR)
                g_api.DestroyImageKHR(st->edpy, st->dead[i].img);
        }
        st->dead_n = 0;
        if (st->prog)
            g_gl.DeleteProgram(st->prog);

        g_api.MakeCurrent(st->edpy, nullptr, nullptr, nullptr);
        if (st->ctx)
            g_api.DestroyContext(st->edpy, st->ctx);
        g_api.Terminate(st->edpy);
    }
    if (st->dpy)
        g_api.XCloseDisplay(st->dpy);

    delete st;
}
