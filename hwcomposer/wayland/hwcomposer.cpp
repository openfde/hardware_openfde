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

// HWC2 composer device with a wayland backend. All display functions are
// per display as required by hwcomposer2.h; the protocol state machine lives
// in common/hwc2_device.cpp, this file only implements the backend that
// presents per-display layer lists on wayland surfaces.

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>
#include <wayland-client.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <string>
#include <sstream>
#include <set>
#include <functional>

#include <log/log.h>
#include <cutils/properties.h>
#include <hardware/hwcomposer.h>
#include <hidl/HidlTransportSupport.h>
#include <system/graphics.h>
#include <ui/Rect.h>
#include <ui/GraphicBufferMapper.h>
#include <sync/sync.h>
#include <drm_fourcc.h>
#include <presentation-time-client-protocol.h>
#include <viewporter-client-protocol.h>
#include <gralloc_handle.h>
#include <cros_gralloc/cros_gralloc_handle.h>

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include <cutils/trace.h>
#include <utils/Trace.h>

#include "../common/hwc2_device.h"
#include "OpenfdeWindow.h"
#include "egl-tools.h"
#include "xdg-shell-client-protocol.h"

#include <hidl/HidlTransportSupport.h>

using ::android::OK;
using ::android::status_t;
using ::android::hardware::configureRpcThreadpool;
using ::android::hardware::joinRpcThreadpool;

using ::openfde::Hwc2Backend;
using ::openfde::Hwc2Device;
using ::openfde::Hwc2Display;
using ::openfde::Hwc2DumpState;
using ::openfde::Hwc2Layer;
using ::openfde::handle_ext;

using ::vendor::openfde::window::V1_1::IOpenfdeWindow;
using ::vendor::openfde::window::implementation::OpenfdeWindow;

using namespace HWC2;

namespace {

class WaylandBackend : public Hwc2Backend {
  public:
    WaylandBackend(struct display* display, bool useSubsurface, bool multiWindows)
        : mDisplay(display), mUseSubsurface(useSubsurface), mMultiWindows(multiWindows) {}

    ~WaylandBackend() override {
        for (auto& pair : mDisplay->buffer_map) {
            if (pair.second) {
                destroy_buffer(pair.second);
            }
        }
        mDisplay->buffer_map.clear();
        destroy_display(mDisplay);
    }

    // Hwc2Backend interface
    std::vector<hwc2_config_t> getConfigs(Hwc2Display& display) override {
        if (!display.isPrimary()) {
            // The external display exposes a single fixed configuration matching
            // the secondary wl_output's physical resolution.
            return {0};
        }
        // The primary display supports multiple resolutions: config 0 is the
        // real one, the rest are deduplicated presets
        std::vector<hwc2_config_t> configs;
        for (int i = 0; i < gScreenConfigCount; i++) {
            configs.push_back((hwc2_config_t)i);
        }
        return configs;
    }

    Error getActiveConfig(Hwc2Display& display, hwc2_config_t* outConfig) override {
        if (!display.isPrimary()) {
            *outConfig = 0;
            return Error::None;
        }
        *outConfig = mDisplay->active_config;
        return Error::None;
    }

    Error setActiveConfig(Hwc2Display& display, hwc2_config_t config) override;
    Error getAttribute(Hwc2Display& display, hwc2_config_t config, Attribute attribute,
                       int32_t* outValue) override;
    Error validate(Hwc2Display& display) override;
    Error present(Hwc2Display& display) override;
    // Renders the external display (non-primary) onto the secondary-output window.
    Error presentExternal(Hwc2Display& display);
    Hwc2DumpState getDumpState(Hwc2Display& display) override;

    // Creates the initial "Openfde" window matching the desktop file
    void createInitialWindow();

    // External display (wayland secondary output) management, mirroring the
    // x11 backend. The external display is a second wl_output on the same
    // wl_display connection, presented through a dedicated fullscreen window.
    void createExternalDisplays(Hwc2Device& device) override;
    void destroyExternalDisplay(void* backendData) override;
    bool isExternalDisplaySupported() override;

    // Re-evaluate external-display support after an output hotplug and fire the
    // kSupported/kUnsupported transition. Auto-closes the external display when
    // it can no longer be hosted (secondary output removed).
    static void onExternalDisplayChanged(void* userdata);
    void updateSupported(bool supported);
    void closeExternalIfUnsupported();

    // Backend threads: wayland dispatch, window service and egl worker
    int startThreads();

  private:
    void resizeWindows(int target_width, int target_height);

    struct buffer* getWlBuffer(buffer_handle_t handle, const handle_ext& ext,
                               hwc_rect_t fallbackFrame);
    struct wl_surface* getSurface(hwc_rect_t frame, hwc_frect_t crop, Transform transform,
                                  struct window* window, bool multi);
    bool updateCursorSurface(Hwc2Layer& layer);
    void eraseCursorLayerBuffer(buffer_handle_t handle);

    static void* waylandThreadEntry(void* data);
    static void* windowServiceThreadEntry(void* data);

    struct display* mDisplay;
    std::map<std::string, struct window*> mWindows;
    bool mUseSubsurface;
    bool mMultiWindows;

    pthread_t mWaylandThread = 0;
    pthread_t mWindowServiceThread = 0;
    pthread_t mEglWorkerThread = 0;

