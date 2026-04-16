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
#include <ui/Rect.h>
#include <ui/GraphicBufferMapper.h>
#include <libsync/sw_sync.h>
#include <sync/sync.h>
#include <drm_fourcc.h>
#include <presentation-time-client-protocol.h>
#include <viewporter-client-protocol.h>
#include <gralloc_handle.h>
#include <cros_gralloc/cros_gralloc_handle.h>
#include <gralloc_cb_bp.h>
#include <system/graphics.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/xproto.h>


#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include <cutils/trace.h>
#include <utils/Trace.h>

#include "extension.h"
#include "WaydroidWindow.h"
#include "egl-tools.h"

//using ::android::hardware::configureRpcThreadpool;
using ::android::hardware::joinRpcThreadpool;

using ::vendor::openfde::display::V1_1::IOpenfdeDisplay;
using ::vendor::openfde::display::V1_1::implementation::OpenfdeDisplay;
using ::vendor::openfde::window::V1_1::IOpenfdeWindow;
using ::vendor::openfde::window::implementation::OpenfdeWindow;

using ::android::OK;
using ::android::status_t;

struct waydroid_hwc_composer_device_1 {
    hwc_composer_device_1_t base; // constant after init
    const hwc_procs_t *procs;     // constant after init
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
};

int cancel_maximum(xcb_connection_t *conn,xcb_screen_t * screen, xcb_window_t main_win);
static struct buffer *get_wl_buffer(struct waydroid_hwc_composer_device_1 *pdev, hwc_layer_1_t *layer, size_t pos,struct window *window);

static void erase_cursor_layer_buffer(waydroid_hwc_composer_device_1* pdev, buffer_handle_t handle){
    auto it = pdev->display->buffer_map.find(handle);
    if (it != pdev->display->buffer_map.end()) {
        destroy_buffer(pdev->display,it->second);
        pdev->display->buffer_map.erase(it);
    }
}

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

static void x11_set_custom_cursor(waydroid_hwc_composer_device_1* pdev, Picture xpicture, int hot_x, int hot_y) {
    ALOGV("x11_set_custom_cursor hot_x: %d, hot_y: %d", hot_x, hot_y);
    struct display *display = pdev->display;

    if(!display)
       return;

    if(!xpicture){
        ALOGE("error xpicture is null");
        Cursor empty_cursor = create_empty_cursor(pdev->display->x11display, pdev->display->xcbscreen->root);
        std::scoped_lock lock(pdev->display->windowsMutex);
        for (auto it = pdev->windows.begin(); it != pdev->windows.end(); it++) {
            if (it->second){
                XDefineCursor(display->x11display, it->second->xcbwindow, empty_cursor);
            }
        }
        return;
    }

    // Create the cursor from the picture
    Cursor cursor = XRenderCreateCursor(display->x11display, xpicture, hot_x, hot_y);
    if(cursor == None1){
        return;
    }
    std::scoped_lock lock(pdev->display->windowsMutex);
    for (auto it = pdev->windows.begin(); it != pdev->windows.end(); it++) {
        if (it->second){
            XDefineCursor(display->x11display, it->second->xcbwindow, cursor);
        }
    }

}

static bool update_cursor_surface(waydroid_hwc_composer_device_1* pdev, hwc_layer_1_t* fb_layer, size_t layer) {
    std::string layer_name = pdev->display->layer_names[layer];

    if (layer_name.substr(0, 6) != "Sprite" || fb_layer->compositionType == HWC_FRAMEBUFFER_TARGET) {
        return false;
    }

    fb_layer->compositionType = HWC_OVERLAY; // Not participating in SurfaceFlinger GPU compositing hide internal cursor
    int64_t mouse_icon_addr = property_get_int64("fde.mouse_icon_addr", 0);
    if (pdev->display->mouse_icon_addr != mouse_icon_addr) {
        pdev->display->mouse_icon_addr = mouse_icon_addr;
        pdev->display->additional_refresh_cursor_times = 0;
        erase_cursor_layer_buffer(pdev, fb_layer->handle);
    }else{
        if(pdev->display->additional_refresh_cursor_times > 60){      //Refresh the wayland cursor three additional times
            return true;
        }else{
            erase_cursor_layer_buffer(pdev, fb_layer->handle);
        }
    }

    struct buffer *buf = get_wl_buffer(pdev, fb_layer, layer,NULL);
    if (!buf) {
        ALOGE("Failed to get wayland buffer");
        return false;
    }
    int32_t icon_hotspot_x = property_get_int32("fde.mouse_icon_hotspot_x", 5);
    int32_t icon_hotspot_y = property_get_int32("fde.mouse_icon_hotspot_y", 5);
    x11_set_custom_cursor(pdev, buf->xpicture, icon_hotspot_x, icon_hotspot_y);
    pdev->display->additional_refresh_cursor_times++;
    return true;
}


