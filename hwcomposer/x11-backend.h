/*
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

#pragma once

#include "hwc_backend.h"

/* X11 协议后端，实现见 x11-backend.cpp（仅 arm64 编译） */
class X11Backend : public HwcBackend {
public:
    HwcProtocol protocol() const override { return HwcProtocol::X11; }

    void preInit() override;
    bool createDisplay(const char *gralloc) override;
    void destroyDisplay() override;

    struct window *createWindow(bool use_subsurfaces, const std::string &appID,
                                const std::string &taskID, hwc_color_t color) override;
    void destroyWindow(struct window *window, bool keep) override;

    struct buffer *getLayerBuffer(hwc_layer_1_t *layer, size_t pos,
                                  struct window *window) override;
    void destroyBuffer(struct buffer *buf) override;

    bool updateCursorSurface(hwc_layer_1_t *fb_layer, size_t layer) override;
    void hideCursor() override;

    bool skipCursorLayers() const override { return true; }
    bool multiWindowOverlayOnly() const override { return true; }
    std::string getBlacklistApps() override;
    bool handleFallbackLayer(const std::string &layerRawName, hwc_layer_1_t *fb_layer,
                             size_t layer, struct window **out_window) override;
    void presentLayer(struct window *window, struct buffer *buf, hwc_layer_1_t *layer) override;
    void endFrame(const std::string &active_apps, const std::string &single_layer_tid) override;

    bool minimizeWindow(const std::string &packageName,
                        std::map<std::string, struct window *> *windows) override;
    void setPointerCapture(const std::string &packageName, bool enabled) override;
    void setIdleInhibit(const std::string &task, bool enabled) override;
};
