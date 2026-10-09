/*
 * Copyright (C) 2012 The Android Open Source Project
 * Copyright (C) 2021 The Openfde Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "hwc_backend.h"
#include "wayland-backend.h"
#include "egl-tools.h"

#if defined(__aarch64__)
#include "x11-backend.h"
#endif

#include <stdlib.h>
#include <functional>

#include <log/log.h>
#include <system/graphics.h>
#include <ui/Rect.h>
#include <ui/GraphicBufferMapper.h>

/*
 * 按 DISPLAY 环境变量选择协议后端：
 *   - DISPLAY 非空 -> X11 后端
 *   - 否则         -> Wayland 后端
 * DISPLAY 可由系统属性 openfde.x11_display 在 hwc_open 中注入。
 */
HwcBackend *HwcBackend::create() {
#if defined(__aarch64__)
    const char *x11_display = getenv("DISPLAY");
    if (x11_display && x11_display[0] != '\0') {
        ALOGI("DISPLAY=%s detected, using X11 hwcomposer backend", x11_display);
        return new X11Backend();
    }
#endif
    ALOGI("DISPLAY not set, using Wayland hwcomposer backend");
    return new WaylandBackend();
}

void update_shm_buffer(struct display* display, struct buffer *buffer)
{
    // Slower but always correct
    if (display->gtype != GRALLOC_DEFAULT) {
        display->egl_work_queue.push_back(std::bind(egl_render_to_pixels, display, buffer));
        sem_post(&display->egl_go);
        sem_wait(&display->egl_done);
        return;
    }

    // Fast path for when the buffer is guaranteed to be linear and 4bpp
    void *data;
    int shm_stride, src_stride;
    android::Rect bounds(buffer->width, buffer->height);
    if (android::GraphicBufferMapper::get().lock(buffer->handle, GRALLOC_USAGE_SW_READ_OFTEN, bounds, &data) == 0) {
        src_stride = buffer->pixel_stride;
        shm_stride = buffer->width;
        for (int i = 0; i < buffer->height; i++) {
            uint32_t* source = (uint32_t*)data + (i * src_stride);
            uint32_t* dist = (uint32_t*)buffer->shm_data + (i * shm_stride);
            uint32_t* end = dist + shm_stride;

            while (dist < end) {
                uint32_t c = *source;
                *dist = (c & 0xFF00FF00) | ((c & 0xFF0000) >> 16) | ((c & 0xFF) << 16);
                source++;
                dist++;
            }
        }
        android::GraphicBufferMapper::get().unlock(buffer->handle);
    }
}
