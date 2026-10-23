/*
 * Copyright (C) 2012 The Android Open Source Project
 * Copyright (C) 2021 The Waydroid Project
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

// HWC2 composer device with an X11 backend. All display functions are
// per display as required by hwcomposer2.h; the protocol state machine lives
// in common/hwc2_device.cpp, this file only implements the backend that
// presents per-display layer lists through XRender.

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
#include <string>
#include <sstream>
#include <functional>
#include <algorithm>

#include <log/log.h>
#include <cutils/properties.h>
#include <hardware/hwcomposer.h>
#include <hidl/HidlTransportSupport.h>
#include <system/graphics.h>
#include <ui/Rect.h>
#include <ui/GraphicBufferMapper.h>
#include <sync/sync.h>
#include <drm_fourcc.h>
#include <gralloc_handle.h>
#include <cros_gralloc/cros_gralloc_handle.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/shape.h>
#include <xcb/xproto.h>

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include <cutils/trace.h>
#include <utils/Trace.h>

#include "../common/hwc2_device.h"
#include "WaydroidWindow.h"
#include "egl-tools.h"

using ::android::OK;
using ::android::status_t;
using ::android::hardware::joinRpcThreadpool;

using ::openfde::Hwc2Backend;
using ::openfde::Hwc2Device;
using ::openfde::Hwc2Display;
using ::openfde::Hwc2Layer;
using ::openfde::handle_ext;

using ::vendor::openfde::window::V1_1::IOpenfdeWindow;
using ::vendor::openfde::window::implementation::OpenfdeWindow;

using namespace HWC2;

int cancel_maximum(xcb_connection_t *conn,xcb_screen_t * screen, xcb_window_t main_win);

namespace {

class X11Backend : public Hwc2Backend {
  public:
    X11Backend(struct display* display, bool useSubsurface, bool multiWindows,
               int32_t aospVersion)
        : mDisplay(display),
          mUseSubsurface(useSubsurface),
          mMultiWindows(multiWindows),
          mAospVersion(aospVersion) {
        mDisplay->x11_windows = &mWindows;
    }

    ~X11Backend() override {
        for (auto* disp : {mDisplay, mSecondaryDisplay}) {
            if (!disp) {
                continue;
            }
            for (auto& pair : disp->buffer_map) {
                if (pair.second) {
                    destroy_buffer(disp, pair.second);
                }
            }
            disp->buffer_map.clear();
            destroy_display(disp);
        }
    }

    // Hwc2Backend interface
    std::vector<hwc2_config_t> getConfigs(Hwc2Display& /*display*/) override {
        std::vector<hwc2_config_t> configs;
        for (int i = 0; i < gScreenConfigCount; i++) {
            configs.push_back((hwc2_config_t)i);
        }
        return configs;
    }

    Error getActiveConfig(Hwc2Display& display, hwc2_config_t* outConfig) override {
        *outConfig = resolve(display).disp->active_config;
        return Error::None;
    }

    Error setActiveConfig(Hwc2Display& display, hwc2_config_t config) override;
    Error getAttribute(Hwc2Display& display, hwc2_config_t config, Attribute attribute,
                       int32_t* outValue) override;
    Error validate(Hwc2Display& display) override;
    Error present(Hwc2Display& display) override;
    std::string dump(Hwc2Display& display) override;

    // The external display is always available, as in the HWC1 backend
    void createExternalDisplays(Hwc2Device& device) override;
    void destroyExternalDisplay(void* backendData) override;

    // Creates the initial "Openfde" window matching the desktop file, for
    // the primary display and for the external one when present
    void createInitialWindows();

    // Backend threads: window service, egl workers and x11 event loops
    int startThreads();

  private:
    // Backend resources of one composer display
    struct BackendDisplay {
        struct display* disp;
        std::map<std::string, struct window*>* windows;
    };
    BackendDisplay resolve(Hwc2Display& display);

    struct buffer* getX11Buffer(struct display* disp, buffer_handle_t handle,
                                const handle_ext& ext, hwc_rect_t fallbackFrame,
                                struct window* window);
    void createDri3XRenderPicture(struct display* disp, buffer_handle_t handle,
                                  struct window* window, struct buffer* buf,
                                  int pixel_stride);
    void* produceBGRA8888(struct display* disp, sp<android::GraphicBuffer> src_gb,
                          sp<android::GraphicBuffer> dst_gb);
    bool updateCursorSurface(struct display* disp, Hwc2Layer& layer,
                             std::map<std::string, struct window*>& windows);
    void eraseCursorLayerBuffer(struct display* disp, buffer_handle_t handle);
    void setCustomCursor(struct display* disp, Picture xpicture, int hot_x, int hot_y,
                         std::map<std::string, struct window*>& windows);
    int setBlackBackground(struct display* disp, struct window* win);
    int adjustWindowGeo(struct display* disp, hwc_rect_t frame, hwc_frect_t crop,
                        Transform transform, struct buffer* buf,
                        struct window* window, bool useSubsurface);
    void resizeX11Windows(struct display* disp,
                          std::map<std::string, struct window*>& windows,
                          int target_width, int target_height);

    static void* windowServiceThreadEntry(void* data);

    struct display* mDisplay;
    std::map<std::string, struct window*> mWindows;
    // External display, always present in the x11 backend
    struct display* mSecondaryDisplay = nullptr;
    std::map<std::string, struct window*> mSecondaryWindows;
    bool mUseSubsurface;
    bool mMultiWindows;
    int32_t mAospVersion;

    pthread_t mWindowServiceThread = 0;
    pthread_t mEglWorkerThread = 0;
    pthread_t mEglSecondaryWorkerThread = 0;
    pthread_t mEventThreadPrimary = 0;
    pthread_t mEventThreadSecondary = 0;
};

Cursor create_empty_cursor(Display *display, Window root) {
    // Create a 1x1 empty pixmap (transparent)
    Pixmap empty_pixmap = XCreatePixmap(display, root, 1, 1, 1);

    // Create an empty mask (for transparency)
    Pixmap mask_pixmap = XCreatePixmap(display, root, 1, 1, 1);

    // Create a graphics context
    XGCValues xgc;
    GC gc = XCreateGC(display, empty_pixmap, 0, &xgc);

    // Clear both pixmaps (fill them with "0", meaning transparency)
    XSetForeground(display, gc, 0);
    XFillRectangle(display, empty_pixmap, gc, 0, 0, 1, 1);
    XFillRectangle(display, mask_pixmap, gc, 0, 0, 1, 1);

    // Create an empty cursor with the transparent pixmap and mask
    XColor black;
    black.red = black.green = black.blue = 0;
    Cursor cursor = XCreatePixmapCursor(display, empty_pixmap, mask_pixmap, &black, &black, 0, 0);

    // Free resources
    XFreeGC(display, gc);
    XFreePixmap(display, empty_pixmap);
    XFreePixmap(display, mask_pixmap);

    return cursor;
}

