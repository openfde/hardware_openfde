/*
 * Copyright (C) 2026 Openfde Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

// Shared HWC2 composer-device layer.
//
// This file previously implemented the HWC 1.x entry points (prepare/set
// operating on an array of displays). It has been ported to the HWC 2.x
// model:
//
//   * The device embeds hwc2_device_t and is wired through getFunction().
//   * Every operation is expressed "per display": the HWC 1.x helpers that
//     used to take a display *index* (HWC_DISPLAY_PRIMARY /
//     HWC_DISPLAY_EXTERNAL) and loop over an array of displays now operate on
//     a single hwc2_display_t. SurfaceFlinger calls prepare/validate and
//     present once per display instead of once for all displays.
//   * Per-display state (configs, layers, client target, vsync enable,
//     power mode, release/retire fences, cursor state) lives in
//     hwc2_display_data. The two physical displays (primary + external
//     secondary) are still represented, but each is addressed by its own
//     hwc2_display_t handle.
//
// The shared entry points are compiled twice (once per backend) selected by
// the USE_WAYLAND_BACKEND / USE_X11_BACKEND macros set in Android.bp, exactly
// as before. The two backends still differ in a handful of places, now reduced
// to the two behavioural hooks they must supply: hwc2_present() (the
// render/present of one display's frame) and hwc_resize_windows().

#pragma once

#include "screen_config.h"

#if defined(USE_WAYLAND_BACKEND)
#include "../wayland/wayland-hwc.h"
#elif defined(USE_X11_BACKEND)
#include "../x11/x11-hwc.h"
#else
#error "hwc_composer.cpp must be compiled with -DUSE_WAYLAND_BACKEND or -DUSE_X11_BACKEND"
#endif

#include <map>
#include <string>
#include <vector>

#include <hardware/hwcomposer.h>
#include <hardware/hwcomposer2.h>
#include <pthread.h>
#include <stdint.h>

// HWC2 device version. This is the C HAL device version exposed in
// hwc2_device_t.common.version. For HWC2 it is ALWAYS
// HWC_DEVICE_API_VERSION_2_0 -- there is no 2.1/2.2 device constant. The
// "2.1" people usually mean is the HIDL IComposer interface version
// (android.hardware.graphics.composer@2.1), which is a separate layer handled
// by the composer passthrough impl/service, not by this field. Defined here so
// the backends don't depend on a specific AOSP header snapshot providing it.
// Matches the (major << 16) layout used by HWC_DEVICE_API_VERSION() in
// hardware/hwcomposer.h.
#ifndef HWC_DEVICE_API_VERSION_2_0
#define HWC_DEVICE_API_VERSION_2_0 ((2u << 16) | 0)
#endif

// Per-layer state kept between validate and present for one display. We reuse
// the HWC 1.x hwc_layer_1_t as the internal layer representation so the
// backend render path (which was written against hwc_display_contents_1_t)
// can be reused unchanged.
struct hwc2_layer_state {
    hwc2_layer_t id;
    hwc_layer_1_t layer; // internal representation consumed by the backend
    int32_t z;           // z-order supplied via setLayerZOrder
};

// All HWC2 state that is scoped to a single display.
struct hwc2_display_data {
    hwc2_display_t id;
    int32_t type;          // HWC2_DISPLAY_TYPE_PHYSICAL / VIRTUAL
    int32_t display_class; // HWC_DISPLAY_PRIMARY / HWC_DISPLAY_EXTERNAL

    struct display *disp;  // backend display (== pdev->display / secondary)
    std::map<std::string, struct window *> *windows; // == &pdev->windows / secondary

    uint32_t active_config;
    int32_t power_mode;    // HWC2_POWER_MODE_*
    bool vsync_enabled;   // per-display vsync toggle (setVsyncEnabled)

    // Layer configuration set by the client across frames.
    std::map<hwc2_layer_t, hwc2_layer_state> layers;
    std::vector<hwc2_layer_t> layer_order; // ascending z-order
    hwc2_layer_t next_layer_id;

    // Client target (the framebuffer target in HWC 1.x terms).
    bool has_client_target;
    hwc_layer_1_t client_target_layer;
    buffer_handle_t client_target_handle;
    int32_t client_target_acquire_fence;

    // Fences produced for the last presented frame.
    int32_t retire_fence;
    std::map<hwc2_layer_t, int32_t> release_fences;

    // validateDisplay outputs.
    std::vector<hwc2_layer_t> changed_types;
    bool validated;

    // Cursor bookkeeping (migrated from the old file-scope globals).
    bool found_cursor;
};

// Private data hanging off hwc2_device_t. Members that only one backend uses
// are still declared for both so that neither backend has to keep its own
// copy of the struct.
struct openfde_hwc_composer_device_1 {
    hwc2_device_t base; // constant after init (was hwc_composer_device_1_t)

    // HWC2 callback registration (replaces the HWC1 hwc_procs_t).
    hwc2_callback_data_t callback_data;
    HWC2_PFN_HOTPLUG hotplug;
    HWC2_PFN_VSYNC vsync;
    HWC2_PFN_REFRESH refresh;

    pthread_t wayland_thread;     // constant after init (wayland backend only)
    pthread_t vsync_thread;       // constant after init
    pthread_t extension_thread;   // constant after init
    pthread_t window_service_thread; // constant after init
    pthread_t egl_worker_thread;  // constant after init
    pthread_t egl_secondary_worker_thread;  // constant after init
    pthread_t event_thread_primary;
    pthread_t event_thread_secondary;

    int32_t vsync_period_ns;      // constant after init
    struct display *display;      // constant after init (primary)
    struct display *secondary_display;      // constant after init
    bool secondary_hotpluged;
    std::map<std::string, struct window *> windows;
    std::map<std::string, struct window *> secondary_windows;
    struct window *calib_window;

    // Per-display HWC2 state, keyed by the hwc2_display_t handle we assign.
    std::map<hwc2_display_t, hwc2_display_data> displays;
    hwc2_display_t primary_id;
    hwc2_display_t external_id;

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

// ---- Backend hooks -------------------------------------------------------
// Recreate the backend's windows at the new resolution. Invoked by
// hwc_set_active_config() while holding display->windowsMutex. Resizes the
// primary display's windows (the only display that supports config switching,
// matching the HWC 1.x behaviour).
void hwc_resize_windows(struct openfde_hwc_composer_device_1* pdev,
                        int target_width, int target_height);

// Render/present one display's frame. `contents` is the synthetic
// hwc_display_contents_1_t built by the common layer from the display's HWC2
// layer state. Implemented by each backend (was the old hwc_set() body).
int hwc2_present(struct openfde_hwc_composer_device_1 *pdev,
                 struct hwc2_display_data *dd,
                 hwc_display_contents_1_t *contents);

// ---- Shared HWC2 entry points --------------------------------------------
// Wired into pdev->base via hwc2_get_function(); also used directly by the
// backends.

long time_to_sleep_to_next_vsync(struct timespec *rt, uint64_t last_vsync_ns,
                                 unsigned vsync_period_ns);

void* hwc_vsync_thread(void* data);

// getFunction dispatcher: returns the implementation of the requested HWC2
// per-display/per-layer function, or NULL if unsupported. (The device-level
// entry points below are direct hwc2_device_t members, not dispatched here.)
hwc2_function_pointer_t hwc2_get_function(struct hwc2_device *device,
                                          int32_t descriptor);

// Callback registration (hotplug / vsync / refresh).
int32_t hwc2_register_callback(struct hwc2_device *device, int32_t descriptor,
                               hwc2_callback_data_t callback_data,
                               hwc2_function_pointer_t pointer);

// Device-level entry points (direct hwc2_device_t members).
void hwc2_get_capabilities(struct hwc2_device *device, uint32_t *out_count,
                           int32_t *out_capabilities);
void hwc2_dump(struct hwc2_device *device, uint32_t *out_size, char *out_buffer);
uint32_t hwc2_get_max_virtual_display_count(struct hwc2_device *device);
int32_t hwc2_create_virtual_display(struct hwc2_device *device, uint32_t width,
                                    uint32_t height, int32_t *format,
                                    hwc2_display_t *out_display);
void hwc2_destroy_virtual_display(struct hwc2_device *device,
                                  hwc2_display_t display);

int32_t hwc2_get_display_configs(struct hwc2_device *device,
                                 hwc2_display_t display, uint32_t *out_num_configs,
                                 hwc2_config_t *out_configs);
int32_t hwc2_get_display_attribute(struct hwc2_device *device,
                                   hwc2_display_t display, hwc2_config_t config,
                                   int32_t attribute, int32_t *out_value);
int32_t hwc2_get_active_config(struct hwc2_device *device, hwc2_display_t display,
                               hwc2_config_t *out_config);
int32_t hwc2_set_active_config(struct hwc2_device *device, hwc2_display_t display,
                               hwc2_config_t config);

int32_t hwc2_get_display_type(struct hwc2_device *device, hwc2_display_t display,
                              int32_t *out_type);
int32_t hwc2_set_power_mode(struct hwc2_device *device, hwc2_display_t display,
                            int32_t mode);
int32_t hwc2_set_vsync_enabled(struct hwc2_device *device, hwc2_display_t display,
                               int32_t enabled);

int32_t hwc2_get_color_modes(struct hwc2_device *device, hwc2_display_t display,
                             uint32_t *out_num_modes, int32_t *out_modes);
int32_t hwc2_set_color_mode(struct hwc2_device *device, hwc2_display_t display,
                            int32_t mode);
int32_t hwc2_set_color_transform(struct hwc2_device *device, hwc2_display_t display,
                                 const float *matrix, int32_t hint);

int32_t hwc2_create_layer(struct hwc2_device *device, hwc2_display_t display,
                          hwc2_layer_t *out_layer);
int32_t hwc2_destroy_layer(struct hwc2_device *device, hwc2_display_t display,
                           hwc2_layer_t layer);

int32_t hwc2_get_display_requests(struct hwc2_device *device,
                                  hwc2_display_t display, int32_t *out_display_requests,
                                  uint32_t *out_num_layers,
                                  hwc2_layer_t *out_layers, int32_t *out_layer_requests);
int32_t hwc2_accept_display_changes(struct hwc2_device *device,
                                    hwc2_display_t display);

int32_t hwc2_validate_display(struct hwc2_device *device, hwc2_display_t display,
                              uint32_t *out_num_types, uint32_t *out_num_requests);
int32_t hwc2_get_changed_composition_types(struct hwc2_device *device,
                                           hwc2_display_t display,
                                           uint32_t *out_num_types,
                                           hwc2_layer_t *out_layers,
                                           int32_t *out_types);
int32_t hwc2_get_release_fences(struct hwc2_device *device, hwc2_display_t display,
                                uint32_t *out_num_elements, hwc2_layer_t *out_layers,
                                int32_t *out_fences);
int32_t hwc2_present_display(struct hwc2_device *device, hwc2_display_t display,
                             int32_t *out_retire_fence);

int32_t hwc2_set_client_target(struct hwc2_device *device, hwc2_display_t display,
                               buffer_handle_t target, int32_t acquire_fence,
                               int32_t dataspace, hwc_region_t damage);

int32_t hwc2_get_display_name(struct hwc2_device *device, hwc2_display_t display,
                              uint32_t *out_size, char *out_name);
int32_t hwc2_get_display_capabilities(struct hwc2_device *device,
                                      hwc2_display_t display,
                                      uint32_t *out_num_capabilities,
                                      uint32_t *out_capabilities);
int32_t hwc2_get_doze_support(struct hwc2_device *device, hwc2_display_t display,
                              int32_t *out_support);
int32_t hwc2_get_hdr_capabilities(struct hwc2_device *device, hwc2_display_t display,
                                  uint32_t *out_num_types, int32_t *out_types,
                                  float *out_max_luminance,
                                  float *out_max_average_luminance,
                                  float *out_min_luminance);
int32_t hwc2_get_client_target_support(struct hwc2_device *device,
                                       hwc2_display_t display, uint32_t width,
                                       uint32_t height, int32_t format,
                                       int32_t dataspace);
int32_t hwc2_set_output_buffer(struct hwc2_device *device, hwc2_display_t display,
                               buffer_handle_t buffer, int32_t release_fence);

// ---- setLayer* / getLayer* helpers (per-layer configuration) -------------
int32_t hwc2_set_layer_composition_type(struct hwc2_device *device,
                                        hwc2_display_t display, hwc2_layer_t layer,
                                        int32_t type);
int32_t hwc2_set_layer_buffer(struct hwc2_device *device, hwc2_display_t display,
                              hwc2_layer_t layer, buffer_handle_t buffer,
                              int32_t acquire_fence);
int32_t hwc2_set_layer_dataspace(struct hwc2_device *device, hwc2_display_t display,
                                 hwc2_layer_t layer, int32_t dataspace);
int32_t hwc2_set_layer_display_frame(struct hwc2_device *device,
                                     hwc2_display_t display, hwc2_layer_t layer,
                                     hwc_rect_t frame);
int32_t hwc2_set_layer_source_crop(struct hwc2_device *device,
                                   hwc2_display_t display, hwc2_layer_t layer,
                                   hwc_frect_t crop);
int32_t hwc2_set_layer_z_order(struct hwc2_device *device, hwc2_display_t display,
                               hwc2_layer_t layer, uint32_t z);
int32_t hwc2_set_layer_surface_damage(struct hwc2_device *device,
                                      hwc2_display_t display, hwc2_layer_t layer,
                                      hwc_region_t damage);
int32_t hwc2_set_layer_blend_mode(struct hwc2_device *device,
                                  hwc2_display_t display, hwc2_layer_t layer,
                                  int32_t mode);
int32_t hwc2_set_layer_plane_alpha(struct hwc2_device *device,
                                   hwc2_display_t display, hwc2_layer_t layer,
                                   float alpha);
int32_t hwc2_set_layer_transform(struct hwc2_device *device,
                                 hwc2_display_t display, hwc2_layer_t layer,
                                 int32_t transform);
int32_t hwc2_set_layer_visible_region(struct hwc2_device *device,
                                      hwc2_display_t display, hwc2_layer_t layer,
                                      hwc_region_t region);
int32_t hwc2_set_layer_color(struct hwc2_device *device, hwc2_display_t display,
                             hwc2_layer_t layer, hwc_color_t color);
int32_t hwc2_set_layer_color_transform(struct hwc2_device *device,
                                       hwc2_display_t display, hwc2_layer_t layer,
                                       const float *matrix);
int32_t hwc2_set_layer_per_frame_metadata(struct hwc2_device *device,
                                          hwc2_display_t display, hwc2_layer_t layer,
                                          uint32_t num_elements,
                                          const int32_t *keys, const float *values);
int32_t hwc2_set_layer_cursor_position(struct hwc2_device *device,
                                       hwc2_display_t display, hwc2_layer_t layer,
                                       int32_t x, int32_t y);
int32_t hwc2_set_layer_buffer_crop(struct hwc2_device *device,
                                   hwc2_display_t display, hwc2_layer_t layer,
                                   hwc_rect_t crop);

// teardown
int hwc_close(hw_device_t* dev);

#ifdef __cplusplus
}
#endif