static int set_black_background(struct waydroid_hwc_composer_device_1 * pdev, struct window * win){
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
	if (frame_width < pdev->display->width) {
		frame_x += (border - src_x);
	}
	frame_y += borderAndRadius;
	//ALOGI("black background src_x %d src_Y %d  w %d h %d  frame_x %d fram_y %d ", src_x ,src_y, frame_width, frame_height, frame_x, frame_y);
	if (frame_width + frame_x >= pdev->display->width + border) {
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
	Pixmap tempPixmap = XCreatePixmap(pdev->display->x11display, win->xcbwindow, pdev->display->width,pdev->display->height, 32);
	Picture temp_picture = XRenderCreatePicture(pdev->display->x11display, tempPixmap, pdev->display->argb_format, 0, NULL);
	// Copy current backxpicture to temporary picture
	XRenderComposite(pdev->display->x11display, PictOpSrc, win->backxpicture, None1, temp_picture,
		    0, 0, 0, 0, 0, 0, pdev->display->width, pdev->display->height);
	Picture solid_picture = XRenderCreateSolidFill(pdev->display->x11display, &frame_color);
	XRenderComposite(pdev->display->x11display, PictOpSrc, solid_picture, None1, win->backxpicture,
		    src_x, src_y, 0, 0, frame_x, frame_y, frame_width , frame_height);
	XRenderFreePicture(pdev->display->x11display, solid_picture);
	// Blend the temporary picture back onto backxpicture with PictOpOver
	XRenderComposite(pdev->display->x11display, PictOpOver, temp_picture, None1, win->backxpicture,
		    0, 0, 0, 0, 0, 0, pdev->display->width, pdev->display->height);
	XFreePixmap(pdev->display->x11display, tempPixmap);
	// Clean up temporary picture
	XRenderFreePicture(pdev->display->x11display, temp_picture);
	return 0;
}

static int hwc_prepare(hwc_composer_device_1_t* dev,
                       size_t numDisplays, hwc_display_contents_1_t** displays) {
    struct waydroid_hwc_composer_device_1 *pdev = (struct waydroid_hwc_composer_device_1 *)dev;

    if (!numDisplays || !displays) return 0;

    hwc_display_contents_1_t* contents = displays[HWC_DISPLAY_PRIMARY];

    if (!contents) return 0;

    if ((contents->flags & HWC_GEOMETRY_CHANGED) && pdev->use_subsurface)
        pdev->display->geo_changed = true;

    std::pair<int, int> skipped(-1, -1);
    for (size_t i = 0; i < contents->numHwLayers; i++) {
      hwc_layer_1_t* fb_layer = &contents->hwLayers[i];

      if (!(fb_layer->flags & HWC_SKIP_LAYER))
        continue;

      if (skipped.first == -1)
        skipped.first = i;
      skipped.second = i;
    }

    bool foundCursorLayer = false;
    for (size_t i = 0; i < contents->numHwLayers; i++) {
        hwc_layer_1_t* fb_layer = &contents->hwLayers[i];

        if (fb_layer->compositionType == HWC_FRAMEBUFFER_TARGET)
            continue;
        if (fb_layer->flags & HWC_SKIP_LAYER)
            continue;

        /* skipped layers have to be composited by SurfaceFlinger; so in order
           have correct z-ordering, we must ask SurfaceFlinger to composite
           everything between the first and the last skipped layer. Unfortunately,
           this can't be done in multi windows mode, which relies on layers not
           being composited, so we won't render skipped layers correctly in that mode */
        if (!pdev->multi_windows)
            if (skipped.first >= 0 && i > skipped.first && i < skipped.second)
                fb_layer->compositionType = HWC_FRAMEBUFFER;

        if (fb_layer->compositionType ==
            (pdev->use_subsurface ? HWC_FRAMEBUFFER : HWC_OVERLAY))
            fb_layer->compositionType =
                (pdev->use_subsurface ? HWC_OVERLAY : HWC_FRAMEBUFFER);
        foundCursorLayer |= update_cursor_surface(pdev, fb_layer, i);
    }
    if(!foundCursorLayer && pdev->display->mouse_icon_addr != -1){
        x11_set_custom_cursor(pdev, 0, 0, 0);
        pdev->display->mouse_icon_addr = -1;
        ALOGI("x11 cursor hidden");
    }

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


static void * produce_BGRA_8888(struct waydroid_hwc_composer_device_1 *pdev, sp<android::GraphicBuffer> src_gb, sp<android::GraphicBuffer> dst_gb) {
    pdev->display->egl_work_queue.push_back(std::bind(egl_convert_buffer_to_BGRA_8888, pdev->display, src_gb,dst_gb));
	sem_post(&pdev->display->egl_go);
	sem_wait(&pdev->display->egl_done);
	return (void *)dst_gb->getNativeBuffer()->handle;
}

static void createDri3XRenderPicture (struct waydroid_hwc_composer_device_1 *pdev, hwc_layer_1_t *layer, struct window *window, struct buffer *buf, int pixel_stride) {
	int width,height, stride, format, prime_fd, size;
	int usage = GRALLOC_USAGE_HW_TEXTURE;
    if (pdev->display->gtype == GRALLOC_GBM) {
        struct gralloc_handle_t *drm_handle = (struct gralloc_handle_t *)layer->handle;
        width = drm_handle->width;
        height = drm_handle->height;
        stride = drm_handle->stride;
        format = drm_handle->format;
        usage = drm_handle->usage;
        prime_fd = drm_handle->prime_fd;
    }else if (pdev->display->gtype == GRALLOC_FTG340) {
        const gc_private_handle_t *gc_handle = (const gc_private_handle_t *)layer->handle;
        width = gc_handle->width;
        height = gc_handle->height;
        stride = gc_handle->stride;
        format = gc_handle->format;
        prime_fd = gc_handle->prime_fd;
    }else if (pdev->display->gtype == GRALLOC_X100) {
        const X100_native_handle_t *x100_handle = (const X100_native_handle_t *)layer->handle;
        width = x100_handle->iWidth;
        height = x100_handle->iHeight;
        stride = pixel_stride * 4;
        format = x100_handle->iFormat;
        prime_fd = x100_handle->fd[0];
   // }else if (pdev->display->gtype == GRALLOC_CROS){
    }else { 
	const struct cros_gralloc_handle *cros_handle = (const struct cros_gralloc_handle *)layer->handle;
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
		(native_handle_t*)layer->handle, android::GraphicBuffer::WRAP_HANDLE,
		width, height, format, 1, uint64_t(usage), stride_for_src_gb);
	    if (src_gb->initCheck() != android::NO_ERROR) {
		ALOGE("Failed to create source GraphicBuffer from handle");
		return ;
	    }
            if (!produce_BGRA_8888(pdev, src_gb, dst_gb)) {
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
	//ALOGE("Found  app: %s layer  ,drop %d", window->appID.c_str(), lastlayer);
        xcb_window_t xcbwindow = window->xcbwindow;
        int x11_fd = dup(prime_fd);
        if (x11_fd >= 0) {
            fcntl(x11_fd, F_SETFD, FD_CLOEXEC);
        }else {
            ALOGE("dup fd failed");
            return ;
        }

        buf->xcbpixmap = xcb_generate_id(pdev->display->xcbconnection);
        XRenderPictureAttributes pa;
        pa.repeat = False;
        xcb_void_cookie_t pixmap_cookie = xcb_dri3_pixmap_from_buffer(pdev->display->xcbconnection,
            buf->xcbpixmap, xcbwindow, size, width, height, stride, 32, 32, x11_fd);
        xcb_generic_error_t *pixmap_error = xcb_request_check(pdev->display->xcbconnection, pixmap_cookie);
        if (pixmap_error) {
           ALOGE("XCB error in xcb_dri3_pixmap_from_buffer: %d", pixmap_error->error_code);
           free(pixmap_error);
           close(x11_fd);
           return ;
        }
        buf->xpicture = XRenderCreatePicture(pdev->display->x11display, buf->xcbpixmap,pdev->display->argb_format, CPRepeat, &pa);
        close(x11_fd);
    }else{
        int x11_fd = dup(prime_fd);
        if (x11_fd >= 0) {
            fcntl(x11_fd, F_SETFD, FD_CLOEXEC);
        }else {
            ALOGE("dup fd failed");
            return ;
        }

        buf->xcbpixmap = xcb_generate_id(pdev->display->xcbconnection);
        XRenderPictureAttributes pa;
        pa.repeat = False;
        xcb_void_cookie_t pixmap_cookie = xcb_dri3_pixmap_from_buffer(pdev->display->xcbconnection,
            buf->xcbpixmap, pdev->display->xcbscreen->root, size, width, height, stride, 32, 32, x11_fd);
        xcb_generic_error_t *pixmap_error = xcb_request_check(pdev->display->xcbconnection, pixmap_cookie);
        if (pixmap_error) {
           ALOGE("XCB error in xcb_dri3_pixmap_from_buffer: %d", pixmap_error->error_code);
           free(pixmap_error);
           close(x11_fd);
           return ;
        }
        buf->xpicture = XRenderCreatePicture(pdev->display->x11display, buf->xcbpixmap,pdev->display->argb_format, CPRepeat, &pa);
        close(x11_fd);
    }
}

static struct buffer *get_wl_buffer(struct waydroid_hwc_composer_device_1 *pdev, hwc_layer_1_t *layer, size_t pos,struct window *window)
{
    uint32_t format;
    uint32_t pixel_stride;
    uint32_t width;
    uint32_t height;
    if (layer->compositionType == HWC_FRAMEBUFFER_TARGET) {
        format = pdev->display->target_layer_handle_ext.format;
        pixel_stride = pdev->display->target_layer_handle_ext.stride;
        width = pdev->display->target_layer_handle_ext.width;
        height = pdev->display->target_layer_handle_ext.height;
    } else {
        format = pdev->display->layer_handles_ext[pos].format;
        pixel_stride = pdev->display->layer_handles_ext[pos].stride;
        width = pdev->display->layer_handles_ext[pos].width;
        height = pdev->display->layer_handles_ext[pos].height;
    }

    if (!width)
        width = layer->displayFrame.right - layer->displayFrame.left;
    if (!height)
        height = layer->displayFrame.bottom - layer->displayFrame.top;

    auto it = pdev->display->buffer_map.find(layer->handle);
    if (it != pdev->display->buffer_map.end()) {
        if (it->second->isShm) {
            if (width != it->second->width || height != it->second->height) {
                destroy_buffer(pdev->display, it->second);
                pdev->display->buffer_map.erase(it);
            } else {
                update_shm_pixmap(pdev->display, it->second,window);
                return it->second;
            }
        } else {
            return it->second;
	}
    }

    struct buffer *buf;
    int ret = 0;

    buf = new struct buffer();
    buf->xcbpixmap = 0;
    buf->xpicture = 0;

    if (pdev->display->gtype == GRALLOC_GBM) {
        struct gralloc_handle_t *drm_handle = (struct gralloc_handle_t *)layer->handle;
        buf->width=drm_handle->width;
        buf->height=drm_handle->height;
	buf->pixel_stride = pixel_stride;
        createDri3XRenderPicture(pdev, layer, window, buf,pixel_stride);
        if (!buf->xpicture) {
            delete buf;
            return NULL;
        }
    } else if (pdev->display->gtype == GRALLOC_RANCHU) {
        struct cb_handle_t* cb_handle = (struct cb_handle_t*)layer->handle;
        auto width = cb_handle->width;
        auto height = cb_handle->height;
        auto hal_format = cb_handle->format;
	create_shm_buffer(buf, width, height, hal_format,pixel_stride,layer->handle);
	buf->xcbpixmap = xcb_generate_id(pdev->display->xcbconnection);
	xcb_void_cookie_t create_pixmap_cookie;
	if (window != NULL){
		create_pixmap_cookie = xcb_create_pixmap(
		    pdev->display->xcbconnection, 32, buf->xcbpixmap, window->xcbwindow, width, height);
	}else {
		create_pixmap_cookie = xcb_create_pixmap(
		    pdev->display->xcbconnection, 32, buf->xcbpixmap, pdev->display->xcbscreen->root, width, height);
	}
	xcb_generic_error_t *pixmap_error = xcb_request_check(pdev->display->xcbconnection, create_pixmap_cookie);
	if (pixmap_error) {
	    ALOGE("XCB error in xcb_create_pixmap: %d\n", pixmap_error->error_code);
	    free(pixmap_error);
	    delete buf;
	    return NULL;
	}
	update_shm_pixmap(pdev->display, buf,window);
        XRenderPictureAttributes pa;
        pa.repeat = False;
        buf->xpicture = XRenderCreatePicture(pdev->display->x11display, buf->xcbpixmap,
            pdev->display->argb_format, CPRepeat, &pa);
        if (!buf->xpicture) {
            delete buf;
            return NULL;
        }
    } else if (pdev->display->gtype == GRALLOC_CROS) {
        const struct cros_gralloc_handle *cros_handle = (const struct cros_gralloc_handle *)layer->handle;
        buf->width=cros_handle->width;
        buf->height=cros_handle->height;
        createDri3XRenderPicture(pdev, layer, window, buf,pixel_stride);
        if (!buf->xpicture) {
            delete buf;
            return NULL;
        }
    } else if (pdev->display->gtype == GRALLOC_X100) {
        const X100_native_handle_t *x100_handle = (const X100_native_handle_t *)layer->handle;
        buf->width=x100_handle->iWidth;
        buf->height=x100_handle->iHeight;
        createDri3XRenderPicture(pdev, layer, window, buf,pixel_stride);
        if (! buf->xpicture) {
            delete buf;
            return NULL;
        }
    } else if (pdev->display->gtype == GRALLOC_FTG340) {
        const gc_private_handle_t *gc_handle = (const gc_private_handle_t *)layer->handle;
	    buf->width=gc_handle->width;
	    buf->height=gc_handle->height;
        createDri3XRenderPicture(pdev, layer, window, buf,pixel_stride);
        if (!buf->xpicture) {
            delete buf;
            return NULL;
        }
    } else {
        ALOGE("unsupport gralloc type %d", pdev->display->gtype);
        delete buf;
        return NULL;
    }

    if (ret) {
        ALOGE("failed to create a x11 pixcture");
        return NULL;
    }
    pdev->display->buffer_map[layer->handle] = buf;
    return pdev->display->buffer_map[layer->handle];
}


static int adjust_window_geo(struct waydroid_hwc_composer_device_1 * pdev, hwc_layer_1_t * layer, struct buffer *buf, struct window *window, bool use_subsurface){
    if (!use_subsurface)
        return  0;
    hwc_rect_t sourceCrop = layer->sourceCropi;

    if (layer->transform & HWC_TRANSFORM_ROT_90) {
        sourceCrop.left = layer->sourceCropi.top;
        sourceCrop.top = layer->sourceCropi.left;
        sourceCrop.right = layer->sourceCropi.bottom;
        sourceCrop.bottom = layer->sourceCropi.right;
    }
    //ALOGE("frame geo left %d top %d right %d bottom %d lastlayer %d", sourceCrop.left,sourceCrop.top, sourceCrop.right,sourceCrop.bottom, window->lastLayer);
    xcb_configure_window_value_list_t values;
    values.x = floor(layer->displayFrame.left / pdev->display->scale);
    values.y = floor(layer->displayFrame.top / pdev->display->scale);
    // uint16_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
   //xcb_configure_window(pdev->display->xcbconnection, window->xcbwindow, mask, (uint32_t*)&values);
    // Calculate source crop dimensions
    int src_x = fmax(0, sourceCrop.left);
    int src_y = fmax(0, sourceCrop.top);
    int src_width = fmax(1, sourceCrop.right - sourceCrop.left);
    int src_height = fmax(1, sourceCrop.bottom - sourceCrop.top);

    // Calculate destination dimensions (scaled)
    int dst_width = fmax(1, ceil((layer->displayFrame.right - layer->displayFrame.left) / pdev->display->scale));
    int dst_height = fmax(1, ceil((layer->displayFrame.bottom - layer->displayFrame.top) / pdev->display->scale));


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
        XRenderSetPictureTransform(pdev->display->x11display, buf->xpicture, &scale_transform);
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
        XRenderSetPictureTransform(pdev->display->x11display, buf->xpicture, &identity_transform);
    }

    if (use_subsurface) {
	if (window->lastLayer == 0) {
		window->rects.clear();
		window->crops.clear();
	}
        xcb_rectangle_t rect;
        rect = {static_cast<int16_t>(values.x), static_cast<int16_t>(values.y), static_cast<uint16_t>(dst_width),static_cast<uint16_t>(dst_height)};
        window->rects.push_back(rect);
        window->crops.push_back(sourceCrop);
    }



    ALOGI("src x %d y%d dst width %d dst height %d values.x %d, values.y %d app %s lastLayer %d  display right%d", src_x,src_y, dst_width, dst_height,
			values.x,values.y,window->appID.c_str(), window->lastLayer, layer->displayFrame.right);
    XRenderComposite(pdev->display->x11display, PictOpOver, buf->xpicture, None1, window->backxpicture,
                  src_x, src_y, 0, 0, values.x, values.y, dst_width, dst_height);

    return 0;
}

static long time_to_sleep_to_next_vsync(struct timespec *rt, uint64_t last_vsync_ns, unsigned vsync_period_ns)
{
    uint64_t now = (uint64_t)rt->tv_sec * 1e9 + rt->tv_nsec;
    uint64_t frames_since_last_vsync = (now - last_vsync_ns) / vsync_period_ns + 1;
    uint64_t next_vsync = last_vsync_ns + frames_since_last_vsync * vsync_period_ns;

    return next_vsync - now;
}

static void* hwc_vsync_thread(void* data) {
    struct waydroid_hwc_composer_device_1* pdev = (struct waydroid_hwc_composer_device_1*)data;
    setpriority(PRIO_PROCESS, 0, HAL_PRIORITY_URGENT_DISPLAY);

    struct timespec rt;
    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in vsync thread clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }
    bool vsync_enabled = false;

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

        int64_t timestamp = (uint64_t)rt.tv_sec * 1e9 + rt.tv_nsec;
        pdev->procs->vsync(pdev->procs, 0, timestamp);
        ATRACE_END();
    }

    return NULL;
}

