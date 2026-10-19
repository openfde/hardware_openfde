/*
 * Copyright (C) 2026 Openfde Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

// Shared HWC composer-device layer.
//
// hwc_query / hwc_event_control / hwc_vsync_thread / hwc_attribute /
// hwc_get_display_configs / hwc_get_active_config / hwc_set_active_config /
// hwc_get_display_attributes / hwc_close and the
// time_to_sleep_to_next_vsync helper used to be duplicated almost verbatim
// in wayland/hwcomposer.cpp and x11/hwcomposer.cpp. The only difference was
// the private device struct name (openfde_hwc_composer_device_1 in the
// wayland backend vs waydroid_hwc_composer_device_1 in the x11 backend).
//
// That struct is therefore declared exactly once here, holding the union of
// both backends' members, so the shared entry points can be compiled for
// either backend. As with scroll_touch_sim.cpp the translation unit is
// built twice, once per backend, selected by the USE_WAYLAND_BACKEND /
// USE_X11_BACKEND macros set in Android.bp.
//
// The handful of places where the two backends genuinely differ are guarded
// by those same macros inside hwc_composer.cpp; the one behavioural hook the
// backends must supply themselves is hwc_resize_windows().

#pragma once

#if defined(USE_WAYLAND_BACKEND)
#include "../wayland/wayland-hwc.h"
#elif defined(USE_X11_BACKEND)
#include "../x11/x11-hwc.h"
#else
#error "hwc_composer.cpp must be compiled with -DUSE_WAYLAND_BACKEND or -DUSE_X11_BACKEND"
#endif

#include <hardware/hwcomposer.h>
#include <pthread.h>
#include <stdint.h>

// Private data hanging off hwc_composer_device_1_t. Members that only one
// backend uses are still declared for both so that neither backend has to
// keep its own copy of the struct.
struct openfde_hwc_composer_device_1 {
    hwc_composer_device_1_t base; // constant after init
    const hwc_procs_t *procs;     // constant after init
    pthread_t wayland_thread;     // constant after init (wayland backend only)
    pthread_t vsync_thread;       // constant after init
    pthread_t extension_thread;   // constant after init
    pthread_t window_service_thread; // constant after init
    pthread_t egl_worker_thread;  // constant after init
    int32_t vsync_period_ns;      // constant after init
    struct display *display;      // constant after init
    std::map<std::string, struct window *> windows;
    struct window *calib_window;

    pthread_mutex_t vsync_lock;
    bool vsync_callback_enabled; // protected by this->vsync_lock
    uint64_t last_vsync_ns;

    int timeline_fd;
    int next_sync_point;
    bool use_subsurface;
    bool multi_windows;
    int32_t aosp_version;         // x11 backend only
};

#ifdef __cplusplus
extern "C" {
#endif

// ---- Backend hook -------------------------------------------------------
// Recreate the backend's windows at the new resolution. Invoked by
// hwc_set_active_config() while holding display->windowsMutex.
void hwc_resize_windows(struct openfde_hwc_composer_device_1* pdev,
                        int target_width, int target_height);

// ---- Shared HWC entry points --------------------------------------------
// Wired into pdev->base by each backend's hwc_open(); also used directly by
// the backends (e.g. hwc_attribute from their own code paths).

long time_to_sleep_to_next_vsync(struct timespec *rt, uint64_t last_vsync_ns,
                                 unsigned vsync_period_ns);

void* hwc_vsync_thread(void* data);
int   hwc_query(struct hwc_composer_device_1* dev, int what, int* value);
int   hwc_event_control(struct hwc_composer_device_1* dev, int dpy,
                        int event, int enabled);
void  hwc_register_procs(struct hwc_composer_device_1* dev,
                         hwc_procs_t const* procs);
int   hwc_blank(struct hwc_composer_device_1* dev, int disp, int blank);

int   hwc_get_display_configs(struct hwc_composer_device_1* dev, int disp,
                              uint32_t* configs, size_t* numConfigs);
int32_t hwc_attribute(struct openfde_hwc_composer_device_1* pdev,
                      const uint32_t attribute, uint32_t config);
int   hwc_get_display_attributes(struct hwc_composer_device_1* dev, int disp,
                                 uint32_t config, const uint32_t* attributes,
                                 int32_t* values);
int   hwc_get_active_config(struct hwc_composer_device_1* dev, int disp);
int   hwc_set_active_config(struct hwc_composer_device_1* dev, int disp, int config);
int   hwc_close(hw_device_t* dev);

#ifdef __cplusplus
}
#endif