void X11Backend::setCustomCursor(struct display* disp, Picture xpicture, int hot_x,
                                 int hot_y, std::map<std::string, struct window*>& windows) {
    ALOGD("x11_set_custom_cursor hot_x: %d, hot_y: %d", hot_x, hot_y);

    if (!disp)
       return;

    if(!xpicture){
        ALOGE("error xpicture is null");
        Cursor empty_cursor = create_empty_cursor(disp->x11display, disp->xcbscreen->root);
        std::scoped_lock lock(disp->windowsMutex);
        for (auto it = windows.begin(); it != windows.end(); it++) {
            if (it->second){
                XDefineCursor(disp->x11display, it->second->xcbwindow, empty_cursor);
            }
        }
        return;
    }

    // Create the cursor from the picture
    Cursor cursor = XRenderCreateCursor(disp->x11display, xpicture, hot_x, hot_y);
    if(cursor == None1){
        return;
    }
    std::scoped_lock lock(disp->windowsMutex);
    for (auto it = windows.begin(); it != windows.end(); it++) {
        if (it->second){
            XDefineCursor(disp->x11display, it->second->xcbwindow, cursor);
        }
    }
}

void X11Backend::eraseCursorLayerBuffer(struct display* disp, buffer_handle_t handle) {
    auto it = disp->buffer_map.find(handle);
    if (it != disp->buffer_map.end()) {
        destroy_buffer(disp, it->second);
        disp->buffer_map.erase(it);
    }
}

static bool isCursorLayer(const std::string& layerName) {
    return layerName.length() >= 12 && layerName.substr(0, 12) == "[BBQ] Sprite";
}

bool X11Backend::updateCursorSurface(struct display* disp, Hwc2Layer& layer,
                                     std::map<std::string, struct window*>& windows) {
    const std::string& layerName = layer.getName();

    if (!isCursorLayer(layerName)) {
        return false;
    }

    // The cursor layer is not participating in SurfaceFlinger GPU compositing
    int64_t mouse_icon_addr = property_get_int64("fde.mouse_icon_addr", 0);
    if (disp->mouse_icon_addr != mouse_icon_addr) {
        disp->mouse_icon_addr = mouse_icon_addr;
        disp->additional_refresh_cursor_times = 0;
        eraseCursorLayerBuffer(disp, layer.buffer());
    }else{
        if(disp->additional_refresh_cursor_times > 60){      //Refresh the cursor three additional times
            return true;
        }else{
            eraseCursorLayerBuffer(disp, layer.buffer());
        }
    }

    struct buffer *buf = getX11Buffer(disp, layer.buffer(), layer.getHandleExt(),
                                      layer.displayFrame(), NULL);
    if (!buf) {
        ALOGE("Failed to get x11 buffer");
        return false;
    }
    int32_t icon_hotspot_x = property_get_int32("fde.mouse_icon_hotspot_x", 5);
    int32_t icon_hotspot_y = property_get_int32("fde.mouse_icon_hotspot_y", 5);
    setCustomCursor(disp, buf->xpicture, icon_hotspot_x, icon_hotspot_y, windows);
    disp->additional_refresh_cursor_times++;
    return true;
}

Error X11Backend::validate(Hwc2Display& display) {
    const BackendDisplay backend = resolve(display);
    struct display* disp = backend.disp;
    std::map<std::string, struct window*>& windows = *backend.windows;

    if (mAospVersion == 17 || (display.geometryChanged() && mUseSubsurface)) {
        disp->geo_changed = true;
    }

    bool foundCursorLayer = false;
    for (auto* layer : display.getLayersByZ()) {
        if (isCursorLayer(layer->getName())) {
            // The cursor layer never goes through SurfaceFlinger GPU
            // compositing; it is presented as the X11 cursor.
            display.requestCompositionChange(*layer, Composition::Device);
            foundCursorLayer |= updateCursorSurface(disp, *layer, windows);
            continue;
        }

        Composition desired = mUseSubsurface ? Composition::Device : Composition::Client;
        if (display.colorTransformForced()) {
            desired = Composition::Client;
        }
        display.requestCompositionChange(*layer, desired);
    }

    if (!foundCursorLayer && disp->mouse_icon_addr != -1) {
        setCustomCursor(disp, 0, 0, 0, windows);
        disp->mouse_icon_addr = -1;
        ALOGI("x11 cursor hidden");
    }

    return Error::None;
}

int X11Backend::setBlackBackground(struct display* disp, struct window* win){
	if (win->rects.size() <= 1) {
		return 0;
	}
	int min_x = INT_MAX, min_y = INT_MAX , cropx = 0, cropy = 0;
	int max_right = INT_MIN, max_bottom = INT_MIN;
	// Calculate bounding box from all rectangles
	for (size_t i = 0; i < win->rects.size(); i++) {
		const auto& rect = win->rects[i];
		const auto& crop = win->crops[i];
		min_x = std::min(min_x, (int)rect.x);
		if (min_x == rect.x){
			cropx = crop.left;
		}
		min_y = std::min(min_y, (int)rect.y);
		if (min_y == rect.y) {
			cropy = crop.top;
		}
		max_right = std::max(max_right, (int)(rect.x + rect.width));
		max_bottom = std::max(max_bottom, (int)(rect.y + rect.height));
	}

	// Get outer frame dimensions
	int frame_x = min_x;
	int frame_y = min_y;
	int border = 10;
	int borderAndRadius = 14;
	int src_x = fmax(0, cropx);
	int src_y = fmax(0, cropy);
	int frame_width = max_right - min_x;
	int frame_height = max_bottom - min_y;
	if (frame_width < disp->width) {
		frame_x += (border - src_x);
	}
	frame_y += borderAndRadius;
	if (frame_width + frame_x >= disp->width + border) {
		frame_width -= (border - src_x);
	}else {
		frame_width -=(border + border - src_x);
	}
	frame_height -= 2* borderAndRadius;
	if ( src_x <= border ) {
		src_x = 0;
	}
	XRenderColor frame_color = {0, 0, 0, 0xFFFF}; // black

	// Create a temporary picture to store current backxpicture content
	Pixmap tempPixmap = XCreatePixmap(disp->x11display, win->xcbwindow, disp->width,disp->height, 32);
	Picture temp_picture = XRenderCreatePicture(disp->x11display, tempPixmap, disp->argb_format, 0, NULL);
	// Copy current backxpicture to temporary picture
	XRenderComposite(disp->x11display, PictOpSrc, win->backxpicture, None1, temp_picture,
		    0, 0, 0, 0, 0, 0, disp->width, disp->height);
	Picture solid_picture = XRenderCreateSolidFill(disp->x11display, &frame_color);
	XRenderComposite(disp->x11display, PictOpSrc, solid_picture, None1, win->backxpicture,
		    src_x, src_y, 0, 0, frame_x, frame_y, frame_width , frame_height);
	XRenderFreePicture(disp->x11display, solid_picture);
	// Blend the temporary picture back onto backxpicture with PictOpOver
	XRenderComposite(disp->x11display, PictOpOver, temp_picture, None1, win->backxpicture,
		    0, 0, 0, 0, 0, 0, disp->width, disp->height);
	XFreePixmap(disp->x11display, tempPixmap);
	// Clean up temporary picture
	XRenderFreePicture(disp->x11display, temp_picture);
	return 0;
}

static void update_shm_buffer(struct display* display, struct buffer *buffer)
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

