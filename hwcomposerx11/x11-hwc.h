/*
 * Copyright © 2011 Benjamin Franzke
 * Copyright © 2010 Intel Corporation
 * Copyright © 2014 Collabora Ltd.
 * Copyright © 2021 Waydroid Project.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#pragma once

#include <cutils/native_handle.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>
#include <fcntl.h>
#include <getopt.h>
#include <errno.h>
#include <map>
#include <list>
#include <pthread.h>
#include <semaphore.h>
#include <hardware/hwcomposer.h>
#include <vendor/openfde/task/1.0/IOpenfdeTask.h>
#include <wayland-util.h>

#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <functional>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/xproto.h>
#include <Xlib-xcb.h>
#include <Xrender.h>

#include <xcb/xcb_aux.h>
#include <xcb-imdkit/encoding.h>
#include <xcb-imdkit/imclient.h>
#include <xcb-imdkit/ximproto.h>
#include <xcb-imdkit/imclient_p.h>

using ::android::sp;
using ::vendor::openfde::task::V1_0::IOpenfdeTask;

enum {
    INPUT_TOUCH,
    INPUT_KEYBOARD,
    INPUT_POINTER,
    INPUT_TABLET,
    INPUT_TOTAL
};

static const char *INPUT_PIPE_NAME[INPUT_TOTAL] = {
    "/dev/input/wl_touch_events",
    "/dev/input/wl_keyboard_events",
    "/dev/input/wl_pointer_events",
    "/dev/input/wl_tablet_events"
};

enum {
    GRALLOC_ANDROID,
    GRALLOC_GBM,
    GRALLOC_CROS,
    GRALLOC_X100,
    GRALLOC_DEFAULT,
    GRALLOC_RANCHU,
    GRALLOC_FTG340
};

enum x11_keyboard_key_state {
        /**
         * key is not pressed
         */
        X11_KEYBOARD_KEY_STATE_RELEASED = 0,
        /**
         * key is pressed
         */
        X11_KEYBOARD_KEY_STATE_PRESSED = 1,
};


#define MAX_TOUCHPOINTS 10

struct layerFrame {
    int x;
    int y;
};

struct handleExt {
    uint32_t format;
    uint32_t stride;
    uint32_t width;
    uint32_t height;
};

struct window;

struct display {
    xcb_visualid_t visualid;
    xcb_colormap_t colormap;
    Display * x11display;
    xcb_connection_t *xcbconnection;
    xcb_screen_t *xcbscreen;
    XRenderPictFormat *  argb_format;
    int screen_default_nbr;
    xcb_xic_t ic;
    xcb_xim_t *im;
    int gtype;
    double scale;
 

    int input_fd[INPUT_TOTAL];
    int ptrPrvX;
    int ptrPrvY;
    double wheelAccumulatorX;
    double wheelAccumulatorY;
    bool wheelEvtIsDiscrete;
    bool reverseScroll;
    bool isTouchDown;
    bool isMouseLeftDown;
    int axisY;
    int axisX;
    int64_t lastAxisEventNanoSeconds;
    int touch_id[MAX_TOUCHPOINTS];
    std::map<struct wl_surface *, struct layerFrame> layers;
    std::map<struct wl_surface *, struct window *> windows;
    std::map<std::string, struct window *> *x11_windows;
    std::mutex windowsMutex;

    EGLDisplay egl_dpy;
    std::list<std::function<void()>> egl_work_queue;
    sem_t egl_go;
    sem_t egl_done;

    int width;
    int height;
    int full_width;
    int full_height;
    int refresh;
    uint32_t *formats;
    int formats_count;
    std::map<uint32_t, std::vector<uint64_t>> modifiers;
    bool geo_changed;
    std::map<uint32_t, std::string> layer_names;
    std::map<uint32_t, struct handleExt> layer_handles_ext;
    struct handleExt target_layer_handle_ext;
    std::map<buffer_handle_t, struct buffer *> buffer_map;
    std::array<uint8_t, 239> keysDown;

    bool isMaximized;
    sp<IOpenfdeTask> task;
    uint32_t serial;
    int64_t mouse_icon_addr;
    int additional_refresh_cursor_times;     //In order to get the final cursor shape
    bool ctrl_key_pressed;
    wl_fixed_t gesture_scale;
    bool axis_simulation_two_finger_started;
    bool internalCapsLockState;
    bool internalNumLockState;
    std::mutex internalCapsLockStateMutex;
    std::mutex internalNumLockStateMutex;
    bool multi_windows;

