/*
 * Copyright (C) 2026 Openfde Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

// Shared HWC2 composer-device entry points. See hwc_composer.h for why this
// file exists and how the backend is selected, and for the HWC2 porting notes.

#include "hwc_composer.h"

#include <errno.h>
#include <log/log.h>
#include <cutils/properties.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <utils/ThreadDefs.h>

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include <cutils/trace.h>
#include <utils/Trace.h>

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static struct hwc2_display_data *get_display_data(hwc2_device_t *device,
                                                 hwc2_display_t display) {
    auto *pdev = (struct openfde_hwc_composer_device_1 *)device;
    auto it = pdev->displays.find(display);
    if (it == pdev->displays.end())
        return nullptr;
    return &it->second;
}

// Translate an HWC2 composition type into the internal HWC 1.x composition
// type the backend render path understands. We composite everything on the
// device (OVERLAY); CURSOR layers keep the cursor flag so the backend can
// spot them.
static int32_t hwc2_to_internal_comp(int32_t hwc2_type, bool *is_cursor) {
    *is_cursor = false;
    switch (hwc2_type) {
        case HWC2_COMPOSITION_CURSOR:
            *is_cursor = true;
            return HWC_OVERLAY;
        case HWC2_COMPOSITION_SIDEBAND:
            return HWC_SIDEBAND;
        case HWC2_COMPOSITION_SOLID_COLOR:
        case HWC2_COMPOSITION_CLIENT:
        case HWC2_COMPOSITION_DEVICE:
        default:
            return HWC_OVERLAY;
    }
}

// ---------------------------------------------------------------------------
// vsync timing
// ---------------------------------------------------------------------------

long time_to_sleep_to_next_vsync(struct timespec *rt, uint64_t last_vsync_ns,
                                 unsigned vsync_period_ns) {
    uint64_t now = (uint64_t)rt->tv_sec * 1e9 + rt->tv_nsec;
    uint64_t frames_since_last_vsync = (now - last_vsync_ns) / vsync_period_ns + 1;
    uint64_t next_vsync = last_vsync_ns + frames_since_last_vsync * vsync_period_ns;

    return next_vsync - now;
}

void* hwc_vsync_thread(void* data) {
    struct openfde_hwc_composer_device_1* pdev = (struct openfde_hwc_composer_device_1*)data;
    setpriority(PRIO_PROCESS, 0, HAL_PRIORITY_URGENT_DISPLAY);

    struct timespec rt;
    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in vsync thread clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }
    bool vsync_enabled = false;
    bool hotpluged = false;

    struct timespec wait_time;
    wait_time.tv_sec = 0;

    pthread_mutex_lock(&pdev->vsync_lock);
    wait_time.tv_nsec = time_to_sleep_to_next_vsync(&rt, pdev->last_vsync_ns, pdev->vsync_period_ns);
    pthread_mutex_unlock(&pdev->vsync_lock);

    while (true) {
        ATRACE_BEGIN("hwc_vsync_thread");
        int err = nanosleep(&wait_time, NULL);
        if (err == -1) {
            if (errno == EINTR) {
                break;
            }
            ATRACE_END();
            ALOGE("error in vsync thread: %s", strerror(errno));
            continue;
        }

        pthread_mutex_lock(&pdev->vsync_lock);
        vsync_enabled = pdev->vsync_callback_enabled;
        hotpluged = pdev->secondary_hotpluged;
        pthread_mutex_unlock(&pdev->vsync_lock);

        if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
            ALOGE("%s:%d error in vsync thread clock_gettime: %s",
                  __FILE__, __LINE__, strerror(errno));
        }

        pthread_mutex_lock(&pdev->vsync_lock);
        wait_time.tv_nsec = time_to_sleep_to_next_vsync(&rt, pdev->last_vsync_ns, pdev->vsync_period_ns);
        pthread_mutex_unlock(&pdev->vsync_lock);

        if (!vsync_enabled)
            continue;

        if (!pdev->hotplug) {
            ALOGW("hotplug callback not registered");
        } else {
            if (!hotpluged) {
                for (auto &kv : pdev->displays)
                    pdev->hotplug(pdev->callback_data, kv.second.id,
                                  HWC2_CONNECTION_CONNECTED);
                pdev->secondary_hotpluged = true;
            }
        }

        int64_t timestamp = (uint64_t)rt.tv_sec * 1e9 + rt.tv_nsec;

        // Fire a per-display vsync callback for every display that has vsync
        // enabled. HWC2 models vsync as a per-display event.
        if (pdev->vsync) {
            for (auto &kv : pdev->displays) {
                struct hwc2_display_data *dd = &kv.second;
                if (dd->vsync_enabled)
                    pdev->vsync(pdev->callback_data, dd->id, timestamp);
            }
        }
        ATRACE_END();
    }

    return NULL;
}

// ---------------------------------------------------------------------------
// HWC2 attribute / config helpers (per display)
// ---------------------------------------------------------------------------

// 判断被查询的 config 是否是该显示屏当前生效的配置。
static bool hwc_is_active_config(struct display *display, uint32_t config) {
    if (display->display_type != HWC_DISPLAY_PRIMARY) {
        return config == (uint32_t)defaultConfigIndex;
    }
    uint32_t active = (uint32_t)display->active_config;
    if (active >= (uint32_t)gScreenConfigCount) {
        active = (uint32_t)defaultConfigIndex;
    }
    return config == active;
}

static int32_t hwc2_attribute(struct openfde_hwc_composer_device_1* pdev,
                              struct display *display, const uint32_t attribute,
                              uint32_t config) {
    char property[PROPERTY_VALUE_MAX];
    int width = display->full_width;
    int height = display->full_height;
    int density = 160;
    if (config > 0 && config < (uint32_t)gScreenConfigCount) {
        width = screenConfigs[config].Width;
        height = screenConfigs[config].Height;
        density = screenConfigs[config].Density;
    }

    switch(attribute) {
        case HWC_DISPLAY_VSYNC_PERIOD:
            return pdev->vsync_period_ns;
        case HWC_DISPLAY_WIDTH: {
            if (property_get("persist.openfde.width_padding", property, nullptr) > 0)
                width -= atoi(property);
            std::string width_str = std::to_string(width);
            if (hwc_is_active_config(display, config)) {
                if(display->display_type == HWC_DISPLAY_PRIMARY){
                    property_set("openfde.display_width", width_str.c_str());
                }else{
                    property_set("openfde.external_display_width", width_str.c_str());
                }
            }
            return width;
        }
        case HWC_DISPLAY_HEIGHT: {
            if (property_get("persist.openfde.height_padding", property, nullptr) > 0)
                height -= atoi(property);
            std::string height_str = std::to_string(height);
            if (hwc_is_active_config(display, config)) {
                if(display->display_type == HWC_DISPLAY_PRIMARY){
                    property_set("openfde.display_height", height_str.c_str());
                }else{
                    property_set("openfde.external_display_height", height_str.c_str());
                }
            }
            return height;
        }
        case HWC_DISPLAY_DPI_X:
        case HWC_DISPLAY_DPI_Y:
            if (property_get("ro.sf.lcd_density", property, nullptr) > 0)
                density = atoi(property);
            return density * 1000;
        case HWC_DISPLAY_COLOR_TRANSFORM:
            return HAL_COLOR_TRANSFORM_IDENTITY;
        default:
            ALOGE("unknown display attribute %u", attribute);
            return -EINVAL;
    }
}

// ---------------------------------------------------------------------------
// getFunction dispatch
// ---------------------------------------------------------------------------

void hwc2_get_function(struct hwc2_device *device, int32_t descriptor,
                       hwc2_function_pointer_t *out_pointer) {
    if (!out_pointer)
        return;

    switch (descriptor) {
        case HWC2_FUNCTION_REGISTER_CALLBACK:
            *out_pointer = (hwc2_function_pointer_t)hwc2_register_callback;
            break;
        case HWC2_FUNCTION_GET_MAX_VIRTUAL_DISPLAY_COUNT:
            *out_pointer = (hwc2_function_pointer_t)hwc2_get_max_virtual_display_count;
            break;
        case HWC2_FUNCTION_CREATE_VIRTUAL_DISPLAY:
            *out_pointer = (hwc2_function_pointer_t)hwc2_create_virtual_display;
            break;
        case HWC2_FUNCTION_DESTROY_VIRTUAL_DISPLAY:
            *out_pointer = (hwc2_function_pointer_t)hwc2_destroy_virtual_display;
            break;
        case HWC2_FUNCTION_GET_DISPLAY_CONFIGS:
            *out_pointer = (hwc2_function_pointer_t)hwc2_get_display_configs;
            break;
        case HWC2_FUNCTION_GET_DISPLAY_ATTRIBUTE:
            *out_pointer = (hwc2_function_pointer_t)hwc2_get_display_attribute;
            break;
        case HWC2_FUNCTION_GET_ACTIVE_CONFIG:
            *out_pointer = (hwc2_function_pointer_t)hwc2_get_active_config;
            break;
        case HWC2_FUNCTION_SET_ACTIVE_CONFIG:
            *out_pointer = (hwc2_function_pointer_t)hwc2_set_active_config;
            break;
        case HWC2_FUNCTION_GET_DISPLAY_TYPE:
            *out_pointer = (hwc2_function_pointer_t)hwc2_get_display_type;
            break;
        case HWC2_FUNCTION_SET_POWER_MODE:
            *out_pointer = (hwc2_function_pointer_t)hwc2_set_power_mode;
            break;
        case HWC2_FUNCTION_SET_VSYNC_ENABLED:
            *out_pointer = (hwc2_function_pointer_t)hwc2_set_vsync_enabled;
            break;
        case HWC2_FUNCTION_GET_COLOR_MODES:
            *out_pointer = (hwc2_function_pointer_t)hwc2_get_color_modes;
            break;
        case HWC2_FUNCTION_SET_COLOR_MODE:
            *out_pointer = (hwc2_function_pointer_t)hwc2_set_color_mode;
            break;
        case HWC2_FUNCTION_SET_COLOR_TRANSFORM:
            *out_pointer = (hwc2_function_pointer_t)hwc2_set_color_transform;
            break;
        case HWC2_FUNCTION_CREATE_LAYER:
            *out_pointer = (hwc2_function_pointer_t)hwc2_create_layer;
            break;
        case HWC2_FUNCTION_DESTROY_LAYER:
            *out_pointer = (hwc2_function_pointer_t)hwc2_destroy_layer;
            break;
        case HWC2_FUNCTION_VALIDATE_DISPLAY:
            *out_pointer = (hwc2_function_pointer_t)hwc2_validate_display;
            break;
        case HWC2_FUNCTION_GET_CHANGED_COMPOSITION_TYPES:
            *out_pointer = (hwc2_function_pointer_t)hwc2_get_changed_composition_types;
            break;
        case HWC2_FUNCTION_GET_DISPLAY_REQUESTS:
            *out_pointer = (hwc2_function_pointer_t)hwc2_get_display_requests;
            break;
        case HWC2_FUNCTION_ACCEPT_DISPLAY_CHANGES:
            *out_pointer = (hwc2_function_pointer_t)hwc2_accept_display_changes;
            break;
        case HWC2_FUNCTION_PRESENT_DISPLAY:
            *out_pointer = (hwc2_function_pointer_t)hwc2_present_display;
            break;
        case HWC2_FUNCTION_GET_RELEASE_FENCES:
            *out_pointer = (hwc2_function_pointer_t)hwc2_get_release_fences;
            break;
        case HWC2_FUNCTION_SET_CLIENT_TARGET:
            *out_pointer = (hwc2_function_pointer_t)hwc2_set_client_target;
            break;
        case HWC2_FUNCTION_GET_DISPLAY_NAME:
            *out_pointer = (hwc2_function_pointer_t)hwc2_get_display_name;
            break;
        case HWC2_FUNCTION_GET_DISPLAY_CAPABILITIES:
            *out_pointer = (hwc2_function_pointer_t)hwc2_get_display_capabilities;
            break;
        case HWC2_FUNCTION_GET_DOZE_SUPPORT:
            *out_pointer = (hwc2_function_pointer_t)hwc2_get_doze_support;
            break;
        case HWC2_FUNCTION_GET_HDR_CAPABILITIES:
            *out_pointer = (hwc2_function_pointer_t)hwc2_get_hdr_capabilities;
            break;
        case HWC2_FUNCTION_GET_CLIENT_TARGET_SUPPORT:
            *out_pointer = (hwc2_function_pointer_t)hwc2_get_client_target_support;
            break;
        case HWC2_FUNCTION_SET_OUTPUT_BUFFER:
            *out_pointer = (hwc2_function_pointer_t)hwc2_set_output_buffer;
            break;
        case HWC2_FUNCTION_SET_LAYER_COMPOSITION_TYPE:
            *out_pointer = (hwc2_function_pointer_t)hwc2_set_layer_composition_type;
            break;
        case HWC2_FUNCTION_SET_LAYER_BUFFER:
            *out_pointer = (hwc2_function_pointer_t)hwc2_set_layer_buffer;
            break;
        case HWC2_FUNCTION_SET_LAYER_DATASPACE:
            *out_pointer = (hwc2_function_pointer_t)hwc2_set_layer_dataspace;
            break;
        case HWC2_FUNCTION_SET_LAYER_DISPLAY_FRAME:
            *out_pointer = (hwc2_function_pointer_t)hwc2_set_layer_display_frame;
            break;
        case HWC2_FUNCTION_SET_LAYER_SOURCE_CROP:
            *out_pointer = (hwc2_function_pointer_t)hwc2_set_layer_source_crop;
            break;
        case HWC2_FUNCTION_SET_LAYER_Z_ORDER:
            *out_pointer = (hwc2_function_pointer_t)hwc2_set_layer_z_order;
            break;
        case HWC2_FUNCTION_SET_LAYER_SURFACE_DAMAGE:
            *out_pointer = (hwc2_function_pointer_t)hwc2_set_layer_surface_damage;
            break;
        case HWC2_FUNCTION_SET_LAYER_BLEND_MODE:
            *out_pointer = (hwc2_function_pointer_t)hwc2_set_layer_blend_mode;
            break;
        case HWC2_FUNCTION_SET_LAYER_PLANE_ALPHA:
            *out_pointer = (hwc2_function_pointer_t)hwc2_set_layer_plane_alpha;
            break;
        case HWC2_FUNCTION_SET_LAYER_TRANSFORM:
            *out_pointer = (hwc2_function_pointer_t)hwc2_set_layer_transform;
            break;
        case HWC2_FUNCTION_SET_LAYER_VISIBLE_REGION:
            *out_pointer = (hwc2_function_pointer_t)hwc2_set_layer_visible_region;
            break;
        case HWC2_FUNCTION_SET_LAYER_COLOR:
            *out_pointer = (hwc2_function_pointer_t)hwc2_set_layer_color;
            break;
        case HWC2_FUNCTION_SET_LAYER_COLOR_TRANSFORM:
            *out_pointer = (hwc2_function_pointer_t)hwc2_set_layer_color_transform;
            break;
        case HWC2_FUNCTION_SET_LAYER_PER_FRAME_METADATA:
            *out_pointer = (hwc2_function_pointer_t)hwc2_set_layer_per_frame_metadata;
            break;
        case HWC2_FUNCTION_SET_CURSOR_POSITION:
            *out_pointer = (hwc2_function_pointer_t)hwc2_set_layer_cursor_position;
            break;
        case HWC2_FUNCTION_DUMP:
            // Optional; not implemented.
            *out_pointer = nullptr;
            break;
        default:
            *out_pointer = nullptr;
            break;
    }
}

// ---------------------------------------------------------------------------
// callbacks
// ---------------------------------------------------------------------------

int32_t hwc2_register_callback(struct hwc2_device *device, int32_t descriptor,
                               hwc2_callback_data_t callback_data,
                               hwc2_function_pointer_t pointer) {
    auto *pdev = (struct openfde_hwc_composer_device_1 *)device;
    pdev->callback_data = callback_data;
    switch (descriptor) {
        case HWC2_CALLBACK_HOTPLUG:
            pdev->hotplug = (HWC2_PFN_HOTPLUG)pointer;
            break;
        case HWC2_CALLBACK_VSYNC:
            pdev->vsync = (HWC2_PFN_VSYNC)pointer;
            break;
        case HWC2_CALLBACK_REFRESH:
            pdev->refresh = (HWC2_PFN_REFRESH)pointer;
            break;
        default:
            return HWC2_ERROR_BAD_PARAMETER;
    }
    return HWC2_ERROR_NONE;
}

// ---------------------------------------------------------------------------
// virtual displays
// ---------------------------------------------------------------------------

int32_t hwc2_get_max_virtual_display_count(struct hwc2_device *device,
                                           uint32_t *out_count) {
    (void)device;
    *out_count = 0;
    return HWC2_ERROR_NONE;
}

int32_t hwc2_create_virtual_display(struct hwc2_device *device, uint32_t width,
                                    uint32_t height, int32_t *format,
                                    hwc2_display_t *out_display) {
    (void)device; (void)width; (void)height; (void)format; (void)out_display;
    return HWC2_ERROR_UNSUPPORTED;
}

int32_t hwc2_destroy_virtual_display(struct hwc2_device *device,
                                     hwc2_display_t display) {
    (void)device; (void)display;
    return HWC2_ERROR_UNSUPPORTED;
}

// ---------------------------------------------------------------------------
// per-display config / attributes
// ---------------------------------------------------------------------------

int32_t hwc2_get_display_configs(struct hwc2_device *device,
                                 hwc2_display_t display, uint32_t *out_num_configs,
                                 hwc2_config_t *out_configs) {
    auto *pdev = (struct openfde_hwc_composer_device_1 *)device;
    auto *dd = get_display_data(device, display);
    if (!dd)
        return HWC2_ERROR_BAD_DISPLAY;

    // 扩展屏只有一种固定配置
    if (dd->display_class == HWC_DISPLAY_EXTERNAL) {
        if (*out_num_configs == 0) {
            *out_num_configs = 1;
            return HWC2_ERROR_NONE;
        }
        if (!out_configs)
            return HWC2_ERROR_BAD_PARAMETER;
        out_configs[0] = 0;
        *out_num_configs = 1;
        return HWC2_ERROR_NONE;
    }

    // 主屏支持多分辨率
    size_t supportedConfigCount = gScreenConfigCount;
    if (*out_num_configs == 0) {
        *out_num_configs = supportedConfigCount;
        return HWC2_ERROR_NONE;
    }
    if (!out_configs)
        return HWC2_ERROR_BAD_PARAMETER;
    size_t i;
    for (i = 0; i < *out_num_configs && i < supportedConfigCount; i++)
        out_configs[i] = (hwc2_config_t)i;
    *out_num_configs = i;
    return HWC2_ERROR_NONE;
}

int32_t hwc2_get_display_attribute(struct hwc2_device *device,
                                   hwc2_display_t display, hwc2_config_t config,
                                   int32_t attribute, int32_t *out_value) {
    auto *pdev = (struct openfde_hwc_composer_device_1 *)device;
    auto *dd = get_display_data(device, display);
    if (!dd)
        return HWC2_ERROR_BAD_DISPLAY;
    *out_value = hwc2_attribute(pdev, dd->disp, (uint32_t)attribute, (uint32_t)config);
    if (*out_value == -EINVAL)
        return HWC2_ERROR_BAD_PARAMETER;
    return HWC2_ERROR_NONE;
}

int32_t hwc2_get_active_config(struct hwc2_device *device, hwc2_display_t display,
                               hwc2_config_t *out_config) {
    auto *dd = get_display_data(device, display);
    if (!dd)
        return HWC2_ERROR_BAD_DISPLAY;
    *out_config = dd->active_config;
    return HWC2_ERROR_NONE;
}

int32_t hwc2_set_active_config(struct hwc2_device *device, hwc2_display_t display,
                               hwc2_config_t config) {
    auto *pdev = (struct openfde_hwc_composer_device_1 *)device;
    auto *dd = get_display_data(device, display);
    if (!dd)
        return HWC2_ERROR_BAD_DISPLAY;

    // 只有主屏支持切换分辨率；扩展屏为固定配置
    if (dd->display_class != HWC_DISPLAY_PRIMARY)
        return HWC2_ERROR_UNSUPPORTED;

    if (config < 0 || config >= gScreenConfigCount) {
        ALOGE("unsupported active config %u for display %llu", config, (unsigned long long)display);
        return HWC2_ERROR_BAD_PARAMETER;
    }

    if (dd->active_config == (uint32_t)config)
        return HWC2_ERROR_NONE;

    int target_width = 0;
    int target_height = 0;
    if (config < gScreenConfigCount) {
        if (screenConfigs[config].Width == 0 || screenConfigs[config].Height == 0)
            return HWC2_ERROR_BAD_PARAMETER;
        target_width = screenConfigs[config].Width;
        target_height = screenConfigs[config].Height;
    }

    dd->disp->full_width = target_width;
    dd->disp->full_height = target_height;
    dd->disp->geo_changed = true;
    dd->disp->width = target_width;
    dd->disp->height = target_height;
    dd->disp->primary_x = 0;
    dd->disp->primary_y = 0;
    {
        std::scoped_lock lock(dd->disp->windowsMutex);
        hwc_resize_windows(pdev, target_width, target_height);
    }

    ALOGD("setActiveConfig: config=%d resized windows to %dx%d", config, target_width, target_height);
    dd->active_config = config;

    {
        char property[PROPERTY_VALUE_MAX];
        int prop_width = target_width;
        int prop_height = target_height;
        if (property_get("persist.openfde.width_padding", property, nullptr) > 0)
            prop_width -= atoi(property);
        if (property_get("persist.openfde.height_padding", property, nullptr) > 0)
            prop_height -= atoi(property);
        property_set("openfde.display_width", std::to_string(prop_width).c_str());
        property_set("openfde.display_height", std::to_string(prop_height).c_str());
    }

    return HWC2_ERROR_NONE;
}

int32_t hwc2_get_display_type(struct hwc2_device *device, hwc2_display_t display,
                              int32_t *out_type) {
    auto *dd = get_display_data(device, display);
    if (!dd)
        return HWC2_ERROR_BAD_DISPLAY;
    *out_type = dd->type;
    return HWC2_ERROR_NONE;
}

int32_t hwc2_set_power_mode(struct hwc2_device *device, hwc2_display_t display,
                            int32_t mode) {
    auto *dd = get_display_data(device, display);
    if (!dd)
        return HWC2_ERROR_BAD_DISPLAY;
    dd->power_mode = mode;
    return HWC2_ERROR_NONE;
}

int32_t hwc2_set_vsync_enabled(struct hwc2_device *device, hwc2_display_t display,
                               int32_t enabled) {
    auto *dd = get_display_data(device, display);
    if (!dd)
        return HWC2_ERROR_BAD_DISPLAY;
    dd->vsync_enabled = (enabled == HWC2_VSYNC_ENABLE);
    return HWC2_ERROR_NONE;
}

// ---------------------------------------------------------------------------
// color modes / transforms
// ---------------------------------------------------------------------------

int32_t hwc2_get_color_modes(struct hwc2_device *device, hwc2_display_t display,
                             uint32_t *out_num_modes, int32_t *out_modes) {
    (void)device; (void)display;
    if (*out_num_modes == 0) {
        *out_num_modes = 1;
        return HWC2_ERROR_NONE;
    }
    if (!out_modes)
        return HWC2_ERROR_BAD_PARAMETER;
    out_modes[0] = HAL_COLOR_MODE_NATIVE;
    *out_num_modes = 1;
    return HWC2_ERROR_NONE;
}

int32_t hwc2_set_color_mode(struct hwc2_device *device, hwc2_display_t display,
                            int32_t mode) {
    (void)device; (void)display; (void)mode;
    return HWC2_ERROR_NONE;
}

int32_t hwc2_set_color_transform(struct hwc2_device *device, hwc2_display_t display,
                                 const float *matrix, int32_t hint) {
    (void)device; (void)display; (void)matrix; (void)hint;
    return HWC2_ERROR_NONE;
}

// ---------------------------------------------------------------------------
// layers
// ---------------------------------------------------------------------------

int32_t hwc2_create_layer(struct hwc2_device *device, hwc2_display_t display,
                          hwc2_layer_t *out_layer) {
    auto *dd = get_display_data(device, display);
    if (!dd)
        return HWC2_ERROR_BAD_DISPLAY;
    hwc2_layer_t id = dd->next_layer_id++;
    hwc_layer_1_t layer = {};
    layer.compositionType = HWC_OVERLAY;
    layer.planeAlpha = 0xFF;
    hwc2_layer_state st{};
    st.id = id;
    st.layer = layer;
    st.z = 0;
    dd->layers[id] = st;
    dd->layer_order.push_back(id);
    *out_layer = id;
    return HWC2_ERROR_NONE;
}

int32_t hwc2_destroy_layer(struct hwc2_device *device, hwc2_display_t display,
                           hwc2_layer_t layer) {
    auto *dd = get_display_data(device, display);
    if (!dd)
        return HWC2_ERROR_BAD_DISPLAY;
    auto it = dd->layers.find(layer);
    if (it == dd->layers.end())
        return HWC2_ERROR_BAD_LAYER;
    dd->layers.erase(it);
    dd->layer_order.erase(
        std::remove(dd->layer_order.begin(), dd->layer_order.end(), layer),
        dd->layer_order.end());
    dd->release_fences.erase(layer);
    return HWC2_ERROR_NONE;
}

// ---------------------------------------------------------------------------
// validate / present
// ---------------------------------------------------------------------------

int32_t hwc2_validate_display(struct hwc2_device *device, hwc2_display_t display,
                              uint32_t *out_num_types, uint32_t *out_num_requests) {
    auto *dd = get_display_data(device, display);
    if (!dd)
        return HWC2_ERROR_BAD_DISPLAY;

    dd->disp->geo_changed = true;

    dd->changed_types.clear();
    dd->found_cursor = false;
    for (auto &kv : dd->layers) {
        hwc2_layer_state &st = kv.second;
        bool is_cursor = false;
        int32_t internal = hwc2_to_internal_comp(st.layer.compositionType, &is_cursor);
        st.layer.compositionType = (uint32_t)internal;
        if (is_cursor) {
            st.layer.flags |= HWC_IS_CURSOR_LAYER;
            dd->found_cursor = true;
        } else {
            st.layer.flags &= ~HWC_IS_CURSOR_LAYER;
        }
    }

    // Cursor hide bookkeeping (mirrors the HWC1 prepare logic, now per display).
    if (!dd->found_cursor && dd->disp->mouse_icon_addr != -1) {
        dd->disp->mouse_icon_addr = -1;
        dd->disp->additional_refresh_cursor_times = 0;
        ALOGI("hwc2 cursor hidden on display %llu", (unsigned long long)display);
    }

    dd->validated = true;
    *out_num_types = 0;
    *out_num_requests = 0;
    return HWC2_ERROR_NONE;
}

int32_t hwc2_get_changed_composition_types(struct hwc2_device *device,
                                           hwc2_display_t display,
                                           uint32_t *out_num_types,
                                           hwc2_layer_t *out_layers,
                                           int32_t *out_types) {
    auto *dd = get_display_data(device, display);
    if (!dd)
        return HWC2_ERROR_BAD_DISPLAY;
    *out_num_types = 0;
    (void)out_layers; (void)out_types;
    return HWC2_ERROR_NONE;
}

int32_t hwc2_get_display_requests(struct hwc2_device *device,
                                  hwc2_display_t display, int32_t *out_display_requests,
                                  uint32_t *out_num_layers,
                                  hwc2_layer_t *out_layers, int32_t *out_layer_requests) {
    (void)device; (void)display;
    *out_display_requests = 0;
    *out_num_layers = 0;
    (void)out_layers; (void)out_layer_requests;
    return HWC2_ERROR_NONE;
}

int32_t hwc2_accept_display_changes(struct hwc2_device *device,
                                    hwc2_display_t display) {
    auto *dd = get_display_data(device, display);
    if (!dd)
        return HWC2_ERROR_BAD_DISPLAY;
    dd->validated = false;
    return HWC2_ERROR_NONE;
}

// Build a synthetic hwc_display_contents_1_t from the display's HWC2 layer
// state. The client target (framebuffer target) is placed at index 0; device
// layers are placed at index == their hwc2_layer_t id, so that the
// backend render path's use of disp->layer_names[id] / layer_handles_ext[id]
// stays aligned with the indices the IOpenfdeDisplay side-channel publishes.
//
// NOTE: in this tree hwc_display_contents_1_t::hwLayers is a flexible array
// member (hwc_layer_1_t hwLayers[0]), so it cannot be assigned. We therefore
// allocate one contiguous block: the base struct followed by the layer array.
static hwc_display_contents_1_t *build_contents(struct hwc2_display_data *dd) {
    size_t size = (size_t)dd->next_layer_id; // ids start at 1
    if (dd->has_client_target)
        size = std::max(size, (size_t)1);

    // hwLayers is the trailing flexible array member, so sizeof already equals
    // its start offset.
    size_t alloc = sizeof(hwc_display_contents_1_t) + size * sizeof(hwc_layer_1_t);
    hwc_display_contents_1_t *contents = (hwc_display_contents_1_t *)calloc(1, alloc);
    if (!contents)
        return nullptr;
    contents->numHwLayers = (uint32_t)size;

    // Gaps (non-contiguous layer ids) must not carry a 0 acquire fence, or the
    // backend would close fd 0. -1 is the "no fence" sentinel.
    for (size_t i = 0; i < size; i++) {
        contents->hwLayers[i].acquireFenceFd = -1;
        contents->hwLayers[i].releaseFenceFd = -1;
    }

    if (dd->has_client_target) {
        contents->hwLayers[0] = dd->client_target_layer;
        contents->hwLayers[0].compositionType = HWC_FRAMEBUFFER_TARGET;
        contents->hwLayers[0].handle = dd->client_target_handle;
        contents->hwLayers[0].acquireFenceFd = dd->client_target_acquire_fence;
    }

    for (auto &kv : dd->layers) {
        hwc2_layer_state &st = kv.second;
        size_t idx = (size_t)st.id;
        if (idx >= size)
            continue;
        contents->hwLayers[idx] = st.layer;
        contents->hwLayers[idx].handle = st.handle;
        contents->hwLayers[idx].acquireFenceFd = st.acquireFenceFd;
        contents->hwLayers[idx].releaseFenceFd = -1;
        if (st.found_cursor) {
            contents->hwLayers[idx].compositionType = HWC_CURSOR;
            contents->hwLayers[idx].transform = 0;
        }
    }

    contents->retireFenceFd = -1;
    contents->outbufAcquireFenceFd = -1;
    contents->flags = HWC_GEOMETRY_CHANGED;
    return contents;
}

int32_t hwc2_present_display(struct hwc2_device *device, hwc2_display_t display,
                             int32_t *out_retire_fence) {
    auto *pdev = (struct openfde_hwc_composer_device_1 *)device;
    auto *dd = get_display_data(device, display);
    if (!dd)
        return HWC2_ERROR_BAD_DISPLAY;

    *out_retire_fence = -1;

    if (!dd->has_client_target && dd->layers.empty())
        return HWC2_ERROR_NONE;

    hwc_display_contents_1_t *contents = build_contents(dd);
    if (!contents)
        return HWC2_ERROR_NO_RESOURCES;

    int ret = hwc2_present(pdev, dd, contents);
    if (ret != 0) {
        free(contents);
        return HWC2_ERROR_NONE;
    }

    // Collect per-layer release fences and the frame retire fence.
    dd->retire_fence = contents->retireFenceFd;
    dd->release_fences.clear();
    for (auto &kv : dd->layers) {
        size_t idx = (size_t)kv.first;
        if (idx < (size_t)contents->numHwLayers)
            dd->release_fences[kv.first] = contents->hwLayers[idx].releaseFenceFd;
    }
    if (dd->has_client_target)
        dd->client_target_acquire_fence = -1;
    *out_retire_fence = dd->retire_fence;
    dd->validated = false;
    free(contents);
    return HWC2_ERROR_NONE;
}

int32_t hwc2_get_release_fences(struct hwc2_device *device, hwc2_display_t display,
                                uint32_t *out_num_elements, hwc2_layer_t *out_layers,
                                int32_t *out_fences) {
    auto *dd = get_display_data(device, display);
    if (!dd)
        return HWC2_ERROR_BAD_DISPLAY;
    uint32_t n = (uint32_t)dd->release_fences.size();
    if (*out_num_elements == 0) {
        *out_num_elements = n;
        return HWC2_ERROR_NONE;
    }
    uint32_t to_copy = std::min(n, *out_num_elements);
    uint32_t i = 0;
    for (auto &kv : dd->release_fences) {
        if (i >= to_copy)
            break;
        out_layers[i] = kv.first;
        out_fences[i] = kv.second;
        i++;
    }
    *out_num_elements = i;
    return HWC2_ERROR_NONE;
}

int32_t hwc2_set_client_target(struct hwc2_device *device, hwc2_display_t display,
                               buffer_handle_t target, int32_t acquire_fence,
                               int32_t dataspace, hwc_region_t damage) {
    auto *dd = get_display_data(device, display);
    if (!dd)
        return HWC2_ERROR_BAD_DISPLAY;
    (void)dataspace; (void)damage;
    hwc_layer_1_t layer = {};
    layer.compositionType = HWC_FRAMEBUFFER_TARGET;
    layer.handle = target;
    layer.acquireFenceFd = acquire_fence;
    dd->client_target_layer = layer;
    dd->client_target_handle = target;
    dd->client_target_acquire_fence = acquire_fence;
    dd->has_client_target = true;
    return HWC2_ERROR_NONE;
}

// ---------------------------------------------------------------------------
// misc display queries
// ---------------------------------------------------------------------------

int32_t hwc2_get_display_name(struct hwc2_device *device, hwc2_display_t display,
                              uint32_t *out_size, char *out_name) {
    (void)device;
    const char *name = (display == 0) ? "openfde" : "openfde_external";
    uint32_t len = (uint32_t)strlen(name) + 1;
    if (*out_size == 0) {
        *out_size = len;
        return HWC2_ERROR_NONE;
    }
    if (!out_name)
        return HWC2_ERROR_BAD_PARAMETER;
    strncpy(out_name, name, *out_size - 1);
    out_name[*out_size - 1] = '\0';
    return HWC2_ERROR_NONE;
}

int32_t hwc2_get_display_capabilities(struct hwc2_device *device,
                                      hwc2_display_t display,
                                      uint32_t *out_num_capabilities,
                                      uint32_t *out_capabilities) {
    (void)device; (void)display;
    if (*out_num_capabilities == 0) {
        *out_num_capabilities = 1;
        return HWC2_ERROR_NONE;
    }
    if (!out_capabilities)
        return HWC2_ERROR_BAD_PARAMETER;
    out_capabilities[0] = HWC2_DISPLAY_CAPABILITY_SKIP_CLIENT_COLOR_TRANSFORM;
    *out_num_capabilities = 1;
    return HWC2_ERROR_NONE;
}

int32_t hwc2_get_doze_support(struct hwc2_device *device, hwc2_display_t display,
                              int32_t *out_support) {
    (void)device; (void)display;
    *out_support = 0;
    return HWC2_ERROR_NONE;
}

int32_t hwc2_get_hdr_capabilities(struct hwc2_device *device, hwc2_display_t display,
                                  uint32_t *out_num_types, int32_t *out_types,
                                  float *out_max_luminance,
                                  float *out_max_average_luminance,
                                  float *out_min_luminance) {
    (void)device; (void)display; (void)out_types;
    (void)out_max_luminance; (void)out_max_average_luminance; (void)out_min_luminance;
    *out_num_types = 0;
    return HWC2_ERROR_NONE;
}

int32_t hwc2_get_client_target_support(struct hwc2_device *device,
                                       hwc2_display_t display, uint32_t width,
                                       uint32_t height, int32_t format,
                                       int32_t dataspace) {
    (void)device; (void)display; (void)width; (void)height; (void)format; (void)dataspace;
    return HWC2_ERROR_NONE;
}

int32_t hwc2_set_output_buffer(struct hwc2_device *device, hwc2_display_t display,
                               buffer_handle_t buffer, int32_t release_fence) {
    (void)device; (void)display; (void)buffer; (void)release_fence;
    return HWC2_ERROR_UNSUPPORTED;
}

// ---------------------------------------------------------------------------
// setLayer* / getLayer* configuration
// ---------------------------------------------------------------------------

int32_t hwc2_set_layer_composition_type(struct hwc2_device *device,
                                        hwc2_display_t display, hwc2_layer_t layer,
                                        int32_t type) {
    auto *dd = get_display_data(device, display);
    if (!dd) return HWC2_ERROR_BAD_DISPLAY;
    auto it = dd->layers.find(layer);
    if (it == dd->layers.end()) return HWC2_ERROR_BAD_LAYER;
    it->second.layer.compositionType = (uint32_t)type;
    return HWC2_ERROR_NONE;
}

int32_t hwc2_set_layer_buffer(struct hwc2_device *device, hwc2_display_t display,
                              hwc2_layer_t layer, buffer_handle_t buffer,
                              int32_t acquire_fence) {
    auto *dd = get_display_data(device, display);
    if (!dd) return HWC2_ERROR_BAD_DISPLAY;
    auto it = dd->layers.find(layer);
    if (it == dd->layers.end()) return HWC2_ERROR_BAD_LAYER;
    it->second.layer.handle = buffer;
    it->second.layer.acquireFenceFd = acquire_fence;
    return HWC2_ERROR_NONE;
}

int32_t hwc2_set_layer_dataspace(struct hwc2_device *device, hwc2_display_t display,
                                 hwc2_layer_t layer, int32_t dataspace) {
    (void)device; (void)display; (void)layer; (void)dataspace;
    return HWC2_ERROR_NONE;
}

int32_t hwc2_set_layer_display_frame(struct hwc2_device *device,
                                     hwc2_display_t display, hwc2_layer_t layer,
                                     hwc_rect_t frame) {
    auto *dd = get_display_data(device, display);
    if (!dd) return HWC2_ERROR_BAD_DISPLAY;
    auto it = dd->layers.find(layer);
    if (it == dd->layers.end()) return HWC2_ERROR_BAD_LAYER;
    it->second.layer.displayFrame = frame;
    return HWC2_ERROR_NONE;
}

int32_t hwc2_set_layer_source_crop(struct hwc2_device *device,
                                   hwc2_display_t display, hwc2_layer_t layer,
                                   hwc_frect_t crop) {
    auto *dd = get_display_data(device, display);
    if (!dd) return HWC2_ERROR_BAD_DISPLAY;
    auto it = dd->layers.find(layer);
    if (it == dd->layers.end()) return HWC2_ERROR_BAD_LAYER;
    hwc_rect_t r;
    r.left = (int32_t)crop.left;
    r.top = (int32_t)crop.top;
    r.right = (int32_t)crop.right;
    r.bottom = (int32_t)crop.bottom;
    it->second.layer.sourceCrop = r;
    return HWC2_ERROR_NONE;
}

int32_t hwc2_set_layer_z_order(struct hwc2_device *device, hwc2_display_t display,
                               hwc2_layer_t layer, uint32_t z) {
    auto *dd = get_display_data(device, display);
    if (!dd) return HWC2_ERROR_BAD_DISPLAY;
    auto it = dd->layers.find(layer);
    if (it == dd->layers.end()) return HWC2_ERROR_BAD_LAYER;
    it->second.z = (int32_t)z;
    // Keep layer_order sorted ascending by z.
    dd->layer_order.erase(
        std::remove(dd->layer_order.begin(), dd->layer_order.end(), layer),
        dd->layer_order.end());
    auto pos = std::lower_bound(dd->layer_order.begin(), dd->layer_order.end(), layer,
        [&](hwc2_layer_t a, hwc2_layer_t) {
            return dd->layers[a].z < (int32_t)z;
        });
    dd->layer_order.insert(pos, layer);
    return HWC2_ERROR_NONE;
}

int32_t hwc2_set_layer_surface_damage(struct hwc2_device *device,
                                      hwc2_display_t display, hwc2_layer_t layer,
                                      hwc_region_t damage) {
    (void)device; (void)display; (void)layer; (void)damage;
    return HWC2_ERROR_NONE;
}

int32_t hwc2_set_layer_blend_mode(struct hwc2_device *device,
                                  hwc2_display_t display, hwc2_layer_t layer,
                                  int32_t mode) {
    auto *dd = get_display_data(device, display);
    if (!dd) return HWC2_ERROR_BAD_DISPLAY;
    auto it = dd->layers.find(layer);
    if (it == dd->layers.end()) return HWC2_ERROR_BAD_LAYER;
    int32_t hwc_blend = HWC_BLENDING_NONE;
    switch (mode) {
        case HWC2_BLEND_MODE_PREMULTIPLIED:
            hwc_blend = HWC_BLENDING_PREMULT;
            break;
        case HWC2_BLEND_MODE_COVERAGE:
            hwc_blend = HWC_BLENDING_COVERAGE;
            break;
        default:
            hwc_blend = HWC_BLENDING_NONE;
            break;
    }
    it->second.layer.blending = (int32_t)hwc_blend;
    return HWC2_ERROR_NONE;
}

int32_t hwc2_set_layer_plane_alpha(struct hwc2_device *device,
                                   hwc2_display_t display, hwc2_layer_t layer,
                                   float alpha) {
    auto *dd = get_display_data(device, display);
    if (!dd) return HWC2_ERROR_BAD_DISPLAY;
    auto it = dd->layers.find(layer);
    if (it == dd->layers.end()) return HWC2_ERROR_BAD_LAYER;
    it->second.layer.planeAlpha = (uint8_t)(alpha * 255.0f);
    return HWC2_ERROR_NONE;
}

int32_t hwc2_set_layer_transform(struct hwc2_device *device,
                                 hwc2_display_t display, hwc2_layer_t layer,
                                 int32_t transform) {
    auto *dd = get_display_data(device, display);
    if (!dd) return HWC2_ERROR_BAD_DISPLAY;
    auto it = dd->layers.find(layer);
    if (it == dd->layers.end()) return HWC2_ERROR_BAD_LAYER;
    it->second.layer.transform = transform;
    return HWC2_ERROR_NONE;
}

int32_t hwc2_set_layer_visible_region(struct hwc2_device *device,
                                      hwc2_display_t display, hwc2_layer_t layer,
                                      hwc_region_t region) {
    auto *dd = get_display_data(device, display);
    if (!dd) return HWC2_ERROR_BAD_DISPLAY;
    auto it = dd->layers.find(layer);
    if (it == dd->layers.end()) return HWC2_ERROR_BAD_LAYER;
    hwc2_layer_state &st = it->second;
    if (region.numRects > 0 && region.rect)
        st.visible_region.assign(region.rect, region.rect + region.numRects);
    else
        st.visible_region.clear();
    st.layer.visibleRegionScreen.numRects = (uint32_t)st.visible_region.size();
    st.layer.visibleRegionScreen.rect = st.visible_region.data();
    return HWC2_ERROR_NONE;
}

int32_t hwc2_set_layer_color(struct hwc2_device *device, hwc2_display_t display,
                             hwc2_layer_t layer, hwc_color_t color) {
    auto *dd = get_display_data(device, display);
    if (!dd) return HWC2_ERROR_BAD_DISPLAY;
    auto it = dd->layers.find(layer);
    if (it == dd->layers.end()) return HWC2_ERROR_BAD_LAYER;
    // This tree's hwc_layer_1_t has no 'color' field; solid-color layers are not
    // separately tracked (they are still composited via their buffer).
    (void)color;
    return HWC2_ERROR_NONE;
}

int32_t hwc2_set_layer_color_transform(struct hwc2_device *device,
                                       hwc2_display_t display, hwc2_layer_t layer,
                                       const float *matrix) {
    (void)device; (void)display; (void)layer; (void)matrix;
    return HWC2_ERROR_NONE;
}

int32_t hwc2_set_layer_per_frame_metadata(struct hwc2_device *device,
                                          hwc2_display_t display, hwc2_layer_t layer,
                                          uint32_t num_elements,
                                          const int32_t *keys, const float *values) {
    (void)device; (void)display; (void)layer; (void)num_elements;
    (void)keys; (void)values;
    return HWC2_ERROR_NONE;
}

int32_t hwc2_set_layer_cursor_position(struct hwc2_device *device,
                                       hwc2_display_t display, hwc2_layer_t layer,
                                       int32_t x, int32_t y) {
    auto *dd = get_display_data(device, display);
    if (!dd) return HWC2_ERROR_BAD_DISPLAY;
    auto it = dd->layers.find(layer);
    if (it == dd->layers.end()) return HWC2_ERROR_BAD_LAYER;
    (void)x; (void)y;
    return HWC2_ERROR_NONE;
}

int32_t hwc2_set_layer_buffer_crop(struct hwc2_device *device,
                                   hwc2_display_t display, hwc2_layer_t layer,
                                   hwc_rect_t crop) {
    auto *dd = get_display_data(device, display);
    if (!dd) return HWC2_ERROR_BAD_DISPLAY;
    auto it = dd->layers.find(layer);
    if (it == dd->layers.end()) return HWC2_ERROR_BAD_LAYER;
    it->second.layer.sourceCrop = crop;
    return HWC2_ERROR_NONE;
}

// ---------------------------------------------------------------------------
// teardown
// ---------------------------------------------------------------------------

int hwc_close(hw_device_t* dev) {
    struct openfde_hwc_composer_device_1* pdev = (struct openfde_hwc_composer_device_1*)dev;

    for (std::map<buffer_handle_t, struct buffer *>::iterator it = pdev->display->buffer_map.begin(); it != pdev->display->buffer_map.end(); it++)
    {
#ifdef USE_X11_BACKEND
        destroy_buffer(pdev->display, it->second);
#else
        destroy_buffer(it->second);
#endif
    }
    pdev->display->buffer_map.clear();

    destroy_display(pdev->display);

    if (pdev->secondary_display) {
        for (std::map<buffer_handle_t, struct buffer *>::iterator it = pdev->secondary_display->buffer_map.begin(); it != pdev->secondary_display->buffer_map.end(); it++)
        {
#ifdef USE_X11_BACKEND
            destroy_buffer(pdev->secondary_display, it->second);
#else
            destroy_buffer(it->second);
#endif
        }

        pdev->secondary_display->buffer_map.clear();
        destroy_display(pdev->secondary_display);
    }


#ifndef USE_X11_BACKEND
    pthread_kill(pdev->wayland_thread, SIGTERM);
    pthread_join(pdev->wayland_thread, NULL);
#endif

    delete dev;
    return 0;
}