static int update_shm_pixmap(struct display * display, struct buffer *buffer, struct window *window) {
	int width = buffer->width;
	int height = buffer->height;
	update_shm_buffer(display,buffer);
	if (window != NULL){
		xcb_put_image(display->xcbconnection, XCB_IMAGE_FORMAT_Z_PIXMAP,
		    buffer->xcbpixmap, window->xcbgc, width, height, 0, 0, 0, 32,
		    width * height * 4, (uint8_t*)buffer->shm_data);
	}else{
		xcb_gcontext_t gc = xcb_generate_id(display->xcbconnection);
		xcb_create_gc(display->xcbconnection, gc, buffer->xcbpixmap, 0, NULL);
		xcb_put_image(display->xcbconnection, XCB_IMAGE_FORMAT_Z_PIXMAP,
		    buffer->xcbpixmap, gc, width, height, 0, 0, 0, 32,
		    width * height * 4, (uint8_t*)buffer->shm_data);
		xcb_free_gc(display->xcbconnection, gc);
	}
	return 0;
}

void* X11Backend::produceBGRA8888(struct display* disp,
                                  sp<android::GraphicBuffer> src_gb,
                                  sp<android::GraphicBuffer> dst_gb) {
    disp->egl_work_queue.push_back(std::bind(egl_convert_buffer_to_BGRA_8888, disp, src_gb,dst_gb));
	sem_post(&disp->egl_go);
	sem_wait(&disp->egl_done);
	return (void *)dst_gb->getNativeBuffer()->handle;
}

void X11Backend::createDri3XRenderPicture(struct display* disp, buffer_handle_t handle,
                                          struct window *window, struct buffer *buf,
                                          int pixel_stride) {
	int width,height, stride, format, prime_fd, size;
	int usage = GRALLOC_USAGE_HW_TEXTURE;
    if (disp->gtype == GRALLOC_GBM) {
        struct gralloc_handle_t *drm_handle = (struct gralloc_handle_t *)handle;
        width = drm_handle->width;
        height = drm_handle->height;
        stride = drm_handle->stride;
        format = drm_handle->format;
        usage = drm_handle->usage;
        prime_fd = drm_handle->prime_fd;
    }else if (disp->gtype == GRALLOC_FTG340) {
        const gc_private_handle_t *gc_handle = (const gc_private_handle_t *)handle;
        width = gc_handle->width;
        height = gc_handle->height;
        stride = gc_handle->stride;
        format = gc_handle->format;
        prime_fd = gc_handle->prime_fd;
    }else if (disp->gtype == GRALLOC_X100) {
        const X100_native_handle_t *x100_handle = (const X100_native_handle_t *)handle;
        width = x100_handle->iWidth;
        height = x100_handle->iHeight;
        stride = pixel_stride * 4;
        format = x100_handle->iFormat;
        prime_fd = x100_handle->fd[0];
    }else {
	const struct cros_gralloc_handle *cros_handle = (const struct cros_gralloc_handle *)handle;
        width = cros_handle->width;
        height = cros_handle->height;
        stride = cros_handle->strides[0];
        format = cros_handle->droid_format;
        prime_fd = cros_handle->fds[0];
    }

	size = stride * height * 4;
    // Create destination GraphicBuffer for store HAL_PIXEL_FORMAT_BGRA_8888
    sp<android::GraphicBuffer> dst_gb = new android::GraphicBuffer(
       width, height, HAL_PIXEL_FORMAT_BGRA_8888,
       GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_HW_RENDER);
    if (dst_gb->initCheck() != android::NO_ERROR) {
	ALOGE("Failed to create destination GraphicBuffer");
	return ;
    }
    if (format != HAL_PIXEL_FORMAT_BGRA_8888) {
	    sp<android::GraphicBuffer> gb_for_stride = new android::GraphicBuffer(width, height,
		format, 1, GRALLOC_USAGE_HW_COMPOSER |
		GRALLOC_USAGE_HW_TEXTURE, std::string("gb_for_stride") + std::to_string(getpid()));
	    if (gb_for_stride->initCheck() != android::NO_ERROR) {
		ALOGE("Failed to create gb_for_stride");
		return;
	    }

	    int stride_for_src_gb;
	    switch (format) {
		case HAL_PIXEL_FORMAT_RGBA_8888:
		case HAL_PIXEL_FORMAT_RGBX_8888:
		case HAL_PIXEL_FORMAT_BGRA_8888:
		    stride_for_src_gb = gb_for_stride->getStride() * 4;
		    break;
		case HAL_PIXEL_FORMAT_RGB_888:
		    stride_for_src_gb = gb_for_stride->getStride() * 3;
		    break;
		case HAL_PIXEL_FORMAT_RGB_565:
		    stride_for_src_gb = gb_for_stride->getStride() * 2;
		    break;
		default: //other formats need to test!!!
		    stride_for_src_gb = gb_for_stride->getStride();
	    }

	    // Create source GraphicBuffer from existing handle
	    sp<android::GraphicBuffer> src_gb = new android::GraphicBuffer(
		(native_handle_t*)handle, android::GraphicBuffer::WRAP_HANDLE,
		width, height, format, 1, uint64_t(usage), stride_for_src_gb);
	    if (src_gb->initCheck() != android::NO_ERROR) {
		ALOGE("Failed to create source GraphicBuffer from handle");
		return ;
	    }
            if (!produceBGRA8888(disp, src_gb, dst_gb)) {
                ALOGE("produce_BGRA_8888 failed");
                return ;
            }
		const native_handle_t* native_handle = dst_gb->getNativeBuffer()->handle;
		struct gralloc_handle_t * drm_handle = (struct gralloc_handle_t*)native_handle;
		prime_fd = drm_handle->prime_fd;
		size = dst_gb->getStride() * height * 4;
		stride = dst_gb->getStride() * 4 ;
    }
    if (window != NULL ) {
        xcb_window_t xcbwindow = window->xcbwindow;
        int x11_fd = dup(prime_fd);
        if (x11_fd >= 0) {
            fcntl(x11_fd, F_SETFD, FD_CLOEXEC);
        }else {
            ALOGE("dup fd failed");
            return ;
        }

        buf->xcbpixmap = xcb_generate_id(disp->xcbconnection);
        XRenderPictureAttributes pa;
        pa.repeat = False;
        xcb_void_cookie_t pixmap_cookie = xcb_dri3_pixmap_from_buffer(disp->xcbconnection,
            buf->xcbpixmap, xcbwindow, size, width, height, stride, 32, 32, x11_fd);
        xcb_generic_error_t *pixmap_error = xcb_request_check(disp->xcbconnection, pixmap_cookie);
        if (pixmap_error) {
           ALOGE("XCB error in xcb_dri3_pixmap_from_buffer: %d", pixmap_error->error_code);
           free(pixmap_error);
           close(x11_fd);
           return ;
        }
        buf->xpicture = XRenderCreatePicture(disp->x11display, buf->xcbpixmap,disp->argb_format, CPRepeat, &pa);
        close(x11_fd);
    }else{
        int x11_fd = dup(prime_fd);
        if (x11_fd >= 0) {
            fcntl(x11_fd, F_SETFD, FD_CLOEXEC);
        }else {
            ALOGE("dup fd failed");
            return ;
        }

        buf->xcbpixmap = xcb_generate_id(disp->xcbconnection);
        XRenderPictureAttributes pa;
        pa.repeat = False;
        xcb_void_cookie_t pixmap_cookie = xcb_dri3_pixmap_from_buffer(disp->xcbconnection,
            buf->xcbpixmap, disp->xcbscreen->root, size, width, height, stride, 32, 32, x11_fd);
        xcb_generic_error_t *pixmap_error = xcb_request_check(disp->xcbconnection, pixmap_cookie);
        if (pixmap_error) {
           ALOGE("XCB error in xcb_dri3_pixmap_from_buffer: %d", pixmap_error->error_code);
           free(pixmap_error);
           close(x11_fd);
           return ;
        }
        buf->xpicture = XRenderCreatePicture(disp->x11display, buf->xcbpixmap,disp->argb_format, CPRepeat, &pa);
        close(x11_fd);
    }
}

