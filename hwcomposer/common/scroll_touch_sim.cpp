/*
 * Copyright (C) 2026 Openfde Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

// Common scroll-wheel / mouse-button -> Android multi-touch simulation.
// See scroll_touch_sim.h for an architectural note.

#include "scroll_touch_sim.h"

#if defined(USE_WAYLAND_BACKEND)
#include "../wayland/wayland-hwc.h"
#elif defined(USE_X11_BACKEND)
#include "../x11/x11-hwc.h"
#else
#error "scroll_touch_sim.cpp must be compiled with -DUSE_WAYLAND_BACKEND or -DUSE_X11_BACKEND"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <cutils/properties.h>
#include <log/log.h>

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------

// How often the drip timer fires while a scroll session is alive.
#define SCROLL_TICK_MS 16
// Idle time (counted from the last wheel event) before the simulated touch
// is lifted, per scroll gear. The slow gear waits longer so a slow scroll
// glides to a stop instead of snapping.
#define SCROLL_END_TIMEOUT_SLOW_MS   280
#define SCROLL_END_TIMEOUT_MEDIUM_MS 160
#define SCROLL_END_TIMEOUT_FAST_MS   120
// Scroll speed gears: decided by how many wheel events arrive within a time
// window.
#define SCROLL_GEAR_WINDOW_NS        (200 * 1000 * 1000LL)
#define SCROLL_GEAR_FAST_COUNT       5
#define SCROLL_GEAR_MEDIUM_COUNT     2
#define SCROLL_GEAR_SLOW_MULTIPLIER   1
#define SCROLL_GEAR_MEDIUM_MULTIPLIER 2
#define SCROLL_GEAR_FAST_MULTIPLIER   3
#define WHEEL_EVENT_TIMESTAMP_MAX    64
// Extra pixels on top of the touch slop to make sure a scroll is never
// taken as a click.
#define SCROLL_MIN_DISTANCE_MARGIN   4

// Two-finger pinch gesture strides used when ctrl+wheel scaling is on.
#define GESTURE_SCALING_DOWN_STRIDE          30
#define GESTURE_SCALING_UP_STRIDE            480
#define GESTURE_SCALING_DOWN_START_DISTANCE  180.0
#define GESTURE_SCALING_UP_START_DISTANCE    10.0

// ---------------------------------------------------------------------------
// Module-private state. Visible to the backends only via the sim_*
// accessors below.
// ---------------------------------------------------------------------------

// Forward decl so sim_init_timer can wire SIGALRM to it.
extern "C" void sim_timer_handler(int sig);

// Active display, used by the SIGALRM drip-timer handler.
static struct display *sim_mDisplay = nullptr;

// Direction of the most recent wheel axis event.
static bool sim_mVerticalScroll = true;

// Most recent touchMove value (signed pixels per discrete step) emitted by
// the host backend. Used to extend the synthetic touch to the touch slop
// distance if it would otherwise be interpreted as a click.
static int  sim_mTouchMove = 0;

// Gear-triggered last-used start distance / stride for the two-finger pinch.
static double sim_gesture_scaling_start_distance = 0.0;
static int    sim_gesture_scaling_stride = 0;

// Wheel-event rate detector.
static int64_t sim_wheel_event_timestamps[WHEEL_EVENT_TIMESTAMP_MAX];
static int     sim_wheel_event_count = 0;
static int     sim_wheel_event_next = 0;
static int     sim_scroll_session_event_count = 0;
static int     sim_scroll_gear = SIM_SCROLL_GEAR_MEDIUM;

// Pending wheel distance that still has to be dripped out as touch moves.
static int     sim_pending_wheel_move = 0;
static bool    sim_pending_wheel_vertical = true;
// Timestamp of the most recent wheel event, used to detect the scroll end.
static int64_t sim_last_wheel_event_ns = 0;

// Drip timer state.
static struct itimerval sim_timer;
static int sim_timer_active = 0;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static int64_t sim_now_ns(void) {
    struct timespec rt;
    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1)
        return 0;
    return rt.tv_sec * 1000000000LL + rt.tv_nsec;
}

static int sim_end_timeout_ms(void) {
    switch (sim_scroll_gear) {
        case SIM_SCROLL_GEAR_SLOW:   return SCROLL_END_TIMEOUT_SLOW_MS;
        case SIM_SCROLL_GEAR_FAST:   return SCROLL_END_TIMEOUT_FAST_MS;
        case SIM_SCROLL_GEAR_MEDIUM:
        default:                     return SCROLL_END_TIMEOUT_MEDIUM_MS;
    }
}

// Fraction of the remaining pending distance applied per tick. A larger
// divisor spreads the same distance over more frames: slower and smoother.
static int sim_drip_divisor(void) {
    switch (sim_scroll_gear) {
        case SIM_SCROLL_GEAR_SLOW:   return 8;
        case SIM_SCROLL_GEAR_FAST:   return 2;
        case SIM_SCROLL_GEAR_MEDIUM:
        default:                     return 3;
    }
}

static int sim_ensure_pipe_impl(struct display *display, int input_type) {
    // Detect fd that was closed elsewhere (e.g. stray close in hwcomposer.cpp)
    // and force a reopen instead of writing to a stale descriptor.
    if (display->input_fd[input_type] >= 0 &&
        fcntl(display->input_fd[input_type], F_GETFD) == -1 && errno == EBADF) {
        ALOGE("ensure_pipe: input_fd[%d]=%d was closed elsewhere, reopening %s",
              input_type, display->input_fd[input_type], INPUT_PIPE_NAME[input_type]);
        display->input_fd[input_type] = -1;
    }
    if (display->input_fd[input_type] < 0) {
        display->input_fd[input_type] = open(INPUT_PIPE_NAME[input_type],
                                            O_WRONLY | O_NONBLOCK);
        ALOGI("ensure_pipe display->input_fd[%d]: %d",
              input_type, display->input_fd[input_type]);
        if (display->input_fd[input_type] < 0) {
            ALOGE("Failed to open pipe to InputFlinger: %s", strerror(errno));
            return -1;
        }
    }
    return 0;
}

extern "C" int sim_ensure_pipe(struct display *display, int input_type) {
    return sim_ensure_pipe_impl(display, input_type);
}

// SIM_ADD_EVENT is defined in the public header so the wayland/x11
// backends can also use it for the non-scroll paths (mouse motion,
// button press, native wl_touch dispatch, etc.).

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

extern "C" void sim_init_timer(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sim_timer_handler;
    sigaction(SIGALRM, &sa, NULL);
}

extern "C" void sim_set_display(struct display *d) {
    sim_mDisplay = d;
}

extern "C" void sim_clear_display(void) {
    sim_mDisplay = nullptr;
}

// ---------------------------------------------------------------------------
// Timer helpers
// ---------------------------------------------------------------------------

extern "C" void sim_stop_scroll_timer(void) {
    memset(&sim_timer, 0, sizeof(sim_timer));
    setitimer(ITIMER_REAL, &sim_timer, NULL);
    sim_timer_active = 0;
}

extern "C" void sim_reset_timer(void) {
    property_set("fde.axis_converting_touch", "true");

    memset(&sim_timer, 0, sizeof(sim_timer));

    sim_timer.it_value.tv_sec = 0;
    sim_timer.it_value.tv_usec = SCROLL_TICK_MS * 1000;
    sim_timer.it_interval.tv_sec = 0;
    sim_timer.it_interval.tv_usec = SCROLL_TICK_MS * 1000;

    setitimer(ITIMER_REAL, &sim_timer, NULL);
    sim_timer_active = 1;
    sim_last_wheel_event_ns = sim_now_ns();
}

// ---------------------------------------------------------------------------
// Input event emission
// ---------------------------------------------------------------------------

extern "C" void sim_send_key_event(struct display *display,
                                   uint32_t key, uint32_t state) {
    struct input_event event[1];
    struct timespec rt;
    ssize_t written;
    unsigned int n = 0;

    if (key >= display->keysDown.size()) {
        ALOGE("Invalid key: %u", key);
        return;
    }

    if (sim_ensure_pipe(display, INPUT_KEYBOARD))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }
    SIM_ADD_EVENT(EV_KEY, key, state);

    ALOGE("send_key_event write INPUT_KEYBOARD key: %d, state: %u", key, state);
    written = write(display->input_fd[INPUT_KEYBOARD], &event, sizeof(event));
    if (written < (ssize_t)sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
    display->keysDown[(uint8_t)key] = (uint8_t)state;
}

extern "C" void sim_pointer_button_to_touch_down(struct display *display) {
    struct input_event event[6];
    struct timespec rt;
    ssize_t res;
    unsigned int n = 0;

    if (sim_ensure_pipe(display, INPUT_TOUCH))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    SIM_ADD_EVENT(EV_ABS, ABS_MT_SLOT, 0);
    SIM_ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, 0);
    SIM_ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, display->ptrPrvX);
    SIM_ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, display->ptrPrvY);
    SIM_ADD_EVENT(EV_ABS, ABS_MT_PRESSURE, 50);
    SIM_ADD_EVENT(EV_SYN, SYN_REPORT, 0);
    display->isTouchDown = true;
    res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));

    if (res < (ssize_t)sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

extern "C" void sim_pointer_button_to_touch_up(struct display *display) {
    struct input_event event[3];
    struct timespec rt;
    ssize_t res;
    unsigned int n = 0;

    if (sim_ensure_pipe(display, INPUT_TOUCH))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    SIM_ADD_EVENT(EV_ABS, ABS_MT_SLOT, 0);
    SIM_ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, -1);
    SIM_ADD_EVENT(EV_SYN, SYN_REPORT, 0);
    display->isTouchDown = false;
    res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));

    if (res < (ssize_t)sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

extern "C" void sim_pointer_cancel_axis_to_two_finger_touch(struct display *display) {
    struct input_event event[6];
    struct timespec rt;
    ssize_t res;
    unsigned int n = 0;

    if (sim_ensure_pipe(display, INPUT_TOUCH))
        return;

    display->axis_simulation_two_finger_started = false;
    display->gesture_scale = 160;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
       ALOGE("%s:%d error in touch clock_gettime: %s",
            __FILE__, __LINE__, strerror(errno));
    }

    SIM_ADD_EVENT(EV_ABS, ABS_MT_SLOT, 0);
    SIM_ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, -1);
    SIM_ADD_EVENT(EV_SYN, SYN_REPORT, 0);
    SIM_ADD_EVENT(EV_ABS, ABS_MT_SLOT, 1);
    SIM_ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, -1);
    SIM_ADD_EVENT(EV_SYN, SYN_REPORT, 0);
    res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
    if (res < (ssize_t)sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

extern "C" int sim_min_scroll_distance(struct display *display) {
    int density = property_get_int32("ro.sf.lcd_density",
                                     int(180 * display->scale));
    return 8 * density / 160 + SCROLL_MIN_DISTANCE_MARGIN;
}

extern "C" bool sim_pointer_cancel_axis_to_touch(struct display *display,
                                                 bool from_axis_stop_event,
                                                 bool force) {
    display->wheelEvtIsDiscrete = false;
    // Check if the scroll wheel event has started.
    if (display->lastAxisEventNanoSeconds == 0) {
        return true;
    }

    struct input_event event[18];
    struct timespec rt;
    ssize_t res;
    unsigned int n = 0;

    if (sim_ensure_pipe(display, INPUT_TOUCH))
        return false;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    // Prevent minor mouse movements from interrupting the touch scrolling
    // process while scrolling the wheel.
    if (!force && ((rt.tv_sec * 1000 * 1000 * 1000 + rt.tv_nsec -
                    display->lastAxisEventNanoSeconds) < 300 * 1000 * 1000)) {
        return false;
    }

    // The scroll session is being terminated: drop any wheel distance that is
    // still pending and stop the drip timer so it cannot restart the touch.
    sim_pending_wheel_move = 0;
    sim_stop_scroll_timer();

    // Guarantee the simulated scroll moves at least the touch slop distance,
    // otherwise the system interprets the short down/up sequence as a click
    // instead of a scroll.
    {
        int moved = sim_mVerticalScroll ? display->axisY - display->ptrPrvY
                                       : display->axisX - display->ptrPrvX;
        int minDistance = sim_min_scroll_distance(display);
        if (std::abs(moved) < minDistance) {
            int direction;
            if (moved != 0)
                direction = moved > 0 ? 1 : -1;
            else
                direction = sim_mTouchMove >= 0 ? 1 : -1;
            if (sim_mVerticalScroll)
                display->axisY += direction * (minDistance - std::abs(moved));
            else
                display->axisX += direction * (minDistance - std::abs(moved));
            SIM_ADD_EVENT(EV_ABS, ABS_MT_SLOT, SIM_AXIS_TOUCH_SLOT_ID);
            SIM_ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, SIM_AXIS_TOUCH_TRACKING_ID);
            if (sim_mVerticalScroll) {
                SIM_ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, display->ptrPrvX);
                SIM_ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, display->axisY);
            } else {
                SIM_ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, display->axisX);
                SIM_ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, display->ptrPrvY);
            }
            SIM_ADD_EVENT(EV_ABS, ABS_MT_PRESSURE, 50);
            SIM_ADD_EVENT(EV_SYN, SYN_REPORT, 0);
        }
    }

    display->axisY = display->ptrPrvY;
    display->lastAxisEventNanoSeconds = 0;

    if (!from_axis_stop_event) {
        // Use the second touch click to prevent inertial scrolling
        SIM_ADD_EVENT(EV_ABS, ABS_MT_SLOT, SIM_AXIS_TOUCH_SLOT_ID + 1);
        SIM_ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, SIM_AXIS_TOUCH_TRACKING_ID + 1);
        SIM_ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, -1);
        SIM_ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, -1);
        SIM_ADD_EVENT(EV_ABS, ABS_MT_PRESSURE, 50);
        SIM_ADD_EVENT(EV_SYN, SYN_REPORT, 0);
    }

    SIM_ADD_EVENT(EV_ABS, ABS_MT_SLOT, SIM_AXIS_TOUCH_SLOT_ID);
    SIM_ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, -1);
    SIM_ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    if (!from_axis_stop_event) {
        SIM_ADD_EVENT(EV_ABS, ABS_MT_SLOT, SIM_AXIS_TOUCH_SLOT_ID + 1);
        SIM_ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, -1);
        SIM_ADD_EVENT(EV_SYN, SYN_REPORT, 0);
    }
    res = write(display->input_fd[INPUT_TOUCH], &event,
                n * sizeof(struct input_event));
    if (res < (ssize_t)(n * sizeof(struct input_event))) {
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
        return false;
    }

    return true;
}

extern "C" void sim_pointer_axis_to_touch(struct display *display,
                                          int move_amount,
                                          bool vertical_scroll) {
    struct input_event event[6];
    struct timespec rt;
    ssize_t res;
    unsigned int n = 0;

    if (sim_ensure_pipe(display, INPUT_TOUCH))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    int64_t nanoSeconds = rt.tv_sec * 1000 * 1000 * 1000 + rt.tv_nsec;
    // move_amount is already scaled by the scroll gear multiplier at
    // accumulation time.
    if (vertical_scroll) {
        display->axisY += move_amount;
    } else {
        display->axisX += move_amount;
    }

    if (display->lastAxisEventNanoSeconds == 0) {
        display->axisY = display->ptrPrvY;
        display->axisX = display->ptrPrvX;
        SIM_ADD_EVENT(EV_ABS, ABS_MT_SLOT, SIM_AXIS_TOUCH_SLOT_ID);
        SIM_ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, SIM_AXIS_TOUCH_TRACKING_ID);
        if (vertical_scroll) {
            SIM_ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, display->ptrPrvX);
            SIM_ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, display->axisY);
        } else {
            SIM_ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, display->axisX);
            SIM_ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, display->ptrPrvY);
        }
        SIM_ADD_EVENT(EV_ABS, ABS_MT_PRESSURE, 50);
        SIM_ADD_EVENT(EV_SYN, SYN_REPORT, 0);
        res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
        if (res < (ssize_t)sizeof(event)) {
            ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
            return;
        }

        n = 0;
        display->axisY += move_amount;
    }

    display->lastAxisEventNanoSeconds = nanoSeconds;
    SIM_ADD_EVENT(EV_ABS, ABS_MT_SLOT, SIM_AXIS_TOUCH_SLOT_ID);
    SIM_ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, SIM_AXIS_TOUCH_TRACKING_ID);
    if (vertical_scroll) {
        SIM_ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, display->ptrPrvX);
        SIM_ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, display->axisY);
    } else {
        SIM_ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, display->axisX);
        SIM_ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, display->ptrPrvY);
    }
    SIM_ADD_EVENT(EV_ABS, ABS_MT_PRESSURE, 50);
    SIM_ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
    if (res < (ssize_t)sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

// ---------------------------------------------------------------------------
// Multi-touch dispatch (also used by touch listeners)
// ---------------------------------------------------------------------------

extern "C" void sim_send_multi_touch_frame(struct display *display) {
    if (sim_ensure_pipe(display, INPUT_TOUCH)) return;

    struct input_event event[128];
    struct timespec rt;
    unsigned int n = 0;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d clock_gettime error", __FILE__, __LINE__);
        return;
    }

    int active = 0;
    for (int i = 0; i < SIM_MAX_TOUCHPOINTS; i++) {
        if (display->touch_id[i] != -1) active++;
    }
    ALOGD("send_multi_touch_frame: active=%d", active);

    if (display->need_send_touch_btn_down) {
        SIM_ADD_EVENT(EV_KEY, BTN_TOUCH, 1);
        display->need_send_touch_btn_down = false;
    }

    for (int i = 0; i < SIM_MAX_TOUCHPOINTS; i++) {
        if (display->touch_changed[i] || display->touch_id[i] != -1) {
            if (display->touch_id[i] == -1 && display->touch_tracking_id[i] == -1) continue;
            SIM_ADD_EVENT(EV_ABS, ABS_MT_SLOT, i);
            if (display->touch_id[i] == -1) {
                SIM_ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, -1);
                display->touch_tracking_id[i] = -1;
            } else {
                SIM_ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, display->touch_tracking_id[i]);
                SIM_ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, display->touch_x[i]);
                SIM_ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, display->touch_y[i]);
                SIM_ADD_EVENT(EV_ABS, ABS_MT_PRESSURE, 80);
            }
        }
    }

    if (display->need_send_touch_btn_up) {
        SIM_ADD_EVENT(EV_KEY, BTN_TOUCH, 0);
        display->need_send_touch_btn_up = false;
    }

    SIM_ADD_EVENT(EV_SYN, SYN_REPORT, 0);
    size_t bytes = n * sizeof(struct input_event);
    ALOGD("send_multi_touch_frame write INPUT_TOUCH");
    write(display->input_fd[INPUT_TOUCH], event, bytes);
}

extern "C" int sim_get_touch_id(struct display *display, uint32_t id) {
    int i = 0;
    int active = 0;
    for (i = 0; i < SIM_MAX_TOUCHPOINTS; i++) {
        if (display->touch_id[i] != -1) active++;
    }
    if (active > SIM_MAX_TOUCHPOINTS) {
        ALOGW("Touch point limit reached (%d), ignore new touch id=%d",
              SIM_MAX_TOUCHPOINTS, id);
        return -1;
    }
    for (i = 0; i < SIM_MAX_TOUCHPOINTS; i++) {
        if (display->touch_id[i] == id)
            return i;
    }
    for (i = 0; i < SIM_MAX_TOUCHPOINTS; i++) {
        if (display->touch_id[i] == -1) {
            display->touch_id[i] = id;
            display->next_tracking_id = (display->next_tracking_id + 1) % 0x7FFFFFFF;
            if (display->next_tracking_id == 0)
                display->next_tracking_id = 1;
            display->touch_tracking_id[i] = display->next_tracking_id;
            display->active_touch_count++;

            // first finger send BTN_TOUCH DOWN
            if (display->active_touch_count == 1) {
                display->need_send_touch_btn_down = true;
            }
            return i;
        }
    }
    return -1;
}

extern "C" int sim_release_touch_id(struct display *display, uint32_t id) {
    for (int i = 0; i < SIM_MAX_TOUCHPOINTS; i++) {
        if (display->touch_id[i] == id) {
            display->touch_id[i] = -1;
            display->touch_changed[i] = true;
            display->active_touch_count =
                std::max(0, display->active_touch_count - 1);
            // last finger send BTN_TOUCH UP
            if (display->active_touch_count == 0) {
                ALOGD("sim_release_touch_id id: %d, active_touch_count: %d",
                      id, display->active_touch_count);
                display->need_send_touch_btn_up = true;
            }
            sim_send_multi_touch_frame(display);
            return i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Scroll gear
// ---------------------------------------------------------------------------

extern "C" void sim_update_scroll_gear(void) {
    int64_t now = sim_now_ns();

    sim_wheel_event_timestamps[sim_wheel_event_next] = now;
    sim_wheel_event_next = (sim_wheel_event_next + 1) % WHEEL_EVENT_TIMESTAMP_MAX;
    if (sim_wheel_event_count < WHEEL_EVENT_TIMESTAMP_MAX)
        sim_wheel_event_count++;
    sim_scroll_session_event_count++;

    int recent_events = 0;
    for (int i = 0; i < sim_wheel_event_count; i++) {
        if (now - sim_wheel_event_timestamps[i] <= SCROLL_GEAR_WINDOW_NS)
            recent_events++;
    }
    if (recent_events >= SCROLL_GEAR_FAST_COUNT)
        sim_scroll_gear = SIM_SCROLL_GEAR_FAST;
    else if (recent_events >= SCROLL_GEAR_MEDIUM_COUNT)
        sim_scroll_gear = SIM_SCROLL_GEAR_MEDIUM;
    else
        sim_scroll_gear = SIM_SCROLL_GEAR_SLOW;
}

extern "C" int sim_scroll_gear_multiplier(void) {
    switch (sim_scroll_gear) {
        case SIM_SCROLL_GEAR_SLOW: return SCROLL_GEAR_SLOW_MULTIPLIER;
        case SIM_SCROLL_GEAR_FAST: return SCROLL_GEAR_FAST_MULTIPLIER;
        case SIM_SCROLL_GEAR_MEDIUM:
        default:                   return SCROLL_GEAR_MEDIUM_MULTIPLIER;
    }
}

// ---------------------------------------------------------------------------
// Drip-timer SIGALRM handler
// ---------------------------------------------------------------------------

extern "C" void sim_timer_handler(int sig) {
    if (sig != SIGALRM)
        return;
    if (!sim_mDisplay) {
        sim_stop_scroll_timer();
        return;
    }

    if (sim_mDisplay->axis_simulation_two_finger_started) {
        ALOGD("pointer axis stopped.");
        sim_pointer_cancel_axis_to_two_finger_touch(sim_mDisplay);
        sim_pending_wheel_move = 0;
        sim_scroll_session_event_count = 0;
        sim_scroll_gear = SIM_SCROLL_GEAR_MEDIUM;
        sim_stop_scroll_timer();
        return;
    }

    // Drip out the pending wheel distance a bit per tick so the simulated
    // touch move glides instead of jumping.
    if (sim_pending_wheel_move != 0) {
        int step = sim_pending_wheel_move / sim_drip_divisor();
        if (step == 0)
            step = sim_pending_wheel_move > 0 ? 1 : -1;
        sim_pending_wheel_move -= step;
        sim_pointer_axis_to_touch(sim_mDisplay, step, sim_pending_wheel_vertical);
        return;
    }

    // Nothing left to drip: lift the touch once the gear-specific idle
    // timeout has elapsed. The slow gear waits longer, extending the glide.
    if (sim_now_ns() - sim_last_wheel_event_ns <
        (int64_t)sim_end_timeout_ms() * 1000000LL)
        return;

    ALOGD("pointer axis stopped.");
    sim_pointer_cancel_axis_to_touch(sim_mDisplay, true, true);
    sim_scroll_session_event_count = 0;
    sim_scroll_gear = SIM_SCROLL_GEAR_MEDIUM;
    sim_stop_scroll_timer();
}

// Public helpers consumed by wayland-hwc.cpp / x11-hwc.cpp for the pinch
// gesture (ctrl + wheel). These accessors let the backends read the
// scroll-gear-driven scaling parameters without exposing the static state.
void sim_get_gesture_scaling(double *start_distance, int *stride) {
    if (start_distance) *start_distance = sim_gesture_scaling_start_distance;
    if (stride)         *stride = sim_gesture_scaling_stride;
}

void sim_set_gesture_scaling(double start_distance, int stride) {
    sim_gesture_scaling_start_distance = start_distance;
    sim_gesture_scaling_stride = stride;
}

void sim_set_axis_move(bool vertical, int touch_move) {
    sim_mVerticalScroll = vertical;
    sim_mTouchMove = touch_move;
}

void sim_record_wheel_event(bool vertical, int touch_move) {
    sim_update_scroll_gear();
    sim_mTouchMove = touch_move;
    sim_mVerticalScroll = vertical;
    sim_pending_wheel_move += touch_move * sim_scroll_gear_multiplier();
    sim_pending_wheel_vertical = vertical;
    // Apply the first step right away to keep the scroll responsive.
    sim_timer_handler(SIGALRM);
    sim_reset_timer();
}