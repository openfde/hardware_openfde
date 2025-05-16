/*
 * Copyright (C) 2021 The Opendfe Project
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
#ifndef VENDOR_OPENFDE_DISPLAY_V1_1_OPENFDEDISPLAY_H
#define VENDOR_OPENFDE_DISPLAY_V1_1_OPENFDEDISPLAY_H

#include <android/hardware/graphics/composer/2.1/IComposer.h>
#include <vendor/openfde/display/1.1/IOpenfdeDisplay.h>
#include <hidl/HidlTransportSupport.h>
#include <hidl/MQDescriptor.h>
#include <hidl/Status.h>

#include "wayland-hwc.h"

namespace vendor {
namespace openfde {
namespace display {
namespace V1_1 {
namespace implementation {

using ::android::hardware::hidl_string;
using ::android::hardware::Return;
using ::android::hardware::graphics::composer::V2_1::Error;
using ::android::sp;
using ::vendor::openfde::display::V1_1::IOpenfdeDisplay;

class OpenfdeDisplay : public IOpenfdeDisplay {
  public:
    OpenfdeDisplay(struct display *display, struct display *secondary_display);

    // Methods from ::vendor::openfde::display::V1_0::IOpenfdeDisplay follow.
    Return<Error> setLayerName(uint32_t layer, const hidl_string &name) override;
    Return<Error> setLayerHandleInfo(uint32_t layer, uint32_t format, uint32_t stride) override;
    Return<Error> setTargetLayerHandleInfo(uint32_t format, uint32_t stride) override;

    // Methods from ::vendor::openfde::display::V1_1::IOpenfdeDisplay follow.
    Return<Error> setLayerSize(uint32_t layer, uint32_t width, uint32_t height) override;
    Return<Error> setTargetLayerSize(uint32_t width, uint32_t height) override;
  private:
    struct display *mDisplay;
    struct display *msecondary_Display;
};

}  // namespace implementation
}  // namespace V1_1
}  // namespace display
}  // namespace openfde
}  // namespace vendor

#endif  // VENDOR_OPENFDE_DISPLAY_V1_1_OPENFDEDISPLAY_H
