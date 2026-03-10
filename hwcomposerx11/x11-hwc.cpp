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

#include "x11-hwc.h"
#include "egl-tools.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <getopt.h>
#include <errno.h>
#include <linux/input.h>
#include <linux/memfd.h>
#include <drm_fourcc.h>
#include <system/graphics.h>
#include <syscall.h>
#include <cmath>
#include <algorithm>

#include <libsync/sw_sync.h>
#include <sync/sync.h>
#include <hardware/gralloc.h>
#include <log/log.h>
#include <thread>

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include <cutils/trace.h>
#include <cutils/properties.h>

#include <xkbcommon/xkbcommon.h>
#include <X11/XKBlib.h>
#include <xcb/xinput.h>

#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "tablet-unstable-v2-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include <pointer-gestures-unstable-v1-client-protocol.h>

using ::android::hardware::hidl_string;

const int AXIS_TOUCH_SLOT_ID = 8;
const int AXIS_TOUCH_TRACKING_ID = AXIS_TOUCH_SLOT_ID;
const int SCROLLING_STRIDE = 2560;
struct display *mDisplay;
int timer_active = 0;
struct itimerval timer;
#define SCROLL_END_TIMEOUT_MS 240
#define GESTURE_SCALING_DOWN_STRIDE 30
#define GESTURE_SCALING_UP_STRIDE 480
#define GESTURE_SCALING_DOWN_START_DISTANCE 180.0
#define GESTURE_SCALING_UP_START_DISTANCE 10.0
#define XI_FP1616_TO_DOUBLE(val) ((double)(val) / 65536.0)


static double gesture_scaling_start_distance;
static int gesture_scaling_stride;
static uint8_t xi_opcode;

static int find_argb_visual(struct display *display) ;
void
destroy_buffer(struct display * display ,struct buffer* buf) {
    if (buf->xcbpixmap) {
        xcb_free_pixmap(display->xcbconnection, buf->xcbpixmap);
        buf->xcbpixmap = 0;
    }
    if (buf->xpicture) {
        XRenderFreePicture(display->x11display, buf->xpicture);
        buf->xpicture = 0;
    }
    if (buf->isShm)
        munmap(buf->shm_data, buf->size);
    /*if (buf->shm_data) {
	    free(buf->shm_data);
	    buf->shm_data = 0;
    }
    */
    delete buf;
}

static int
str_starts_with(const char *a, const char *b)
{
    return strncmp(a, b, strlen(b));
}


int
get_gralloc_type(const char *gralloc)
{
    if (strcmp(gralloc, "default") == 0) {
        return GRALLOC_DEFAULT;
    } else if (strcmp(gralloc, "gbm") == 0) {
        return GRALLOC_GBM;
    } else if (strcmp(gralloc, "ranchu") == 0) {
    return GRALLOC_RANCHU;
    } else if (str_starts_with(gralloc, "minigbm_") == 0) {
        return GRALLOC_CROS;
    } else if (strcmp(gralloc, "ft2004") == 0) {
        return GRALLOC_X100;
    } else if (strcmp(gralloc, "FTG340") == 0) {
        return GRALLOC_FTG340;
    } else {
        return GRALLOC_ANDROID;
    }
}


bool isFormatSupported(struct display *display, uint32_t format) {
    for (int i = 0; i < display->formats_count; i++) {
        if (format == display->formats[i])
            return true;
    }
    return false;
}

int ConvertHalFormatToDrm(struct display *display, uint32_t hal_format) {
    uint32_t fmt;

    switch (hal_format) {
        case HAL_PIXEL_FORMAT_RGB_888:
            fmt = DRM_FORMAT_BGR888;
            if (!isFormatSupported(display, fmt))
                fmt = DRM_FORMAT_RGB888;
            break;
        case HAL_PIXEL_FORMAT_BGRA_8888:
            fmt = DRM_FORMAT_ARGB8888;
            if (!isFormatSupported(display, fmt))
                fmt = DRM_FORMAT_ABGR8888;
            break;
        case HAL_PIXEL_FORMAT_RGBX_8888:
            fmt = DRM_FORMAT_XBGR8888;
            if (!isFormatSupported(display, fmt))
                fmt = DRM_FORMAT_XRGB8888;
            break;
        case HAL_PIXEL_FORMAT_RGBA_8888:
            fmt = DRM_FORMAT_ABGR8888;
            if (!isFormatSupported(display, fmt))
                fmt = DRM_FORMAT_ARGB8888;
            break;
        case HAL_PIXEL_FORMAT_RGB_565:
            fmt = DRM_FORMAT_BGR565;
            if (!isFormatSupported(display, fmt))
                fmt = DRM_FORMAT_RGB565;
            break;
        case HAL_PIXEL_FORMAT_YV12:
            fmt = DRM_FORMAT_YVU420;
            if (!isFormatSupported(display, fmt))
                fmt = DRM_FORMAT_GR88;
            break;
        default:
            ALOGE("Cannot convert hal format to drm format %u", hal_format);
            return -EINVAL;
    }
    if (!isFormatSupported(display, fmt)) {
        ALOGE("Current wayland display doesn't support hal format %u", hal_format);
        return -EINVAL;
    }
    return fmt;
}



static void
finished_computing_scale(struct display *d)
{
    char property[PROPERTY_VALUE_MAX];
    int default_density = 180;
    std::string display_scale = std::to_string(d->scale);
    property_set("openfde.display_scale", display_scale.c_str());
    if (property_get("ro.sf.lcd_density", property, nullptr) <= 0) {
        std::string lcd_density = std::to_string(int(default_density * d->scale));
        property_set("ro.sf.lcd_density", lcd_density.c_str());
    }
}

void choose_width_height(struct display* display, int32_t hint_width, int32_t hint_height) {
    char property[PROPERTY_VALUE_MAX];
    int width = hint_width;
    int height = hint_height;

    // Ignore hint it requested
    if (property_get("persist.openfde.width", property, nullptr) > 0) {
        display->isMaximized = false;
        width = atoi(property);
    }

    if (property_get("persist.openfde.height", property, nullptr) > 0) {
        display->isMaximized = false;
        height = atoi(property);
    }

    display->width = width;
    display->height = height;
}

void
destroy_window(struct window *window, bool keep)
{
    if (window->backxpicture) {
        XRenderFreePicture(window->display->x11display, window->backxpicture);
        window->backxpicture = 0;
    }
    if (!window->rects.empty()) {
        window->rects.clear();
    }
    if (!window->crops.empty()) {
        window->crops.clear();
    }
    if (window->backpixmap) {
        XFreePixmap(window->display->x11display, window->backpixmap);
        window->backpixmap = 0;
    }

    if (window->xcbgc) {
        xcb_free_gc(window->display->xcbconnection, window->xcbgc);
        window->xcbgc = 0;
    }
    if (window->dri3_fd > 0) {
        close(window->dri3_fd);
        window->dri3_fd = -1;
    }
    if (window->xpicture) {
        XRenderFreePicture(window->display->x11display, window->xpicture);
        window->xpicture = 0;
    }
     if (window->xcbwindow) {
        ALOGE("destroy window window->xcbwindow: %u", window->xcbwindow);
        for (auto it = window->display->x11_windows->begin(); it != window->display->x11_windows->end(); it++) {
            if (it->second->xcbwindow == window->xcbwindow){
                if (it->first != "Toast" && it->first.find("Application Not Responding:") ==  std::string::npos && !isStartWithSpecialSymbols(it->first)) {
                    ALOGI("destroy window window->xcbwindow: %u Task %s XAutoRepeatOn", window->xcbwindow, it->first.c_str());
                    enable_auto_repeat(window->display->x11display);
                }
            }
        }
        xcb_unmap_window(window->display->xcbconnection, window->xcbwindow);
        xcb_destroy_window(window->display->xcbconnection, window->xcbwindow);
        window->xcbwindow = 0;
    }
    xcb_flush(window->display->xcbconnection);
    if (keep)
        window->isActive = false;
    else
        delete window;
}



static int
ensure_pipe(struct display* display, int input_type)
{
    if (display->input_fd[input_type] == -1) {
        display->input_fd[input_type] = open(INPUT_PIPE_NAME[input_type], O_WRONLY | O_NONBLOCK);
        if (display->input_fd[input_type] == -1) {
            ALOGE("Failed to open pipe to InputFlinger: %s", strerror(errno));
            return -1;
        }
    }
    return 0;
}

#define ADD_EVENT(type_, code_, value_)            \
    event[n].time.tv_sec = rt.tv_sec;              \
    event[n].time.tv_usec = rt.tv_nsec / 1000;     \
    event[n].type = type_;                         \
    event[n].code = code_;                         \
    event[n].value = value_;                       \
    n++;
static void
send_key_event(display *data, uint32_t key, x11_keyboard_key_state state)
{
    struct display* display = (struct display*)data;
    struct input_event event[1];
    struct timespec rt;
    unsigned int res, n = 0;

    if (key >= display->keysDown.size()) {
        ALOGE("Invalid key: %u", key);
        return;
    }

    if (ensure_pipe(display, INPUT_KEYBOARD))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }
    ADD_EVENT(EV_KEY, key, state);

    ALOGE("send_key_event write INPUT_KEYBOARD key: %d, state: %d", key, state);
    res = write(display->input_fd[INPUT_KEYBOARD], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
    display->keysDown[(uint8_t)key] = state;
}

