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
//
// Both the wayland and the x11 backends carry out exactly the same
// "convert desktop pointer input into Android input events" logic. That
// logic was duplicated almost verbatim across wayland-hwc.cpp and
// x11-hwc.cpp; it now lives in scroll_touch_sim.cpp and is linked into
// both backends.
//
// The functions exposed here operate on the backends' existing
// `struct display` (declared in either wayland-hwc.h or x11-hwc.h).
// Each backend selects which header is included in scroll_touch_sim.cpp
// via the compile-time macro USE_WAYLAND_BACKEND / USE_X11_BACKEND set
// in Android.bp.
//
// All entry points are C-linkage so they do not pollute the C++ namespace
// and so legacy C callers (wl_pointer listeners etc.) can invoke them
// without ceremony.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Slot reserved by the simulated scroll finger. Tracking id mirrors slot
// id so multiple simulated scrolls (every finger where the touch slop is
// moved away from) feel like one logical finger.
#define SIM_AXIS_TOUCH_SLOT_ID      8
#define SIM_AXIS_TOUCH_TRACKING_ID  8

// Maximum number of concurrent simulated touch points. Mirrors
// MAX_TOUCHPOINTS in the backends' display struct.
#define SIM_MAX_TOUCHPOINTS         10

enum SimKeyState {
    SIM_KEY_RELEASED = 0,
    SIM_KEY_PRESSED  = 1,
};

enum SimScrollGear {
    SIM_SCROLL_GEAR_SLOW   = 0,
    SIM_SCROLL_GEAR_MEDIUM = 1,
    SIM_SCROLL_GEAR_FAST   = 2,
};

// ---- Lifecycle ----------------------------------------------------------

// Register the SIGALRM handler that drives the periodic scroll drip timer.
// Call once per process at startup (replaces init_timer()).
void sim_init_timer(void);

// Inform the simulator about the active display. The periodic timer
// callback needs this pointer; pass nullptr in destroy_display.
void sim_set_display(struct display *d);
void sim_clear_display(void);

// ---- Timer helpers ------------------------------------------------------

// Stop the drip timer if active. Safe to call repeatedly.
void sim_stop_scroll_timer(void);

// Arm the drip timer; resets the "no more wheel events" baseline.
void sim_reset_timer(void);

// Periodic SIGALRM handler. Public so wayland/x11's drip-tick logic can
// also invoke it directly on the first wheel event for responsiveness.
void sim_timer_handler(int sig);

// ---- Wheel event dispatch ----------------------------------------------
//
// Backends call these from their pointer-axis listeners. They translate a
// raw wheel/mouse event into (a) accumulated pending distance, (b) the
// pinch gesture parameters, and (c) a touch sequence.

// One-shot touchMove / direction setter (replaces the legacy globals
// mTouchMove / mVerticalScroll). The pointer-axis cancel logic still needs
// to know the most recent touchMove and whether the axis was vertical.
void sim_set_axis_move(bool vertical, int touch_move);

// Record one wheel event. Updates the gear detector, accumulates the
// scaled pending distance, then triggers one drip-timer tick and re-arms
// the periodic drip timer. The backend's pointer-axis listener should call
// this once per wheel event when the "click_as_touch" mode is on and the
// ctrl key is NOT pressed.
void sim_record_wheel_event(bool vertical, int touch_move);

// Save the gesture-scaling parameters the backend computed for ctrl+wheel
// pinch. Call from pointer-axis when the gear flips between UP and DOWN.
void sim_set_gesture_scaling(double start_distance, int stride);

void sim_get_gesture_scaling(double *start_distance, int *stride);

// ---- Input event emission ----------------------------------------------

// Send an Android key down/up event.
void sim_send_key_event(struct display *display, uint32_t key, uint32_t state);

// Simulate a left-button-down press as a single-finger touch down.
void sim_pointer_button_to_touch_down(struct display *display);

// Simulate a left-button-up as a single-finger touch up.
void sim_pointer_button_to_touch_up(struct display *display);

// Drop the two-finger pinch gesture if it is active.
void sim_pointer_cancel_axis_to_two_finger_touch(struct display *display);

// Cancel an in-progress wheel-to-touch translation. Returns true if
// there was nothing to cancel.
bool sim_pointer_cancel_axis_to_touch(struct display *display,
                                      bool from_axis_stop_event,
                                      bool force);

// Apply one step of the pending wheel distance as a touch move.
void sim_pointer_axis_to_touch(struct display *display,
                               int move,
                               bool vertical_scroll);

// Send a multi-touch frame summarising the current touch_id[] /
// touch_x[] / touch_y[] state to /dev/input/.
void sim_send_multi_touch_frame(struct display *display);

// Allocate / look up a touch slot for the given pointer id.
int sim_get_touch_id(struct display *display, uint32_t id);

// Release a touch slot allocated by sim_get_touch_id. After release the
// multi-touch frame is flushed so Android immediately sees the slot going
// inactive. Returns the slot index, or -1 if no such id was found.
int sim_release_touch_id(struct display *display, uint32_t id);

// ---- Scroll gear --------------------------------------------------------

// Update the scroll-gear detector using the current monotonic clock.
void sim_update_scroll_gear(void);

// Current scroll-gear multiplier (1=slow, 2=medium, 3=fast).
int sim_scroll_gear_multiplier(void);

// ---- Helpers ------------------------------------------------------------

// Lazy-open the input pipe for a given INPUT_* type. Returns 0 on success,
// -1 on failure. The backend reuses this to write touch / pointer / keyboard
// events in the non-scroll paths (mouse motion, button press, native wl_touch
// dispatch, etc.).
int sim_ensure_pipe(struct display *display, int input_type);

// Append one input_event to the local `event[]` array / `n` counter that
// backends use to build a multi-event write into the input pipe. The macro
// reads the locally-declared `rt` (struct timespec) and `n` (unsigned int).
// It does NOT call write() -- the caller does that once the buffer is full.
// This is the public replacement for the legacy ADD_EVENT macro so the same
// helper is available to scroll_touch_sim.cpp and the backend sources.
#define SIM_ADD_EVENT(type_, code_, value_)            \
    event[n].time.tv_sec = rt.tv_sec;                  \
    event[n].time.tv_usec = rt.tv_nsec / 1000;         \
    event[n].type = type_;                             \
    event[n].code = code_;                             \
    event[n].value = value_;                           \
    n++;

// Convert the 8dp touch-slop distance to pixels for the current density.
int sim_min_scroll_distance(struct display *display);

#ifdef __cplusplus
}
#endif