void get_input_shape(xcb_connection_t *conn, xcb_window_t window) {
    if (!xcb_get_extension_data(conn, &xcb_shape_id)->present) {
        ALOGE("Shape extension not available\n");
        return;
    }

    xcb_shape_get_rectangles_reply_t *reply = xcb_shape_get_rectangles_reply(
        conn, xcb_shape_get_rectangles(conn, window, XCB_SHAPE_SK_INPUT), NULL);
    if (!reply) {
        ALOGE("Failed to query input shape\n");
        return;
    }

    int num_rects = xcb_shape_get_rectangles_rectangles_length(reply);
    xcb_rectangle_t *rects = xcb_shape_get_rectangles_rectangles(reply);
    ALOGI("hwc_set Current input shape (%d rectangles):\n", num_rects);
    for (int i = 0; i < num_rects; i++) {
        ALOGE("  hwc_set rectangle %d: x=%d, y=%d, width=%u, height=%u\n",
               i, rects[i].x, rects[i].y, rects[i].width, rects[i].height);
    }

    free(reply);
}

static int hwc_set(struct hwc_composer_device_1* dev,size_t numDisplays,
                   hwc_display_contents_1_t** displays) {
    char property[PROPERTY_VALUE_MAX];
    struct waydroid_hwc_composer_device_1* pdev = (struct waydroid_hwc_composer_device_1*)dev;

    if (!numDisplays || !displays) {
        return 0;
    }

    hwc_display_contents_1_t* contents = displays[HWC_DISPLAY_PRIMARY];
    size_t fb_target = -1;
    int err = 0;
    if (pdev->display->geo_changed) {
        for (auto it = pdev->display->buffer_map.begin(); it != pdev->display->buffer_map.end(); it++) {
            if (it->second) {
                destroy_buffer(pdev->display, it->second);
            }
        }
        pdev->display->buffer_map.clear();
    }

    std::pair<int, int> skipped(-1, -1);
    if (pdev->use_subsurface && !pdev->multi_windows) {
        for (size_t i = 0; i < contents->numHwLayers; i++) {
          if (!(contents->hwLayers[i].flags & HWC_SKIP_LAYER))
            continue;

          if (skipped.first == -1)
            skipped.first = i;
          skipped.second = i;
        }
    }


    /*
     * In prop "persist.waydroid.multi_windows" we detect HWC let SF rander layers
     * And just show the target client layer (single windows mode) or
     * render each layers in wayland surface and subsurfaces.
     * In prop "openfde.active_apps" we choose what to be shown in window
     * and here if HWC is in single mode we show the screen only if any task are in screen
     * and in multi windows mode we group layers with same task ID in a wayland window.
     * And in prop "waydroid.blacklist_apps" we select apps to not show in display.
     *
     * "openfde.active_apps" prop can be:
     * "none": No windows
     * "Waydroid": Shows android screen in a single window
     * "AppID": Shows apps in related windows as explained above
     */
    property_get("openfde.active_apps", property, "none");
    std::string active_apps = std::string(property);
    std::string blacklist_apps = std::string("com.android.launcher3");
    property_get("openfde.blacklist_apps", property, "");
    if (strlen(property) > 0 && strncmp(property, "com.android.launcher3", strlen("com.android.launcher3")) != 0) {
        blacklist_apps = blacklist_apps + ":" + std::string(property);
    }
    std::string single_layer_tid;
    std::string single_layer_aid;

    if (active_apps != "Openfde" && !property_get_bool("openfde.background_start", true)) {
        for (size_t l = 0; l < contents->numHwLayers; l++) {
            std::string layer_name = pdev->display->layer_names[l];
            if (layer_name.rfind("BootAnimation#", 0) == 0) {
                // force single window mode during boot animation
                active_apps = "Openfde";
                break;
            }
        }
    }

    std::scoped_lock lock(pdev->display->windowsMutex);
    if (active_apps == "none") {
        // Clear all open windows
        for (auto it = pdev->windows.begin(); it != pdev->windows.end(); it++) {
            if (it->second)
                destroy_window(it->second);
        }
        pdev->windows.clear();
        for (size_t layer = 0; layer < contents->numHwLayers; layer++) {
            hwc_layer_1_t* fb_layer = &contents->hwLayers[layer];
            if (fb_layer->acquireFenceFd != -1)
                close(fb_layer->acquireFenceFd);
        }

        property_set("openfde.open_windows", "0");
        goto sync;
    } else if (active_apps == "Openfde") {
        // Clear all open windows if there's any and just keep "Waydroid"
        if (pdev->windows.find(active_apps) == pdev->windows.end() || !pdev->windows[active_apps]->isActive) {
            for (auto it = pdev->windows.begin(); it != pdev->windows.end(); it++) {
                if (it->second) {
                    destroy_window(it->second);
                }
            }
            pdev->windows.clear();
        } else {
            pdev->windows[active_apps]->lastLayer = 0;
            pdev->windows[active_apps]->last_layer_buffer = nullptr;
        }
    } else if (!pdev->multi_windows) {
        // Single window mode, detecting if any unblacklisted app is on screen
        bool showWindow = false;
        for (size_t l = 0; l < contents->numHwLayers; l++) {
            std::string layer_name = pdev->display->layer_names[l];
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
                        if (pdev->windows.find(single_layer_tid) != pdev->windows.end()) {
                            pdev->windows[single_layer_tid]->lastLayer = 0;
                            pdev->windows[single_layer_tid]->last_layer_buffer = nullptr;
                        }
                    }
                }
            }
        }
        // Nothing to show on screen, so clear all open windows
        if (!showWindow) {
            for (auto it = pdev->windows.begin(); it != pdev->windows.end(); it++) {
                if (it->second)
                    destroy_window(it->second);
            }
            pdev->windows.clear();
            for (size_t layer = 0; layer < contents->numHwLayers; layer++) {
                hwc_layer_1_t* fb_layer = &contents->hwLayers[layer];
                if (fb_layer->acquireFenceFd != -1)
                    close(fb_layer->acquireFenceFd);
            }

            property_set("openfde.open_windows", "0");
            goto sync;
        }
        bool shouldCloseLeftover = true;
        for (auto it = pdev->windows.cbegin(); it != pdev->windows.cend();) {
            if (it->second) {
                // This window is closed, but android is still showing leftover layers, we detect it here
                if (!it->second->isActive || it->first == "Openfde") {
                    for (size_t l = 0; l < contents->numHwLayers; l++) {
                        std::string layer_name = pdev->display->layer_names[l];
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
                        pdev->windows.erase(it++);
                        shouldCloseLeftover = true;
                        std::string windows_size_str = std::to_string(pdev->windows.size());
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
        for (auto it = pdev->windows.cbegin(); it != pdev->windows.cend();) {
            bool foundApp = false;
            for (size_t l = 0; l < contents->numHwLayers; l++) {
                if (contents->hwLayers[l].compositionType != HWC_OVERLAY)
                    continue;
                std::string layer_name = pdev->display->layer_names[l];
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
                pdev->windows.erase(it++);
                std::string windows_size_str = std::to_string(pdev->windows.size());
                property_set("openfde.open_windows", windows_size_str.c_str());
            } else {
                ++it;
            }
        }
    }

    for (size_t l = 0; l < contents->numHwLayers; l++) {
        hwc_layer_1_t* fb_layer = &contents->hwLayers[l];
        if (fb_layer->compositionType == HWC_FRAMEBUFFER_TARGET) {
            fb_target = l;
            break;
        }
    }

    for (size_t l = 0; l < contents->numHwLayers; l++) {
        size_t layer = l;
        if (l == skipped.first && fb_target >= 0) {
            // draw framebuffer target instead of skipped layers
            if (contents->hwLayers[layer].acquireFenceFd != -1) {
                close(contents->hwLayers[layer].acquireFenceFd);
            }
            layer = fb_target;
        }
        if (skipped.first >= 0 && l == fb_target) {
            // don't handle fb_target twice
            continue;
        }

        hwc_layer_1_t* fb_layer = &contents->hwLayers[layer];

        if (fb_layer->flags & HWC_SKIP_LAYER) {
            if (fb_layer->acquireFenceFd != -1) {
                close(fb_layer->acquireFenceFd);
            }
            continue;
        }

	//if ((fb_layer->flags & HWC_IS_CURSOR_LAYER) && pdev->display->cursor_surface) {
	if (fb_layer->flags & HWC_IS_CURSOR_LAYER)  {
            // Cursor was already handled separately
            if (fb_layer->acquireFenceFd != -1) {
                close(fb_layer->acquireFenceFd);
            }
            continue;
        }

        if (fb_layer->compositionType !=
            (pdev->use_subsurface ? HWC_OVERLAY : HWC_FRAMEBUFFER_TARGET) && layer == l) {
            if (fb_layer->acquireFenceFd != -1) {
                close(fb_layer->acquireFenceFd);
            }
            continue;
        }

        if (!fb_layer->handle) {
            if (fb_layer->acquireFenceFd != -1) {
                close(fb_layer->acquireFenceFd);
            }
            continue;
        }

        struct window *window = NULL;
        std::string layer_name = pdev->display->layer_names[layer];

        if (active_apps == "Openfde") {
            // Show everything in a single window
            if (pdev->windows.find(active_apps) == pdev->windows.end()) {
                pdev->windows[active_apps] = create_window(pdev->display, pdev->use_subsurface, active_apps, "0", {0, 0, 0, 255});
                std::string windows_size_str = std::to_string(pdev->windows.size());
                property_set("openfde.open_windows", windows_size_str.c_str());
            }
            window = pdev->windows[active_apps];
        } else if (!pdev->multi_windows) {
            if (single_layer_tid.length()) {
                if (pdev->windows.find(single_layer_tid) == pdev->windows.end()) {
                    pdev->windows[single_layer_tid] = create_window(pdev->display, pdev->use_subsurface, single_layer_aid, single_layer_tid, {0, 0, 0, 255});
                    std::string windows_size_str = std::to_string(pdev->windows.size());
                    property_set("openfde.open_windows", windows_size_str.c_str());
                }
                window = pdev->windows[single_layer_tid];
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
                    if (pdev->windows.find(layer_tid) == pdev->windows.end()) {
                        pdev->windows[layer_tid] = create_window(pdev->display, pdev->use_subsurface, layer_aid, layer_tid, {0, 0, 0, 0});
                        std::string windows_size_str = std::to_string(pdev->windows.size());
                        property_set("openfde.open_windows", windows_size_str.c_str());
                    }
                    if (pdev->windows.find(layer_tid) != pdev->windows.end())
                        window = pdev->windows[layer_tid];
                }
            }
        }
        // Detecting cursor layer
        if (!window) {
            std::string LayerRawName;
            std::istringstream issLayer(layer_name);
            std::getline(issLayer, LayerRawName, '#');
	        if (pdev->multi_windows && ((LayerRawName == "Toast")
	            || (LayerRawName.find("Application Not Responding:") !=  std::string::npos))) {
                if (pdev->windows.find(LayerRawName) == pdev->windows.end()) {
                    pdev->windows[LayerRawName] = create_window(pdev->display, pdev->use_subsurface, LayerRawName, "none", {0, 0, 0, 0});
                    std::string windows_size_str = std::to_string(pdev->windows.size());
                    property_set("openfde.open_windows", windows_size_str.c_str());
                }
                if (pdev->windows.find(LayerRawName) != pdev->windows.end()) {
                    window = pdev->windows[LayerRawName];
                }
            }
        }

        if (!window || !window->isActive) {
            if (fb_layer->acquireFenceFd != -1) {
                close(fb_layer->acquireFenceFd);
            }
            continue;
        }

        struct buffer *buf = get_wl_buffer(pdev, fb_layer, layer,window);
        if (!buf) {
            ALOGE("Failed to get wayland buffer");
            if (fb_layer->acquireFenceFd != -1) {
               close(fb_layer->acquireFenceFd);
            }
            continue;
        }

        // TODO: Implement per-layer explicit synchronization
        fb_layer->releaseFenceFd = -1;

	    XTransform transform;
        switch (fb_layer->transform) {
            case HWC_TRANSFORM_FLIP_H:
                transform.matrix[0][0] = XDoubleToFixed(-1.0); // scale x by -1
                transform.matrix[0][1] = XDoubleToFixed(0.0);
                transform.matrix[0][2] = XDoubleToFixed(buf->width);
                transform.matrix[1][0] = XDoubleToFixed(0.0);
                transform.matrix[1][1] = XDoubleToFixed(1.0);
                transform.matrix[1][2] = XDoubleToFixed(0.0);
                transform.matrix[2][0] = XDoubleToFixed(0.0);
                transform.matrix[2][1] = XDoubleToFixed(0.0);
                transform.matrix[2][2] = XDoubleToFixed(1.0);
                XRenderSetPictureTransform(pdev->display->x11display, buf->xpicture, &transform);
		        break;
            case HWC_TRANSFORM_FLIP_V:
                transform.matrix[0][0] = XDoubleToFixed(1.0);
                transform.matrix[0][1] = XDoubleToFixed(0.0);
                transform.matrix[0][2] = XDoubleToFixed(0.0);
                transform.matrix[1][0] = XDoubleToFixed(0.0);
                transform.matrix[1][1] = XDoubleToFixed(-1.0); // scale y by -1
                transform.matrix[1][2] = XDoubleToFixed(buf->height);
                transform.matrix[2][0] = XDoubleToFixed(0.0);
                transform.matrix[2][1] = XDoubleToFixed(0.0);
                transform.matrix[2][2] = XDoubleToFixed(1.0);
                XRenderSetPictureTransform(pdev->display->x11display, buf->xpicture, &transform);
                break;
            case HWC_TRANSFORM_ROT_90:
                transform.matrix[0][0] = XDoubleToFixed(0.0);
                transform.matrix[0][1] = XDoubleToFixed(-1.0);
                transform.matrix[0][2] = XDoubleToFixed(buf->height);
                transform.matrix[1][0] = XDoubleToFixed(1.0);
                transform.matrix[1][1] = XDoubleToFixed(0.0);
                transform.matrix[1][2] = XDoubleToFixed(0.0);
                transform.matrix[2][0] = XDoubleToFixed(0.0);
                transform.matrix[2][1] = XDoubleToFixed(0.0);
                transform.matrix[2][2] = XDoubleToFixed(1.0);
                XRenderSetPictureTransform(pdev->display->x11display, buf->xpicture, &transform);
                break;
            case HWC_TRANSFORM_ROT_180:
                transform.matrix[0][0] = XDoubleToFixed(-1.0);
                transform.matrix[0][1] = XDoubleToFixed(0.0);
                transform.matrix[0][2] = XDoubleToFixed(buf->width);
                transform.matrix[1][0] = XDoubleToFixed(0.0);
                transform.matrix[1][1] = XDoubleToFixed(-1.0);
                transform.matrix[1][2] = XDoubleToFixed(buf->height);
                transform.matrix[2][0] = XDoubleToFixed(0.0);
                transform.matrix[2][1] = XDoubleToFixed(0.0);
                transform.matrix[2][2] = XDoubleToFixed(1.0);
                XRenderSetPictureTransform(pdev->display->x11display, buf->xpicture, &transform);
                break;
            case HWC_TRANSFORM_ROT_270:
                transform.matrix[0][0] = XDoubleToFixed(0.0);
                transform.matrix[0][1] = XDoubleToFixed(1.0);
                transform.matrix[0][2] = XDoubleToFixed(0.0);
                transform.matrix[1][0] = XDoubleToFixed(-1.0);
                transform.matrix[1][1] = XDoubleToFixed(0.0);
                transform.matrix[1][2] = XDoubleToFixed(buf->width);
                transform.matrix[2][0] = XDoubleToFixed(0.0);
                transform.matrix[2][1] = XDoubleToFixed(0.0);
                transform.matrix[2][2] = XDoubleToFixed(1.0);
                XRenderSetPictureTransform(pdev->display->x11display, buf->xpicture, &transform);
                break;
            case HWC_TRANSFORM_FLIP_H_ROT_90:
                transform.matrix[0][0] = XDoubleToFixed(0.0);
                transform.matrix[0][1] = XDoubleToFixed(1.0);
                transform.matrix[0][2] = XDoubleToFixed(0.0);
                transform.matrix[1][0] = XDoubleToFixed(1.0);
                transform.matrix[1][1] = XDoubleToFixed(0.0);
                transform.matrix[1][2] = XDoubleToFixed(0.0);
                transform.matrix[2][0] = XDoubleToFixed(0.0);
                transform.matrix[2][1] = XDoubleToFixed(0.0);
                transform.matrix[2][2] = XDoubleToFixed(1.0);
                XRenderSetPictureTransform(pdev->display->x11display, buf->xpicture, &transform);
                break;
            case HWC_TRANSFORM_FLIP_V_ROT_90:
                transform.matrix[0][0] = XDoubleToFixed(0.0);
                transform.matrix[0][1] = XDoubleToFixed(-1.0);
                transform.matrix[0][2] = XDoubleToFixed(buf->height);
                transform.matrix[1][0] = XDoubleToFixed(-1.0);
                transform.matrix[1][1] = XDoubleToFixed(0.0);
                transform.matrix[1][2] = XDoubleToFixed(buf->width);
                transform.matrix[2][0] = XDoubleToFixed(0.0);
                transform.matrix[2][1] = XDoubleToFixed(0.0);
                transform.matrix[2][2] = XDoubleToFixed(1.0);
                XRenderSetPictureTransform(pdev->display->x11display, buf->xpicture, &transform);
                break;
            default:
                break;
	    }
      
        if (pdev->use_subsurface ) {
        	adjust_window_geo(pdev, fb_layer, buf,window, pdev->use_subsurface);
        }else {
	     	XRenderComposite(pdev->display->x11display, PictOpOver, buf->xpicture, None1, window->xpicture,
		   	 0, 0, 0, 0, 0,0, pdev->display->width, pdev->display->height);
        }
        window->last_layer_buffer = buf;
        window->lastLayer++;

        if (window->snapshot_buffer) {
           // Snapshot buffer should be detached by now, clean up
            destroy_buffer(pdev->display,window->snapshot_buffer);
            window->snapshot_buffer = nullptr;
        }

        const int kAcquireWarningMS = 100;
        err = sync_wait(fb_layer->acquireFenceFd, kAcquireWarningMS);
        if (err < 0 && errno == ETIME) {
            ALOGE("hwcomposer waited on fence %d for %d ms",
            fb_layer->acquireFenceFd, kAcquireWarningMS);
        }
        close(fb_layer->acquireFenceFd);
    }

    // Layers order is changed from SF so we rearrange wayland surfaces
     if (pdev->use_subsurface)
        for (auto it = pdev->windows.begin(); it != pdev->windows.end(); it++)
            if (it->second){
                if (it->second->rects.size() > 1)
                {
                    set_black_background(pdev,it->second);
                }
                XRenderComposite(pdev->display->x11display, PictOpSrc, it->second->backxpicture, None1, it->second->xpicture,
                    0, 0, 0, 0, 0,0, pdev->display->width, pdev->display->height);
                if (!it->second->rects.empty()) {
                    xcb_shape_rectangles(pdev->display->xcbconnection,
                                        XCB_SHAPE_SO_SET,      // 设置操作
                                        XCB_SHAPE_SK_INPUT,    // 输入形状
                                        XCB_CLIP_ORDERING_UNSORTED,
                                        it->second->xcbwindow,
                                        0, 0,
                                        it->second->rects.size(),
                                        it->second->rects.data());
                }
	}
    XFlush(pdev->display->x11display);
    if (pdev->display->geo_changed) {
        for (auto it = pdev->windows.begin(); it != pdev->windows.end(); it++) {
            if (it->second) {
                // This window has no changes in layers, leaving it
                if (!it->second->lastLayer)
                    continue;
                // Clear the window's back xpicture
		if (it->second->backxpicture) {
			XRenderColor clear_color = {0, 0, 0, 0}; // Transparent black
			XRenderFillRectangle(pdev->display->x11display, PictOpClear, it->second->backxpicture, &clear_color, 
			    0, 0, pdev->display->width, pdev->display->height);
		}
            }
        }
        pdev->display->geo_changed = false;
    }
    xcb_flush(pdev->display->xcbconnection); // 确保请求发送

sync:
    sw_sync_timeline_inc(pdev->timeline_fd, 1);
    contents->retireFenceFd = sw_sync_fence_create(pdev->timeline_fd, "hwc_contents_release", ++pdev->next_sync_point);

    return err;
}

static int hwc_query(struct hwc_composer_device_1* dev, int what, int* value) {
    struct waydroid_hwc_composer_device_1* pdev =
            (struct waydroid_hwc_composer_device_1*)dev;

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

static int hwc_event_control(struct hwc_composer_device_1* dev, int dpy __unused,
                             int event, int enabled) {
    struct waydroid_hwc_composer_device_1* pdev =
            (struct waydroid_hwc_composer_device_1*)dev;
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

static int hwc_blank(struct hwc_composer_device_1* dev __unused, int disp __unused,
                     int blank __unused) {
    return 0;
}

static void hwc_dump(hwc_composer_device_1* dev __unused, char* buff __unused,
                     int buff_len __unused) {
    // This is run when running dumpsys.
    // No-op for now.
}


static int hwc_get_display_configs(struct hwc_composer_device_1* dev __unused,
                                   int disp, uint32_t* configs, size_t* numConfigs) {
    if (*numConfigs == 0) {
        return 0;
    }

    if (disp == HWC_DISPLAY_PRIMARY) {
        configs[0] = 0;
        *numConfigs = 1;
        return 0;
    }

    return -EINVAL;
}


static int32_t hwc_attribute(struct waydroid_hwc_composer_device_1* pdev,
                             const uint32_t attribute) {
    char property[PROPERTY_VALUE_MAX];
    int width = floor(pdev->display->width * pdev->display->scale);
    int height = floor(pdev->display->height * pdev->display->scale);
    int density = 180;

    switch(attribute) {
        case HWC_DISPLAY_VSYNC_PERIOD:
            return pdev->vsync_period_ns;
        case HWC_DISPLAY_WIDTH: {
            if (property_get("persist.openfde.width_padding", property, nullptr) > 0)
                width -= atoi(property);
            std::string width_str = std::to_string(width);
            property_set("openfde.display_width", width_str.c_str());
            return width;
        }
        case HWC_DISPLAY_HEIGHT: {
            if (property_get("persist.openfde.height_padding", property, nullptr) > 0)
                height -= atoi(property);
            std::string height_str = std::to_string(height);
            property_set("openfde.display_height", height_str.c_str());
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

static int hwc_get_display_attributes(struct hwc_composer_device_1* dev __unused,
                                      int disp, uint32_t config __unused,
                                      const uint32_t* attributes, int32_t* values) {
    struct waydroid_hwc_composer_device_1* pdev = (struct waydroid_hwc_composer_device_1*)dev;
    for (int i = 0; attributes[i] != HWC_DISPLAY_NO_ATTRIBUTE; i++) {
        if (disp == HWC_DISPLAY_PRIMARY) {
            values[i] = hwc_attribute(pdev, attributes[i]);
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

static int hwc_close(hw_device_t* dev) {
    struct waydroid_hwc_composer_device_1* pdev = (struct waydroid_hwc_composer_device_1*)dev;

    for (std::map<buffer_handle_t, struct buffer *>::iterator it = pdev->display->buffer_map.begin(); it != pdev->display->buffer_map.end(); it++)
    {
        destroy_buffer(pdev->display,it->second);
    }
    pdev->display->buffer_map.clear();

    destroy_display(pdev->display);

    delete dev;
    return 0;
}


static void* hwc_extension_thread(void* data) {
    struct waydroid_hwc_composer_device_1* pdev = (struct waydroid_hwc_composer_device_1*)data;
    sp<IOpenfdeDisplay> waydroidDisplay;
    status_t status;

    setpriority(PRIO_PROCESS, 0, HAL_PRIORITY_URGENT_DISPLAY);

    waydroidDisplay = new OpenfdeDisplay(pdev->display);
    if (waydroidDisplay == nullptr) {
        ALOGE("Can not create an instance of Waydroid Display HAL, exiting.");
        goto shutdown;
    }

    //configureRpcThreadpool(1, true /*callerWillJoin*/);

    status = waydroidDisplay->registerAsService();
    if (status != OK) {
        ALOGE("Could not register service for Waydroid Display HAL (%d).", status);
    }

    ALOGI("Waydroid Display HAL thread is ready.");
    joinRpcThreadpool();
    // Should not pass this line

shutdown:
    // In normal operation, we don't expect the thread pool to shutdown
    ALOGE("Waydroid Display HAL service is shutting down.");
    return NULL;
}

static void* hwc_window_service_thread(void* data) {
    struct waydroid_hwc_composer_device_1* pdev = (struct waydroid_hwc_composer_device_1*)data;
    sp<IOpenfdeWindow> waydroidWindow;
    status_t status;

    //configureRpcThreadpool(1, true /*callerWillJoin*/);

    waydroidWindow = new OpenfdeWindow(pdev->display, &pdev->windows);
    if (waydroidWindow == nullptr) {
        ALOGE("Can not create an instance of Waydroid Window HAL, exiting.");
        goto shutdown;
    }


    status = waydroidWindow->registerAsService();
    if (status != OK) {
        ALOGE("Could not register service for Waydroid Window HAL (%d).", status);
    }

    ALOGI("Waydroid Window HAL thread is ready.");
    joinRpcThreadpool();
    // Should not pass this line

shutdown:
    // In normal operation, we don't expect the thread pool to shutdown
    ALOGE("Waydroid Window HAL service is shutting down.");
    return NULL;
}

static void hwc_register_procs(struct hwc_composer_device_1* dev,
                               hwc_procs_t const* procs) {
    struct waydroid_hwc_composer_device_1* pdev = (struct waydroid_hwc_composer_device_1*)dev;
    pdev->procs = procs;
}

static int hwc_open(const struct hw_module_t* module, const char* name,
                    struct hw_device_t** device) {
    int ret = 0;
    char property[PROPERTY_VALUE_MAX];

    if (strcmp(name, HWC_HARDWARE_COMPOSER)) {
        ALOGE("%s called with bad name %s", __FUNCTION__, name);
        return -EINVAL;
    }

    waydroid_hwc_composer_device_1 *pdev = new waydroid_hwc_composer_device_1();
    if (!pdev) {
        ALOGE("%s failed to allocate dev", __FUNCTION__);
        return -ENOMEM;
    }

    pdev->base.common.tag = HARDWARE_DEVICE_TAG;
    pdev->base.common.version = HWC_DEVICE_API_VERSION_1_1;
    pdev->base.common.module = const_cast<hw_module_t *>(module);
    pdev->base.common.close = hwc_close;

    pdev->base.prepare = hwc_prepare;
    pdev->base.set = hwc_set;
    pdev->base.eventControl = hwc_event_control;
    pdev->base.blank = hwc_blank;
    pdev->base.query = hwc_query;
    pdev->base.registerProcs = hwc_register_procs;
    pdev->base.dump = hwc_dump;
    pdev->base.getDisplayConfigs = hwc_get_display_configs;
    pdev->base.getDisplayAttributes = hwc_get_display_attributes;

    pdev->vsync_period_ns = 1000*1000*1000/60; // vsync is 60 hz

    pdev->multi_windows = property_get_bool("persist.openfde.multi_windows", false);
    pdev->use_subsurface = property_get_bool("persist.openfde.use_subsurface", false) || pdev->multi_windows;
    pdev->timeline_fd = sw_sync_timeline_create();
    pdev->next_sync_point = 1;

    if(pdev->multi_windows){
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
    if (property_get("ro.hardware.gralloc", property, "default") > 0) {
        pdev->display = create_display(property);
    }
    if (!pdev->display) {
        ALOGE("failed to open x11 connection");
        return -ENODEV;
    }else{
        pdev->display->x11_windows = &pdev->windows;
    }
    ALOGE("wayland display %p", pdev->display);
    pdev->display->mouse_icon_addr = -1;

    pthread_mutex_init(&pdev->vsync_lock, NULL);
    pdev->vsync_callback_enabled = true;

    //create Openfde window to match desktop file openfde.desktop
    auto first_window = create_window(pdev->display, pdev->use_subsurface, "Openfde", "0", {0, 0, 0, 255});
    if (!property_get_bool("openfde.background_start", true)) {
        pdev->windows["Openfde"] = first_window;
        property_set("openfde.active_apps", "Openfde");
        property_set("openfde.open_windows", "1");
    } else {
        destroy_window(first_window);
    }

    if (pdev->display->refresh > 1000 && pdev->display->refresh < 1000000)
        pdev->vsync_period_ns = 1000 * 1000 * 1000 / (pdev->display->refresh / 1000);

    struct timespec rt;
    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
       ALOGE("%s:%d error in vsync thread clock_gettime: %s",
            __FILE__, __LINE__, strerror(errno));
    }

    pdev->last_vsync_ns = int64_t(rt.tv_sec) * 1e9 + rt.tv_nsec;

    if (!pdev->vsync_thread) {
        ret = pthread_create (&pdev->vsync_thread, NULL, hwc_vsync_thread, pdev);
        if (ret) {
            ALOGE("waydroid_hw_composer could not start vsync_thread\n");
        }
    }

    ret = pthread_create (&pdev->extension_thread, NULL, hwc_extension_thread, pdev);
    if (ret) {
        ALOGE("waydroid_hw_composer could not start extension_thread\n");
    }

    ret = pthread_create(&pdev->window_service_thread, NULL, hwc_window_service_thread, pdev);
    if (ret) {
        ALOGE("waydroid_hw_composer could not start window_service_thread\n");
    }

    ret = pthread_create(&pdev->egl_worker_thread, NULL, egl_loop, pdev->display);
    if (ret) {
        ALOGE("waydroid_hw_composer could not start egl_worker_thread");
    }

    *device = &pdev->base.common;

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