static void pointer_handle_button_to_touch_down(struct display *display) {
    struct input_event event[6];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TOUCH))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_ABS, ABS_MT_SLOT, 0);
    ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, 0);
    ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, display->ptrPrvX);
    ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, display->ptrPrvY);
    ADD_EVENT(EV_ABS, ABS_MT_PRESSURE, 50);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);
    display->isTouchDown = true;
    ALOGI("pointer_handle_button_to_touch_down write INPUT_TOUCH");
    res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));

    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void pointer_handle_button_to_touch_up(struct display *display) {
    struct input_event event[3];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TOUCH))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_ABS, ABS_MT_SLOT, 0);
    ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, -1);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);
    display->isTouchDown = false;
    ALOGI("pointer_handle_button_to_touch_up write INPUT_TOUCH");
    res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));

    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

/*static void
pointer_handle_enter(void *data, struct wl_pointer *pointer,
                     uint32_t serial, struct wl_surface *surface,
                     wl_fixed_t, wl_fixed_t)
{
    struct display *display = (struct display *)data;
    display->pointer_surface = surface;
    if (display->cursor_surface){
        display->serial = serial;
        int32_t icon_hotspot_x = property_get_int32("fde.mouse_icon_hotspot_x", 5);
        int32_t icon_hotspot_y = property_get_int32("fde.mouse_icon_hotspot_y", 5);
        wl_pointer_set_cursor(pointer, serial,
                              display->cursor_surface, icon_hotspot_x, icon_hotspot_y);
    }
    //When the cursor is hidden, it will trigger a new hide request in hwcomposer's hwc_prepare.
    display->mouse_icon_addr = 0;
}

static void
pointer_handle_leave(void *data, struct wl_pointer *pointer,
                     uint32_t serial, struct wl_surface *)
{
    struct display *display = (struct display *)data;
    display->pointer_surface = NULL;
    display->mouse_icon_addr = -1;
    if (display->cursor_surface){
        wl_pointer_set_cursor(pointer, serial, NULL, 0, 0);
    }
}
*/

static void
pointer_cancel_axis_to_two_finger_touch(struct display *display){
    struct input_event event[6];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TOUCH))
        return;

    display->axis_simulation_two_finger_started = false;
    display->gesture_scale = 160;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
       ALOGE("%s:%d error in touch clock_gettime: %s",
            __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_ABS, ABS_MT_SLOT, 0);
    ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, -1);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);
    ADD_EVENT(EV_ABS, ABS_MT_SLOT, 1);
    ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, -1);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);
    ALOGI("pointer_cancel_axis_to_two_finger_touch write INPUT_TOUCH");
    res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static bool
pointer_cancel_axis_to_touch(struct display *display, bool fromAxisStopEvent, bool force)
{
    display->wheelEvtIsDiscrete = false;
    // Check if the scroll wheel event has started.
    if (display->lastAxisEventNanoSeconds == 0) {
        return true;
    }

    struct input_event event[12];
    int eventSize = fromAxisStopEvent ? 3 * sizeof(input_event) : sizeof(event);
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TOUCH))
        return false;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    // Prevent minor mouse movements from interrupting the touch scrolling process while scrolling the wheel.
    if (!force && ((rt.tv_sec * 1000 * 1000 * 1000 + rt.tv_nsec - display->lastAxisEventNanoSeconds) < 300 * 1000 * 1000)) {
        return false;
    }

    display->axisY = display->ptrPrvY;
    display->lastAxisEventNanoSeconds = 0;

    if (!fromAxisStopEvent) {
        // Use the second touch click to prevent inertial scrolling
        ADD_EVENT(EV_ABS, ABS_MT_SLOT, AXIS_TOUCH_SLOT_ID + 1);
        ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, AXIS_TOUCH_TRACKING_ID + 1);
        ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, -1);
        ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, -1);
        ADD_EVENT(EV_ABS, ABS_MT_PRESSURE, 50);
        ADD_EVENT(EV_SYN, SYN_REPORT, 0);
    }

    ADD_EVENT(EV_ABS, ABS_MT_SLOT, AXIS_TOUCH_SLOT_ID);
    ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, -1);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    if (!fromAxisStopEvent) {
        ADD_EVENT(EV_ABS, ABS_MT_SLOT, AXIS_TOUCH_SLOT_ID + 1);
        ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, -1);
        ADD_EVENT(EV_SYN, SYN_REPORT, 0);
    }
    ALOGI("pointer_cancel_axis_to_touch write INPUT_TOUCH");
    res = write(display->input_fd[INPUT_TOUCH], &event, eventSize);
    if (res < sizeof(event)) {
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
        return false;
    }

    return true;
}

void timer_handler(int sig) {
    if (sig == SIGALRM) {
        ALOGD("pointer axis stopped.");
        if(mDisplay){
            if(mDisplay->axis_simulation_two_finger_started){
                pointer_cancel_axis_to_two_finger_touch(mDisplay);
            }else{
                pointer_cancel_axis_to_touch(mDisplay, true, true);
            }
        }
        timer_active = 0;
    }
}

void reset_timer() {
    memset(&timer, 0, sizeof(timer));

    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = SCROLL_END_TIMEOUT_MS * 1000;

    setitimer(ITIMER_REAL, &timer, NULL);
    timer_active = 1;
}

void init_timer() {
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = timer_handler;
    sigaction(SIGALRM, &sa, NULL);
}


typedef void (*KeyPressCallback)(void *data, xcb_key_press_event_t *event);
typedef void (*KeyReleaseCallback)(void *data, xcb_key_release_event_t *event);
typedef void (*ButtonPressCallback)(void *data, xcb_button_press_event_t *event);
typedef void (*ButtonReleaseCallback)(void *data, xcb_button_release_event_t *event);
typedef void (*MotionNotifyCallback)(void *data, xcb_motion_notify_event_t *event);

typedef struct {
    KeyPressCallback key_press_cb;
    KeyReleaseCallback key_release_cb;
    ButtonPressCallback button_press_cb;
    ButtonReleaseCallback button_release_cb;
    MotionNotifyCallback motion_notify_cb;
} EventDispatcher;

static EventDispatcher dispatcher = {0,0,0,0,0};

void register_key_press_callback(KeyPressCallback cb) { dispatcher.key_press_cb = cb; }
void register_key_release_callback(KeyReleaseCallback cb) { dispatcher.key_release_cb = cb; }
void register_button_press_callback(ButtonPressCallback cb) { dispatcher.button_press_cb = cb; }
void register_button_release_callback(ButtonReleaseCallback cb) { dispatcher.button_release_cb = cb; }
void register_motion_notify_callback(MotionNotifyCallback cb) { dispatcher.motion_notify_cb = cb; }

void on_key_press(void *data, xcb_key_press_event_t *event) {
    ALOGI("x11 keyboard press: keycode=%u\n", event->detail);
    uint32_t key = event->detail - 8;
    if (key == KEY_POWER)
        return;
    struct display* display = (struct display*)data;
    if (key == KEY_LEFTCTRL || key == KEY_RIGHTCTRL){
        display->ctrl_key_pressed = 1;
    }
    if(key == KEY_CAPSLOCK || key == KEY_NUMLOCK){
        return;
    }
    send_key_event((struct display*)data, key, X11_KEYBOARD_KEY_STATE_PRESSED);
}

void on_key_release(void *data, xcb_key_release_event_t *event) {
    ALOGI("x11 keyboard release: keycode=%u\n", event->detail);
    uint32_t key = event->detail - 8;
    if (key == KEY_POWER)
        return;
    struct display* display = (struct display*)data;
    if (key == KEY_LEFTCTRL || key == KEY_RIGHTCTRL){
        display->ctrl_key_pressed = 0;
    }
    if(key == KEY_CAPSLOCK){
        XkbStateRec state;
        XkbGetState(display->x11display, XkbUseCoreKbd, &state);
        bool externalCapsLockState = state.locked_mods & LockMask;
        if (externalCapsLockState) {
            ALOGD("on_key_release External Caps Lock is ON");
        }else{
            ALOGD("on_key_release External Caps Lock is OFF");
        }
        std::scoped_lock caps_lock(display->internalCapsLockStateMutex);
        if(display->internalCapsLockState != externalCapsLockState){
            display->internalCapsLockState = externalCapsLockState;
            ALOGD("on_key_release update display->internalCapsLockState: %d", display->internalCapsLockState);
            send_key_event(display, KEY_CAPSLOCK, X11_KEYBOARD_KEY_STATE_PRESSED);
            send_key_event(display, KEY_CAPSLOCK, X11_KEYBOARD_KEY_STATE_RELEASED);
        }
        return;
    }
    if(key == KEY_NUMLOCK){
        XkbStateRec state;
        XkbGetState(display->x11display, XkbUseCoreKbd, &state);
        bool externalNumLockState = state.locked_mods & Mod2Mask;
        if (externalNumLockState) {
            ALOGD("on_key_release External Num Lock is ON");
        }else{
            ALOGD("on_key_release External Num Lock is OFF");
        }
        std::scoped_lock num_lock(display->internalNumLockStateMutex);
        if(display->internalNumLockState != externalNumLockState){
            display->internalNumLockState = externalNumLockState;
            ALOGD("on_key_release update display->internalNumLockState: %d", display->internalNumLockState);
            send_key_event(display, KEY_NUMLOCK, X11_KEYBOARD_KEY_STATE_PRESSED);
            send_key_event(display, KEY_NUMLOCK, X11_KEYBOARD_KEY_STATE_RELEASED);
        }
        return;
    }
    send_key_event((struct display*)data, key, X11_KEYBOARD_KEY_STATE_RELEASED);
}