    int primary_x = 0;      //primary display X offset
    int primary_y = 0;      //primary display Y offset
    bool is_fullscreen = true;  //Initially full screen (for toggle state tracking)
};

struct buffer {
    struct wl_buffer *buffer;
    struct wp_presentation_feedback *feedback;
    xcb_pixmap_t xcbpixmap;
    Picture xpicture;
    buffer_handle_t handle;
    int width;
    int height;
    unsigned long pixel_stride;
    int format;
    uint32_t hal_format;

    bool isShm;
    void *shm_data;
    int size;
};

typedef struct
{
	native_handle_t base;
	int fd[3];
	uint64_t ui64SUnknown;
	int uUnknown;
	int iWidth;
	int iHeight;
	int iFormat;
	unsigned int uiBUnknown;
	int iPUnknown;
	int aiSUnknown[3];
	int aiVSUnknown[3];
	uint64_t aulUnknownO[3];
	unsigned int auiMUnknownU[3];
	unsigned int auiNumUnknownVCs[3];
	unsigned int auiNumPUnknownPCs[3];
	int iNumSUnknownAs;
	int iLunKnown;

} __attribute__((aligned(sizeof(int)),packed))  X100_native_handle_t;

typedef struct {
    native_handle_t nativeHandle;

    /* file descriptors */
    int prime_fd;


    /* integers */
    int magic;

    int flags;
    int size;
    int offset;
    uint64_t base __attribute__((aligned(8)));
    uint64_t phys __attribute__((aligned(8)));

    int width;
    int height;
    int format;
    int stride; /* the stride in bytes. */
    int b_unknown;

    int u_unknown;
    int p_unknown;
    uint64_t f_unknown[3] __attribute__((aligned(8)));
    uint64_t s_unknown;

    /* pointer to some bo struct. */
    uint64_t d_unknown __attribute__((aligned(8)));
    uint64_t r_unknown[3];
} gc_private_handle_t;

struct window {
    struct display *display;
    xcb_window_t xcbwindow;
    xcb_atom_t wm_protocols;
    xcb_atom_t wm_delete_window;
    xcb_gcontext_t xcbgc;
    Picture xpicture;
    Picture backxpicture;
    Pixmap backpixmap;
    int dri3_fd;
    std::vector<xcb_rectangle_t> rects;
    std::vector<hwc_rect_t> crops;
    
    struct buffer *last_layer_buffer;
    struct buffer *snapshot_buffer;
    int lastLayer;
    std::string appID;
    std::string taskID;
    bool isActive;
};

void
handle_relative_motion(void *data, struct zwp_relative_pointer_v1*,
        uint32_t, uint32_t, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t, wl_fixed_t);

void
destroy_buffer(struct display *display ,struct buffer* buf);

void
snapshot_inactive_app_window(struct display *display, struct window *window);

struct display *
create_display(const char* gralloc);
void
destroy_display(struct display *display);
int remove_title(xcb_connection_t *conn, xcb_window_t main_win);

void
destroy_window(struct window *window, bool keep = false);
struct window *
create_window(struct display *display, bool with_dummy, std::string appID, std::string taskID, hwc_color_t color);
void
choose_width_height(struct display* display, int32_t hint_width, int32_t hint_height);

void forward_event(xcb_xim_t *im, xcb_xic_t ic, xcb_key_press_event_t *event, void *user_data);
void commit_string(xcb_xim_t *im, xcb_xic_t ic, uint32_t flag, char *str,
                   uint32_t length, uint32_t *keysym, size_t nKeySym,
                   void *user_data);
void disconnected(xcb_xim_t *im, void *user_data);
void create_ic_callback(xcb_xim_t *im, xcb_xic_t new_ic, void *user_data);
void open_im_callback(xcb_xim_t *im, void *user_data);
void update_spot_location(xcb_xim_t *im, xcb_xic_t ic, xcb_point_t spot);
void set_window_title(xcb_connection_t *connection, xcb_window_t window, const std::string &title);
void set_window_class(xcb_connection_t *connection, xcb_window_t window, const std::string &instance_name, const std::string &class_name);
void disable_auto_repeat(Display *display);
void enable_auto_repeat(Display *display);
int create_shm_buffer(struct buffer *buffer, int width, int height, int format, int pixel_stride, buffer_handle_t target);
bool isValidInteger(const std::string& str);
bool isStartWithSpecialSymbols(const std::string& layer_name);
bool isStartWithTidSymbols(const std::string& layer_name);
int add_title(xcb_connection_t *conn, xcb_window_t main_win);
