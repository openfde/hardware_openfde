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

OpenfdeDisplay::OpenfdeDisplay(struct display *display , struct display *secondary_display)
    : mDisplay(display), msecondary_Display(secondary_display)
{
}

// Methods from ::vendor::openfde::display::V1_0::IOpenfdeDisplay follow.
Return<Error> OpenfdeDisplay::setLayerName(uint32_t layer, const hidl_string &name) {
    mDisplay->layer_names[layer] = std::string(name);
    msecondary_Display->layer_names[layer] = std::string(name);
    return Error::NONE;
}
Return<Error> OpenfdeDisplay::setLayerHandleInfo(uint32_t layer, uint32_t format, uint32_t stride) {
    mDisplay->layer_handles_ext[layer] = 
    {
        .format = format,
        .stride = stride
    };
    msecondary_Display->layer_handles_ext[layer] = 
    {
        .format = format,
        .stride = stride
    };
    return Error::NONE;
}
Return<Error> OpenfdeDisplay::setTargetLayerHandleInfo(uint32_t format, uint32_t stride) {
    mDisplay->target_layer_handle_ext = 
    {
        .format = format,
        .stride = stride
    };
    msecondary_Display->target_layer_handle_ext = 
    {
        .format = format,
        .stride = stride
    };
    return Error::NONE;
}

// Methods from ::vendor::openfde::display::V1_1::IOpenfdeDisplay follow.
Return<Error> OpenfdeDisplay::setLayerSize(uint32_t layer, uint32_t width, uint32_t height) {
    mDisplay->layer_handles_ext[layer].width = width;
    mDisplay->layer_handles_ext[layer].height = height;
    msecondary_Display->layer_handles_ext[layer].width = width;
    msecondary_Display->layer_handles_ext[layer].height = height;
    return Error::NONE;
}

Return<Error> OpenfdeDisplay::setTargetLayerSize(uint32_t width, uint32_t height) {
    mDisplay->target_layer_handle_ext.width = width;
    mDisplay->target_layer_handle_ext.height = height;
    msecondary_Display->target_layer_handle_ext.width = width;
    msecondary_Display->target_layer_handle_ext.height = height;
    return Error::NONE;
}

}  // namespace implementation
}  // namespace V1_1
}  // namespace display
}  // namespace openfde
}  // namespace vendor