static void handle_pinch_update(void *data, uint32_t time, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t scale, wl_fixed_t rotation)
{
    (void) data;
    (void) time;
    (void) dx;
    (void) dy;
    (void) scale;
    (void) rotation;
    struct display* display = (struct display*)data;
    struct input_event event[12];
    struct timespec rt;
    int x, y;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TOUCH))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
       ALOGE("%s:%d error in touch clock_gettime: %s",
            __FILE__, __LINE__, strerror(errno));
    }
    x = wl_fixed_to_int(dx) + display->ptrPrvX;
    y = wl_fixed_to_int(dy) + display->ptrPrvY;


    double iscale = wl_fixed_to_double(scale);
    double irotation = 90;

    int x0 = x - (gesture_scaling_start_distance * iscale * cos(irotation));
    int y0 = y - (gesture_scaling_start_distance * iscale * sin(irotation));
    int x1 = x + (gesture_scaling_start_distance * iscale * cos(irotation));
    int y1 = y + (gesture_scaling_start_distance * iscale * sin(irotation));

    ADD_EVENT(EV_ABS, ABS_MT_SLOT, 0);
    ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, 0);
    ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, x0);
    ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, y0);
    ADD_EVENT(EV_ABS, ABS_MT_PRESSURE, 50);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);
    ADD_EVENT(EV_ABS, ABS_MT_SLOT, 1);
    ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, 1);
    ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, x1);
    ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, y1);
    ADD_EVENT(EV_ABS, ABS_MT_PRESSURE, 50);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    ALOGI("on_button_release write INPUT_TOUCH");
    res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));

    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));

}


static void
pointer_axis_to_touch(struct display *display, int move, bool verticalScroll)
{
    struct input_event event[6];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TOUCH))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    int64_t nanoSeconds = rt.tv_sec * 1000 * 1000 * 1000 + rt.tv_nsec;
    if(verticalScroll){
        display->axisY += move;
    }else{
        display->axisX += move;
    }

    // if ((nanoSeconds - display->lastAxisEventNanoSeconds) < 20 * 1000 * 1000) {
    //     return;
    // }

    if (display->lastAxisEventNanoSeconds == 0) {
        display->axisY = display->ptrPrvY;
        display->axisX = display->ptrPrvX;
        ADD_EVENT(EV_ABS, ABS_MT_SLOT, AXIS_TOUCH_SLOT_ID);
        ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, AXIS_TOUCH_TRACKING_ID);
        if(verticalScroll){
            ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, display->ptrPrvX);
            ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, display->axisY);
        }else{
            ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, display->axisX);
            ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, display->ptrPrvY);
        }
        ADD_EVENT(EV_ABS, ABS_MT_PRESSURE, 50);
        ADD_EVENT(EV_SYN, SYN_REPORT, 0);
        ALOGE("pointer_axis_to_touch write INPUT_TOUCH");
        res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
        if (res < sizeof(event)) {
            ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
            return;
        }

        n = 0;
        display->axisY += move;
    }

    display->lastAxisEventNanoSeconds = nanoSeconds;
    ADD_EVENT(EV_ABS, ABS_MT_SLOT, AXIS_TOUCH_SLOT_ID);
    ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, AXIS_TOUCH_TRACKING_ID);
    if(verticalScroll){
        ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, display->ptrPrvX);
        ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, display->axisY);
    }else{
        ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, display->axisX);
        ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, display->ptrPrvY);
    }
    ADD_EVENT(EV_ABS, ABS_MT_PRESSURE, 50);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);
    ALOGI("pointer_axis_to_touch write INPUT_TOUCH");
    res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}


static void
x11_pointer_handle_axis(void *data,  uint32_t axis, int value)
{
    struct display* display = (struct display*)data;
    int touchMove = display->reverseScroll ? wl_fixed_to_int(value) : -wl_fixed_to_int(value);
    if (display->wheelEvtIsDiscrete) {
        touchMove *= 6;
    }

    struct input_event event[2];
    struct timespec rt;
    unsigned int move, res, n = 0;
    double fVal = wl_fixed_to_double(value) / 10.0f;
    double step = 1.0f;

    if (ensure_pipe(display, INPUT_POINTER))
        return;

    if (!display->reverseScroll) {
        fVal = -fVal;
    }

    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        display->wheelAccumulatorY += fVal;
        if (std::abs(display->wheelAccumulatorY) < step)
            return;
        move = (int)(display->wheelAccumulatorY / step);
        display->wheelAccumulatorY = display->wheelEvtIsDiscrete ? 0 :
                                     std::fmod(display->wheelAccumulatorY, step);
    } else {
        display->wheelAccumulatorX += fVal;
        if (std::abs(display->wheelAccumulatorX) < step)
            return;
        move = (int)(display->wheelAccumulatorX / step);
        display->wheelAccumulatorX = display->wheelEvtIsDiscrete ? 0 :
                                     std::fmod(display->wheelAccumulatorX, step);
    }

    if(property_get_bool("fde.click_as_touch", false)){
        if(display->ctrl_key_pressed){
            if(!display->axis_simulation_two_finger_started){
                display->axis_simulation_two_finger_started = true;
                if(touchMove > 0){
                    gesture_scaling_start_distance = GESTURE_SCALING_UP_START_DISTANCE;
                    gesture_scaling_stride = GESTURE_SCALING_UP_STRIDE;
                }else{
                    gesture_scaling_start_distance = GESTURE_SCALING_DOWN_START_DISTANCE;
                    gesture_scaling_stride = GESTURE_SCALING_DOWN_STRIDE;
                }
            }
            if(touchMove > 0){
                display->gesture_scale += gesture_scaling_stride;
            }else{
                display->gesture_scale -= gesture_scaling_stride;
                if(display->gesture_scale < gesture_scaling_stride){
                    display->gesture_scale = 5;
                }
            }
            if(display->lastAxisEventNanoSeconds != 0){
                pointer_cancel_axis_to_touch(display, true, true);
            }
            handle_pinch_update(data,0,0,0,display->gesture_scale,0);
        }else{
            if(display->axis_simulation_two_finger_started){
                pointer_cancel_axis_to_two_finger_touch(display);
            }
            pointer_axis_to_touch(display, touchMove, axis == WL_POINTER_AXIS_VERTICAL_SCROLL);
        }
    }else{
        if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
            ALOGE("%s:%d error in touch clock_gettime: %s",
                  __FILE__, __LINE__, strerror(errno));
        }
        ADD_EVENT(EV_REL, (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
              ? REL_WHEEL : REL_HWHEEL, move);
        ADD_EVENT(EV_SYN, SYN_REPORT, 0);

        ALOGE("x11_pointer_handle_axis write INPUT_POINTER");
        res = write(display->input_fd[INPUT_POINTER], &event, sizeof(event));
        if (res < sizeof(event))
            ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
    }
}

static bool
is_x11_touch_begin(struct display *display)
{
    for (int i = 0; i < MAX_TOUCHPOINTS; i++) {
        if (display->touch_id[i] != -1) {
            return true;
        }
    }
    return false;
}


void on_button_press(void *data, xcb_button_press_event_t *xcb_button_event) {
    ALOGI("on_button_press: botton=%u, position=(%d, %d)\n",
           xcb_button_event->detail, xcb_button_event->event_x, xcb_button_event->event_y);
    struct display* display = (struct display*)data;
    if(is_x11_touch_begin(display)){
        ALOGI("on_button_press x11 touch started return");
        return;
    }
    if(xcb_button_event->detail == XCB_BUTTON_INDEX_4 || xcb_button_event->detail == XCB_BUTTON_INDEX_5
        || xcb_button_event->detail == 6 || xcb_button_event->detail == 7 ){
        ALOGE("on_button_press %d return", xcb_button_event->detail);
        reset_timer();
        return;
    }
    ALOGI("display->ptrPrvX: %d, display->ptrPrvY: %d", display->ptrPrvX, display->ptrPrvY);

    pointer_cancel_axis_to_touch(display, false, true);
    if(display->axis_simulation_two_finger_started){
        pointer_cancel_axis_to_two_finger_touch(display);
    }

    // Left button convert to touch event, right button reserved mouse event
    if ( xcb_button_event->detail != 2 &&
            ((xcb_button_event->detail == 1 && property_get_bool("fde.click_as_touch", false)) || display->isTouchDown)
            && !display->isMouseLeftDown
    ) {
        struct timespec rt;
        if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
            ALOGE("%s:%d error in touch clock_gettime: %s",
                   __FILE__, __LINE__, strerror(errno));
        }
      // convert pointer event to touch event
        pointer_handle_button_to_touch_down(display);
    } else {
        struct input_event event[2];
        struct timespec rt;
        unsigned int res, n = 0;

        if (ensure_pipe(display, INPUT_POINTER))
            return;

        if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
            ALOGE("%s:%d error in touch clock_gettime: %s",
                   __FILE__, __LINE__, strerror(errno));
        }
        if(xcb_button_event->detail == 1){
            display->isMouseLeftDown = true;
            ALOGE("on_button_press isMouseLeftDown set true");
        }

        uint32_t button = 0;
        switch(xcb_button_event->detail){
            case XCB_BUTTON_INDEX_1:
                button = BTN_LEFT;
                break;
            case XCB_BUTTON_INDEX_3:
                button = BTN_RIGHT;
                break;
            case XCB_BUTTON_INDEX_2:
                button = BTN_MIDDLE;
                break;
        }
        if(button != 0){
            ADD_EVENT(EV_KEY, button, 1);
            ADD_EVENT(EV_SYN, SYN_REPORT, 0);
            ALOGE("on_button_press write INPUT_POINTER");
            res = write(display->input_fd[INPUT_POINTER], &event, sizeof(event));
        }
    }
}

