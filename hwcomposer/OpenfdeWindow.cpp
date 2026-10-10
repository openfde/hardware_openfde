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

#include "OpenfdeWindow.h"

#include <log/log.h>

namespace vendor::openfde::window::implementation {

OpenfdeWindow::OpenfdeWindow(HwcBackend *backend, std::map<std::string, struct window *> *windows)
    : mBackend(backend), mWindows(windows)
{
}

// Methods from ::vendor::openfde::window::V1_0::IOpenfdeWindow follow.
Return<bool> OpenfdeWindow::minimize(const hidl_string& packageName) {
    return mBackend->minimizeWindow(packageName.c_str(), mWindows);
}

// Methods from ::vendor::openfde::window::V1_1::IOpenfdeWindow follow.
Return<void> OpenfdeWindow::setPointerCapture(const hidl_string& packageName, bool enabled) {
    mBackend->setPointerCapture(packageName.c_str(), enabled);
    return Void();
}

// Methods from ::vendor::openfde::window::V1_2::IOpenfdeWindow follow.
Return<void> OpenfdeWindow::setIdleInhibit(const hidl_string& task, bool enabled) {
    mBackend->setIdleInhibit(task.c_str(), enabled);
    return Void();
}

}  // namespace vendor::openfde::window::implementation
