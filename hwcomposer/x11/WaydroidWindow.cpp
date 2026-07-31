/*
 * Copyright (C) 2022 The Waydroid Project
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

#include "WaydroidWindow.h"

#include <cutils/properties.h>
#include <log/log.h>

#include "xdg-shell-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>

namespace vendor::openfde::window::implementation {

OpenfdeWindow::OpenfdeWindow(struct display *display, std::map<std::string, struct window *> *windows)
    : mDisplay(display),mWindows(windows)
{
}

/*static const struct zwp_relative_pointer_v1_listener relative_pointer_listener = {
    handle_relative_motion,
};
*/


// Methods from ::vendor::waydroid::window::V1_0::IWaydroidWindow follow.
Return<bool> OpenfdeWindow::minimize(const hidl_string& packageName) {
    ALOGE("WaydroidWindow minimize packageName %s", packageName.c_str());

    if(!mWindows || mWindows->size() < 1)
        return false;

    char property[PROPERTY_VALUE_MAX];

   if (!mDisplay->xcbconnection)
        return false;

    property_get("openfde.active_apps", property, "Openfde");
    if (!strcmp(property, "Openfde"))
        return false;

    std::scoped_lock lock(mDisplay->windowsMutex);
    for (auto it = mWindows->begin(); it != mWindows->end(); it++) {
        struct window* window = it->second;
        if (window && window->appID == packageName) {
           ALOGE("minimize window->appID: %s ", window->appID.c_str());
           xcb_intern_atom_cookie_t wm_change_state_cookie = xcb_intern_atom(mDisplay->xcbconnection, 0, strlen("WM_CHANGE_STATE"), "WM_CHANGE_STATE");
            xcb_intern_atom_reply_t *wm_change_state_atom = xcb_intern_atom_reply(mDisplay->xcbconnection, wm_change_state_cookie, NULL);

            if (wm_change_state_atom) {
                xcb_client_message_event_t ev;
                ev.response_type = XCB_CLIENT_MESSAGE;
                ev.format = 32;
                ev.window = window->xcbwindow;
                ev.type = wm_change_state_atom->atom;
                ev.data.data32[0] = XCB_ICCCM_WM_STATE_ICONIC;
                ev.data.data32[1] = 0;
                ev.data.data32[2] = 0;
                ev.data.data32[3] = 0;
                ev.data.data32[4] = 0;
                xcb_send_event(mDisplay->xcbconnection, 0, mDisplay->xcbscreen->root,
                    XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                    (const char *)&ev);
                free(wm_change_state_atom);
            }

            xcb_flush(mDisplay->xcbconnection);
            return true;
        }
    }
    return true;
}

// Methods from ::vendor::waydroid::window::V1_1::IWaydroidWindow follow.
Return<void> OpenfdeWindow::setPointerCapture(const hidl_string& packageName, bool enabled) {
    /*char property[PROPERTY_VALUE_MAX];
    std::string windowName = packageName;
    */
	ALOGE("%s %d", packageName.c_str(),enabled);

    return Void();
   /* if (!mDisplay->pointer_constraints)
        return Void();

    if (!mDisplay->pointer)
        return Void();

    property_get("openfde.active_apps", property, "Openfde");
    if (!strcmp(property, "Openfde"))
        windowName = "Openfde";

    std::scoped_lock lock(mDisplay->windowsMutex);
    for (auto it = mDisplay->windows.begin(); it != mDisplay->windows.end(); it++) {
        struct window* window = it->second;
        if (window && window->appID == windowName) {
            if (enabled && window->locked_pointer == nullptr) {
                window->locked_pointer = zwp_pointer_constraints_v1_lock_pointer(
                        mDisplay->pointer_constraints,
                        window->surface, mDisplay->pointer, nullptr,
                        ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
                if (mDisplay->relative_pointer == nullptr) {
                    mDisplay->relative_pointer = zwp_relative_pointer_manager_v1_get_relative_pointer(
                            mDisplay->relative_pointer_manager, mDisplay->pointer);
                    zwp_relative_pointer_v1_add_listener(mDisplay->relative_pointer, &relative_pointer_listener, mDisplay);
                }
            } else if (!enabled && window->locked_pointer != nullptr) {
                zwp_locked_pointer_v1_destroy(window->locked_pointer);
                window->locked_pointer = nullptr;
                bool anyLocks = false;
                for (auto jt = mDisplay->windows.begin(); jt != mDisplay->windows.end(); jt++) {
                    struct window* window = jt->second;
                    if (window->locked_pointer) {
                        anyLocks = true;
                        break;
                    }
                }
                if (!anyLocks) {
                    zwp_relative_pointer_v1_destroy(mDisplay->relative_pointer);
                    mDisplay->relative_pointer = nullptr;
                }
            }
            break;
        }
    }
    return Void();
    */
}

// Methods from ::vendor::waydroid::window::V1_2::IWaydroidWindow follow.
Return<void> OpenfdeWindow::setIdleInhibit(const hidl_string& task, bool enabled) {
    char property[PROPERTY_VALUE_MAX];
    std::string taskID = task;

    if (enabled)
        return Void();

    property_get("openfde.active_apps", property, "Openfde");
    if (!strcmp(property, "Openfde"))
        taskID = "0";

    return Void();
}

}  // namespace vendor::waydroid::window::implementation