void on_button_release(void *data, xcb_button_release_event_t *xcb_button_event) {
    struct display* display = (struct display*)data;
    if(is_x11_touch_begin(display)){
        ALOGI("on_button_release x11 touch started return");
        return;
    }
    if(xcb_button_event->detail == XCB_BUTTON_INDEX_4 || xcb_button_event->detail == XCB_BUTTON_INDEX_5){
        uint32_t axis = 0;
        int value = (xcb_button_event->detail == XCB_BUTTON_INDEX_4) ? -SCROLLING_STRIDE : SCROLLING_STRIDE;
        display->wheelEvtIsDiscrete = true;
        x11_pointer_handle_axis(data, axis, value);
        return;
    }else if(xcb_button_event->detail == 6 || xcb_button_event->detail == 7){
        uint32_t axis = 1;
        int value = (xcb_button_event->detail == 6) ? -SCROLLING_STRIDE : SCROLLING_STRIDE;
        display->wheelEvtIsDiscrete = true;
        x11_pointer_handle_axis(data, axis, value);
        return;
    }
    pointer_cancel_axis_to_touch(display, false, true);
    if(display->axis_simulation_two_finger_started){
        pointer_cancel_axis_to_two_finger_touch(display);
    }

    // Left button convert to touch event, right button reserved mouse event
    if(((xcb_button_event->detail == 1 && property_get_bool("fde.click_as_touch", false)) || display->isTouchDown) && !display->isMouseLeftDown) {
        // convert pointer event to touch event
        pointer_handle_button_to_touch_up(display);
    }else{
        struct input_event event[2];
        struct timespec rt;
        unsigned int res, n = 0;

        if (ensure_pipe(display, INPUT_POINTER))
            return;

        //if (!display->pointer_surface)
        //    return;

        if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
            ALOGE("%s:%d error in touch clock_gettime: %s",
                   __FILE__, __LINE__, strerror(errno));
        }
        if(xcb_button_event->detail == 1){
            display->isMouseLeftDown = false;
        }

        uint32_t button = 0;
        switch(xcb_button_event->detail){
            case XCB_BUTTON_INDEX_1:
                button = BTN_LEFT;
                break;
            case XCB_BUTTON_INDEX_3:
                button = BTN_RIGHT;
                break;
            case XCB_BUTTON_INDEX_2:
                button = BTN_MIDDLE;
                break;
        }
        if(button != 0){
            ADD_EVENT(EV_KEY, button, 0);
            ADD_EVENT(EV_SYN, SYN_REPORT, 0);
            res = write(display->input_fd[INPUT_POINTER], &event, sizeof(event));
        }
    }
}

void on_motion_notify(void *data, xcb_motion_notify_event_t *event) {
    struct display* display = (struct display*)data;
    //ALOGI("display->ptrPrvX: %d, display->ptrPrvY: %d", display->ptrPrvX, display->ptrPrvY);
    //ALOGI("x11 mouse move: position=(%d, %d)\n",
     //      event->event_x, event->event_y);
    //ALOGI("鼠标移动: 窗口坐标 (%d, %d) -> 屏幕坐标 (%d, %d)\n", event->event_x, event->event_y, event->root_x, event->root_y);
    if(display->axis_simulation_two_finger_started){
        return;
    }
    int x, y;

    if (ensure_pipe(display, INPUT_POINTER)){
        return;
    }

    x = event->root_x;
    y = event->root_y;

    if (display->scale != 1) {
        x = int(x * display->scale);
        y = int(y * display->scale);
    }

    if (display->isTouchDown) {
        display->ptrPrvX = x;
        display->ptrPrvY = y;
        pointer_handle_button_to_touch_down(display);
    } else if (pointer_cancel_axis_to_touch(display, false, false)) {
        struct input_event event[5];
        struct timespec rt;
        unsigned int res, n = 0;

        if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
            ALOGE("%s:%d error in touch clock_gettime: %s",
                __FILE__, __LINE__, strerror(errno));
        }

        ADD_EVENT(EV_ABS, ABS_X, x);
        ADD_EVENT(EV_ABS, ABS_Y, y);
        ADD_EVENT(EV_REL, REL_X, x - display->ptrPrvX);
        ADD_EVENT(EV_REL, REL_Y, y - display->ptrPrvY);
        ADD_EVENT(EV_SYN, SYN_REPORT, 0);
        display->ptrPrvX = x;
        display->ptrPrvY = y;
        if(property_get_bool("fde.inject_as_touch", false)){
            return;
        }

        ALOGI("on_motion_notify write INPUT_POINTER");
        res = write(display->input_fd[INPUT_POINTER], &event, sizeof(event));
        if (res < sizeof(event))
            ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));

    }
}

bool isValidInteger(const std::string& str) {
    if (str.empty()) return false;
    size_t pos = 0;
    if (str[0] == '-' || str[0] == '+') pos = 1;
    if (pos == str.size()) return false;
    for (size_t i = pos; i < str.size(); ++i) {
        if (!std::isdigit(str[i])) return false;
    }
    return true;
}

void set_ic_values_callback(xcb_xim_t *im, xcb_xic_t ic, void *user_data) {
    ALOGD("set ic %d done\n", ic);
    (void) im;
    (void) ic;
    (void) user_data;
}


void update_spot_location(xcb_xim_t *im, xcb_xic_t ic, xcb_point_t spot) {
    if(!im){
        ALOGE("error im is null.");
        return;
    }
    xcb_xim_nested_list nested =
        xcb_xim_create_nested_list(im, XCB_XIM_XNSpotLocation, &spot, NULL);
    xcb_xim_set_ic_values(im, ic, set_ic_values_callback, NULL,
                              XCB_XIM_XNPreeditAttributes, &nested, NULL);
    free(nested.data);
}

static int
get_touch_id(struct display *display, int id)
{
    int i = 0;
    for (i = 0; i < MAX_TOUCHPOINTS; i++) {
        if (display->touch_id[i] == id)
            return i;
    }
    for (i = 0; i < MAX_TOUCHPOINTS; i++) {
        if (display->touch_id[i] == -1) {
            display->touch_id[i] = id;
            return i;
        }
    }
    return -1;
}

static int
flush_touch_id(struct display *display, int id)
{
    for (int i = 0; i < MAX_TOUCHPOINTS; i++) {
        if (display->touch_id[i] == id) {
            display->touch_id[i] = -1;
            return i;
        }
    }
    return -1;
}

static void
touch_handle_down(void *data,
          int32_t id, int x, int y)
{
    struct display* display = (struct display*)data;
    struct input_event event[6];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TOUCH))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
       ALOGE("%s:%d error in touch clock_gettime: %s",
            __FILE__, __LINE__, strerror(errno));
    }

    if (display->scale != 1) {
        x = int(x * display->scale);
        y = int(y * display->scale);
    }

    ADD_EVENT(EV_ABS, ABS_MT_SLOT, get_touch_id(display, id));
    ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, get_touch_id(display, id));
    ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, x);
    ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, y);
    ADD_EVENT(EV_ABS, ABS_MT_PRESSURE, 50);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    ALOGI("touch_handle_down write INPUT_TOUCH id: %d", get_touch_id(display, id));
    res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
touch_handle_up(void *data, int32_t id)
{
    struct display* display = (struct display*)data;
    struct input_event event[3];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TOUCH))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
       ALOGE("%s:%d error in touch clock_gettime: %s",
            __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_ABS, ABS_MT_SLOT, flush_touch_id(display, id));
    ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, -1);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
touch_handle_motion(void *data,      int32_t id, int x, int y)
{
    struct display* display = (struct display*)data;
    struct input_event event[6];
    struct timespec rt;

    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TOUCH))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
       ALOGE("%s:%d error in touch clock_gettime: %s",
            __FILE__, __LINE__, strerror(errno));
    }

    if (display->scale != 1) {
        x = int(x * display->scale);
        y = int(y * display->scale);
    }

    ADD_EVENT(EV_ABS, ABS_MT_SLOT, get_touch_id(display, id));
    ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, get_touch_id(display, id));
    ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, x);
    ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, y);
    ADD_EVENT(EV_ABS, ABS_MT_PRESSURE, 50);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
touch_handle_cancel(void *data)
{
    struct display* display = (struct display*)data;
    struct input_event event[6];
    struct timespec rt;
    unsigned int res, n;
    int i, id;

    if (ensure_pipe(display, INPUT_TOUCH))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
       ALOGE("%s:%d error in touch clock_gettime: %s",
            __FILE__, __LINE__, strerror(errno));
    }

    // Cancel all touch points.
    for (i = 0; i < MAX_TOUCHPOINTS; i++) {
        if (display->touch_id[i] != -1) {
            id = display->touch_id[i];
            display->touch_id[i] = -1;

            n = 0;
            // Turn finger into palm.
            ADD_EVENT(EV_ABS, ABS_MT_SLOT, i);
            ADD_EVENT(EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_PALM);
            ADD_EVENT(EV_SYN, SYN_REPORT, 0);
            // Lift off.
            ADD_EVENT(EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER);
            ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, -1);
            ADD_EVENT(EV_SYN, SYN_REPORT, 0);

            res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
            if (res < sizeof(event))
                ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
        }
    }
}

