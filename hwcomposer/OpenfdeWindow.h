/*
 * Copyright (C) 2022 The Openfde Project
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

#include <vendor/openfde/window/1.2/IOpenfdeWindow.h>
#include <hidl/MQDescriptor.h>
#include <hidl/Status.h>

#include "hwc_backend.h"

namespace vendor::openfde::window::implementation {

using ::android::hardware::hidl_array;
using ::android::hardware::hidl_memory;
using ::android::hardware::hidl_string;
using ::android::hardware::hidl_vec;
using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::sp;

/* 统一的窗口 HIDL 服务实现，具体协议操作委托给 HwcBackend 子类 */
struct OpenfdeWindow : public V1_2::IOpenfdeWindow {
  public:
    OpenfdeWindow(HwcBackend *backend, std::map<std::string, struct window *> *windows);
    // Methods from ::vendor::openfde::window::V1_0::IOpenfdeWindow follow.
    Return<bool> minimize(const hidl_string& packageName) override;

    // Methods from ::vendor::openfde::window::V1_1::IOpenfdeWindow follow.
    Return<void> setPointerCapture(const hidl_string& packageName, bool enabled) override;

    // Methods from ::vendor::openfde::window::V1_2::IOpenfdeWindow follow.
    Return<void> setIdleInhibit(const hidl_string& packageName, bool enabled) override;
  private:
    HwcBackend *mBackend;
    std::map<std::string, struct window *> *mWindows;
};

}  // namespace vendor::openfde::window::implementation
