/*
 * Copyright (C) 2026 Openfde Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

// Shared HWC composer-device entry points. See hwc_composer.h for why this
// file exists and how the backend is selected.

#include "hwc_composer.h"

#include <errno.h>
#include <log/log.h>
#include <cutils/properties.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <utils/ThreadDefs.h>

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include <cutils/trace.h>
#include <utils/Trace.h>

// ---------------------------------------------------------------------------
// vsync timing
// ---------------------------------------------------------------------------

long time_to_sleep_to_next_vsync(struct timespec *rt, uint64_t last_vsync_ns, unsigned vsync_period_ns)
{
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

        if (!vsync_enabled || !pdev->procs || !pdev->procs->vsync) {
            ATRACE_END();
            continue;
        }
        if (  !pdev->procs || !pdev->procs->hotplug ){
            ALOGW("hotplug is null");
        } else {
            if (!hotpluged){
                pdev->procs->hotplug(pdev->procs,1,1);
                pdev->secondary_hotpluged = true;
            }
        }

        int64_t timestamp = (uint64_t)rt.tv_sec * 1e9 + rt.tv_nsec;
        pdev->procs->vsync(pdev->procs, 0, timestamp);
        ATRACE_END();
    }

    return NULL;
}

// ---------------------------------------------------------------------------
// simple entry points (identical in both backends)
// ---------------------------------------------------------------------------

int hwc_blank(struct hwc_composer_device_1* dev __unused, int disp __unused,
              int blank __unused) {
    return 0;
}

int hwc_query(struct hwc_composer_device_1* dev, int what, int* value) {
    struct openfde_hwc_composer_device_1* pdev =
            (struct openfde_hwc_composer_device_1*)dev;

    switch (what) {
        case HWC_VSYNC_PERIOD:
            value[0] = pdev->vsync_period_ns;
            break;
        default:
            // unsupported query
            ALOGE("%s badness unsupported query what=%d", __FUNCTION__, what);
            return -EINVAL;
    }
    return 0;
}

int hwc_event_control(struct hwc_composer_device_1* dev, int dpy __unused,
                      int event, int enabled) {
    struct openfde_hwc_composer_device_1* pdev =
            (struct openfde_hwc_composer_device_1*)dev;
    int ret = -EINVAL;

    // enabled can only be 0 or 1
    if (!(enabled & ~1)) {
        if (event == HWC_EVENT_VSYNC) {
            pthread_mutex_lock(&pdev->vsync_lock);
            pdev->vsync_callback_enabled = enabled;
            pthread_mutex_unlock(&pdev->vsync_lock);
            ret = 0;
        }
    }
    return ret;
}

void hwc_register_procs(struct hwc_composer_device_1* dev,
                        hwc_procs_t const* procs) {
    struct openfde_hwc_composer_device_1* pdev = (struct openfde_hwc_composer_device_1*)dev;
    pdev->procs = procs;
}

// ---------------------------------------------------------------------------
// HWC 1.4 configuration / attributes
// ---------------------------------------------------------------------------

int hwc_get_display_configs(struct hwc_composer_device_1* dev __unused,
                            int disp, uint32_t* configs, size_t* numConfigs) {
    // 扩展屏只有一种固定配置
    if (disp == HWC_DISPLAY_EXTERNAL) {
        if (*numConfigs == 0) {
            *numConfigs = 1;
            return 0;
        }
        if (configs == NULL) {
            return -EINVAL;
        }
        configs[0] = 0;
        *numConfigs = 1;
        return 0;
    }

    if (disp == HWC_DISPLAY_PRIMARY) {
        // 主屏支持多分辨率：config 0 为真实分辨率，其余为去重后的预设分辨率
        size_t supportedConfigCount = gScreenConfigCount;
        if (*numConfigs == 0) {
            *numConfigs = supportedConfigCount;
            return 0;
        }
        if (configs == NULL) {
            return -EINVAL;
        }
        size_t i;
        for (i = 0; i < *numConfigs && i < supportedConfigCount; i++) {
            configs[i] = (uint32_t)i;
        }

        // 更新实际写入的数量
        *numConfigs = i;
        return 0;
    }

    return -EINVAL;
}

int32_t hwc_attribute(struct openfde_hwc_composer_device_1* pdev, struct display * display,
                      const uint32_t attribute, uint32_t config) {
    char property[PROPERTY_VALUE_MAX];
    int width = display->full_width; //pdev->display->full_width;
    int height = display->full_height; //pdev->display->full_height;
    int density = 160;
    // config > 0 表示选中了非默认（预设）分辨率，按 screenConfigs 覆盖宽高与密度
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
            if(display->display_type == HWC_DISPLAY_PRIMARY){
                property_set("openfde.display_width", width_str.c_str());
            }else{
                property_set("openfde.external_display_width", width_str.c_str());
            }
            return width;
        }
        case HWC_DISPLAY_HEIGHT: {
            if (property_get("persist.openfde.height_padding", property, nullptr) > 0)
                height -= atoi(property);
            std::string height_str = std::to_string(height);
            if(display->display_type == HWC_DISPLAY_PRIMARY){
                property_set("openfde.display_height", height_str.c_str());
            }else{
                property_set("openfde.external_display_height", height_str.c_str());
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

int hwc_get_display_attributes(struct hwc_composer_device_1* dev __unused,
                               int disp, uint32_t config,
                               const uint32_t* attributes, int32_t* values) {
    struct openfde_hwc_composer_device_1* pdev = (struct openfde_hwc_composer_device_1*)dev;
    for (int i = 0; attributes[i] != HWC_DISPLAY_NO_ATTRIBUTE; i++) {
        if (disp == HWC_DISPLAY_PRIMARY || disp == HWC_DISPLAY_EXTERNAL) {
            if(disp == HWC_DISPLAY_PRIMARY){
                values[i] = hwc_attribute(pdev, pdev->display, attributes[i], config);
            }else{
                // 扩展屏为固定配置，忽略传入的 config
                values[i] = hwc_attribute(pdev, pdev->secondary_display, attributes[i], 0);
            }
            if (values[i] == -EINVAL) {
                return -EINVAL;
            }
        } else {
            ALOGE("unknown display type %u", disp);
            return -EINVAL;
        }
    }

    return 0;
}

int hwc_get_active_config(struct hwc_composer_device_1* dev, int disp) {
    struct openfde_hwc_composer_device_1* pdev = (struct openfde_hwc_composer_device_1*)dev;

    if (disp != HWC_DISPLAY_PRIMARY) {
        return -EINVAL;
    }

    return disp->active_config;
}

int hwc_set_active_config(struct hwc_composer_device_1* dev, int disp, int config) {
    struct openfde_hwc_composer_device_1* pdev = (struct openfde_hwc_composer_device_1*)dev;

    // 只有主屏支持切换分辨率；扩展屏为固定配置
    if (disp != HWC_DISPLAY_PRIMARY) {
        return -EINVAL;
    }

    if (config < 0 || config >= gScreenConfigCount) {
        ALOGE("unsupported active config %u for display %d", config, disp);
        return -EINVAL;
    }

    if (disp->active_config == config) {
        return 0;
    }
    int target_width = 0;
    int target_height = 0;
    // config 0 为真实分辨率，其余为去重后的预设分辨率
    if (config < gScreenConfigCount) {
        if (screenConfigs[config].Width == 0 || screenConfigs[config].Height == 0) {
            return -EINVAL;
        }
        target_width = screenConfigs[config].Width;
        target_height = screenConfigs[config].Height;
    }

    disp->full_width = target_width;
    disp->full_height = target_height;
#ifdef USE_X11_BACKEND
    disp->geo_changed = true;
    disp->width = target_width;
    disp->height = target_height;
    disp->primary_x = 0;
    disp->primary_y = 0;
    {
        std::scoped_lock lock(disp->windowsMutex);
        hwc_resize_windows(pdev, target_width, target_height);
    }
#else
    double scale = disp->scale > 0 ? disp->scale : 1.0;
    disp->width = target_width / scale;
    disp->height = target_height / scale;
    hwc_resize_windows(pdev, target_width, target_height);
#endif

    ALOGE("setActiveConfig: config=%d resized windows to %dx%d", config, target_width, target_height);
    disp->active_config = config;

    return 0;
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

    for (std::map<buffer_handle_t, struct buffer *>::iterator it = pdev->secondary_display->buffer_map.begin(); it != pdev->secondary_display->buffer_map.end(); it++)
    {
        destroy_buffer(pdev->secondary_display, it->second);
    }

    pdev->secondary_display->buffer_map.clear();
    destroy_display(pdev->secondary_display);


#ifndef USE_X11_BACKEND
    pthread_kill(pdev->wayland_thread, SIGTERM);
    pthread_join(pdev->wayland_thread, NULL);
#endif

    delete dev;
    return 0;
}