void *event_loop_thread(void *arg) {
    ALOGE("input_loop_event start");
    struct display* display = (struct display*)arg;
    xcb_connection_t *connection = display->xcbconnection;

    xcb_generic_event_t *event;
    while ((event = xcb_wait_for_event(connection))) {
        if (!event) {
            if (xcb_connection_has_error(connection)) {
                ALOGE("XCB connection error: %d", xcb_connection_has_error(connection));
                break;
            }
            ALOGE("error event is null. ");
            continue;
        }
        uint8_t event_type = event->response_type & ~0x80;
        ALOGD("Processing event: type=%d", event_type);
        if(display->multi_windows){
            if(display->im){
                if (!xcb_xim_filter_event(display->im, event)) {
                    // Forward event to input method if IC is created.
                    if (display->ic && (((event->response_type & ~0x80) == XCB_KEY_PRESS) ||
                               ((event->response_type & ~0x80) == XCB_KEY_RELEASE))) {
                        xcb_xim_forward_event(display->im, display->ic, (xcb_key_press_event_t *)event);
                        free(event);
                        continue;
                    }
                }else{
                    ALOGI("event has been consumed by input method. ");
                    free(event);
                    continue;
                }
            }else{
                ALOGE("error im is null. ");
            }
        }

        switch (event_type) {
            case XCB_FOCUS_IN:{
                xcb_focus_in_event_t *focus = (xcb_focus_in_event_t *)event;
                xcb_window_t focused_win = focus->event;
                int taskId = -1;
                {
                    std::scoped_lock lock(display->windowsMutex);
                    for (auto it = display->x11_windows->begin(); it != display->x11_windows->end(); it++) {
                        ALOGI("Task : %s", it->first.c_str());
                        if (it->second->xcbwindow == focused_win){
                            ALOGI("Task %s focus_in", it->first.c_str());
                            if (display->task != nullptr) {
                                if (it->first != "Openfde" && it->first != "none" && it->first != "0") {
                                    if(isValidInteger(it->first)){
                                        taskId = stoi(it->first);
                                    }
                                }
                            }
                            if (it->first != "Toast" && it->first.find("Application Not Responding:") ==  std::string::npos && !isStartWithSpecialSymbols(it->first)) {
                                ALOGI("XCB_FOCUS_IN focus_win: %u task %s XAutoRepeatOff", focused_win, it->first.c_str());
                                disable_auto_repeat(display->x11display);
                            }
                        }
                    }
                }
                if(taskId != -1){
                    display->task->setFocusedTask(taskId);
                    ALOGI("display->task->setFocusedTask: %d", taskId);
                }
                XkbStateRec state;
                XkbGetState(display->x11display, XkbUseCoreKbd, &state);
                bool externalCapsLockState = state.locked_mods & LockMask;
                if (externalCapsLockState) {
                    ALOGD("XCB_FOCUS_IN External Caps Lock is ON");
                }else{
                    ALOGD("XCB_FOCUS_IN External Caps Lock is OFF");
                }
                {
                    std::scoped_lock caps_lock(display->internalCapsLockStateMutex);
                    if(display->internalCapsLockState != externalCapsLockState){
                        display->internalCapsLockState = externalCapsLockState;
                        ALOGD("XCB_FOCUS_IN update display->internalCapsLockState: %d", display->internalCapsLockState);
                        send_key_event(display, KEY_CAPSLOCK, X11_KEYBOARD_KEY_STATE_PRESSED);
                        send_key_event(display, KEY_CAPSLOCK, X11_KEYBOARD_KEY_STATE_RELEASED);
                    }
                }

                bool externalNumLockState = state.locked_mods & Mod2Mask;
                if (externalNumLockState) {
                    ALOGD("XCB_FOCUS_IN External Num Lock is ON");
                }else{
                    ALOGD("XCB_FOCUS_IN External Num Lock is OFF");
                }
                {
                    std::scoped_lock num_lock(display->internalNumLockStateMutex);
                    if(display->internalNumLockState != externalNumLockState){
                        display->internalNumLockState = externalNumLockState;
                        ALOGD("XCB_FOCUS_IN update display->internalNumLockState: %d", display->internalNumLockState);
                        send_key_event(display, KEY_NUMLOCK, X11_KEYBOARD_KEY_STATE_PRESSED);
                        send_key_event(display, KEY_NUMLOCK, X11_KEYBOARD_KEY_STATE_RELEASED);
                    }
                }
                break;
            }
            case XCB_FOCUS_OUT:{
                xcb_focus_out_event_t *focus_out = (xcb_focus_out_event_t *)event;
                xcb_window_t focus_out_win = focus_out->event;
                {
                    std::scoped_lock lock(display->windowsMutex);
                    for (auto it = display->x11_windows->begin(); it != display->x11_windows->end(); it++) {
                        if (it->second->xcbwindow == focus_out_win){
                            ALOGI("Task : %s focus_out", it->first.c_str());
                            if (it->first != "Toast" && it->first.find("Application Not Responding:") ==  std::string::npos && !isStartWithSpecialSymbols(it->first)) {
                                ALOGI("XCB_FOCUS_OUT focus_out_win: %u task %s XAutoRepeatOn", focus_out_win, it->first.c_str());
                                enable_auto_repeat(display->x11display);
                            }
                        }
                    }
                }
                for (size_t i = 0; i < display->keysDown.size(); i++) {
                    if (display->keysDown[i] == X11_KEYBOARD_KEY_STATE_PRESSED) {
                        send_key_event(display, i, X11_KEYBOARD_KEY_STATE_RELEASED);
                    }
                }
                break;
            }
            case XCB_CLIENT_MESSAGE: {
                xcb_client_message_event_t *cm = (xcb_client_message_event_t *)event;
                ALOGI("cm->type: %d, cm->data.data32[0]: %d", cm->type, cm->data.data32[0]);
                xcb_window_t focused_win = cm ->window;
                int taskId = -1;
                {
                    std::scoped_lock lock(display->windowsMutex);
                    for (auto it = display->x11_windows->begin(); it != display->x11_windows->end(); it++) {
                        ALOGI("Task : %s", it->first.c_str());
                        if (it->second->xcbwindow == focused_win){
                            ALOGI("it->second->wm_protocols: %d, it->second->wm_delete_window: %d",
                                it->second->wm_protocols, it->second->wm_delete_window);
                            if (cm->type == it->second->wm_protocols && cm->data.data32[0] == it->second->wm_delete_window) {
                                if (display->task != nullptr) {
                                    if (it->first != "Openfde" && it->first != "none" && it->first != "0") {
                                        ALOGI("remove task %s", it->first.c_str());
                                        if(isValidInteger(it->first)){
                                            property_set(("fde_running_task_" + it->first).c_str(), "false");
                                            taskId = stoi(it->first);
                                        }
                                    }else{
                                        ALOGE("Received XCB_CLIENT_MESSAGE, ignoring\n");
                                    }
                                    if (it->first != "Toast" && it->first.find("Application Not Responding:") ==  std::string::npos && !isStartWithSpecialSymbols(it->first)) {
                                        ALOGI("XCB_CLIENT_MESSAGE delete window: %u task %s XAutoRepeatOn", focused_win, it->first.c_str());
                                        enable_auto_repeat(display->x11display);
                                    }
                                }
                            }
                            break;
                        }
                    }
                }
                if(taskId != -1){
                    display->task->removeTask(taskId);
                    ALOGI("display->task->removeTask: %d", taskId);
                }
                break;
            }
            case XCB_BUTTON_PRESS:{
                    ALOGV("XCB_BUTTON_PRESS received");
                    xcb_button_press_event_t *xcb_button_event = (xcb_button_press_event_t *)event;
                    xcb_window_t focus_window = xcb_button_event->event;
                    int taskId = -1;
                    {
                        std::scoped_lock lock(display->windowsMutex);
                        for (auto it = display->x11_windows->begin(); it != display->x11_windows->end(); it++) {
                            ALOGI("XCB_BUTTON_PRESS Task : %s", it->first.c_str());
                            if (it->second->xcbwindow == focus_window){
                                ALOGI("XCB_BUTTON_PRESS Task %s gained focus", it->first.c_str());
                                if (display->task != nullptr) {
                                    if (it->first != "Openfde" && it->first != "none" && it->first != "0") {
                                        if(isValidInteger(it->first)){
                                            taskId = stoi(it->first);
                                        }
                                    }
                                }
                            }
                        }
                    }
                    if(taskId != -1){
                        display->task->setFocusedTask(taskId);
                    }
                    if (dispatcher.button_press_cb) {
                        dispatcher.button_press_cb(arg, (xcb_button_press_event_t *)event);
                    }
                    if(display->multi_windows){
                        xcb_button_press_event_t *xcb_button_event = (xcb_button_press_event_t *)event;
                        if(display->im){
                            xcb_point_t spot = {xcb_button_event->root_x, xcb_button_event->root_y};
                            ALOGI("on button press update_spot_location x: %d, y: %d", spot.x, spot.y);
                            update_spot_location(display->im, display->ic, spot);
                        }
                    }
                    break;
                }
            case XCB_BUTTON_RELEASE:
                ALOGD("XCB_BUTTON_RELEASE received");
                if (dispatcher.button_release_cb) {
                    dispatcher.button_release_cb(arg, (xcb_button_release_event_t *)event);
                }
                break;
            case XCB_MOTION_NOTIFY:
                if (dispatcher.motion_notify_cb) {
                    dispatcher.motion_notify_cb(arg, (xcb_motion_notify_event_t *)event);
                }
                break;
            case XCB_KEY_PRESS:
                ALOGD("XCB_KEY_PRESS received");
                if (dispatcher.key_press_cb) {
                    dispatcher.key_press_cb(arg, (xcb_key_press_event_t *)event);
                }else{
                    ALOGE("error dispatcher.key_press_cb is null.");
                }
                break;
            case XCB_KEY_RELEASE:
                ALOGD("XCB_KEY_RELEASE received");
                if (dispatcher.key_release_cb) {
                    dispatcher.key_release_cb(arg, (xcb_key_release_event_t *)event);
                }else{
                    ALOGE("error dispatcher.key_release_cb is null.");
                }
                break;
            case XCB_GE_GENERIC:
                xcb_ge_generic_event_t *ge = (xcb_ge_generic_event_t *)event;

                if (ge->extension == xi_opcode) {
                    switch (ge->event_type) {
                        case XCB_INPUT_TOUCH_BEGIN: {
                            xcb_input_touch_begin_event_t *tb = (xcb_input_touch_begin_event_t *)ge;
                            double x = XI_FP1616_TO_DOUBLE(tb->event_x);
                            double y = XI_FP1616_TO_DOUBLE(tb->event_y);
                            ALOGD("Touch Begin: touchid=%" PRIu32 ", x=%.2f, y=%.2f, deviceid=%d\n", tb->detail, x, y, tb->deviceid);
                            touch_handle_down(display, tb->detail, (int)x, (int)y);
                            break;
                        }
                        case XCB_INPUT_TOUCH_UPDATE: {
                            xcb_input_touch_update_event_t *tu = (xcb_input_touch_update_event_t *)ge;
                            double x = XI_FP1616_TO_DOUBLE(tu->event_x);
                            double y = XI_FP1616_TO_DOUBLE(tu->event_y);
                            //ALOGD("Touch Update: touchid=%" PRIu32 ", x=%.2f, y=%.2f, deviceid=%d\n", tu->detail, x, y, tu->deviceid);
                            touch_handle_motion(display, tu->detail, (int)x, (int)y);
                            break;
                        }
                        case XCB_INPUT_TOUCH_END: {
                            xcb_input_touch_end_event_t *te = (xcb_input_touch_end_event_t *)ge;
                            double x = XI_FP1616_TO_DOUBLE(te->event_x);
                            double y = XI_FP1616_TO_DOUBLE(te->event_y);
                            ALOGD("Touch End: touchid=%" PRIu32 ", x=%.2f, y=%.2f, deviceid=%d\n", te->detail, x, y, te->deviceid);
                            touch_handle_up(display, te->detail);
                            break;
                        }
                    }
                }
                break;
        }
        free(event);
    }
    if(display->multi_windows){
        if(display->im){
            xcb_xim_close(display->im);
            xcb_xim_destroy(display->im);
            display->im = NULL;
        }
    }

    ALOGE("Exiting XCB event loop");
    abort();
    return NULL;
}