struct buffer *X11Backend::getX11Buffer(struct display* disp, buffer_handle_t handle,
                                         const handle_ext& ext, hwc_rect_t fallbackFrame,
                                         struct window *window)
{
    uint32_t pixel_stride = ext.stride;
    uint32_t width = ext.width;
    uint32_t height = ext.height;

    if (!width)
        width = fallbackFrame.right - fallbackFrame.left;
    if (!height)
        height = fallbackFrame.bottom - fallbackFrame.top;

    auto it = disp->buffer_map.find(handle);
    if (it != disp->buffer_map.end()) {
        if (it->second->isShm) {
            if (width != (uint32_t)it->second->width || height != (uint32_t)it->second->height) {
                destroy_buffer(disp, it->second);
                disp->buffer_map.erase(it);
            } else {
                update_shm_pixmap(disp, it->second, window);
                return it->second;
            }
        } else {
            return it->second;
	}
    }

    struct buffer *buf;

    buf = new struct buffer();
    buf->xcbpixmap = 0;
    buf->xpicture = 0;

    if (disp->gtype == GRALLOC_GBM) {
        struct gralloc_handle_t *drm_handle = (struct gralloc_handle_t *)handle;
        buf->width=drm_handle->width;
        buf->height=drm_handle->height;
	buf->pixel_stride = pixel_stride;
        createDri3XRenderPicture(disp, handle, window, buf, pixel_stride);
        if (!buf->xpicture) {
            delete buf;
            return NULL;
        }
    } else if (disp->gtype == GRALLOC_CROS) {
        const struct cros_gralloc_handle *cros_handle = (const struct cros_gralloc_handle *)handle;
        buf->width=cros_handle->width;
        buf->height=cros_handle->height;
        createDri3XRenderPicture(disp, handle, window, buf, pixel_stride);
        if (!buf->xpicture) {
            delete buf;
            return NULL;
        }
    } else if (disp->gtype == GRALLOC_X100) {
        const X100_native_handle_t *x100_handle = (const X100_native_handle_t *)handle;
        buf->width=x100_handle->iWidth;
        buf->height=x100_handle->iHeight;
        createDri3XRenderPicture(disp, handle, window, buf, pixel_stride);
        if (! buf->xpicture) {
            delete buf;
            return NULL;
        }
    } else if (disp->gtype == GRALLOC_FTG340) {
        const gc_private_handle_t *gc_handle = (const gc_private_handle_t *)handle;
	    buf->width=gc_handle->width;
	    buf->height=gc_handle->height;
        createDri3XRenderPicture(disp, handle, window, buf, pixel_stride);
        if (!buf->xpicture) {
            delete buf;
            return NULL;
        }
    } else {
        ALOGE("unsupport gralloc type %d", disp->gtype);
        delete buf;
        return NULL;
    }

    disp->buffer_map[handle] = buf;
    return buf;
}

int X11Backend::adjustWindowGeo(struct display* disp, hwc_rect_t frame, hwc_frect_t crop,
                                Transform transform, struct buffer *buf,
                                struct window *window, bool useSubsurface){
    if (!useSubsurface)
        return  0;
    hwc_rect_t sourceCrop = {
        .left = (int)lroundf(crop.left),
        .top = (int)lroundf(crop.top),
        .right = (int)lroundf(crop.right),
        .bottom = (int)lroundf(crop.bottom),
    };

    if (static_cast<int>(transform) & HWC_TRANSFORM_ROT_90) {
        sourceCrop.left = (int)lroundf(crop.top);
        sourceCrop.top = (int)lroundf(crop.left);
        sourceCrop.right = (int)lroundf(crop.bottom);
        sourceCrop.bottom = (int)lroundf(crop.right);
    }
    xcb_configure_window_value_list_t values;
    values.x = floor(frame.left / disp->scale);
    values.y = floor(frame.top / disp->scale);
    // Calculate source crop dimensions
    int src_x = fmax(0, sourceCrop.left);
    int src_y = fmax(0, sourceCrop.top);
    int src_width = fmax(1, sourceCrop.right - sourceCrop.left);
    int src_height = fmax(1, sourceCrop.bottom - sourceCrop.top);

    // Calculate destination dimensions (scaled)
    int dst_width = fmax(1, ceil((frame.right - frame.left) / disp->scale));
    int dst_height = fmax(1, ceil((frame.bottom - frame.top) / disp->scale));


     // Check if scaling is needed
    if (src_width != dst_width || src_height != dst_height) {
        // Set scale transform for the picture
        XTransform scale_transform;
        scale_transform.matrix[0][0] = XDoubleToFixed((double)src_width / dst_width);
        scale_transform.matrix[0][1] = XDoubleToFixed(0.0);
        scale_transform.matrix[0][2] = XDoubleToFixed(0.0);
        scale_transform.matrix[1][0] = XDoubleToFixed(0.0);
        scale_transform.matrix[1][1] = XDoubleToFixed((double)src_height / dst_height);
        scale_transform.matrix[1][2] = XDoubleToFixed(0.0);
        scale_transform.matrix[2][0] = XDoubleToFixed(0.0);
        scale_transform.matrix[2][1] = XDoubleToFixed(0.0);
        scale_transform.matrix[2][2] = XDoubleToFixed(1.0);
        XRenderSetPictureTransform(disp->x11display, buf->xpicture, &scale_transform);
    }else {
        // Reset to identity transform when no scaling is needed
        XTransform identity_transform;
        identity_transform.matrix[0][0] = XDoubleToFixed(1.0);
        identity_transform.matrix[0][1] = XDoubleToFixed(0.0);
        identity_transform.matrix[0][2] = XDoubleToFixed(0.0);
        identity_transform.matrix[1][0] = XDoubleToFixed(0.0);
        identity_transform.matrix[1][1] = XDoubleToFixed(1.0);
        identity_transform.matrix[1][2] = XDoubleToFixed(0.0);
        identity_transform.matrix[2][0] = XDoubleToFixed(0.0);
        identity_transform.matrix[2][1] = XDoubleToFixed(0.0);
        identity_transform.matrix[2][2] = XDoubleToFixed(1.0);
        XRenderSetPictureTransform(disp->x11display, buf->xpicture, &identity_transform);
    }

    if (useSubsurface) {
	if (window->lastLayer == 0) {
		window->rects.clear();
		window->crops.clear();
	}
        xcb_rectangle_t rect;
        rect = {static_cast<int16_t>(values.x), static_cast<int16_t>(values.y), static_cast<uint16_t>(dst_width),static_cast<uint16_t>(dst_height)};
        window->rects.push_back(rect);
        window->crops.push_back(sourceCrop);
    }

    XRenderComposite(disp->x11display, PictOpOver, buf->xpicture, None1, window->backxpicture,
                  src_x, src_y, 0, 0, values.x, values.y, dst_width, dst_height);

    return 0;
}

