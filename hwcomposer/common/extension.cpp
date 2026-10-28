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

#include "extension.h"

namespace vendor {
namespace openfde {
namespace display {
namespace V1_1 {
namespace implementation {

using ::openfde::Hwc2Device;
using ::openfde::Hwc2Display;
using ::openfde::Hwc2Layer;

OpenfdeDisplay::OpenfdeDisplay(Hwc2Device *device)
    : mDevice(device)
{
}

// Methods from ::vendor::openfde::display::V1_0::IOpenfdeDisplay follow.
Return<Error> OpenfdeDisplay::setLayerName(uint64_t display, uint64_t layer,
                                           const hidl_string &name) {
    std::lock_guard<std::recursive_mutex> lock(mDevice->stateMutex());
    Hwc2Display *hwcDisplay = mDevice->getDisplay(display);
    if (!hwcDisplay) {
        return Error::BAD_DISPLAY;
    }
    Hwc2Layer *hwcLayer = hwcDisplay->getLayer(layer);
    if (!hwcLayer) {
        return Error::BAD_LAYER;
    }
    hwcLayer->setName(std::string(name));
    return Error::NONE;
}

Return<Error> OpenfdeDisplay::setLayerHandleInfo(uint64_t display, uint64_t layer,
                                                 uint32_t format, uint32_t stride) {
    std::lock_guard<std::recursive_mutex> lock(mDevice->stateMutex());
    Hwc2Display *hwcDisplay = mDevice->getDisplay(display);
    if (!hwcDisplay) {
        return Error::BAD_DISPLAY;
    }
    Hwc2Layer *hwcLayer = hwcDisplay->getLayer(layer);
    if (!hwcLayer) {
        return Error::BAD_LAYER;
    }
    auto ext = hwcLayer->getHandleExt();
    ext.format = format;
    ext.stride = stride;
    hwcLayer->setHandleExt(ext);
    return Error::NONE;
}

Return<Error> OpenfdeDisplay::setTargetLayerHandleInfo(uint64_t display, uint32_t format,
                                                       uint32_t stride) {
    std::lock_guard<std::recursive_mutex> lock(mDevice->stateMutex());
    Hwc2Display *hwcDisplay = mDevice->getDisplay(display);
    if (!hwcDisplay) {
        return Error::BAD_DISPLAY;
    }
    auto ext = hwcDisplay->clientTargetExt();
    ext.format = format;
    ext.stride = stride;
    hwcDisplay->setClientTargetExt(ext);
    return Error::NONE;
}

// Methods from ::vendor::openfde::display::V1_1::IOpenfdeDisplay follow.
Return<Error> OpenfdeDisplay::setLayerSize(uint64_t display, uint64_t layer,
                                           uint32_t width, uint32_t height) {
    std::lock_guard<std::recursive_mutex> lock(mDevice->stateMutex());
    Hwc2Display *hwcDisplay = mDevice->getDisplay(display);
    if (!hwcDisplay) {
        return Error::BAD_DISPLAY;
    }
    Hwc2Layer *hwcLayer = hwcDisplay->getLayer(layer);
    if (!hwcLayer) {
        return Error::BAD_LAYER;
    }
    auto ext = hwcLayer->getHandleExt();
    ext.width = width;
    ext.height = height;
    hwcLayer->setHandleExt(ext);
    return Error::NONE;
}

Return<Error> OpenfdeDisplay::setTargetLayerSize(uint64_t display, uint32_t width,
                                                 uint32_t height) {
    std::lock_guard<std::recursive_mutex> lock(mDevice->stateMutex());
    Hwc2Display *hwcDisplay = mDevice->getDisplay(display);
    if (!hwcDisplay) {
        return Error::BAD_DISPLAY;
    }
    auto ext = hwcDisplay->clientTargetExt();
    ext.width = width;
    ext.height = height;
    hwcDisplay->setClientTargetExt(ext);
    return Error::NONE;
}

}  // namespace implementation
}  // namespace V1_1
}  // namespace display
}  // namespace openfde
}  // namespace vendor