void forward_event(xcb_xim_t *im, xcb_xic_t ic, xcb_key_press_event_t *event,
                   void *user_data) {
    (void) im;
    (void) ic;
    //(void) user_data;
    struct display* display = (struct display*)user_data;
    ALOGE("Key %s Keycode %u, State %u\n",
            event->response_type == XCB_KEY_PRESS ? "press" : "release",
            event->detail, event->state);
    if(!display){
        ALOGE("error display is null.");
        return;
    }
    if(event->response_type == XCB_KEY_PRESS){
        if (dispatcher.key_press_cb) {
            dispatcher.key_press_cb(display, (xcb_key_press_event_t *)event);
        }else{
            ALOGE("error dispatcher.key_press_cb is null.");
        }
    }else if(event->response_type == XCB_KEY_RELEASE){
        if (dispatcher.key_release_cb) {
            dispatcher.key_release_cb(display, (xcb_key_release_event_t *)event);
        }else{
            ALOGE("error dispatcher.key_release_cb is null.");
        }
    }
}

void commit_string(xcb_xim_t *im, xcb_xic_t ic, uint32_t flag, char *str,
                   uint32_t length, uint32_t *keysym, size_t nKeySym,
                   void *user_data) {
    (void) im;
    (void) ic;
    (void) flag;
    (void) keysym;
    (void) nKeySym;
    //(void) user_data;
    struct display* display = (struct display*)user_data;
    if(!display){
        ALOGE("commit_string error display is NULL");
        return;
    }
    android::hardware::hidl_string text;
    if (xcb_xim_get_encoding(im) == XCB_XIM_UTF8_STRING) {
        ALOGE("key commit utf8: %.*s", length, str);
        if (length > 0) {
            int l = length;
            text.setToExternal(str, length);
            ALOGE("display->task->commitText: %.*s",l, str);
            display->task->commitText(text);
        }
    } else if (xcb_xim_get_encoding(im) == XCB_XIM_COMPOUND_TEXT) {
        size_t newLength = 0;
        char *utf8 = xcb_compound_text_to_utf8(str, length, &newLength);
        if (utf8) {
            int l = newLength;
            ALOGE("key commit: %.*s", l, utf8);
            if (newLength > 0) {
                text.setToExternal(utf8, newLength);
                ALOGE("display->task->commitText: %.*s",l, utf8);
                display->task->commitText(text);
            }
        }
    }
}

void disconnected(xcb_xim_t *im, void *user_data) {
    (void) im;
    (void) user_data;
    ALOGE("disconnected from input method server.");
}

xcb_xim_im_callback callback = {
    .forward_event = forward_event,
    .commit_string = commit_string,
    .disconnected = disconnected,
};

void create_ic_callback(xcb_xim_t *im, xcb_xic_t new_ic, void *user_data) {
    ALOGE("create_ic_callback called");
    //(void) user_data;
    struct display* display = (struct display*)user_data;
    if(!display){
        ALOGE("error display is NULL");
        return;
    }
    display->ic = new_ic;
    if (display->ic) {
        ALOGE("new ic id:%u ", display->ic);
        xcb_xim_set_ic_focus(im, display->ic);
    }
}

void open_im_callback(xcb_xim_t *im, void *user_data) {
    ALOGE("open_callback called");
    //(void)user_data;
    struct display* display = (struct display*)user_data;
    if(!display){
        ALOGE("error display is NULL");
        return;
    }
    uint32_t input_style = XCB_IM_PreeditPosition | XCB_IM_StatusArea;
    xcb_point_t spot;
    spot.x = 800;
    spot.y = 500;
    xcb_xim_nested_list nested =
        xcb_xim_create_nested_list(im, XCB_XIM_XNSpotLocation, &spot, NULL);
    xcb_xim_create_ic(im, create_ic_callback, display, XCB_XIM_XNInputStyle,
                      &input_style, XCB_XIM_XNClientWindow, &display->xcbscreen->root,
                      XCB_XIM_XNFocusWindow, &display->xcbscreen->root, XCB_XIM_XNPreeditAttributes,
                      &nested, NULL);
    free(nested.data);
}

void set_window_title(xcb_connection_t *connection, xcb_window_t window, const std::string &title) {
    ALOGE("set_window_title %s", title.c_str());

    const char *utf8_title = title.c_str();

    // set WM_NAME (STRING type, compatible with old window managers)
    xcb_change_property(connection,
                        XCB_PROP_MODE_REPLACE,
                        window,
                        XCB_ATOM_WM_NAME,
                        XCB_ATOM_STRING,
                        8, // 8-bit encoding
                        strlen(utf8_title),
                        utf8_title);

    // set _NET_WM_NAME (UTF8_STRING type, supports Chinese)
    xcb_intern_atom_cookie_t utf8_string_cookie = xcb_intern_atom(connection, 0, strlen("UTF8_STRING"), "UTF8_STRING");
    xcb_intern_atom_cookie_t net_wm_name_cookie = xcb_intern_atom(connection, 0, strlen("_NET_WM_NAME"), "_NET_WM_NAME");
    xcb_intern_atom_reply_t *utf8_string_reply = xcb_intern_atom_reply(connection, utf8_string_cookie, NULL);
    xcb_intern_atom_reply_t *net_wm_name_reply = xcb_intern_atom_reply(connection, net_wm_name_cookie, NULL);

    if (utf8_string_reply && net_wm_name_reply) {
        xcb_change_property(connection,
                            XCB_PROP_MODE_REPLACE,
                            window,
                            net_wm_name_reply->atom,
                            utf8_string_reply->atom,
                            8, // UTF-8 Use 8-bit encoding
                            strlen(utf8_title),
                            utf8_title);
    } else {
        ALOGE("unable to obtain UTF8_STRING or _NET_WM_NAME atom");
    }

    free(utf8_string_reply);
    free(net_wm_name_reply);

    xcb_flush(connection);
}

void set_window_class(xcb_connection_t *connection, xcb_window_t window, const std::string &instance_name, const std::string &class_name) {
    std::string wm_class = instance_name + '\0' + class_name + "_fde" + '\0';
    xcb_change_property(connection,
                        XCB_PROP_MODE_REPLACE,
                        window,
                        XCB_ATOM_WM_CLASS,
                        XCB_ATOM_STRING,
                        8,
                        wm_class.length(),
                        wm_class.c_str());
    xcb_flush(connection);
}

bool isStartWithSpecialSymbols(const std::string& layer_name) {
    return !layer_name.empty() && layer_name[0] == '#';
}

bool isStartWithTidSymbols(const std::string& layer_name){
    return !layer_name.empty() && layer_name.find("TID") != std::string::npos;
}