Error X11Backend::present(Hwc2Display& display) {
    char property[PROPERTY_VALUE_MAX];

    const BackendDisplay backend = resolve(display);
    struct display* disp = backend.disp;
    std::map<std::string, struct window*>& windows = *backend.windows;

    if (disp->geo_changed) {
        for (auto it = disp->buffer_map.begin(); it != disp->buffer_map.end(); it++) {
            if (it->second) {
                destroy_buffer(disp, it->second);
            }
        }
        disp->buffer_map.clear();
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
     * render each layers in X11 windows.
     * In prop "openfde.active_apps" we choose what to be shown in window
     * and here if HWC is in single mode we show the screen only if any task are in screen
     * and in multi windows mode we group layers with same task ID in a X11 window.
     * And in prop "openfde.blacklist_apps" we select apps to not show in display.
     *
     * "openfde.active_apps" prop can be:
     * "none": No windows
     * "Openfde": Shows android screen in a single window
     * "AppID": Shows apps in related windows as explained above
     */
    // The external display tracks its own set of active apps
    property_get(disp->display_type == HWC_DISPLAY_EXTERNAL ? "openfde1.active_apps"
                                                           : "openfde.active_apps",
                 property, "none");
    std::string active_apps = std::string(property);
    std::string blacklist_apps = std::string("com.android.launcher3");
    property_get("openfde.blacklist_apps", property, "");
    if (strlen(property) > 0 && strncmp(property, "com.android.launcher3", strlen("com.android.launcher3")) != 0) {
        blacklist_apps = blacklist_apps + ":" + std::string(property);
    }
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

    std::scoped_lock lock(disp->windowsMutex);
    if (active_apps == "none") {
        // Clear all open windows
        for (auto it = windows.begin(); it != windows.end(); it++) {
            if (it->second)
                destroy_window(it->second);
        }
        windows.clear();

        property_set("openfde.open_windows", "0");
        return Error::None;
    } else if (active_apps == "Openfde") {
        // Clear all open windows if there's any and just keep "Openfde"
        if (windows.find(active_apps) == windows.end() || !windows[active_apps]->isActive) {
            for (auto it = windows.begin(); it != windows.end(); it++) {
                if (it->second) {
                    destroy_window(it->second);
                }
            }
            windows.clear();
        } else {
            windows[active_apps]->lastLayer = 0;
            windows[active_apps]->last_layer_buffer = nullptr;
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
                        if (windows.find(single_layer_tid) != windows.end()) {
                            windows[single_layer_tid]->lastLayer = 0;
                            windows[single_layer_tid]->last_layer_buffer = nullptr;
                        }
                    }
                }
            }
        }
        // Nothing to show on screen, so clear all open windows
        if (!showWindow) {
            for (auto it = windows.begin(); it != windows.end(); it++) {
                if (it->second)
                    destroy_window(it->second);
            }
            windows.clear();

            property_set("openfde.open_windows", "0");
            return Error::None;
        }
        bool shouldCloseLeftover = true;
        for (auto it = windows.cbegin(); it != windows.cend();) {
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
                        windows.erase(it++);
                        shouldCloseLeftover = true;
                        std::string windows_size_str = std::to_string(windows.size());
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
        for (auto it = windows.cbegin(); it != windows.cend();) {
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
                windows.erase(it++);
                std::string windows_size_str = std::to_string(windows.size());
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
        struct window *window = NULL;

        if (active_apps == "Openfde") {
            // Show everything in a single window
            if (windows.find(active_apps) == windows.end()) {
                windows[active_apps] = create_window(disp, mUseSubsurface, active_apps, "0", {0, 0, 0, 255});
                std::string windows_size_str = std::to_string(windows.size());
                property_set("openfde.open_windows", windows_size_str.c_str());
            }
            window = windows[active_apps];
        } else if (!mMultiWindows) {
            if (single_layer_tid.length()) {
                if (windows.find(single_layer_tid) == windows.end()) {
                    windows[single_layer_tid] = create_window(disp, mUseSubsurface, single_layer_aid, single_layer_tid, {0, 0, 0, 255});
                    std::string windows_size_str = std::to_string(windows.size());
                    property_set("openfde.open_windows", windows_size_str.c_str());
                }
                window = windows[single_layer_tid];
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
                    if (windows.find(layer_tid) == windows.end()) {
                        windows[layer_tid] = create_window(disp, mUseSubsurface, layer_aid, layer_tid, {0, 0, 0, 0});
                        std::string windows_size_str = std::to_string(windows.size());
                        property_set("openfde.open_windows", windows_size_str.c_str());
                    }
                    if (windows.find(layer_tid) != windows.end())
                        window = windows[layer_tid];
                }
            }
        }
        // Detecting toast and ANR layers
        if (!window) {
            std::string LayerRawName;
            std::istringstream issLayer(layer_name);
            std::getline(issLayer, LayerRawName, '#');
            if (mMultiWindows && ((LayerRawName == "Toast")
                || (LayerRawName.find("Application Not Responding:") !=  std::string::npos))) {
                if (windows.find(LayerRawName) == windows.end()) {
                    windows[LayerRawName] = create_window(disp, mUseSubsurface, LayerRawName, "none", {0, 0, 0, 0});
                    std::string windows_size_str = std::to_string(windows.size());
                    property_set("openfde.open_windows", windows_size_str.c_str());
                }
                if (windows.find(LayerRawName) != windows.end()) {
                    window = windows[LayerRawName];
                }
            }
        }

        if (!window || !window->isActive) {
            if (acquireFence != -1) {
                close(acquireFence);
            }
            return;
        }

        struct buffer *buf = getX11Buffer(disp, handle, ext, frame, window);
        if (!buf) {
            ALOGE("Failed to get x11 buffer");
            if (acquireFence != -1) {
               close(acquireFence);
            }
            return;
        }

	XTransform xtransform;
        switch (transform) {
            case Transform::FlipH:
                xtransform.matrix[0][0] = XDoubleToFixed(-1.0); // scale x by -1
                xtransform.matrix[0][1] = XDoubleToFixed(0.0);
                xtransform.matrix[0][2] = XDoubleToFixed(buf->width);
                xtransform.matrix[1][0] = XDoubleToFixed(0.0);
                xtransform.matrix[1][1] = XDoubleToFixed(1.0);
                xtransform.matrix[1][2] = XDoubleToFixed(0.0);
                xtransform.matrix[2][0] = XDoubleToFixed(0.0);
                xtransform.matrix[2][1] = XDoubleToFixed(0.0);
                xtransform.matrix[2][2] = XDoubleToFixed(1.0);
                XRenderSetPictureTransform(disp->x11display, buf->xpicture, &xtransform);
	        break;
            case Transform::FlipV:
                xtransform.matrix[0][0] = XDoubleToFixed(1.0);
                xtransform.matrix[0][1] = XDoubleToFixed(0.0);
                xtransform.matrix[0][2] = XDoubleToFixed(0.0);
                xtransform.matrix[1][0] = XDoubleToFixed(0.0);
                xtransform.matrix[1][1] = XDoubleToFixed(-1.0); // scale y by -1
                xtransform.matrix[1][2] = XDoubleToFixed(buf->height);
                xtransform.matrix[2][0] = XDoubleToFixed(0.0);
                xtransform.matrix[2][1] = XDoubleToFixed(0.0);
                xtransform.matrix[2][2] = XDoubleToFixed(1.0);
                XRenderSetPictureTransform(disp->x11display, buf->xpicture, &xtransform);
                break;
            case Transform::Rotate90:
                xtransform.matrix[0][0] = XDoubleToFixed(0.0);
                xtransform.matrix[0][1] = XDoubleToFixed(-1.0);
                xtransform.matrix[0][2] = XDoubleToFixed(buf->height);
                xtransform.matrix[1][0] = XDoubleToFixed(1.0);
                xtransform.matrix[1][1] = XDoubleToFixed(0.0);
                xtransform.matrix[1][2] = XDoubleToFixed(0.0);
                xtransform.matrix[2][0] = XDoubleToFixed(0.0);
                xtransform.matrix[2][1] = XDoubleToFixed(0.0);
                xtransform.matrix[2][2] = XDoubleToFixed(1.0);
                XRenderSetPictureTransform(disp->x11display, buf->xpicture, &xtransform);
                break;
            case Transform::Rotate180:
                xtransform.matrix[0][0] = XDoubleToFixed(-1.0);
                xtransform.matrix[0][1] = XDoubleToFixed(0.0);
                xtransform.matrix[0][2] = XDoubleToFixed(buf->width);
                xtransform.matrix[1][0] = XDoubleToFixed(0.0);
                xtransform.matrix[1][1] = XDoubleToFixed(-1.0);
                xtransform.matrix[1][2] = XDoubleToFixed(buf->height);
                xtransform.matrix[2][0] = XDoubleToFixed(0.0);
                xtransform.matrix[2][1] = XDoubleToFixed(0.0);
                xtransform.matrix[2][2] = XDoubleToFixed(1.0);
                XRenderSetPictureTransform(disp->x11display, buf->xpicture, &xtransform);
                break;
            case Transform::Rotate270:
                xtransform.matrix[0][0] = XDoubleToFixed(1.0);
                xtransform.matrix[0][1] = XDoubleToFixed(0.0);
                xtransform.matrix[0][2] = XDoubleToFixed(0.0);
                xtransform.matrix[1][0] = XDoubleToFixed(0.0);
                xtransform.matrix[1][1] = XDoubleToFixed(0.0);
                xtransform.matrix[1][2] = XDoubleToFixed(0.0);
                xtransform.matrix[2][0] = XDoubleToFixed(0.0);
                xtransform.matrix[2][1] = XDoubleToFixed(-1.0);
                xtransform.matrix[2][2] = XDoubleToFixed(buf->width);
                XRenderSetPictureTransform(disp->x11display, buf->xpicture, &xtransform);
                break;
            case Transform::FlipHRotate90:
                xtransform.matrix[0][0] = XDoubleToFixed(0.0);
                xtransform.matrix[0][1] = XDoubleToFixed(1.0);
                xtransform.matrix[0][2] = XDoubleToFixed(0.0);
                xtransform.matrix[1][0] = XDoubleToFixed(1.0);
                xtransform.matrix[1][1] = XDoubleToFixed(0.0);
                xtransform.matrix[1][2] = XDoubleToFixed(0.0);
                xtransform.matrix[2][0] = XDoubleToFixed(0.0);
                xtransform.matrix[2][1] = XDoubleToFixed(0.0);
                xtransform.matrix[2][2] = XDoubleToFixed(1.0);
                XRenderSetPictureTransform(disp->x11display, buf->xpicture, &xtransform);
                break;
            case Transform::FlipVRotate90:
                xtransform.matrix[0][0] = XDoubleToFixed(0.0);
                xtransform.matrix[0][1] = XDoubleToFixed(-1.0);
                xtransform.matrix[0][2] = XDoubleToFixed(buf->height);
                xtransform.matrix[1][0] = XDoubleToFixed(-1.0);
                xtransform.matrix[1][1] = XDoubleToFixed(0.0);
                xtransform.matrix[1][2] = XDoubleToFixed(buf->width);
                xtransform.matrix[2][0] = XDoubleToFixed(0.0);
                xtransform.matrix[2][1] = XDoubleToFixed(0.0);
                xtransform.matrix[2][2] = XDoubleToFixed(1.0);
                XRenderSetPictureTransform(disp->x11display, buf->xpicture, &xtransform);
                break;
            default:
                break;
	}

        if (mUseSubsurface ) {
        	    adjustWindowGeo(disp, frame, crop, transform, buf, window, mUseSubsurface);
        }else {
     	    XRenderComposite(disp->x11display, PictOpOver, buf->xpicture, None1, window->xpicture,
		   	 0, 0, 0, 0, 0,0, disp->width, disp->height);
        }
        window->last_layer_buffer = buf;
        window->lastLayer++;

        if (window->snapshot_buffer) {
           // Snapshot buffer should be detached by now, clean up
            destroy_buffer(disp,window->snapshot_buffer);
            window->snapshot_buffer = nullptr;
        }

        const int kAcquireWarningMS = 100;
        int fenceErr = sync_wait(acquireFence, kAcquireWarningMS);
        if (fenceErr < 0 && errno == ETIME) {
            ALOGE("hwcomposer waited on fence %d for %d ms",
            acquireFence, kAcquireWarningMS);
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
                       {0, 0, disp->full_width, disp->full_height},
                       {0.0f, 0.0f, (float)disp->full_width, (float)disp->full_height},
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
                   {0, 0, disp->full_width, disp->full_height},
                   {0.0f, 0.0f, (float)disp->full_width, (float)disp->full_height},
                   Transform::None, "", display.takeClientTargetAcquireFence());
    }

    // Layers order is changed from SF so we rearrange window contents
    if (mUseSubsurface)
        for (auto it = windows.begin(); it != windows.end(); it++)
            if (it->second){
                if (it->second->rects.size() > 1)
                {
                    setBlackBackground(disp, it->second);
                }
                XRenderComposite(disp->x11display, PictOpSrc, it->second->backxpicture, None1, it->second->xpicture,
                    0, 0, 0, 0, 0,0, disp->width, disp->height);
                if (!it->second->rects.empty()) {
                    xcb_shape_rectangles(disp->xcbconnection,
                                        XCB_SHAPE_SO_SET,
                                        XCB_SHAPE_SK_INPUT,
                                        XCB_CLIP_ORDERING_UNSORTED,
                                        it->second->xcbwindow,
                                        0, 0,
                                        it->second->rects.size(),
                                        it->second->rects.data());
                }
	}
    XFlush(disp->x11display);
    if (disp->geo_changed) {
        for (auto it = windows.begin(); it != windows.end(); it++) {
            if (it->second) {
                // This window has no changes in layers, leaving it
                if (!it->second->lastLayer)
                    continue;
                // Clear the window's back xpicture
		if (it->second->backxpicture) {
			XRenderColor clear_color = {0, 0, 0, 0}; // Transparent black
			XRenderFillRectangle(disp->x11display, PictOpClear, it->second->backxpicture, &clear_color,
			    0, 0, disp->width, disp->height);
		}
            }
        }
        disp->geo_changed = false;
    }
    xcb_flush(disp->xcbconnection);

    return Error::None;
}