    // ---- External display (wayland secondary output) ----
    // Pointer to the owning Hwc2Device, set in createExternalDisplays().
    Hwc2Device* mDevice = nullptr;
    // Last known support state, so kSupported/kUnsupported only fire on change.
    bool mLastSupported = false;
    // The non-primary output used as the external display (nullptr when none).
    struct output* mSecondaryOutput = nullptr;
    // Top-level window presented on the secondary output.
    struct window* mSecondaryWindow = nullptr;
    // Window map for the external display (mirrors mWindows for the primary).
    std::map<std::string, struct window*> mSecondaryWindows;
    // Gralloc handles imported into buffer_map while the external display was
    // open. Used to release exactly those wayland buffers on close: the map is
    // keyed by handle POINTER, and SurfaceFlinger recycles freed handle
    // addresses, so stale entries would be hit by the next open's freshly
    // allocated buffers (getWlBuffer returns orphaned wl_buffers -> flicker).
    std::set<buffer_handle_t> mExternalBufferHandles;
};

static void setupViewportDestination(wp_viewport* viewport, hwc_rect_t frame,
                                     struct display* display) {
    wp_viewport_set_destination(viewport,
            fmax(1, ceil((frame.right - frame.left) / display->scale)),
            fmax(1, ceil((frame.bottom - frame.top) / display->scale)));
}

void WaylandBackend::eraseCursorLayerBuffer(buffer_handle_t handle) {
    auto it = mDisplay->buffer_map.find(handle);
    if (it != mDisplay->buffer_map.end()) {
        destroy_buffer(it->second);
        mDisplay->buffer_map.erase(it);
    }
}

static bool isCursorLayer(const std::string& layerName) {
    return layerName.length() >= 12 && layerName.substr(0, 12) == "[BBQ] Sprite";
}

bool WaylandBackend::updateCursorSurface(Hwc2Layer& layer) {
    if (!mDisplay->cursor_surface) {
        return false;
    }

    const std::string& layerName = layer.getName();
    if (!isCursorLayer(layerName)) {
        return false;
    }

    /*
     * To update the wayland cursor, the fde.mouse_icon_addr system property was introduced.
     * When the internal mouse shape changes, its value will change accordingly.
     * Its value is set by SpriteController and PointerController.
     */
    int64_t mouse_icon_addr = property_get_int64("fde.mouse_icon_addr", 0);
    if (mDisplay->mouse_icon_addr != mouse_icon_addr) {
        mDisplay->mouse_icon_addr = mouse_icon_addr;
        mDisplay->additional_refresh_cursor_times = 0;
        eraseCursorLayerBuffer(layer.buffer());
    } else {
        if (mDisplay->additional_refresh_cursor_times > 60) {
            return true;
        }
        eraseCursorLayerBuffer(layer.buffer());
    }

    struct buffer* buf = getWlBuffer(layer.buffer(), layer.getHandleExt(),
                                     layer.displayFrame());
    if (!buf) {
        ALOGE("Failed to get wayland buffer");
        return true;
    }

    wl_surface_attach(mDisplay->cursor_surface, buf->buffer, 0, 0);
    if (wl_surface_get_version(mDisplay->cursor_surface) >= WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION)
        wl_surface_damage_buffer(mDisplay->cursor_surface, 0, 0, buf->width, buf->height);
    else
        wl_surface_damage(mDisplay->cursor_surface, 0, 0, buf->width, buf->height);
    //locally_calculated_scale add for mutter env
    if ((!mDisplay->viewporter && mDisplay->scale > 1) ||
        mDisplay->scale > mDisplay->locally_calculated_scale) {
        // With no viewporter the scale is guaranteed to be integer
        wl_surface_set_buffer_scale(mDisplay->cursor_surface, (int)mDisplay->scale);
    } else if (mDisplay->viewporter && mDisplay->scale != 1) {
        setupViewportDestination(mDisplay->cursor_viewport, layer.displayFrame(), mDisplay);
    }

    wl_surface_commit(mDisplay->cursor_surface);
    int32_t icon_hotspot_x = property_get_int32("fde.mouse_icon_hotspot_x", 5);
    int32_t icon_hotspot_y = property_get_int32("fde.mouse_icon_hotspot_y", 5);
    if (mDisplay->pointer) {
        wl_pointer_set_cursor(mDisplay->pointer, mDisplay->serial,
                              mDisplay->cursor_surface, icon_hotspot_x, icon_hotspot_y);
    }
    mDisplay->additional_refresh_cursor_times++;

    return true;
}

Error WaylandBackend::validate(Hwc2Display& display) {
    // 仅主屏驱动 geo_changed:该标志只服务于主屏的窗口/子表面重构与
    // buffer_map 全量重建。主/外两屏共享同一个 struct display,若让外部屏的
    // 图层状态变化也置位它,主屏下一帧的 present() 会把整个 buffer_map 连同
    // 外部屏窗口正附着使用的 client target buffer 一起销毁(in-use destroy),
    // 外部屏每帧丢附着 buffer → 应用窗口闪烁。典型触发:重开外部屏后 Android
    // 指针迁移到 display 1,Sprite 光标图层落在外部屏并随鼠标每帧
    // setDisplayFrame → display 1 每帧 markGeometryChanged。首次打开时指针在
    // 主屏、Sprite 在 display 0,所以只有重开后才闪烁。
    // (x11 后端每个 display 有独立的 disp->geo_changed,无此交叉污染;这是
    // wayland 共享 struct display 移植时引入的缺陷。)
    // presentExternal 在 present() 里提前 return,不会复位此标志,外部屏的
    // 置位会泄漏到主屏下一帧,因此这里必须按 display 区分。
    if (display.isPrimary() && display.geometryChanged() && mUseSubsurface) {
        mDisplay->geo_changed = true;
    }

    mDisplay->validate_seq++;
    for (auto* layer : display.getLayersByZ()) {
        if (isCursorLayer(layer->getName())) {
            // The cursor layer never goes through SurfaceFlinger GPU
            // compositing; it is presented on the wayland cursor surface.
            // 记录到全局仲裁状态:任一 display 看到光标图层都会阻止隐藏。
            mDisplay->cursor_found_round = mDisplay->validate_seq;
            display.requestCompositionChange(*layer, Composition::Device);
            updateCursorSurface(*layer);
            continue;
        }

        Composition desired;
        if (display.isPrimary()) {
            desired = mUseSubsurface ? Composition::Device : Composition::Client;
            if (display.colorTransformForced()) {
                desired = Composition::Client;
            }
        } else {
            // 外部屏只有"整帧 client target"一条呈现路径(presentExternal 直接
            // 贴 SF 的 GPU 合成结果),Device 图层无法逐层呈现。若 SF 接受了
            // Device 请求,这些图层既不在 client target 里、也没有呈现路径
            // (预存的内容缺失);更糟的是图层集合变化(如系统开关"指针位置"
            // 会给每块屏加 overlay)会让 SF 的 Device/Client 接受策略帧间摇摆
            // ——某帧全部 Device(无 client target)时 presentExternal 退化成
            // 只贴第一个 Device 图层,与整帧交替 → 应用窗口闪烁。恒请求
            // Client 强制 SF 每帧 GPU 合成出 client target,机制上消灭摇摆。
            desired = Composition::Client;
        }
        display.requestCompositionChange(*layer, desired);
    }

    // 隐藏决策用全局仲裁状态而非本 display 的当帧图层:wl_pointer 光标被
    // 所有 display 的 validate() 共享。外接屏打开后,光标图层只存在于其中
    // 一条 display 的图层列表里,另一条每帧都走"没找到"分支——旧逻辑会
    // 每帧 hide,与持图层那条 display 的 show 互相打架,造成光标闪烁并
    // 反复刷 "wayland cursor hidden" 日志。x11 后端没有此问题是因为每个
    // display 有独立的光标;这里对齐其"全局状态决定显隐"的语义:
    // 连续 ≥2 次 validate(即主/副两条 display 都没看到光标图层)才隐藏。
    if (mDisplay->mouse_icon_addr != -1 &&
        mDisplay->validate_seq - mDisplay->cursor_found_round >= 2) {
        if (mDisplay->pointer) {
            wl_pointer_set_cursor(mDisplay->pointer, mDisplay->serial, NULL, 0, 0);
        }
        mDisplay->mouse_icon_addr = -1;
        ALOGI("wayland cursor hidden");
    }

    return Error::None;
}

static void update_shm_buffer(struct display* display, struct buffer* buffer) {
    // Slower but always correct
    if (display->gtype != GRALLOC_DEFAULT) {
        display->egl_work_queue.push_back(std::bind(egl_render_to_pixels, display, buffer));
        sem_post(&display->egl_go);
        sem_wait(&display->egl_done);
        return;
    }

    // Fast path for when the buffer is guaranteed to be linear and 4bpp
    void* data;
    int shm_stride, src_stride;
    android::Rect bounds(buffer->width, buffer->height);
    if (android::GraphicBufferMapper::get().lock(buffer->handle, GRALLOC_USAGE_SW_READ_OFTEN,
                                                 bounds, &data) == 0) {
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

struct buffer* WaylandBackend::getWlBuffer(buffer_handle_t handle, const handle_ext& ext,
                                           hwc_rect_t fallbackFrame) {
    uint32_t format = ext.format;
    uint32_t pixel_stride = ext.stride;
    uint32_t width = ext.width;
    uint32_t height = ext.height;

    if (!width)
        width = fallbackFrame.right - fallbackFrame.left;
    if (!height)
        height = fallbackFrame.bottom - fallbackFrame.top;

    auto it = mDisplay->buffer_map.find(handle);
    if (it != mDisplay->buffer_map.end()) {
        if (it->second->isShm) {
            if (width != (uint32_t)it->second->width || height != (uint32_t)it->second->height) {
                destroy_buffer(it->second);
                mDisplay->buffer_map.erase(it);
            } else {
                update_shm_buffer(mDisplay, it->second);
                return it->second;
            }
        } else {
            return it->second;
        }
    }

    struct buffer* buf;
    int ret = 0;

    buf = new struct buffer();
    if (mDisplay->gtype == GRALLOC_GBM) {
        struct gralloc_handle_t* drm_handle = (struct gralloc_handle_t*)handle;
        if (mDisplay->dmabuf) {
            ret = create_dmabuf_wl_buffer(mDisplay, buf, drm_handle->width, drm_handle->height, drm_handle->format, -1 /* compute drm format */, drm_handle->prime_fd, pixel_stride, drm_handle->stride, 0 /* offset */, drm_handle->modifier, handle);
        } else {
            ret = create_shm_wl_buffer(mDisplay, buf, drm_handle->width, drm_handle->height, drm_handle->format, pixel_stride, handle);
            update_shm_buffer(mDisplay, buf);
        }
    } else if (mDisplay->gtype == GRALLOC_CROS) {
        const struct cros_gralloc_handle* cros_handle = (const struct cros_gralloc_handle*)handle;
        if (mDisplay->dmabuf) {
            ret = create_dmabuf_wl_buffer(mDisplay, buf, cros_handle->width, cros_handle->height, cros_handle->droid_format, cros_handle->format, cros_handle->fds[0], pixel_stride, cros_handle->strides[0], cros_handle->offsets[0], cros_handle->format_modifier, handle);
        } else {
            ret = create_shm_wl_buffer(mDisplay, buf, cros_handle->width, cros_handle->height, cros_handle->droid_format, pixel_stride, handle);
            update_shm_buffer(mDisplay, buf);
        }
    } else if (mDisplay->gtype == GRALLOC_X100) {
        const X100_native_handle_t* cros_handle = (const X100_native_handle_t*)handle;
        if (mDisplay->dmabuf) {
            //stride = pixel_stride * 4 is based on aligned memory by page(32bit)
            ret = create_dmabuf_wl_buffer(mDisplay, buf, cros_handle->iWidth, cros_handle->iHeight, cros_handle->iFormat, -1, cros_handle->fd[0], pixel_stride, pixel_stride *4, 0,DRM_FORMAT_MOD_INVALID, handle);
            if (ret != 0) {
                ALOGE("x100 create dmabuf wl buffer failed");
            }
        } else {
            ret = create_shm_wl_buffer(mDisplay, buf, cros_handle->iWidth, cros_handle->iHeight, cros_handle->iFormat, pixel_stride, handle);
            if (ret != 0) {
                ALOGE("x100 create shm wl buffer failed");
            }
            update_shm_buffer(mDisplay, buf);
        }
    } else if (mDisplay->gtype == GRALLOC_FTG340) {
        const gc_private_handle_t* gc_handle = (const gc_private_handle_t*)handle;
        if (mDisplay->dmabuf) {
            ret = create_dmabuf_wl_buffer(mDisplay, buf, gc_handle->width, gc_handle->height, gc_handle->format,
                -1, gc_handle->prime_fd, pixel_stride, gc_handle->stride, 0, DRM_FORMAT_MOD_INVALID, handle);
        } else {
            ret = create_shm_wl_buffer(mDisplay, buf, gc_handle->width, gc_handle->height, gc_handle->format, pixel_stride, handle);
            update_shm_buffer(mDisplay, buf);
        }
    } else {
        if (mDisplay->gtype == GRALLOC_ANDROID) {
            ret = create_android_wl_buffer(mDisplay, buf, width, height, format, pixel_stride, handle);
        } else {
            ret = create_shm_wl_buffer(mDisplay, buf, width, height, format, pixel_stride, handle);
            update_shm_buffer(mDisplay, buf);
        }
    }

    if (ret) {
        ALOGE("failed to create a wayland buffer");
        delete buf;
        return NULL;
    }
    mDisplay->buffer_map[handle] = buf;

    return buf;
}

struct wl_surface* WaylandBackend::getSurface(hwc_rect_t frame, hwc_frect_t crop,
                                              Transform transform, struct window* window,
                                              bool multi) {
    mDisplay->windows[window->surface] = window;
    if (!multi) {
        mDisplay->layers[window->surface] = {
            .x = frame.left,
            .y = frame.top };
        if ((!multi && mDisplay->scale != 1 && mDisplay->viewporter && !window->viewport) ||
            mDisplay->preferred_scale) {
            if (mDisplay->preferred_scale) {
                mDisplay->preferred_scale = false;
            }
            if (!window->viewport) {
                window->viewport = wp_viewporter_get_viewport(mDisplay->viewporter,
                                                              window->surface);
            }
            setupViewportDestination(window->viewport, frame, mDisplay);

            mDisplay->width = (int)(mDisplay->full_width / mDisplay->scale);
            mDisplay->height = (int)(mDisplay->full_height / mDisplay->scale);

            if (window->bg_viewport) {
                wl_surface_attach(window->bg_surface, window->bg_buffer, 0, 0);
                wl_surface_damage_buffer(window->bg_surface, 0, 0, 1, 1);
                wp_viewport_set_destination(window->bg_viewport, mDisplay->width,
                                            mDisplay->height);
                wl_surface_commit(window->bg_surface);
            }
            find_primary(mDisplay);
            if (mDisplay->num_outputs > 0 && mDisplay->primary) {
                xdg_toplevel_set_fullscreen(window->xdg_toplevel,
                                            mDisplay->primary->wl_output);
            } else {
                xdg_toplevel_set_fullscreen(window->xdg_toplevel, NULL);
            }
        }
        return window->surface;
    }

    struct wl_surface* surface = NULL;
    struct wl_subsurface* subsurface = NULL;
    struct wp_viewport* viewport = NULL;

    if (window->surfaces.find(window->lastLayer) == window->surfaces.end()) {
        surface = wl_compositor_create_surface(mDisplay->compositor);
        subsurface = wl_subcompositor_get_subsurface(mDisplay->subcompositor,
                                                     surface,
                                                     window->surface);
        if (mDisplay->viewporter)
            viewport = wp_viewporter_get_viewport(mDisplay->viewporter, surface);
        window->surfaces[window->lastLayer] = surface;
        window->subsurfaces[window->lastLayer] = subsurface;
        window->viewports[window->lastLayer] = viewport;
    }

    hwc_rect_t sourceCrop = {
        .left = (int)lroundf(crop.left),
        .top = (int)lroundf(crop.top),
        .right = (int)lroundf(crop.right),
        .bottom = (int)lroundf(crop.bottom),
    };

    if ((static_cast<int>(transform) & HWC_TRANSFORM_ROT_90)) {
        sourceCrop.left = (int)lroundf(crop.top);
        sourceCrop.top = (int)lroundf(crop.left);
        sourceCrop.right = (int)lroundf(crop.bottom);
        sourceCrop.bottom = (int)lroundf(crop.right);
    }

    if (mDisplay->viewporter) {
        wp_viewport_set_source(window->viewports[window->lastLayer],
                               wl_fixed_from_double(fmax(0, mDisplay->viewporter ? sourceCrop.left : sourceCrop.left / mDisplay->scale)),
                               wl_fixed_from_double(fmax(0, mDisplay->viewporter ? sourceCrop.top : sourceCrop.top / mDisplay->scale)),
                               wl_fixed_from_double(fmax(1, mDisplay->viewporter ? (sourceCrop.right - sourceCrop.left) :
                                                                                        (sourceCrop.right - sourceCrop.left) / mDisplay->scale)),
                               wl_fixed_from_double(fmax(1, mDisplay->viewporter ? (sourceCrop.bottom - sourceCrop.top) :
                                                                                        (sourceCrop.bottom - sourceCrop.top) / mDisplay->scale)));

        setupViewportDestination(window->viewports[window->lastLayer], frame, mDisplay);
    }

    wl_subsurface_set_position(window->subsurfaces[window->lastLayer],
                               floor(frame.left / mDisplay->scale),
                               floor(frame.top / mDisplay->scale));

    mDisplay->layers[window->surfaces[window->lastLayer]] = {
        .x = frame.left,
        .y = frame.top };
    return window->surfaces[window->lastLayer];
}

static void feedback_sync_output(void*, struct wp_presentation_feedback*,
                                 struct wl_output*) {
}

static void feedback_presented(void* data,
           struct wp_presentation_feedback* feedback,
           uint32_t tv_sec_hi,
           uint32_t tv_sec_lo,
           uint32_t tv_nsec,
           uint32_t,
           uint32_t,
           uint32_t,
           uint32_t) {
    WaylandBackend* backend = static_cast<WaylandBackend*>(data);
    wp_presentation_feedback_destroy(feedback);

    if (backend->getDevice()) {
        backend->getDevice()->setVsyncTimestamp(
                (((uint64_t)tv_sec_hi << 32) + tv_sec_lo) * 1e9 + tv_nsec);
    }
}

static void feedback_discarded(void*, struct wp_presentation_feedback* feedback) {
    wp_presentation_feedback_destroy(feedback);
}

static const struct wp_presentation_feedback_listener feedback_listener = {
    feedback_sync_output,
    feedback_presented,
    feedback_discarded
};

Error WaylandBackend::present(Hwc2Display& display) {
    // The external display is presented on the secondary wl_output window.
    if (!display.isPrimary()) {
        return presentExternal(display);
    }
    char property[PROPERTY_VALUE_MAX];

    if (mDisplay->geo_changed) {
        // 诊断:此日志连续每帧出现说明仍有路径在反复置位 geo_changed,
        // 全量重建会连带销毁外部屏窗口正附着的 buffer(闪烁)。
        ALOGD("geo_changed: rebuilding %zu cached wayland buffers",
              mDisplay->buffer_map.size());
        for (auto it = mDisplay->buffer_map.begin(); it != mDisplay->buffer_map.end(); it++) {
            if (it->second) {
                destroy_buffer(it->second);
            }
        }
        mDisplay->buffer_map.clear();
    }

    auto layers = display.getLayersByZ();

    /*
     * Client-composited layers must be presented through the client target in
     * order to keep a correct z-ordering; the client target replaces the
     * range of client-composited layers, same as the HWC1 implementation did
     * with the HWC_SKIP_LAYER range and the framebuffer target layer.
     */
    std::pair<int, int> clientRange(-1, -1);
    if (mUseSubsurface && !mMultiWindows) {
        for (size_t i = 0; i < layers.size(); i++) {
            if (layers[i]->compositionType() != Composition::Client)
                continue;

            if (clientRange.first == -1)
                clientRange.first = i;
            clientRange.second = i;
        }
    }

    /*
     * In prop "persist.openfde.multi_windows" we detect HWC let SF rander layers
     * And just show the target client layer (single windows mode) or
     * render each layers in wayland surface and subsurfaces.
     * In prop "openfde.active_apps" we choose what to be shown in window
     * and here if HWC is in single mode we show the screen only if any task are in screen
     * and in multi windows mode we group layers with same task ID in a wayland window.
     * And in prop "openfde.blacklist_apps" we select apps to not show in display.
     *
     * "openfde.active_apps" prop can be:
     * "none": No windows
     * "Openfde": Shows android screen in a single window
     * "AppID": Shows apps in related windows as explained above
     */
    property_get("openfde.active_apps", property, "none");
    std::string active_apps = std::string(property);
    property_get("openfde.blacklist_apps", property, "com.android.launcher3");
    std::string blacklist_apps = std::string(property);
    std::string single_layer_tid;
    std::string single_layer_aid;

    if (active_apps != "Openfde" && !property_get_bool("openfde.background_start", true)) {
        for (auto* layer : layers) {
            if (layer->getName().rfind("BootAnimation#", 0) == 0) {
                // force single window mode during boot animation
                active_apps = "Openfde";
                break;
            }
        }
    }

    std::scoped_lock lock(mDisplay->windowsMutex);
    if (active_apps == "none") {
        // Clear all open windows
        for (auto it = mWindows.begin(); it != mWindows.end(); it++) {
            if (it->second)
                destroy_window(it->second);
        }
        mWindows.clear();

        property_set("openfde.open_windows", "0");
        return Error::None;
    } else if (active_apps == "Openfde") {
        // Clear all open windows if there's any and just keep "Openfde"
        if (mWindows.find(active_apps) == mWindows.end() || !mWindows[active_apps]->isActive) {
            for (auto it = mWindows.begin(); it != mWindows.end(); it++) {
                if (it->second) {
                    destroy_window(it->second);
                }
            }
            mWindows.clear();
        } else {
            mWindows[active_apps]->lastLayer = 0;
            mWindows[active_apps]->last_layer_buffer = nullptr;
        }
    } else if (!mMultiWindows) {
        // Single window mode, detecting if any unblacklisted app is on screen
        bool showWindow = false;
        for (auto* layer : layers) {
            const std::string& layer_name = layer->getName();
            if (layer_name.substr(0, 4) == "TID:") {
                std::string layer_tid = layer_name.substr(4, layer_name.find('#') - 4);
                std::string layer_aid = layer_name.substr(layer_name.find('#') + 1, layer_name.find('/') - layer_name.find('#') - 1);

                std::istringstream iss(blacklist_apps);
                std::string app;
                while (std::getline(iss, app, ':')) {
                    if (app == layer_aid) {
                        showWindow = false;
                        break;
                    } else {
                        showWindow = true;
                        if (!single_layer_tid.length()) {
                            single_layer_tid = layer_tid;
                            single_layer_aid = layer_aid;
                        }
                        if (mWindows.find(single_layer_tid) != mWindows.end()) {
                            mWindows[single_layer_tid]->lastLayer = 0;
                            mWindows[single_layer_tid]->last_layer_buffer = nullptr;
                        }
                    }
                }
            }
        }
        // Nothing to show on screen, so clear all open windows
        if (!showWindow) {
            for (auto it = mWindows.begin(); it != mWindows.end(); it++) {
                if (it->second)
                    destroy_window(it->second);
            }
            mWindows.clear();

            property_set("openfde.open_windows", "0");
            return Error::None;
        }
        bool shouldCloseLeftover = true;
        for (auto it = mWindows.cbegin(); it != mWindows.cend();) {
            if (it->second) {
                // This window is closed, but android is still showing leftover layers, we detect it here
                if (!it->second->isActive || it->first == "Openfde") {
                    for (auto* layer : layers) {
                        const std::string& layer_name = layer->getName();
                        if (layer_name.substr(0, 4) == "TID:") {
                            std::string layer_tid = layer_name.substr(4, layer_name.find('#') - 4);
                            if (layer_tid == it->first) {
                                shouldCloseLeftover = false;
                                break;
                            }
                        }
                    }
                    if (shouldCloseLeftover) {
                        destroy_window(it->second);
                        mWindows.erase(it++);
                        shouldCloseLeftover = true;
                        std::string windows_size_str = std::to_string(mWindows.size());
                        property_set("openfde.open_windows", windows_size_str.c_str());
                    } else
                        ++it;
                } else
                    ++it;
            } else
                ++it;
        }
    } else {
        // Multi window mode
        // Checking current open windows to detect and kill obsolete ones
        for (auto it = mWindows.cbegin(); it != mWindows.cend();) {
            bool foundApp = false;
            for (auto* layer : layers) {
                if (layer->compositionType() != Composition::Device)
                    continue;
                const std::string& layer_name = layer->getName();
                if (layer_name.substr(0, 4) == "TID:") {
                    std::string layer_tid = layer_name.substr(4, layer_name.find('#') - 4);
                    if (layer_tid == it->first) {
                        it->second->lastLayer = 0;
                        it->second->last_layer_buffer = nullptr;
                        foundApp = true;
                        break;
                    }
                } else {
                    std::string LayerRawName;
                    std::istringstream issLayer(layer_name);
                    std::getline(issLayer, LayerRawName, '#');
                    if (LayerRawName == it->first) {
                        it->second->lastLayer = 0;
                        it->second->last_layer_buffer = nullptr;
                        foundApp = true;
                        break;
                    }
                }
            }
            // This window ID doesn't match with any selected app IDs from prop, so kill it
            if (!foundApp || (it->second && !it->second->isActive)) {
                if (it->second)
                    destroy_window(it->second);
                mWindows.erase(it++);
                std::string windows_size_str = std::to_string(mWindows.size());
                property_set("openfde.open_windows", windows_size_str.c_str());
            } else {
                ++it;
            }
        }
    }

    bool hasClientTarget = display.clientTarget() != nullptr;

    // Presents one buffer, either a device-composited hwc2 layer or the
    // client target (which replaces the former HWC1 framebuffer target layer)
    auto presentOne = [&](buffer_handle_t handle, const handle_ext& ext, hwc_rect_t frame,
                          hwc_frect_t crop, Transform transform, const std::string& layer_name,
                          int acquireFence) {
        struct window* window = NULL;

        if (active_apps == "Openfde") {
            // Show everything in a single window
            if (mWindows.find(active_apps) == mWindows.end()) {
                mWindows[active_apps] = create_window(mDisplay, mUseSubsurface, active_apps, "0", {0, 0, 0, 255});
                std::string windows_size_str = std::to_string(mWindows.size());
                property_set("openfde.open_windows", windows_size_str.c_str());
            }
            window = mWindows[active_apps];
        } else if (!mMultiWindows) {
            if (single_layer_tid.length()) {
                if (mWindows.find(single_layer_tid) == mWindows.end()) {
                    mWindows[single_layer_tid] = create_window(mDisplay, mUseSubsurface, single_layer_aid, single_layer_tid, {0, 0, 0, 255});
                    std::string windows_size_str = std::to_string(mWindows.size());
                    property_set("openfde.open_windows", windows_size_str.c_str());
                }
                window = mWindows[single_layer_tid];
            }
        } else {
            // Create windows based on Task ID in layer name
            if (layer_name.substr(0, 4) == "TID:") {
                std::string layer_tid = layer_name.substr(4, layer_name.find('#') - 4);
                std::string layer_aid = layer_name.substr(layer_name.find('#') + 1, layer_name.find('/') - layer_name.find('#') - 1);

                bool showWindow = false;
                std::istringstream iss(blacklist_apps);
                std::string app;
                while (std::getline(iss, app, ':')) {
                    if (app == layer_aid) {
                        showWindow = false;
                        break;
                    } else
                        showWindow = true;
                }

                if (showWindow) {
                    if (mWindows.find(layer_tid) == mWindows.end()) {
                        mWindows[layer_tid] = create_window(mDisplay, mUseSubsurface, layer_aid, layer_tid, {0, 0, 0, 0});
                        std::string windows_size_str = std::to_string(mWindows.size());
                        property_set("openfde.open_windows", windows_size_str.c_str());
                    }
                    if (mWindows.find(layer_tid) != mWindows.end())
                        window = mWindows[layer_tid];
                }
            }
        }

        // Detecting cursor layer
        if (!window) {
            std::string LayerRawName;
            std::istringstream issLayer(layer_name);
            std::getline(issLayer, LayerRawName, '#');
            if (LayerRawName == "Sprite" && mDisplay->pointer_surface) {
                if (mDisplay->cursor_surface) {
                    struct buffer* buf = getWlBuffer(handle, ext, frame);
                    if (!buf) {
                        ALOGE("Failed to get wayland buffer");
                        if (acquireFence != -1) {
                            close(acquireFence);
                        }
                        return;
                    }

                    wl_surface_attach(mDisplay->cursor_surface, buf->buffer, 0, 0);
                    if (wl_surface_get_version(mDisplay->cursor_surface) >= WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION)
                        wl_surface_damage_buffer(mDisplay->cursor_surface, 0, 0, buf->width, buf->height);
                    else
                        wl_surface_damage(mDisplay->cursor_surface, 0, 0, buf->width, buf->height);
                    if (!mDisplay->viewporter && mDisplay->scale > 1) {
                        // With no viewporter the scale is guaranteed to be integer
                        wl_surface_set_buffer_scale(mDisplay->cursor_surface, (int)mDisplay->scale);
                    } else if (mDisplay->viewporter && mDisplay->scale != 1) {
                        setupViewportDestination(mDisplay->cursor_viewport, frame, mDisplay);
                    }

                    wl_surface_commit(mDisplay->cursor_surface);
                    if (acquireFence != -1) {
                        close(acquireFence);
                    }
                    return;
                } else {
                    for (auto it = mWindows.begin(); it != mWindows.end(); it++) {
                        if (it->second) {
                            if (it->second->surface == mDisplay->pointer_surface) {
                                window = it->second;
                                break;
                            }
                            for (auto itt = it->second->surfaces.begin(); itt != it->second->surfaces.end(); itt++) {
                                if (itt->second == mDisplay->pointer_surface) {
                                    window = it->second;
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            if (LayerRawName == "InputMethod") {
                if (mWindows.find(LayerRawName) == mWindows.end()) {
                    mWindows[LayerRawName] = create_window(mDisplay, mUseSubsurface, LayerRawName, "none", {0, 0, 0, 0});
                    std::string windows_size_str = std::to_string(mWindows.size());
                    property_set("openfde.open_windows", windows_size_str.c_str());
                }
                if (mWindows.find(LayerRawName) != mWindows.end())
                    window = mWindows[LayerRawName];
            }
        }

        if (!window || !window->isActive) {
            if (acquireFence != -1) {
                close(acquireFence);
            }
            return;
        }

        struct buffer* buf = getWlBuffer(handle, ext, frame);
        if (!buf) {
            ALOGE("Failed to get wayland buffer");
            if (acquireFence != -1) {
                close(acquireFence);
            }
            return;
        }

        // The client target replaces the client-composited layer range
        struct wl_surface* surface = getSurface(frame, crop, transform, window,
                                                mUseSubsurface);
        if (!surface) {
            ALOGE("Failed to get surface");
            if (acquireFence != -1) {
                close(acquireFence);
            }
            return;
        }
        window->last_layer_buffer = buf;
        window->lastLayer++;

        wl_surface_attach(surface, buf->buffer, 0, 0);
        if (wl_surface_get_version(surface) >= WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION)
            wl_surface_damage_buffer(surface, 0, 0, buf->width, buf->height);
        else
            wl_surface_damage(surface, 0, 0, buf->width, buf->height);
        if (!mDisplay->viewporter && mDisplay->scale > 1) {
            // With no viewporter the scale is guaranteed to be integer
            wl_surface_set_buffer_scale(surface, (int)mDisplay->scale);
        }
        switch (transform) {
            case Transform::FlipH:
                wl_surface_set_buffer_transform(surface, WL_OUTPUT_TRANSFORM_FLIPPED_180);
                break;
            case Transform::FlipV:
                wl_surface_set_buffer_transform(surface, WL_OUTPUT_TRANSFORM_FLIPPED);
                break;
            case Transform::Rotate90:
                wl_surface_set_buffer_transform(surface, WL_OUTPUT_TRANSFORM_90);
                break;
            case Transform::Rotate180:
                wl_surface_set_buffer_transform(surface, WL_OUTPUT_TRANSFORM_180);
                break;
            case Transform::Rotate270:
                wl_surface_set_buffer_transform(surface, WL_OUTPUT_TRANSFORM_270);
                break;
            case Transform::FlipHRotate90:
                wl_surface_set_buffer_transform(surface, WL_OUTPUT_TRANSFORM_FLIPPED_270);
                break;
            case Transform::FlipVRotate90:
                wl_surface_set_buffer_transform(surface, WL_OUTPUT_TRANSFORM_FLIPPED_90);
                break;
            default:
                wl_surface_set_buffer_transform(surface, WL_OUTPUT_TRANSFORM_NORMAL);
                break;
        }

        struct wp_presentation* pres = window->display->presentation;
        if (pres) {
            buf->feedback = wp_presentation_feedback(pres, surface);
            wp_presentation_feedback_add_listener(buf->feedback,
                              &feedback_listener, this);
        }

        wl_surface_commit(surface);

        if (window->snapshot_buffer) {
            // Snapshot buffer should be detached by now, clean up
            destroy_buffer(window->snapshot_buffer);
            window->snapshot_buffer = nullptr;
        }

        const int kAcquireWarningMS = 100;
        int fenceErr = sync_wait(acquireFence, kAcquireWarningMS);
        if (fenceErr < 0 && errno == ETIME) {
            ALOGE("hwcomposer waited on fence %d for %d ms (%s)",
                acquireFence, kAcquireWarningMS,
                layer_name.empty() ? "<client target>" : layer_name.c_str());
        }
        if (acquireFence != -1) {
            close(acquireFence);
        }
    };

    for (size_t l = 0; l < layers.size(); l++) {
        Hwc2Layer* hwcLayer = layers[l];
        if ((int)l == clientRange.first && hasClientTarget) {
            // draw the client target instead of the client-composited layers
            int fence = hwcLayer->takeAcquireFence();
            if (fence != -1) {
                close(fence);
            }
            presentOne(display.clientTarget(), display.clientTargetExt(),
                       {0, 0, mDisplay->full_width, mDisplay->full_height},
                       {0.0f, 0.0f, (float)mDisplay->full_width, (float)mDisplay->full_height},
                       Transform::None, "", display.takeClientTargetAcquireFence());
            continue;
        }
        if (clientRange.first >= 0 && hwcLayer->compositionType() == Composition::Client) {
            // client-composited layers are already in the client target
            continue;
        }
        if (hwcLayer->compositionType() != Composition::Device || !hwcLayer->buffer()) {
            continue;
        }
        presentOne(hwcLayer->buffer(), hwcLayer->getHandleExt(), hwcLayer->displayFrame(),
                   hwcLayer->sourceCrop(), hwcLayer->transform(), hwcLayer->getName(),
                   hwcLayer->takeAcquireFence());
    }

    if (!mUseSubsurface && hasClientTarget) {
        // In non-subsurface mode only the client target is shown, as the
        // topmost layer, same as the former HWC1 framebuffer target
        presentOne(display.clientTarget(), display.clientTargetExt(),
                   {0, 0, mDisplay->full_width, mDisplay->full_height},
                   {0.0f, 0.0f, (float)mDisplay->full_width, (float)mDisplay->full_height},
                   Transform::None, "", display.takeClientTargetAcquireFence());
    }

    // Layers order is changed from SF so we rearrange wayland surfaces
    if (mDisplay->geo_changed) {
        for (auto it = mWindows.begin(); it != mWindows.end(); it++) {
            if (it->second) {
                // This window has no changes in layers, leaving it
                if (!it->second->lastLayer)
                    continue;
                // Neutralize unused surfaces
                for (size_t l = it->second->lastLayer; l < it->second->surfaces.size(); l++) {
                    if (it->second->surfaces.find(l) != it->second->surfaces.end()) {
                        wl_surface_attach(it->second->surfaces[l], NULL, 0, 0);
                        wl_surface_commit(it->second->surfaces[l]);
                    }
                }
            }
        }
        mDisplay->geo_changed = false;
    }

    if (!mMultiWindows && single_layer_tid.length() && active_apps != "Openfde") {
        for (auto const& [layer_tid, window] : mWindows) {
            // Replace inactive app window buffer with snapshot in staged mode
            if (layer_tid != single_layer_tid && !window->snapshot_buffer) {
                mDisplay->egl_work_queue.push_back(std::bind(snapshot_inactive_app_window, mDisplay, window));
            }
        }
        if (!mDisplay->egl_work_queue.empty()) {
            sem_post(&mDisplay->egl_go);
            sem_wait(&mDisplay->egl_done);
        }
    }

    if (mUseSubsurface)
        for (auto it = mWindows.begin(); it != mWindows.end(); it++)
            if (it->second)
                wl_surface_commit(it->second->surface);
    wl_display_flush(mDisplay->display);

    return Error::None;
}

Error WaylandBackend::presentExternal(Hwc2Display& display) {
    // The external display is shown as a single full-screen surface on the
    // secondary wl_output (mSecondaryWindow). We mirror the primary "Openfde"
    // single-window path but never touch mWindows / the primary output.
    if (!mSecondaryWindow) {
        return Error::None;
    }

    auto layers = display.getLayersByZ();
    bool hasClientTarget = display.clientTarget() != nullptr;

    int ext_w = mSecondaryOutput ? mSecondaryOutput->pixel_width : mDisplay->full_width;
    int ext_h = mSecondaryOutput ? mSecondaryOutput->pixel_height : mDisplay->full_height;

    buffer_handle_t handle = nullptr;
    const handle_ext* ext = nullptr;
    hwc_rect_t frame = {0, 0, ext_w, ext_h};
    hwc_frect_t crop = {0.0f, 0.0f, (float)ext_w, (float)ext_h};
    int acquireFence = -1;

    if (hasClientTarget) {
        // The client target is the fully composited external-display frame.
        handle = display.clientTarget();
        ext = &display.clientTargetExt();
        acquireFence = display.takeClientTargetAcquireFence();
    } else {
        for (auto* layer : layers) {
            if (layer->compositionType() == Composition::Device && layer->buffer()) {
                handle = layer->buffer();
                ext = &layer->getHandleExt();
                frame = layer->displayFrame();
                crop = layer->sourceCrop();
                acquireFence = layer->takeAcquireFence();
                break;
            }
        }
    }
    if (!handle) {
        if (acquireFence != -1) {
            close(acquireFence);
        }
        return Error::None;
    }

    struct buffer* buf = getWlBuffer(handle, *ext, frame);
    if (!buf) {
        ALOGE("presentExternal: failed to get wayland buffer");
        if (acquireFence != -1) {
            close(acquireFence);
        }
        return Error::None;
    }
    // 记录本次打开期间导入的句柄,供关闭时精准释放(见 destroyExternalDisplay)。
    mExternalBufferHandles.insert(handle);

    // Attach directly to the secondary window's toplevel surface. We intentionally
    // bypass getSurface() (which would re-fullscreen this window on the primary
    // output) and the subsurface machinery to keep the external path isolated.
    struct wl_surface* surface = mSecondaryWindow->surface;
    wl_surface_attach(surface, buf->buffer, 0, 0);
    if (wl_surface_get_version(surface) >= WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION) {
        wl_surface_damage_buffer(surface, 0, 0, buf->width, buf->height);
    } else {
        wl_surface_damage(surface, 0, 0, buf->width, buf->height);
    }
    if (!mDisplay->viewporter && mDisplay->scale > 1) {
        // With no viewporter the scale is guaranteed to be integer.
        wl_surface_set_buffer_scale(surface, (int)mDisplay->scale);
    }
    wl_surface_commit(surface);
    wl_display_flush(mDisplay->display);

    if (acquireFence != -1) {
        close(acquireFence);
    }
    return Error::None;
}

Error WaylandBackend::setActiveConfig(Hwc2Display& display, hwc2_config_t config) {
    if (!display.isPrimary()) {
        // External display has a single fixed config; nothing to resize.
        if (config != 0) {
            ALOGE("unsupported external active config %u", (unsigned)config);
            return Error::BadConfig;
        }
        return Error::None;
    }
    if (config >= (hwc2_config_t)gScreenConfigCount) {
        ALOGE("unsupported active config %u", (unsigned)config);
        return Error::BadConfig;
    }

    if (mDisplay->active_config == config) {
        return Error::None;
    }

    // config 0 is the real resolution, the others are deduplicated presets
    if (screenConfigs[config].Width == 0 || screenConfigs[config].Height == 0) {
        return Error::BadConfig;
    }
    int target_width = screenConfigs[config].Width;
    int target_height = screenConfigs[config].Height;

    mDisplay->full_width = target_width;
    mDisplay->full_height = target_height;
    mDisplay->geo_changed = true;
    mDisplay->width = target_width;
    mDisplay->height = target_height;
    mDisplay->primary_x = 0;
    mDisplay->primary_y = 0;
    {
        std::scoped_lock lock(mDisplay->windowsMutex);
        resizeWindows(target_width, target_height);
    }

    // Publish the new active resolution so the input side (EventHub raw axis
    // ranges, PointerChoreographer edge threshold) tracks the current display
    // size. getAttribute() only updates these properties when the active
    // config's attributes are queried, which is not guaranteed on a switch.
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

    ALOGI("setActiveConfig: config=%u resized windows to %dx%d",
          (unsigned)config, target_width, target_height);

    mDisplay->active_config = config;

    return Error::None;
}

void WaylandBackend::resizeWindows(int target_width, int target_height) {
    // NOTE: called with display->windowsMutex held (see setActiveConfig).
    //
    // Do NOT destroy and recreate the Wayland windows here. The old
    // destroy/recreate approach raced with the wayland dispatch thread on the
    // same connection, and the wl_display_roundtrip() calls inside
    // create_window() made two threads dispatch the default event queue at
    // the same time. Either race kills the connection ("Wayland client was
    // disconnected") when switching resolution.
    //
    // Everything a window needs for the new size can be updated in place:
    // the xdg window geometry and the background viewport destination.
    // Layer subsurfaces re-derive their viewport destination and position
    // from the new displayFrame on the next present() pass.
    for (auto it = mWindows.begin(); it != mWindows.end(); ++it) {
        struct window* window = it->second;
        if (!window || !window->isActive) {
            // Inactive windows already had their Wayland objects destroyed.
            continue;
        }

        if (window->xdg_surface)
            xdg_surface_set_window_geometry(window->xdg_surface, 0, 0,
                                            target_width, target_height);

        // With subsurfaces enabled the background is attached directly to
        // the main surface, otherwise to the dedicated bg subsurface.
        struct wl_surface* bg_surface = window->bg_surface;
        if (!bg_surface)
            bg_surface = window->surface;

        if (window->bg_viewport && window->bg_buffer) {
            wl_surface_attach(bg_surface, window->bg_buffer, 0, 0);
            wl_surface_damage_buffer(bg_surface, 0, 0, 1, 1);
            wp_viewport_set_destination(window->bg_viewport,
                                        target_width, target_height);
        }
        wl_surface_commit(bg_surface);
        if (bg_surface != window->surface)
            // bg subsurface is in synchronized mode and the new window
            // geometry lives on the parent: commit the parent as well.
            wl_surface_commit(window->surface);
    }
    wl_display_flush(mDisplay->display);
}

Error WaylandBackend::getAttribute(Hwc2Display& display, hwc2_config_t config,
                                   Attribute attribute, int32_t* outValue) {
    if (!display.isPrimary()) {
        // External display: report the secondary wl_output's physical geometry
        // instead of the primary's shared full_width/full_height.
        if (config != 0 || !mSecondaryOutput) {
            return Error::BadConfig;
        }
        int width = mSecondaryOutput->pixel_width;
        int height = mSecondaryOutput->pixel_height;
        int density = 160;
        if (mSecondaryOutput->logical_width > 0) {
            double scale = (double)mSecondaryOutput->pixel_width /
                          mSecondaryOutput->logical_width;
            density = (int)(160 * scale + 0.5);
        }
        switch (attribute) {
            case Attribute::VsyncPeriod: {
                int32_t period = 1000 * 1000 * 1000 / 60;
                if (mDisplay->refresh > 1000 && mDisplay->refresh < 1000000)
                    period = 1000 * 1000 * 1000 / (mDisplay->refresh / 1000);
                *outValue = period;
                return Error::None;
            }
            case Attribute::Width:
                *outValue = width;
                return Error::None;
            case Attribute::Height:
                *outValue = height;
                return Error::None;
            case Attribute::DpiX:
            case Attribute::DpiY: {
                char property[PROPERTY_VALUE_MAX];
                if (property_get("ro.sf.lcd_density", property, nullptr) > 0)
                    density = atoi(property);
                *outValue = density * 1000;
                return Error::None;
            }
            default:
                ALOGE("unknown external display attribute %d",
                      static_cast<int32_t>(attribute));
                return Error::BadParameter;
        }
    }

    if (config >= (hwc2_config_t)gScreenConfigCount) {
        return Error::BadConfig;
    }

    char property[PROPERTY_VALUE_MAX];
    int width = mDisplay->full_width;
    int height = mDisplay->full_height;
    int density = 160;
    // config > 0 means a non-default (preset) resolution was selected,
    // override width/height/density from the table
    if (config > 0 && config < (hwc2_config_t)gScreenConfigCount) {
        width = screenConfigs[config].Width;
        height = screenConfigs[config].Height;
        density = screenConfigs[config].Density;
    }

    switch (attribute) {
        case Attribute::VsyncPeriod: {
            int32_t period = 1000 * 1000 * 1000 / 60;
            if (mDisplay->refresh > 1000 && mDisplay->refresh < 1000000)
                period = 1000 * 1000 * 1000 / (mDisplay->refresh / 1000);
            *outValue = period;
            return Error::None;
        }
        case Attribute::Width: {
            if (property_get("persist.openfde.width_padding", property, nullptr) > 0)
                width -= atoi(property);
            // Only publish the built-in property when the active config is
            // queried. SurfaceFlinger enumerates every config to build the
            // resolution list in Settings; publishing unconditionally would
            // let the last enumerated preset (e.g. 1280x720) clobber the
            // real resolution (1920x1080), leaving openfde.display_width
            // inconsistent with wm size.
            if (screen_config_is_active(true, mDisplay->active_config, (int)config)) {
                property_set("openfde.display_width", std::to_string(width).c_str());
            }
            *outValue = width;
            return Error::None;
        }
        case Attribute::Height: {
            if (property_get("persist.openfde.height_padding", property, nullptr) > 0)
                height -= atoi(property);
            // Same as above: only publish for the active config so other
            // enumerated resolutions cannot clobber it.
            if (screen_config_is_active(true, mDisplay->active_config, (int)config)) {
                property_set("openfde.display_height", std::to_string(height).c_str());
            }
            *outValue = height;
            return Error::None;
        }
        case Attribute::DpiX:
        case Attribute::DpiY:
            if (property_get("ro.sf.lcd_density", property, nullptr) > 0)
                density = atoi(property);
            *outValue = density * 1000;
            return Error::None;
        default:
            ALOGE("unknown display attribute %d", static_cast<int32_t>(attribute));
            return Error::BadParameter;
    }
}

Hwc2DumpState WaylandBackend::getDumpState(Hwc2Display& /*display*/) {
    Hwc2DumpState state;

    {
        std::scoped_lock lock(mDisplay->windowsMutex);
        state.windowCount = mWindows.size();
        state.bufferCount = mDisplay->buffer_map.size();
    }

    state.activeConfig = mDisplay->active_config;
    state.width = mDisplay->width;
    state.height = mDisplay->height;
    state.fullWidth = mDisplay->full_width;
    state.fullHeight = mDisplay->full_height;
    state.scale = mDisplay->scale;
    state.refresh = mDisplay->refresh;
    state.useSubsurface = mUseSubsurface;
    state.multiWindows = mMultiWindows;
    state.geoChanged = mDisplay->geo_changed;
    state.inputFdPtr = static_cast<const void*>(&mDisplay->input_fd[0]);
    state.inputFds.assign(mDisplay->input_fd, mDisplay->input_fd + INPUT_TOTAL);
    return state;
}

void* WaylandBackend::waylandThreadEntry(void* data) {
    WaylandBackend* backend = static_cast<WaylandBackend*>(data);
    int ret = 0;

    setpriority(PRIO_PROCESS, 0, HAL_PRIORITY_URGENT_DISPLAY);

    // 单读线程模式:本线程独占 wayland fd 的读取(prepare_read/read_events),
    // 其余线程(create_window 的校准等待、wl_wait_server_sync 的私有队列往返)
    // 只发请求 + flush,绝不派发默认队列。旧实现这里用阻塞 wl_display_dispatch,
    // 而 create_window 里的 wl_display_roundtrip 会在服务线程并发派发同一队列
    // ——libwayland 明文禁止,每次 create/destroy 窗口都在掷骰子,频繁开关
    // 外部屏时表现为偶发闪烁(参见 setActiveConfig 里的历史竞态注释)。
    struct wl_display* wl = backend->mDisplay->display;
    int wl_fd = wl_display_get_fd(wl);
    while (ret != -1) {
        // 派发已读入队列的事件(dispatch_pending 线程安全)
        wl_display_dispatch_pending(wl);
        wl_display_flush(wl);
        if (wl_display_get_error(wl) != 0)
            break;
        if (wl_display_prepare_read(wl) != 0)
            continue;   // 队列里仍有未派发事件,回环处理
        struct pollfd fds = { wl_fd, POLLIN, 0 };
        if (poll(&fds, 1, -1) <= 0) {
            wl_display_cancel_read(wl);
            continue;
        }
        ret = wl_display_read_events(wl);
    }

    ALOGE("*** %s: Wayland client was disconnected: %s", __PRETTY_FUNCTION__, strerror(ret));

    return NULL;
}

void* WaylandBackend::windowServiceThreadEntry(void* data) {
    WaylandBackend* backend = static_cast<WaylandBackend*>(data);
    sp<IOpenfdeWindow> openfdeWindow;
    status_t status;

    openfdeWindow = new OpenfdeWindow(backend->mDisplay);
    if (openfdeWindow == nullptr) {
        ALOGE("Can not create an instance of Openfde Window HAL, exiting.");
        goto shutdown;
    }

    status = openfdeWindow->registerAsService();
    if (status != OK) {
        ALOGE("Could not register service for Openfde Window HAL (%d / 0x%x).",
              status, status);
        goto shutdown;
    }

    ALOGI("Openfde Window HAL thread is ready.");
    joinRpcThreadpool();
    // Should not pass this line

shutdown:
    // In normal operation, we don't expect the thread pool to shutdown
    ALOGE("Openfde Window HAL service is shutting down.");
    return NULL;
}

void WaylandBackend::createInitialWindow() {
    auto first_window = create_window(mDisplay, mUseSubsurface, "Openfde", "0", {0, 0, 0, 255});
    if (!property_get_bool("openfde.background_start", true)) {
        mWindows["Openfde"] = first_window;
        property_set("openfde.active_apps", "Openfde");
        property_set("openfde.open_windows", "1");
    } else {
        destroy_window(first_window);
    }

    // Install the output-hotplug hook so support-state transitions and
    // auto-close work for the external display.
    mDisplay->on_external_display_changed = &WaylandBackend::onExternalDisplayChanged;
    mDisplay->external_display_changed_userdata = this;
}

int WaylandBackend::startThreads() {
    int ret = pthread_create(&mWaylandThread, NULL, waylandThreadEntry, this);
    if (ret) {
        ALOGE("openfde_hw_composer could not start wayland_thread\n");
    }

    ret = pthread_create(&mWindowServiceThread, NULL, windowServiceThreadEntry, this);
    if (ret) {
        ALOGE("openfde_hw_composer could not start window_service_thread\n");
    }

    ret = pthread_create(&mEglWorkerThread, NULL, egl_loop, mDisplay);
    if (ret) {
        ALOGE("openfde_hw_composer could not start egl_worker_thread");
    }

    return ret;
}

bool WaylandBackend::isExternalDisplaySupported() {
    if (!mDisplay) {
        return false;
    }
    // The external display is any non-primary wl_output (a second, extended
    // monitor). Mirrors x11's ">=2 outputs in extended mode" check.
    // 用"配置完成的活输出计数 >= 2"判定,而不是旧版 out != primary 指针比较:
    // 热插拔窗口期 primary 可能短暂为 NULL(尚未在 done 事件中重选),旧判定
    // 会把唯一显示器误判成外接屏(双拔再插回单屏的场景)。语义上与旧判定
    // 等价(primary 本身总是 (0,0) 输出,两块活输出时必有一块非 primary)。
    int live_outputs = 0;
    for (int i = 0; i < mDisplay->num_outputs; i++) {
        struct output* out = &mDisplay->outputs[i];
        if (out->done && !out->removed) {
            live_outputs++;
        }
    }
    return live_outputs >= 2;
}

void WaylandBackend::updateSupported(bool supported) {
    if (supported == mLastSupported) {
        return;
    }
    mLastSupported = supported;
    if (mDevice) {
        mDevice->notifyExternalDisplayEvent(static_cast<int32_t>(
            supported ? Hwc2Device::ExternalDisplayEvent::kExternalDisplaySupported
                      : Hwc2Device::ExternalDisplayEvent::kExternalDisplayUnsupported));
    }
}

void WaylandBackend::createExternalDisplays(Hwc2Device& device) {
    mDevice = &device;
    // Capture the current support state without firing a transition.
    mLastSupported = isExternalDisplaySupported();

    // Pick the first non-primary output as the external display.
    // primary 必须已确立:热插拔窗口期 primary 可能短暂为 NULL(等待 done
    // 事件重选),此时 out != primary 会把主屏输出本身挑成副屏,外部屏窗口
    // 会全屏到主屏 output 上把主屏盖住(双拔再插回单屏的场景)。没有
    // primary 就拒绝创建外部屏。
    mSecondaryOutput = nullptr;
    if (!mDisplay->primary) {
        ALOGE("wayland external display: primary output not established yet, refuse to create");
        return;
    }
    for (int i = 0; i < mDisplay->num_outputs; i++) {
        struct output* out = &mDisplay->outputs[i];
        if (out->done && out != mDisplay->primary) {
            mSecondaryOutput = out;
            break;
        }
    }
    if (!mSecondaryOutput) {
        ALOGE("wayland external display: no secondary output available");
        return;
    }

    // Create a dedicated fullscreen window on the secondary output.
    // map_with_empty_input=true:映射时输入区域为空,窗口短暂落在主屏上时
    // 不会触发 pointer_enter(副屏 surface)——否则 set_fullscreen 挪走窗口
    // 后合成器不发 leave,pointer_surface 永久卡死 → 全局坐标重建恒错 →
    // 鼠标事件全部路由到外部屏。(宿主合成器不支持映射前的 output 定向
    // 全屏,所以消灭这个窗口期只能靠输入区域。)
    mSecondaryWindow = create_window(mDisplay, mUseSubsurface, "Openfde", "0",
                                     {0, 0, 0, 255}, true);
    if (!mSecondaryWindow) {
        ALOGE("wayland external display: failed to create secondary window");
        mSecondaryOutput = nullptr;
        return;
    }
    // Force it onto the secondary output instead of the primary one.
    // (映射后设置——宿主合成器只对已映射窗口执行 output 定向全屏。)
    if (mSecondaryWindow->xdg_toplevel && mSecondaryOutput->wl_output) {
        xdg_toplevel_set_fullscreen(mSecondaryWindow->xdg_toplevel,
                                    mSecondaryOutput->wl_output);
    }
    // 窗口已挪到外接屏,恢复输入区域(映射阶段为空,见 create_window)。
    // 恢复时指针在主屏、窗口在外接屏,不会产生多余的 pointer_enter;
    // 之后指针移到外部屏时正常 enter。
    {
        struct wl_region *full = wl_compositor_create_region(mDisplay->compositor);
        wl_region_add(full, 0, 0, 0x7ffffff0, 0x7ffffff0);
        wl_surface_set_input_region(mSecondaryWindow->surface, full);
        wl_region_destroy(full);
        wl_surface_commit(mSecondaryWindow->surface);
    }
    // 同步窗口所属 output:pointer_handle_motion 依赖它把 surface-local 坐标
    // 还原成全局坐标(等价 x11 的 root_x),跨屏拖动的坐标换算全靠它。
    mSecondaryWindow->output = mSecondaryOutput;
    mSecondaryWindows["Openfde"] = mSecondaryWindow;
    {
        std::scoped_lock lock(mDisplay->windowsMutex);
        mDisplay->windows[mSecondaryWindow->surface] = mSecondaryWindow;
    }

    // 等合成器把放置请求(set_fullscreen、输入区域恢复、commit)全部处理完
    // 再注册 display:sync 往返返回即代表窗口已在目标 output 上落定。快速
    // 开关时合成器可能还在消化上一个窗口的销毁,若不等就触发 hotplug 让
    // SF 开始合成,窗口会停留在"过渡位置?最终位置"的交替渲染里——外部屏
    // 闪烁,间隔越长越少见(合成器收敛了)。超时则照常注册(降级为旧行为)。
    wl_wait_server_sync(mDisplay, 100);

    // Announce the external display to the client. Hwc2Device::openExternalDisplay
    // fires the kExternalDisplayOpened event after this returns.
    device.createExternalDisplay(mSecondaryWindow);

    ALOGI("wayland external display created on output (%dx%d @ %d,%d)",
          mSecondaryOutput->pixel_width, mSecondaryOutput->pixel_height,
          mSecondaryOutput->logical_x, mSecondaryOutput->logical_y);
}

void WaylandBackend::destroyExternalDisplay(void* backendData) {
    // 释放外部屏合成期间导入的全部 wl_buffer。buffer_map 以 gralloc 句柄指针
    // 为 key 且关闭外部屏时从不清理:SF 释放句柄后分配器会复用地址,第二次
    // 打开时新分配的 client target 句柄命中残留条目,getWlBuffer 的 dmabuf
    // 路径无条件返回缓存——外部屏交替渲染孤儿 buffer(冻结/垃圾帧)和新帧,
    // 表现为应用窗口闪烁。同时这也是真实的资源泄漏(每开关一次漏一批导入)。
    // 只清外部屏自己的条目,主屏缓存的 wl_buffer 不动。
    {
        std::scoped_lock lock(mDisplay->windowsMutex);
        for (auto handle : mExternalBufferHandles) {
            auto it = mDisplay->buffer_map.find(handle);
            if (it != mDisplay->buffer_map.end()) {
                if (it->second) {
                    destroy_buffer(it->second);
                }
                mDisplay->buffer_map.erase(it);
            }
        }
        mExternalBufferHandles.clear();
    }
    if (mSecondaryWindow) {
        std::scoped_lock lock(mDisplay->windowsMutex);
        mDisplay->windows.erase(mSecondaryWindow->surface);
        destroy_window(mSecondaryWindow);
        mSecondaryWindow = nullptr;
    }
    mSecondaryWindows.clear();
    mSecondaryOutput = nullptr;
    // Support state is intentionally left unchanged here; it is updated by
    // updateSupported() when the secondary output is added/removed (hotplug).
}

void WaylandBackend::onExternalDisplayChanged(void* userdata) {
    auto* self = static_cast<WaylandBackend*>(userdata);
    // 按当前状态重新挑选合法的副屏输出;没有合法副屏时置空,由下面的
    // closeExternalIfUnsupported 收尾。primary 为 NULL 时(热插拔窗口期,
    // 尚未在 done 事件中重选)绝不挑选——否则会把主屏输出本身当成副屏。
    // done 处理器已保证回调触发前 primary 重选完成,这里是兜底守卫。
    self->mSecondaryOutput = nullptr;
    if (self->mDisplay->primary) {
        for (int i = 0; i < self->mDisplay->num_outputs; i++) {
            struct output* out = &self->mDisplay->outputs[i];
            if (out->done && out != self->mDisplay->primary) {
                self->mSecondaryOutput = out;
                break;
            }
        }
    }
    // 重同步所有 window->output:槽位按 bind 顺序复用,双拔后重插顺序不同
    // 会改变"逻辑归属 ? 槽位"的对应(主屏可能复用原副屏的槽位),而
    // window->output 记录的是创建时的槽位。不同步的话全局坐标重建
    // (pointer_handle_motion)会把主屏上的指针算到外部屏——Android 焦点屏
    // 与物理屏永久分叉,点击全部路由到错误的屏(宿主光标由合成器绘制,
    // 视觉上看不出异常)。主屏窗口全屏在 primary,副屏窗口全屏在
    // mSecondaryOutput,按此规则整体重指,幂等无副作用。
    for (auto& entry : self->mWindows) {
        if (entry.second)
            entry.second->output = self->mDisplay->primary;
    }
    if (self->mSecondaryWindow)
        self->mSecondaryWindow->output = self->mSecondaryOutput;
    // Re-evaluate support and fire kSupported/kUnsupported on change.
    self->updateSupported(self->isExternalDisplaySupported());
    if (!self->isExternalDisplaySupported()) {
        self->closeExternalIfUnsupported();
    }
}

void WaylandBackend::closeExternalIfUnsupported() {
    if (mDevice) {
        // closeExternalDisplay() is idempotent: it is a no-op when the external
        // display is already closed, so calling it unconditionally is safe.
        mDevice->closeExternalDisplay();
    }
}

}  // namespace

static int hwc_open(const struct hw_module_t* module, const char* name,
                    struct hw_device_t** device) {
    int ret = 0;
    char property[PROPERTY_VALUE_MAX];

    if (strcmp(name, HWC_HARDWARE_COMPOSER)) {
        ALOGE("%s called with bad name %s", __FUNCTION__, name);
        return -EINVAL;
    }

    bool multi_windows = property_get_bool("persist.openfde.multi_windows", false);
    bool use_subsurface = property_get_bool("persist.openfde.use_subsurface", false) || multi_windows;

    if (property_get("openfde.xdg_runtime_dir", property, "/run/user/1000") > 0) {
        setenv("XDG_RUNTIME_DIR", property, 1);
    }
    if (property_get("openfde.wayland_display", property, "wayland-0") > 0) {
        setenv("WAYLAND_DISPLAY", property, 1);
    }

    struct display* wayland_display = nullptr;
    if (property_get("ro.hardware.gralloc", property, "default") > 0) {
        wayland_display = create_display(property, HWC_DISPLAY_PRIMARY);
    }
    if (!wayland_display) {
        ALOGE("failed to open wayland connection");
        return -ENODEV;
    }
    wayland_display->mouse_icon_addr = -1;

    // Initialize width and height with user-provided overrides if any
    choose_width_height(wayland_display, 0, 0);

    auto backend = std::make_unique<WaylandBackend>(wayland_display, use_subsurface,
                                                    multi_windows);
    // The device constructor also lets the backend create any extra display
    // it supports; the wayland backend only has the primary one
    auto* dev = new Hwc2Device(std::move(backend), wayland_display);
    dev->common.module = const_cast<hw_module_t*>(module);

    auto* wlBackend = static_cast<WaylandBackend*>(&dev->backend());
    wlBackend->createInitialWindow();

    if (wayland_display->refresh > 1000 && wayland_display->refresh < 1000000)
        dev->setVsyncPeriod(1000 * 1000 * 1000 / (wayland_display->refresh / 1000));

    wayland_display->cursor_surface =
        wl_compositor_create_surface(wayland_display->compositor);
    if (wayland_display->viewporter) {
        wayland_display->cursor_viewport =
            wp_viewporter_get_viewport(wayland_display->viewporter, wayland_display->cursor_surface);
    }

    // Binderized HIDL services (IOpenfdeWindow / IOpenfdeDisplay) must have the
    // hwbinder RPC threadpool configured before they are registered, otherwise
    // hwservicemanager cannot validate/serve them and registerAsService() fails
    // with UNKNOWN_ERROR (-2147483648).
    configureRpcThreadpool(4, false);

    ret = wlBackend->startThreads();
    dev->startServices();

    *device = &dev->common;

    return ret;
}

static struct hw_module_methods_t hwc_module_methods = {
    .open = hwc_open,
};

hwc_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = HWC_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = HWC_HARDWARE_MODULE_ID,
        .name = "Openfde hwcomposer module",
        .author = "The Android Open Source Project",
        .methods = &hwc_module_methods,
    }
};