struct window *
create_window(struct display *display, bool use_subsurfaces, std::string appID, std::string taskID, hwc_color_t color)
{
    struct window *window = new struct window();
    if (!window)
        return NULL;

    std::string appID_title;
    window->display = display;
    window->appID = appID;
    window->taskID = taskID;
    window->isActive = true;
    window->xcbwindow = 0;

    bool calibrating = !display->height || !display->width;

        const hidl_string appID_hidl(appID);
        hidl_string appName_hidl(appID);
	//rename the appid to Openfde to match the desktop file name openfde.desktop 
        if (appID != "Openfde" && display->task)
            display->task->getAppName(appID_hidl, [&](const hidl_string &value)
                                      {
				       appID_title = value;
                    });
        else{
	     appID_title = appID;
	}

        if (appID != "Openfde")
            appID = "openfde." + appID;
   
    finished_computing_scale(display);

    if (calibrating) {
        // If we did not receive a window size from the compositor we have to fall back to using the whole output size
        // At the time of writing this happens on wlroots compositors
        if (!display->height)
            display->height = display->full_height / display->scale;
        if (!display->width)
            display->width = display->full_width / display->scale;
    }
     display->colormap = xcb_generate_id(display->xcbconnection);
     xcb_create_colormap(display->xcbconnection, XCB_COLORMAP_ALLOC_NONE, display->colormap, display->xcbscreen->root, display->visualid);

    uint32_t value_mask = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
    uint32_t value_list[] = {
        0, 
        0,
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_FOCUS_CHANGE,
        display->colormap
    };
    ALOGE("create window for taskID: %s", taskID.c_str());
    window->xcbwindow = xcb_generate_id(display->xcbconnection);
    ALOGE("xcb create window window->xcbwindow: %u", window->xcbwindow);

    xcb_create_window(display->xcbconnection,
                    32,
                    window->xcbwindow,
                    display->xcbscreen->root,
                    0, 0, display->width, display->height, 0,
                    XCB_WINDOW_CLASS_INPUT_OUTPUT,
                    display->visualid,
                    value_mask, value_list);
    if (!use_subsurfaces) {
	    // 设置窗口全屏
    xcb_intern_atom_cookie_t fullscreen_cookie = xcb_intern_atom(display->xcbconnection, 0, strlen("_NET_WM_STATE_FULLSCREEN"), "_NET_WM_STATE_FULLSCREEN");
    xcb_intern_atom_reply_t *fullscreen_reply = xcb_intern_atom_reply(display->xcbconnection, fullscreen_cookie, NULL);
    xcb_intern_atom_cookie_t state_cookie = xcb_intern_atom(display->xcbconnection, 0, strlen("_NET_WM_STATE"), "_NET_WM_STATE");
    xcb_intern_atom_reply_t *state_reply = xcb_intern_atom_reply(display->xcbconnection, state_cookie, NULL);

    if (fullscreen_reply && state_reply) {
        xcb_change_property(display->xcbconnection,
                            XCB_PROP_MODE_REPLACE,
                            window->xcbwindow,
                            state_reply->atom,
                            XCB_ATOM_ATOM,
                            32,
                            1,
                            &fullscreen_reply->atom);
        free(fullscreen_reply);
        free(state_reply);
    }
    }
	if (use_subsurfaces) {
		xcb_shape_rectangles(display->xcbconnection,
		XCB_SHAPE_SO_SET,        // 设置操作（替换现有形状）
		XCB_SHAPE_SK_INPUT,
		XCB_CLIP_ORDERING_UNSORTED,
		window->xcbwindow,
		0, 0, 0, NULL);          // 0个矩形，NULL数组
	}
    XRenderPictureAttributes pa;
    pa.repeat = False;
    window->xpicture = XRenderCreatePicture(display->x11display, window->xcbwindow,display->argb_format, CPRepeat, &pa);
    window->backpixmap = XCreatePixmap(display->x11display, window->xcbwindow, display->width, display->height, 32);
    if (window->backpixmap == None1) {
        return NULL;
    }
    window->backxpicture = XRenderCreatePicture(display->x11display, window->backpixmap,display->argb_format, CPRepeat, &pa);
        // 用透明填充backpixmap
    XRenderColor transparent_color = {0, 0, 0, 0};  // RGBA all zero for transparent
    XRenderFillRectangle(display->x11display, PictOpSrc, window->backxpicture, &transparent_color, 0, 0, display->width, display->height);

    window->xcbgc = xcb_generate_id(display->xcbconnection);
    xcb_create_gc(display->xcbconnection,window->xcbgc, window->xcbwindow, 0, NULL);
    remove_title(display->xcbconnection, window->xcbwindow);

    xcb_dri3_open_cookie_t dri3_cookie = xcb_dri3_open(display->xcbconnection, window->xcbwindow, 0);
    xcb_dri3_open_reply_t *dri3_reply = xcb_dri3_open_reply(display->xcbconnection, dri3_cookie, NULL);
    if (!dri3_reply) {
        ALOGE("Cannot open DRI3 connection");
        return NULL;
    }
    window->dri3_fd = dri3_reply->nfd > 0 ? xcb_dri3_open_reply_fds(display->xcbconnection, dri3_reply)[0] : -1;
    free(dri3_reply);
    if (window->dri3_fd < 0) {
        ALOGE("Cannot get DRI3 file descriptor");
        return NULL;
    }

    //Register WM_PROTOCOLS and WM_DELETE_WINDOW
    xcb_intern_atom_cookie_t wm_protocols_cookie = xcb_intern_atom(display->xcbconnection, 0, strlen("WM_PROTOCOLS"), "WM_PROTOCOLS");
    xcb_intern_atom_cookie_t wm_delete_cookie = xcb_intern_atom(display->xcbconnection, 0, strlen("WM_DELETE_WINDOW"), "WM_DELETE_WINDOW");
    xcb_intern_atom_reply_t *wm_protocols_reply = xcb_intern_atom_reply(display->xcbconnection, wm_protocols_cookie, NULL);
    xcb_intern_atom_reply_t *wm_delete_reply = xcb_intern_atom_reply(display->xcbconnection, wm_delete_cookie, NULL);
    if(wm_protocols_reply || wm_delete_reply){
        xcb_atom_t wm_protocols = wm_protocols_reply->atom;
        xcb_atom_t wm_delete_window = wm_delete_reply->atom;
        window->wm_protocols = wm_protocols;
        window->wm_delete_window = wm_delete_window;
        free(wm_protocols_reply);
        free(wm_delete_reply);

        //Set the WM_PROTOCOLS property to declare support for WM_DELETE_WINDOW
        xcb_change_property(display->xcbconnection, XCB_PROP_MODE_REPLACE, window->xcbwindow,
                            wm_protocols, XCB_ATOM_ATOM, 32,
                            1, &wm_delete_window);
    }

    // hide Toast window in taskbar
    if (appID_title == "Toast" || isStartWithSpecialSymbols(appID_title)) {
        xcb_intern_atom_cookie_t cookie_net_wm_state =
            xcb_intern_atom(display->xcbconnection, 0, strlen("_NET_WM_STATE"), "_NET_WM_STATE");
        xcb_intern_atom_cookie_t cookie_skip_taskbar =
            xcb_intern_atom(display->xcbconnection, 0, strlen("_NET_WM_STATE_SKIP_TASKBAR"), "_NET_WM_STATE_SKIP_TASKBAR");
        xcb_intern_atom_reply_t *reply_net_wm_state =
            xcb_intern_atom_reply(display->xcbconnection, cookie_net_wm_state, NULL);
        xcb_intern_atom_reply_t *reply_skip_taskbar =
            xcb_intern_atom_reply(display->xcbconnection, cookie_skip_taskbar, NULL);
        if (reply_net_wm_state || reply_skip_taskbar) {
            free(reply_net_wm_state);
            free(reply_skip_taskbar);

            xcb_atom_t atoms[] = { reply_skip_taskbar->atom };
            xcb_change_property(
                display->xcbconnection,
                XCB_PROP_MODE_REPLACE,
                window->xcbwindow,
                reply_net_wm_state->atom,
                XCB_ATOM_ATOM,
                32,
                sizeof(atoms) / sizeof(xcb_atom_t),
                atoms
            );
        }
    }

    set_window_title(display->xcbconnection, window->xcbwindow, appID_title);
    set_window_class(display->xcbconnection, window->xcbwindow, window->appID, window->appID);

    xcb_map_window(display->xcbconnection, window->xcbwindow);
    ALOGI("xcreate xcb window %s color %d, width %d height %d",appID_title.c_str(),color.a, display->width,display->height);

    /* Check the XInput extension and get the opcode. */
    const xcb_query_extension_reply_t *ext_reply =
        xcb_get_extension_data(display->xcbconnection, &xcb_input_id);
    if (!ext_reply || !ext_reply->present) {
        ALOGE("XInput extension is unavailable.");
        return window;
    }
    xi_opcode = ext_reply->major_opcode;

    xcb_input_xi_query_version_reply_t *ver_reply =
        xcb_input_xi_query_version_reply(display->xcbconnection,
                                         xcb_input_xi_query_version(display->xcbconnection, 2, 2),
                                         NULL);
    if (!ver_reply || ver_reply->major_version < 2 ||
        (ver_reply->major_version == 2 && ver_reply->minor_version < 2)) {
        ALOGE("XInput version is too low and does not support touch events.");
        free(ver_reply);
        return window;
    }
    free(ver_reply);
    ALOGE("XInput version: %d.%d\n", ver_reply->major_version, ver_reply->minor_version);

    uint32_t touch_mask = XCB_INPUT_XI_EVENT_MASK_TOUCH_BEGIN |
                          XCB_INPUT_XI_EVENT_MASK_TOUCH_UPDATE |
                          XCB_INPUT_XI_EVENT_MASK_TOUCH_END;
    // Optional：add XCB_INPUT_XI_EVENT_MASK_TOUCH_OWNERSHIP to handle ownership

    struct {
        xcb_input_event_mask_t head;
        uint32_t               mask;
    } event_select;

    memset(&event_select, 0, sizeof(event_select));

    event_select.head.deviceid = XCB_INPUT_DEVICE_ALL_MASTER;
    event_select.head.mask_len = 1;
    event_select.mask = touch_mask;

    xcb_input_xi_select_events(display->xcbconnection, window->xcbwindow, 1, &event_select.head);
    xcb_flush(display->xcbconnection);

    return window;
}