void X11Backend::resizeX11Windows(struct display* disp,
                                  std::map<std::string, struct window*>& windows,
                                  int target_width, int target_height) {
    if (!disp) {
        return;
    }

    struct window_snapshot {
        std::string key;
        std::string app_id;
        std::string task_id;
        bool is_active;
        hwc_color_t color;
    };

    std::vector<window_snapshot> snapshots;
    snapshots.reserve(windows.size());

    for (auto it = windows.begin(); it != windows.end(); ++it) {
        struct window* window = it->second;
        if (!window) {
            continue;
        }

        uint8_t alpha = (window->appID == "Openfde" || !mMultiWindows) ? 255 : 0;
        snapshots.push_back(window_snapshot{
                it->first,
                window->appID,
                window->taskID,
                window->isActive,
                {0, 0, 0, alpha}});
    }

    for (auto it = windows.begin(); it != windows.end(); ++it) {
        if (it->second) {
            destroy_window(it->second);
        }
    }
    windows.clear();

    for (const auto& snapshot : snapshots) {
        struct window* new_window = create_window(
                disp,
                mUseSubsurface,
                snapshot.app_id,
                snapshot.task_id,
                snapshot.color);
        if (!new_window) {
            ALOGE("failed to recreate window for key=%s app=%s task=%s size=%dx%d",
                  snapshot.key.c_str(),
                  snapshot.app_id.c_str(),
                  snapshot.task_id.c_str(),
                  target_width,
                  target_height);
            continue;
        }
        new_window->isActive = snapshot.is_active;
        windows[snapshot.key] = new_window;
    }

    XFlush(disp->x11display);
    xcb_flush(disp->xcbconnection);
    disp->geo_changed = true;
}

Error X11Backend::setActiveConfig(Hwc2Display& display, hwc2_config_t config) {
    if (config >= (hwc2_config_t)gScreenConfigCount) {
        ALOGE("unsupported active config %lu", config);
        return Error::BadConfig;
    }

    const BackendDisplay backend = resolve(display);
    struct display* disp = backend.disp;
    std::map<std::string, struct window*>& windows = *backend.windows;

    if (disp->active_config == config) {
        return Error::None;
    }
    int target_width = 0;
    int target_height = 0;
    // config 0 is the real resolution, the others are deduplicated presets
    if (screenConfigs[config].Width == 0 || screenConfigs[config].Height == 0) {
        return Error::BadConfig;
    }
    target_width = screenConfigs[config].Width;
    target_height = screenConfigs[config].Height;
    disp->geo_changed = true;
    disp->active_config = config;
    disp->width = target_width;
    disp->height = target_height;
    disp->full_width = target_width;
    disp->full_height = target_height;
    disp->primary_x = 0;
    disp->primary_y = 0;

    {
        std::scoped_lock lock(disp->windowsMutex);
        resizeX11Windows(disp, windows, target_width, target_height);
    }

    ALOGI("setActiveConfig: config=%lu resized windows to %dx%d", config, target_width, target_height);

    return Error::None;
}