void disable_auto_repeat(Display *display) {
    ALOGD("disable keyboard auto_repeat_mode");
    XAutoRepeatOff(display);
    XFlush(display);
}

void enable_auto_repeat(Display *display) {
    ALOGD("enable keyboard auto_repeat_mode");
    XAutoRepeatOn(display);
    XFlush(display);
}


struct display *
create_display(const char *gralloc)
{
    struct display *display = new struct display();
    if (display == NULL) {
        ALOGE("out of memory");
        return NULL;
    }
    display->gtype = get_gralloc_type(gralloc);
    display->refresh = 0;
    display->isMaximized = true;

    display->x11display = NULL;
    ALOGI("x11 display env %s", getenv("DISPLAY"));
    const char* display_env = getenv("DISPLAY");
    std::string display_path = "unix:/tmpx11/X0";
    if (display_env && strlen(display_env) > 0) {
        std::string display_str(display_env);
        // remove the colon from :0
        display_str.erase(std::remove(display_str.begin(), display_str.end(), ':'), display_str.end());
        display_path = "unix:/tmpx11/X" + display_str;
    }
    display->x11display = XOpenDisplay(display_path.c_str());
    if (!display->x11display){
        ALOGE("Couldn't connect to X11 display.");
	return NULL;
    }
	display->xcbconnection = XGetXCBConnection(display->x11display);
    if (xcb_connection_has_error(display->xcbconnection)) {
        ALOGE("Couldn't connect to X11 display.");
        xcb_disconnect(display->xcbconnection);
        delete display;
        return NULL;
    }
    ALOGD("XMODIFIERS: %s", getenv("XMODIFIERS"));
    XSetEventQueueOwner(display->x11display, XCBOwnsEventQueue);
    display->xcbscreen = xcb_setup_roots_iterator(xcb_get_setup(display->xcbconnection)).data;
    display->screen_default_nbr = XDefaultScreen(display->x11display);
    ALOGE("XDefaultScreen display->screen_default_nbr: %d", display->screen_default_nbr);

    display->multi_windows = property_get_bool("persist.openfde.multi_windows", false);
    if(display->multi_windows){
        display->im = xcb_xim_create(display->xcbconnection, display->screen_default_nbr, NULL);
        xcb_xim_set_im_callback(display->im, &callback, display);
        xcb_xim_set_use_compound_text(display->im, true);
        xcb_xim_set_use_utf8_string(display->im, true);

        // Open connection to XIM server.
        bool result = xcb_xim_open(display->im, open_im_callback, true, display);
        ALOGE("xcb_xim_open result = %d", result);
        if(!result){
            return NULL;
        }
    }

    display->internalCapsLockState = false;
    display->internalNumLockState = false;
    property_set("openfde.x11.display", "1");
    sem_init(&display->egl_go, 0, 0);
    sem_init(&display->egl_done, 0, 0);

    umask(0);
    mkdir("/dev/input", S_IRWXO | S_IRWXG | S_IRWXU);
    chown("/dev/input", 1000, 1000);
    display->task = IOpenfdeTask::getService();
    display->isTouchDown = false;
    display->lastAxisEventNanoSeconds = 0;
    display->gesture_scale = 160;
      // Get screen resolution and scale
    display->scale = 1;
    display->full_width = display->xcbscreen->width_in_pixels;
    display->full_height = display->xcbscreen->height_in_pixels;
    ALOGE("pixels width %d height %d",display->xcbscreen->width_in_pixels, display->xcbscreen->height_in_pixels);

    display->width = display->full_width /display->scale;
    display->height = display->full_height /display->scale;
    struct display *d = (struct display*)display;
    d->input_fd[INPUT_POINTER] = -1;
    d->ptrPrvX = 0;
    d->ptrPrvY = 0;
    d->reverseScroll = property_get_bool("persist.waydroid.reverse_scrolling", false);
    mkfifo(INPUT_PIPE_NAME[INPUT_POINTER], S_IRWXO | S_IRWXG | S_IRWXU);
    chown(INPUT_PIPE_NAME[INPUT_POINTER], 1000, 1000);
    // for emulate touch input event
    d->input_fd[INPUT_TOUCH] = -1;
    mkfifo(INPUT_PIPE_NAME[INPUT_TOUCH], S_IRWXO | S_IRWXG | S_IRWXU);
    chown(INPUT_PIPE_NAME[INPUT_TOUCH], 1000, 1000);
    for (int i = 0; i < MAX_TOUCHPOINTS; i++)
            d->touch_id[i] = -1;

    d->input_fd[INPUT_KEYBOARD] = -1;
    mkfifo(INPUT_PIPE_NAME[INPUT_KEYBOARD], S_IRWXO | S_IRWXG | S_IRWXU);
    chown(INPUT_PIPE_NAME[INPUT_KEYBOARD], 1000, 1000);
    register_key_press_callback(on_key_press);
    register_key_release_callback(on_key_release);
    register_button_press_callback(on_button_press);
    register_button_release_callback(on_button_release);
    register_motion_notify_callback(on_motion_notify);
    if (! find_argb_visual(display)){
	    ALOGE("can't find argb visualid");
	    return NULL;
    }

    display->argb_format = XRenderFindStandardFormat(display->x11display, PictStandardARGB32);


    pthread_t event_thread;
    if (pthread_create(&event_thread, NULL, event_loop_thread, display) != 0) {
        ALOGE("Unable to create event processing thread\n");
    }

    init_timer();
    mDisplay = display;

    return display;
}





void
destroy_display(struct display *display)
{

    if (display->ic) {
        display->ic = 0;
    }
    if(display->multi_windows){
        if (display->im) {
            xcb_xim_close(display->im);
            xcb_xim_destroy(display->im);
            display->im = NULL;
        }
    }

    if (display->xcbscreen) {
        display->xcbscreen = NULL;
    }

    if (display->colormap) {
        xcb_free_colormap(display->xcbconnection, display->colormap);
        display->colormap = 0;
    }

     if (display->xcbconnection) {
        xcb_flush(display->xcbconnection);
        xcb_disconnect(display->xcbconnection);
        display->xcbconnection = NULL;
    }

    if (display->x11display) {
        XCloseDisplay(display->x11display);
        display->x11display = NULL;
    }

    delete display;
}

int remove_title(xcb_connection_t *conn, xcb_window_t main_win){
    xcb_intern_atom_cookie_t hints_cookie = xcb_intern_atom(conn, 0, strlen("_MOTIF_WM_HINTS"), "_MOTIF_WM_HINTS");
    xcb_intern_atom_reply_t *hints_reply = xcb_intern_atom_reply(conn, hints_cookie, NULL);
    if (hints_reply) {
	struct {
	    uint32_t flags;
	    uint32_t functions;
	    uint32_t decorations;
	    int32_t input_mode;
	    uint32_t status;
	} motif_hints = {2, 0, 0, 0, 0}; // flags=2, decorations=0
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, main_win,
			    hints_reply->atom, hints_reply->atom, 32,
			    sizeof(motif_hints) / 4, &motif_hints);
	free(hints_reply);
    }
    return 0;
}

static int find_argb_visual(struct display *display) {
    XVisualInfo vinfo_template = { .screen = DefaultScreen(display->x11display), .depth = 32, .c_class = TrueColor };
    int n_vinfo;
    XVisualInfo *vinfo = XGetVisualInfo(display->x11display, VisualScreenMask | VisualDepthMask | VisualClassMask, &vinfo_template, &n_vinfo);

    if (!vinfo) {
        ALOGE("visual with 32depth not found");
        return 0;
    }

    for (int i = 0; i < n_vinfo; i++) {
        XRenderPictFormat *format = XRenderFindVisualFormat(display->x11display, vinfo[i].visual);
        if (format && format->type == PictTypeDirect && format->direct.alphaMask) {
            ALOGE("found ARGB Visual: id=0x%lx", vinfo[i].visualid);
            display->visualid  = vinfo[i].visualid;
            XFree(vinfo);
            return 1;
        }
    }

    ALOGE("visual with alpha channel not found");
    XFree(vinfo);
    return 0;
}

int
create_shm_buffer(struct buffer *buffer, int width, int height, int format, int pixel_stride, buffer_handle_t target)
{
    // Assume 4bpp formats or none of this is going to work
    int shm_stride = width * 4;
    int size = shm_stride * height;

    buffer->size = size;
    buffer->hal_format = format;
    buffer->format = format;
    assert(buffer->format >= 0);
    buffer->width = width;
    buffer->height = height;
    buffer->pixel_stride = pixel_stride;
    buffer->handle = target;
    buffer->isShm = true;
    int fd = syscall(__NR_memfd_create, "buffer", MFD_ALLOW_SEALING);
    ftruncate(fd, size);
    buffer->shm_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buffer->shm_data == MAP_FAILED) {
        ALOGE("mmap failed");
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