Error X11Backend::getAttribute(Hwc2Display& display, hwc2_config_t config,
                               Attribute attribute, int32_t* outValue) {
    if (config >= (hwc2_config_t)gScreenConfigCount) {
        return Error::BadConfig;
    }

    const BackendDisplay backend = resolve(display);
    struct display* disp = backend.disp;
    std::map<std::string, struct window*>& windows = *backend.windows;

    char property[PROPERTY_VALUE_MAX];
    int width = floor(disp->width * disp->scale);
    int height = floor(disp->height * disp->scale);
    int density = 160;
    if (config > 0 && config < (hwc2_config_t)gScreenConfigCount) {
        width = screenConfigs[config].Width;
        height = screenConfigs[config].Height;
        density = screenConfigs[config].Density;
    }

    switch (attribute) {
        case Attribute::VsyncPeriod: {
            int32_t period = 1000 * 1000 * 1000 / 60;
            if (disp->refresh > 1000 && disp->refresh < 1000000)
                period = 1000 * 1000 * 1000 / (disp->refresh / 1000);
            *outValue = period;
            return Error::None;
        }
        case Attribute::Width:
            if (property_get("persist.openfde.width_padding", property, nullptr) > 0)
                width -= atoi(property);
            property_set("openfde.display_width", std::to_string(width).c_str());
            *outValue = width;
            return Error::None;
        case Attribute::Height:
            if (property_get("persist.openfde.height_padding", property, nullptr) > 0)
                height -= atoi(property);
            property_set("openfde.display_height", std::to_string(height).c_str());
            *outValue = height;
            return Error::None;
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

std::string X11Backend::dump(Hwc2Display& display) {
    const BackendDisplay backend = resolve(display);
    struct display* disp = backend.disp;
    std::map<std::string, struct window*>& windows = *backend.windows;

    size_t window_count = 0;
    size_t buffer_count = 0;
    {
        std::scoped_lock lock(disp->windowsMutex);
        window_count = windows.size();
        buffer_count = disp->buffer_map.size();
    }

    std::stringstream output;
    output << "    id=" << display.getId()
           << " type=" << (display.isPrimary() ? "primary" : "external")
           << " active_config=" << disp->active_config
           << " width=" << disp->width << " height=" << disp->height
           << " full_width=" << disp->full_width << " full_height=" << disp->full_height
           << " scale=" << disp->scale
           << " refresh=" << disp->refresh
           << " use_subsurface=" << mUseSubsurface << " multi_windows=" << mMultiWindows
           << " geo_changed=" << disp->geo_changed
           << " windows=" << window_count << " buffers=" << buffer_count << '\n';
    return output.str();
}

void X11Backend::createInitialWindow(struct display* disp,
                                     std::map<std::string, struct window*>& windows) {
    auto first_window = create_window(disp, mUseSubsurface, "Openfde", "0", {0, 0, 0, 255});
    if (!property_get_bool("openfde.background_start", true)) {
        windows["Openfde"] = first_window;
        property_set(disp->display_type == HWC_DISPLAY_EXTERNAL ? "openfde1.active_apps"
                                                               : "openfde.active_apps",
                     "Openfde");
        property_set("openfde.open_windows", "1");
    } else {
        destroy_window(first_window);
    }
}

void X11Backend::createInitialWindows() {
    createInitialWindow(mDisplay, mWindows);
    if (mSecondaryDisplay) {
        createInitialWindow(mSecondaryDisplay, mSecondaryWindows);
    }
}

X11Backend::BackendDisplay X11Backend::resolve(Hwc2Display& display) {
    if (display.isPrimary() || !mSecondaryDisplay) {
        return {mDisplay, &mWindows};
    }
    return {mSecondaryDisplay, &mSecondaryWindows};
}

void X11Backend::createExternalDisplays(Hwc2Device& device) {
    char property[PROPERTY_VALUE_MAX];
    property_get("ro.hardware.gralloc", property, "default");

    mSecondaryDisplay = create_display(property, HWC_DISPLAY_EXTERNAL);
    if (!mSecondaryDisplay) {
        ALOGE("failed to open x11 secondary connection");
        return;
    }
    mSecondaryDisplay->x11_windows = &mSecondaryWindows;
    mSecondaryDisplay->mouse_icon_addr = -1;

    // The external display is announced to the client with a hotplug event,
    // which makes it usable through its own hwc2_display_t id
    device.createExternalDisplay(mSecondaryDisplay);

    if (pthread_create(&mEglSecondaryWorkerThread, NULL, egl_loop, mSecondaryDisplay) != 0) {
        ALOGE("openfde_hw_composer could not start egl_secondary_worker_thread");
    }
    if (pthread_create(&mEventThreadSecondary, NULL, event_loop_thread, mSecondaryDisplay) != 0) {
        ALOGE("Failed to create secondary event_loop_thread");
    }
}

void X11Backend::destroyExternalDisplay(void* backendData) {
    struct display* disp = static_cast<struct display*>(backendData);
    if (!disp) {
        return;
    }

    for (auto& pair : disp->buffer_map) {
        if (pair.second) {
            destroy_buffer(disp, pair.second);
        }
    }
    disp->buffer_map.clear();
    destroy_display(disp);

    if (disp == mSecondaryDisplay) {
        mSecondaryDisplay = nullptr;
    }
}

void* X11Backend::windowServiceThreadEntry(void* data) {
    X11Backend* backend = static_cast<X11Backend*>(data);
    sp<IOpenfdeWindow> openfdeWindow;
    status_t status;

    openfdeWindow = new OpenfdeWindow(backend->mDisplay, &backend->mWindows);
    if (openfdeWindow == nullptr) {
        ALOGE("Can not create an instance of Openfde Window HAL, exiting.");
        goto shutdown;
    }

    status = openfdeWindow->registerAsService();
    if (status != OK) {
        ALOGE("Could not register service for Openfde Window HAL (%d).", status);
    }

    ALOGI("Openfde Window HAL thread is ready.");
    joinRpcThreadpool();
    // Should not pass this line

shutdown:
    // In normal operation, we don't expect the thread pool to shutdown
    ALOGE("Openfde Window HAL service is shutting down.");
    return NULL;
}

int X11Backend::startThreads() {
    int ret = pthread_create(&mWindowServiceThread, NULL, windowServiceThreadEntry, this);
    if (ret) {
        ALOGE("openfde_hw_composer could not start window_service_thread\n");
    }

    ret = pthread_create(&mEglWorkerThread, NULL, egl_loop, mDisplay);
    if (ret) {
        ALOGE("openfde_hw_composer could not start egl_worker_thread");
    }

    if (pthread_create(&mEventThreadPrimary, NULL, event_loop_thread, mDisplay) != 0) {
        ALOGE("Failed to create primary event_loop_thread");
    }

    return ret;
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
    int32_t aosp_version = property_get_int32("ro.vendor.build.version.release", 0);

    if (multi_windows) {
        if (property_get("openfde.xmodifiers", property, "@im=fcitx") > 0) {
            setenv("XMODIFIERS", property, 1);
        }
        // Init global state for compound text encoding.
        xcb_compound_text_init();
    }

    if (property_get("openfde.xdg_runtime_dir", property, "/run/user/1000") > 0) {
        setenv("XDG_RUNTIME_DIR", property, 1);
    }
    if (property_get("openfde.x11_display", property, ":0") > 0) {
        setenv("DISPLAY", property, 1);
    }

    struct display* x11_display = nullptr;
    if (property_get("ro.hardware.gralloc", property, "default") > 0) {
        x11_display = create_display(property, HWC_DISPLAY_PRIMARY);
    }
    if (!x11_display) {
        ALOGE("failed to open x11 connection");
        return -ENODEV;
    }
    x11_display->mouse_icon_addr = -1;

    auto backend = std::make_unique<X11Backend>(x11_display, use_subsurface, multi_windows,
                                                aosp_version);
    // The device constructor also lets the backend create the external
    // display, which is reported to the client with a hotplug event
    auto* dev = new Hwc2Device(std::move(backend), x11_display);
    dev->common.module = const_cast<hw_module_t*>(module);

    auto* x11Backend = static_cast<X11Backend*>(&dev->backend());
    x11Backend->createInitialWindows();

    if (x11_display->refresh > 1000 && x11_display->refresh < 1000000)
        dev->setVsyncPeriod(1000 * 1000 * 1000 / (x11_display->refresh / 1000));

    ret = x11Backend->startThreads();
    dev->startServices();

    *device = &dev->common;

    return ret;
}

static struct hw_module_methods_t hwc_module_methods = {
    .open = hwc_open,
};

int cancel_maximum(xcb_connection_t *conn,xcb_screen_t * screen, xcb_window_t main_win){
    // 取消窗口最大化（移除 _NET_WM_STATE_MAXIMIZED_HORZ 和 _NET_WM_STATE_MAXIMIZED_VERT）
    xcb_intern_atom_cookie_t state_cookie = xcb_intern_atom(conn, 0, strlen("_NET_WM_STATE"), "_NET_WM_STATE");
    xcb_intern_atom_cookie_t max_horz_cookie = xcb_intern_atom(conn, 0, strlen("_NET_WM_STATE_MAXIMIZED_HORZ"), "_NET_WM_STATE_MAXIMIZED_HORZ");
    xcb_intern_atom_cookie_t max_vert_cookie = xcb_intern_atom(conn, 0, strlen("_NET_WM_STATE_MAXIMIZED_VERT"), "_NET_WM_STATE_MAXIMIZED_VERT");

    xcb_intern_atom_reply_t *state_atom = xcb_intern_atom_reply(conn, state_cookie, NULL);
    xcb_intern_atom_reply_t *max_horz_atom = xcb_intern_atom_reply(conn, max_horz_cookie, NULL);
    xcb_intern_atom_reply_t *max_vert_atom = xcb_intern_atom_reply(conn, max_vert_cookie, NULL);

    if (state_atom && max_horz_atom && max_vert_atom) {
        xcb_client_message_event_t ev ;
        ev.response_type = XCB_CLIENT_MESSAGE;
        ev.format = 32;
        ev.window = main_win;
        ev.type = state_atom->atom;
        ev.data.data32[0] = 0; // _NET_WM_STATE_REMOVE
        ev.data.data32[1] = max_horz_atom->atom;
        ev.data.data32[2] = max_vert_atom->atom;
        ev.data.data32[3] = 1;
        ev.data.data32[4] = 0;
        xcb_send_event(conn, 0, screen->root,
            XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
            (const char *)&ev);
    }
    return 0;
}

hwc_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = HWC_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = HWC_HARDWARE_MODULE_ID,
        .name = "openfde hwcomposer module",
        .author = "The Android Open Source Project",
        .methods = &hwc_module_methods,
    }
};
