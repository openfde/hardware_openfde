/*
 * Copyright (C) 2024 The Openfde Project
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

#include "hwc2_device.h"

#include <errno.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>

#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <type_traits>
#include <utility>

#include <log/log.h>
#include <libsync/sw_sync.h>
#include <sync/sync.h>
#include <system/thread_defs.h>

#include "extension.h"

namespace openfde {

using namespace HWC2;

using ::android::OK;
using ::android::sp;
using ::android::status_t;
using ::android::hardware::joinRpcThreadpool;
using ::vendor::openfde::display::V1_1::implementation::OpenfdeDisplay;

template <typename PFN, typename T>
static hwc2_function_pointer_t asFP(T function) {
    static_assert(std::is_same<PFN, T>::value, "Incompatible function pointer");
    return reinterpret_cast<hwc2_function_pointer_t>(function);
}

// ---------------------------------------------------------------------------
// Hwc2Layer
// ---------------------------------------------------------------------------

Hwc2Layer::Hwc2Layer(Hwc2Display& display, hwc2_layer_t id) : mId(id), mDisplay(display) {}

int Hwc2Layer::takeAcquireFence() {
    int fence = mAcquireFence;
    mAcquireFence = -1;
    return fence;
}

Error Hwc2Layer::setBuffer(buffer_handle_t buffer, int32_t acquireFence) {
    if (mAcquireFence >= 0) {
        close(mAcquireFence);
    }
    mBuffer = buffer;
    mAcquireFence = acquireFence;
    return Error::None;
}

Error Hwc2Layer::setCursorPosition(int32_t x, int32_t y) {
    mCursorX = x;
    mCursorY = y;
    return Error::None;
}

Error Hwc2Layer::setSurfaceDamage(hwc_region_t damage) {
    mSurfaceDamage.assign(damage.rects, damage.rects + damage.numRects);
    return Error::None;
}

Error Hwc2Layer::setBlendMode(BlendMode mode) {
    mBlendMode = mode;
    mDisplay.markGeometryChanged();
    return Error::None;
}

Error Hwc2Layer::setColor(hwc_color_t color) {
    mColor = color;
    mDisplay.markGeometryChanged();
    return Error::None;
}

Error Hwc2Layer::setCompositionType(Composition type) {
    mCompositionType = type;
    return Error::None;
}

Error Hwc2Layer::setDataspace(android_dataspace_t dataspace) {
    mDataspace = dataspace;
    return Error::None;
}

Error Hwc2Layer::setDisplayFrame(hwc_rect_t frame) {
    mDisplayFrame = frame;
    mDisplay.markGeometryChanged();
    return Error::None;
}

Error Hwc2Layer::setPlaneAlpha(float alpha) {
    mPlaneAlpha = alpha;
    mDisplay.markGeometryChanged();
    return Error::None;
}

Error Hwc2Layer::setSidebandStream(const native_handle_t* /*stream*/) {
    ALOGE("setSidebandStream: sideband streams are not supported");
    return Error::Unsupported;
}

Error Hwc2Layer::setSourceCrop(hwc_frect_t crop) {
    mSourceCrop = crop;
    mDisplay.markGeometryChanged();
    return Error::None;
}

Error Hwc2Layer::setTransform(Transform transform) {
    mTransform = transform;
    mDisplay.markGeometryChanged();
    return Error::None;
}

Error Hwc2Layer::setVisibleRegion(hwc_region_t visible) {
    mVisibleRegion.assign(visible.rects, visible.rects + visible.numRects);
    mDisplay.markGeometryChanged();
    return Error::None;
}

Error Hwc2Layer::setZ(uint32_t z) {
    mZ = z;
    mDisplay.markGeometryChanged();
    return Error::None;
}

// ---------------------------------------------------------------------------
// Hwc2Display
// ---------------------------------------------------------------------------

Hwc2Display::Hwc2Display(Hwc2Device& device, hwc2_display_t id, DisplayType type)
    : mDevice(device),
      mId(id),
      mType(type),
      mName(type == DisplayType::Physical ? "Openfde display" : "Openfde virtual display") {}

Hwc2Layer* Hwc2Display::getLayer(hwc2_layer_t layerId) const {
    auto it = mLayers.find(layerId);
    return it != mLayers.end() ? it->second.get() : nullptr;
}

std::vector<Hwc2Layer*> Hwc2Display::getLayersByZ() const {
    std::vector<Hwc2Layer*> layers;
    layers.reserve(mLayers.size());
    for (const auto& pair : mLayers) {
        layers.push_back(pair.second.get());
    }
    std::sort(layers.begin(), layers.end(),
              [](const Hwc2Layer* lhs, const Hwc2Layer* rhs) { return lhs->z() < rhs->z(); });
    return layers;
}

void Hwc2Display::requestCompositionChange(Hwc2Layer& layer, Composition type) {
    if (layer.compositionType() != type) {
        mTypeChanges[layer.getId()] = type;
    }
}

int Hwc2Display::takeClientTargetAcquireFence() {
    int fence = mClientTargetAcquireFence;
    mClientTargetAcquireFence = -1;
    return fence;
}

Error Hwc2Display::acceptChanges() {
    for (const auto& change : mTypeChanges) {
        auto* layer = getLayer(change.first);
        if (layer) {
            layer->setCompositionType(change.second);
        }
    }
    mTypeChanges.clear();
    return Error::None;
}

Error Hwc2Display::createLayer(hwc2_layer_t* outLayerId) {
    const hwc2_layer_t id = mNextLayerId++;
    mLayers.emplace(id, std::make_shared<Hwc2Layer>(*this, id));
    *outLayerId = id;
    markGeometryChanged();
    return Error::None;
}

Error Hwc2Display::destroyLayer(hwc2_layer_t layerId) {
    auto it = mLayers.find(layerId);
    if (it == mLayers.end()) {
        return Error::BadLayer;
    }
    mLayers.erase(it);
    markGeometryChanged();
    return Error::None;
}

Error Hwc2Display::getActiveConfig(hwc2_config_t* outConfigId) {
    return mDevice.backend().getActiveConfig(*this, outConfigId);
}

Error Hwc2Display::getAttribute(hwc2_config_t configId, Attribute attribute,
                                int32_t* outValue) {
    return mDevice.backend().getAttribute(*this, configId, attribute, outValue);
}

Error Hwc2Display::getChangedCompositionTypes(uint32_t* outNumElements,
                                              hwc2_layer_t* outLayers, int32_t* outTypes) {
    if (outLayers == nullptr || outTypes == nullptr) {
        *outNumElements = static_cast<uint32_t>(mTypeChanges.size());
        return Error::None;
    }

    uint32_t written = 0;
    for (const auto& change : mTypeChanges) {
        if (written >= *outNumElements) {
            break;
        }
        outLayers[written] = change.first;
        outTypes[written] = static_cast<int32_t>(change.second);
        ++written;
    }
    return Error::None;
}

Error Hwc2Display::getColorModes(uint32_t* outNumModes, int32_t* outModes) {
    if (outModes == nullptr) {
        *outNumModes = 1;
        return Error::None;
    }
    if (*outNumModes == 0) {
        return Error::NoResources;
    }
    outModes[0] = HAL_COLOR_MODE_NATIVE;
    return Error::None;
}

Error Hwc2Display::getConfigs(uint32_t* outNumConfigs, hwc2_config_t* outConfigIds) {
    auto configs = mDevice.backend().getConfigs(*this);
    if (outConfigIds == nullptr) {
        *outNumConfigs = static_cast<uint32_t>(configs.size());
        return Error::None;
    }

    uint32_t written = 0;
    for (auto config : configs) {
        if (written >= *outNumConfigs) {
            break;
        }
        outConfigIds[written++] = config;
    }
    *outNumConfigs = written;
    return Error::None;
}

Error Hwc2Display::getDozeSupport(int32_t* outSupport) {
    *outSupport = 0;
    return Error::None;
}

Error Hwc2Display::getHdrCapabilities(uint32_t* outNumTypes, int32_t* /*outTypes*/,
                                      float* /*outMaxLuminance*/,
                                      float* /*outMaxAverageLuminance*/,
                                      float* /*outMinLuminance*/) {
    *outNumTypes = 0;
    return Error::None;
}

Error Hwc2Display::getName(uint32_t* outSize, char* outName) {
    if (outName == nullptr) {
        *outSize = static_cast<uint32_t>(mName.size());
        return Error::None;
    }
    *outSize = static_cast<uint32_t>(mName.copy(outName, *outSize));
    return Error::None;
}

Error Hwc2Display::getReleaseFences(uint32_t* outNumElements, hwc2_layer_t* outLayers,
                                    int32_t* outFences) {
    if (outLayers == nullptr || outFences == nullptr) {
        uint32_t num = 0;
        for (const auto& pair : mLayers) {
            if (pair.second->buffer() != nullptr) {
                ++num;
            }
        }
        *outNumElements = num;
        return Error::None;
    }

    uint32_t written = 0;
    for (const auto& pair : mLayers) {
        if (written >= *outNumElements) {
            break;
        }
        if (pair.second->buffer() == nullptr) {
            continue;
        }
        outLayers[written] = pair.first;
        // Explicit release fences are not supported, same as the HWC1
        // implementation which always returned releaseFenceFd = -1.
        outFences[written] = -1;
        ++written;
    }
    return Error::None;
}

Error Hwc2Display::getRequests(int32_t* outDisplayRequests, uint32_t* outNumElements,
                               hwc2_layer_t* /*outLayers*/, int32_t* /*outLayerRequests*/) {
    *outDisplayRequests = 0;
    *outNumElements = 0;
    return Error::None;
}

Error Hwc2Display::getType(int32_t* outType) {
    *outType = static_cast<int32_t>(mType);
    return Error::None;
}

Error Hwc2Display::present(int32_t* outRetireFence) {
    if (!mValidated) {
        ALOGE("presentDisplay called without a preceding validateDisplay");
        return Error::NotValidated;
    }

    Error error = mDevice.backend().present(*this);

    // The HWC2 device owns the acquire fences; close whatever the backend
    // did not consume so no fence ever leaks.
    for (const auto& pair : mLayers) {
        int fence = pair.second->takeAcquireFence();
        if (fence >= 0) {
            close(fence);
        }
    }
    int targetFence = takeClientTargetAcquireFence();
    if (targetFence >= 0) {
        close(targetFence);
    }

    mValidated = false;
    resetGeometryChanged();
    *outRetireFence = mDevice.createPresentFence();
    return error;
}

Error Hwc2Display::setActiveConfig(hwc2_config_t configId) {
    return mDevice.backend().setActiveConfig(*this, configId);
}

Error Hwc2Display::setClientTarget(buffer_handle_t target, int32_t acquireFence,
                                   int32_t /*dataspace*/, hwc_region_t /*damage*/) {
    if (mClientTargetAcquireFence >= 0) {
        close(mClientTargetAcquireFence);
    }
    mClientTarget = target;
    mClientTargetAcquireFence = acquireFence;
    return Error::None;
}

Error Hwc2Display::setColorMode(android_color_mode_t mode) {
    if (mode != HAL_COLOR_MODE_NATIVE) {
        return Error::Unsupported;
    }
    mColorMode = mode;
    return Error::None;
}

Error Hwc2Display::setColorTransform(android_color_transform_t hint) {
    // Anything other than IDENTITY requires falling back to client
    // composition, which is handled by the backend during validate().
    mColorTransform = hint;
    markGeometryChanged();
    return Error::None;
}

Error Hwc2Display::setOutputBuffer(buffer_handle_t /*buffer*/, int32_t releaseFence) {
    // No virtual display support; just take ownership of the fence.
    if (releaseFence >= 0) {
        close(releaseFence);
    }
    return Error::None;
}

Error Hwc2Display::setPowerMode(PowerMode mode) {
    mPowerMode = mode;
    return mDevice.backend().setPowerMode(*this, mode);
}

Error Hwc2Display::setVsyncEnabled(Vsync enabled) {
    mDevice.setVsyncEnabled(mId, enabled == Vsync::Enable);
    return Error::None;
}

Error Hwc2Display::validate(uint32_t* outNumTypes, uint32_t* outNumRequests) {
    mTypeChanges.clear();
    Error error = mDevice.backend().validate(*this);
    *outNumTypes = static_cast<uint32_t>(mTypeChanges.size());
    *outNumRequests = 0;
    mValidated = true;
    return error;
}

Error Hwc2Display::getClientTargetSupport(uint32_t /*width*/, uint32_t /*height*/,
                                          int32_t /*format*/, int32_t /*dataspace*/) {
    return Error::None;
}

std::string Hwc2Display::dump() {
    std::stringstream output;
    output << "  Display " << mId << " (" << mName << "):\n"
           << "    type=" << to_string(mType) << " layers=" << mLayers.size()
           << " powerMode=" << to_string(mPowerMode) << '\n';
    output << mDevice.backend().dump(*this);
    return output.str();
}

// ---------------------------------------------------------------------------
// Hwc2Device
// ---------------------------------------------------------------------------

Hwc2Device::Hwc2Device(std::unique_ptr<Hwc2Backend> backend, void* primaryBackendData)
      : mBackend(std::move(backend)) {
    common.tag = HARDWARE_DEVICE_TAG;
    common.version = HWC_DEVICE_API_VERSION_2_0;
    common.module = nullptr;
    common.close = closeHook;
    getCapabilities = getCapabilitiesHook;
    getFunction = getFunctionHook;

    mBackend->setDevice(this);

    pthread_mutex_init(&mVsyncLock, nullptr);
    mTimelineFd = sw_sync_timeline_create();
    if (mTimelineFd < 0) {
        // Every present fence created on a broken timeline is an invalid fd,
        // which surfaces as "trackPendingFrame: Invalid present fence" in SF
        ALOGE("sw_sync_timeline_create failed: %s; present fences will be unusable",
              strerror(errno));
    }

    // One-time diagnostic for the sw_sync source. libsync first tries
    // /sys/kernel/debug/sync/sw_sync and falls back to /dev/sw_sync; when the
    // fallback has to kick in, record why the sysfs path was rejected (DAC
    // permissions, an SELinux label the composer domain cannot access, or a
    // different mount namespace). errno from sw_sync_timeline_create itself
    // is useless here since it belongs to whichever open failed last.
    {
        const char* sysPath = "/sys/kernel/debug/sync/sw_sync";
        struct stat st;
        if (stat(sysPath, &st) == 0) {
            const int fd = open(sysPath, O_RDWR);
            ALOGI("sw_sync sysfs path: mode=%o uid=%u gid=%u, open=%s; hal uid=%d",
                  st.st_mode & 07777, st.st_uid, st.st_gid,
                  fd >= 0 ? "ok" : strerror(errno), geteuid());
            if (fd >= 0) {
                close(fd);
            }
        } else {
            ALOGI("sw_sync sysfs path unavailable: %s; hal uid=%d", strerror(errno),
                  geteuid());
        }
    }

    struct timespec rt;
    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in clock_gettime: %s", __FILE__, __LINE__, strerror(errno));
    }
    mLastVsyncNs = int64_t(rt.tv_sec) * 1e9 + rt.tv_nsec;

    // The primary physical display always exists
    auto primary = std::make_shared<Hwc2Display>(*this, 0, DisplayType::Physical);
    primary->setBackendData(primaryBackendData);
    mDisplays.emplace(0, primary);

    // Let the backend report any additional (external) display it supports
     mBackend->createExternalDisplays(*this);
}

Hwc2Device::~Hwc2Device() {
    mVsyncThreadExit = true;
    if (mVsyncThread) {
        pthread_join(mVsyncThread, nullptr);
    }
    pthread_mutex_destroy(&mVsyncLock);
    if (mTimelineFd >= 0) {
        close(mTimelineFd);
    }
}

Hwc2Display* Hwc2Device::getDisplay(hwc2_display_t id) const {
    auto it = mDisplays.find(id);
    return it != mDisplays.end() ? it->second.get() : nullptr;
}

Hwc2Display* Hwc2Device::createExternalDisplay(void* backendData) {
    if (!backendData) {
        return nullptr;
    }

    std::lock_guard<std::recursive_mutex> lock(mStateMutex);
    // Display ids other than 0 are external displays
    hwc2_display_t id = 1;
    while (mDisplays.count(id) > 0) {
        id++;
    }

    auto display = std::make_shared<Hwc2Display>(*this, id, DisplayType::Physical);
    display->setBackendData(backendData);
    mDisplays.emplace(id, display);

    // The client is told about the new display through the hotplug callback
    hotplugEvent(id, true);

    return display.get();
}

HWC2::Error Hwc2Device::destroyExternalDisplay(hwc2_display_t displayId) {
    std::lock_guard<std::recursive_mutex> lock(mStateMutex);

    auto it = mDisplays.find(displayId);
    if (it == mDisplays.end() || it->second->isPrimary()) {
        return Error::BadDisplay;
    }

    void* backendData = it->second->backendData();
    mDisplays.erase(it);

    hotplugEvent(displayId, false);
    mBackend->destroyExternalDisplay(backendData);

    return Error::None;
}

void Hwc2Device::hotplugEvent(hwc2_display_t displayId, bool connected) {
    int32_t connection = static_cast<int32_t>(
            connected ? HWC2::Connection::Connected : HWC2::Connection::Disconnected);

    auto callbackIt = mCallbacks.find(Callback::Hotplug);
    if (callbackIt == mCallbacks.end() || callbackIt->second.pointer == nullptr) {
        // No listener yet: remember the event and deliver it on registration
        mPendingHotplugs.emplace_back(displayId, connection);
        return;
    }

    auto hotplug = reinterpret_cast<HWC2_PFN_HOTPLUG>(callbackIt->second.pointer);
    hotplug(callbackIt->second.data, displayId, connection);
}

int Hwc2Device::createPresentFence() {
    if (mTimelineFd < 0) {
        // The timeline is unusable (sw_sync missing from the kernel); report
        // "no fence" instead of handing SF a bad fd. SF treats a missing
        // present fence as pending and drops the frame from its latency
        // statistics after the timeout.
        return -1;
    }
    const int fence = sw_sync_fence_create(mTimelineFd, "openfde_hwc_present",
                                           mNextSyncPoint);
    mNextSyncPoint++;
    sw_sync_timeline_inc(mTimelineFd, 1);
    if (fence < 0 && !mPresentFenceErrorLogged) {
        mPresentFenceErrorLogged = true;
        ALOGE("sw_sync_fence_create failed (timeline fd %d): %s; SF will report "
              "\"Invalid present fence\"", mTimelineFd, strerror(errno));
    }
    return fence;
}

void Hwc2Device::setVsyncTimestamp(int64_t timestampNs) {
    pthread_mutex_lock(&mVsyncLock);
    mLastVsyncNs = timestampNs;
    pthread_mutex_unlock(&mVsyncLock);
}

void Hwc2Device::setVsyncPeriod(int32_t periodNs) {
    pthread_mutex_lock(&mVsyncLock);
    mVsyncPeriodNs = periodNs;
    pthread_mutex_unlock(&mVsyncLock);
}

void Hwc2Device::setVsyncEnabled(hwc2_display_t displayId, bool enabled) {
    pthread_mutex_lock(&mVsyncLock);
    mVsyncEnabled[displayId] = enabled;
    pthread_mutex_unlock(&mVsyncLock);
}

static long time_to_sleep_to_next_vsync(struct timespec* rt, uint64_t last_vsync_ns,
                                        unsigned vsync_period_ns) {
    uint64_t now = (uint64_t)rt->tv_sec * 1e9 + rt->tv_nsec;
    uint64_t frames_since_last_vsync = (now - last_vsync_ns) / vsync_period_ns + 1;
    uint64_t next_vsync = last_vsync_ns + frames_since_last_vsync * vsync_period_ns;

    return next_vsync - now;
}

void* Hwc2Device::vsyncThreadEntry(void* data) {
    static_cast<Hwc2Device*>(data)->vsyncThread();
    return nullptr;
}

void Hwc2Device::vsyncThread() {
    setpriority(PRIO_PROCESS, 0, HAL_PRIORITY_URGENT_DISPLAY);

    struct timespec rt;
    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in vsync thread clock_gettime: %s", __FILE__, __LINE__,
              strerror(errno));
    }

    struct timespec wait_time;
    wait_time.tv_sec = 0;

    pthread_mutex_lock(&mVsyncLock);
    wait_time.tv_nsec = time_to_sleep_to_next_vsync(&rt, mLastVsyncNs, mVsyncPeriodNs);
    pthread_mutex_unlock(&mVsyncLock);

    while (!mVsyncThreadExit) {
        int err = nanosleep(&wait_time, NULL);
        if (err == -1) {
            if (errno == EINTR) {
                break;
            }
            ALOGE("error in vsync thread: %s", strerror(errno));
            continue;
        }

        if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
            ALOGE("%s:%d error in vsync thread clock_gettime: %s", __FILE__, __LINE__,
                  strerror(errno));
        }
        int64_t timestamp = (uint64_t)rt.tv_sec * 1e9 + rt.tv_nsec;

        pthread_mutex_lock(&mVsyncLock);
        wait_time.tv_nsec = time_to_sleep_to_next_vsync(&rt, mLastVsyncNs, mVsyncPeriodNs);
        std::vector<hwc2_display_t> enabledDisplays;
        for (const auto& pair : mVsyncEnabled) {
            if (pair.second) {
                enabledDisplays.push_back(pair.first);
            }
        }
        pthread_mutex_unlock(&mVsyncLock);

        CallbackInfo callback;
        {
            std::lock_guard<std::recursive_mutex> lock(mStateMutex);
            auto callbackIt = mCallbacks.find(Callback::Vsync);
            if (callbackIt != mCallbacks.end()) {
                callback = callbackIt->second;
            }
        }

        if (callback.pointer == nullptr) {
            continue;
        }
        auto vsync = reinterpret_cast<HWC2_PFN_VSYNC>(callback.pointer);
        for (auto displayId : enabledDisplays) {
            vsync(callback.data, displayId, timestamp);
        }
    }
}

void* Hwc2Device::extensionThreadEntry(void* data) {
    auto* device = static_cast<Hwc2Device*>(data);
    setpriority(PRIO_PROCESS, 0, HAL_PRIORITY_URGENT_DISPLAY);

    sp<::vendor::openfde::display::V1_1::IOpenfdeDisplay> service = new OpenfdeDisplay(device);
    status_t status = service->registerAsService();
    if (status != OK) {
        ALOGE("Could not register service for Openfde Display HAL (%d / 0x%x).",
              status, status);
        goto shutdown;
    }

    ALOGI("Openfde Display HAL thread is ready.");
    joinRpcThreadpool();
    // Should not pass this line

shutdown:
    ALOGE("Openfde Display HAL service is shutting down.");
    return nullptr;
}

void Hwc2Device::startServices() {
    if (pthread_create(&mVsyncThread, nullptr, vsyncThreadEntry, this)) {
        ALOGE("openfde hwc2 could not start vsync_thread");
        mVsyncThread = 0;
    }
    if (pthread_create(&mExtensionThread, nullptr, extensionThreadEntry, this)) {
        ALOGE("openfde hwc2 could not start extension_thread");
        mExtensionThread = 0;
    }
}

int Hwc2Device::closeHook(hw_device_t* device) {
    // The real work is done in the class destructor
    delete fromDevice(reinterpret_cast<hwc2_device_t*>(device));
    return 0;
}

void Hwc2Device::doGetCapabilities(uint32_t* outCount, int32_t* /*outCapabilities*/) {
    // No capabilities are advertised
    *outCount = 0;
}

void Hwc2Device::getCapabilitiesHook(hwc2_device_t* device, uint32_t* outCount,
                                     int32_t* outCapabilities) {
    fromDevice(device)->doGetCapabilities(outCount, outCapabilities);
}

hwc2_function_pointer_t Hwc2Device::getFunctionHook(hwc2_device_t* device,
                                                    int32_t intDescriptor) {
    auto descriptor = static_cast<FunctionDescriptor>(intDescriptor);
    return fromDevice(device)->doGetFunction(descriptor);
}

template <typename... Args>
int32_t Hwc2Device::callDisplayFunction(hwc2_device_t* device, hwc2_display_t displayId,
                                        Error (Hwc2Display::*member)(Args...),
                                        Args... args) {
    auto* self = fromDevice(device);
    std::lock_guard<std::recursive_mutex> lock(self->mStateMutex);
    auto* display = self->getDisplay(displayId);
    if (!display) {
        return static_cast<int32_t>(Error::BadDisplay);
    }
    auto error = ((*display).*member)(std::forward<Args>(args)...);
    return static_cast<int32_t>(error);
}

template <typename MF, MF memFunc, typename... Args>
int32_t Hwc2Device::displayHook(hwc2_device_t* device, hwc2_display_t displayId,
                                Args... args) {
    return callDisplayFunction(device, displayId, memFunc, std::forward<Args>(args)...);
}

template <typename... Args>
int32_t Hwc2Device::callLayerFunction(hwc2_device_t* device, hwc2_display_t displayId,
                                      hwc2_layer_t layerId,
                                      Error (Hwc2Layer::*member)(Args...), Args... args) {
    auto* self = fromDevice(device);
    std::lock_guard<std::recursive_mutex> lock(self->mStateMutex);
    auto* display = self->getDisplay(displayId);
    if (!display) {
        return static_cast<int32_t>(Error::BadDisplay);
    }
    auto* layer = display->getLayer(layerId);
    if (!layer) {
        return static_cast<int32_t>(Error::BadLayer);
    }
    auto error = ((*layer).*member)(std::forward<Args>(args)...);
    return static_cast<int32_t>(error);
}

template <typename MF, MF memFunc, typename... Args>
int32_t Hwc2Device::layerHook(hwc2_device_t* device, hwc2_display_t displayId,
                              hwc2_layer_t layerId, Args... args) {
    return callLayerFunction(device, displayId, layerId, memFunc,
                             std::forward<Args>(args)...);
}

static int32_t getDisplayAttributeHook(hwc2_device_t* device, hwc2_display_t display,
                                       hwc2_config_t config, int32_t intAttribute,
                                       int32_t* outValue) {
    auto attribute = static_cast<Attribute>(intAttribute);
    return Hwc2Device::callDisplayFunction(device, display, &Hwc2Display::getAttribute,
                                           config, attribute, outValue);
}

static int32_t setColorTransformHook(hwc2_device_t* device, hwc2_display_t display,
                                     const float* /*matrix*/, int32_t intHint) {
    // The matrix is intentionally dropped: any hint other than IDENTITY falls
    // back to client composition anyway
    auto hint = static_cast<android_color_transform_t>(intHint);
    return Hwc2Device::callDisplayFunction(device, display, &Hwc2Display::setColorTransform,
                                           hint);
}

static int32_t setColorModeHook(hwc2_device_t* device, hwc2_display_t display,
                                int32_t intMode) {
    auto mode = static_cast<android_color_mode_t>(intMode);
    return Hwc2Device::callDisplayFunction(device, display, &Hwc2Display::setColorMode, mode);
}

static int32_t setPowerModeHook(hwc2_device_t* device, hwc2_display_t display,
                                int32_t intMode) {
    auto mode = static_cast<PowerMode>(intMode);
    return Hwc2Device::callDisplayFunction(device, display, &Hwc2Display::setPowerMode, mode);
}

static int32_t setVsyncEnabledHook(hwc2_device_t* device, hwc2_display_t display,
                                   int32_t intEnabled) {
    auto enabled = static_cast<Vsync>(intEnabled);
    return Hwc2Device::callDisplayFunction(device, display, &Hwc2Display::setVsyncEnabled,
                                           enabled);
}

static int32_t setLayerBlendModeHook(hwc2_device_t* device, hwc2_display_t display,
                                     hwc2_layer_t layer, int32_t intMode) {
    auto mode = static_cast<BlendMode>(intMode);
    return Hwc2Device::callLayerFunction(device, display, layer, &Hwc2Layer::setBlendMode,
                                         mode);
}

static int32_t setLayerCompositionTypeHook(hwc2_device_t* device, hwc2_display_t display,
                                           hwc2_layer_t layer, int32_t intType) {
    auto type = static_cast<Composition>(intType);
    return Hwc2Device::callLayerFunction(device, display, layer,
                                         &Hwc2Layer::setCompositionType, type);
}

static int32_t setLayerDataspaceHook(hwc2_device_t* device, hwc2_display_t display,
                                     hwc2_layer_t layer, int32_t intDataspace) {
    auto dataspace = static_cast<android_dataspace_t>(intDataspace);
    return Hwc2Device::callLayerFunction(device, display, layer, &Hwc2Layer::setDataspace,
                                         dataspace);
}

static int32_t setLayerTransformHook(hwc2_device_t* device, hwc2_display_t display,
                                     hwc2_layer_t layer, int32_t intTransform) {
    auto transform = static_cast<Transform>(intTransform);
    return Hwc2Device::callLayerFunction(device, display, layer, &Hwc2Layer::setTransform,
                                         transform);
}

// Device functions (hwc2_device_t does not support virtual displays, keeping
// the behavior of the former HWC1 implementation)

static int32_t createVirtualDisplayHook(hwc2_device_t* /*device*/, uint32_t /*width*/,
                                        uint32_t /*height*/, int32_t* /*format*/,
                                        hwc2_display_t* /*outDisplay*/) {
    return static_cast<int32_t>(Error::Unsupported);
}

static int32_t destroyVirtualDisplayHook(hwc2_device_t* /*device*/,
                                         hwc2_display_t /*display*/) {
    return static_cast<int32_t>(Error::BadDisplay);
}

static uint32_t getMaxVirtualDisplayCountHook(hwc2_device_t* /*device*/) {
    return 0;
}

static int32_t registerCallbackHook(hwc2_device_t* device, int32_t intDescriptor,
                                    hwc2_callback_data_t callbackData,
                                    hwc2_function_pointer_t pointer) {
    auto descriptor = static_cast<Callback>(intDescriptor);
    auto error = Hwc2Device::fromDevice(device)->registerCallback(descriptor, callbackData, pointer);
    return static_cast<int32_t>(error);
}

static void dumpHook(hwc2_device_t* device, uint32_t* outSize, char* outBuffer) {
    Hwc2Device::fromDevice(device)->dump(outSize, outBuffer);
}

Error Hwc2Device::registerCallback(Callback descriptor, hwc2_callback_data_t callbackData,
                                   hwc2_function_pointer_t pointer) {
    switch (descriptor) {
        case Callback::Hotplug:
        case Callback::Refresh:
        case Callback::Vsync:
            break;
        default:
            return Error::BadParameter;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(mStateMutex);
        if (pointer != nullptr) {
            mCallbacks[descriptor] = {callbackData, pointer};
        } else {
            mCallbacks.erase(descriptor);
        }
    }

    // Report every existing display as soon as the hotplug callback is
    // registered, as required by the HWC2 hotplug rules
    if (descriptor == Callback::Hotplug && pointer != nullptr) {
        auto hotplug = reinterpret_cast<HWC2_PFN_HOTPLUG>(pointer);
        std::vector<std::pair<hwc2_display_t, int32_t>> pending;
        std::vector<std::pair<hwc2_display_t, int32_t>> events;
        {
            std::lock_guard<std::recursive_mutex> lock(mStateMutex);
            pending.swap(mPendingHotplugs);
            // 先按 id 升序上报所有已存在的 display。display 0（主屏）必须是
            // 第一个，否则 SurfaceFlinger 会把第一个上报的显示器当成内建 /
            // 默认屏（见 frameworks native 的 allocatePhysicalDisplay），
            // 导致“屏幕分辨率”设置消失、默认屏与外接屏身份互换。
            for (const auto& pair : mDisplays) {
                events.emplace_back(pair.first,
                                    static_cast<int32_t>(HWC2::Connection::Connected));
            }
            // 再补上 pending 中、已不在 mDisplays 中的事件
            // （例如注册回调前已经被销毁的 display）
            for (const auto& ev : pending) {
                bool found = false;
                for (const auto& e : events) {
                    if (e.first == ev.first) {
                        found = true;
                        break;
                    }
                }
                if (!found) events.emplace_back(ev);
            }
        }
        std::sort(events.begin(), events.end(),
                  [](const std::pair<hwc2_display_t, int32_t>& a,
                     const std::pair<hwc2_display_t, int32_t>& b) {
                      return a.first < b.first;
                  });
        for (const auto& ev : events) {
            hotplug(callbackData, ev.first, ev.second);
        }
    }

    return Error::None;
}

void Hwc2Device::dump(uint32_t* outSize, char* outBuffer) {
    if (outBuffer != nullptr) {
        auto copiedBytes = mDumpString.copy(outBuffer, *outSize);
        *outSize = static_cast<uint32_t>(copiedBytes);
        return;
    }

    std::stringstream output;
    output << "-- Openfde HWC2 device --\n";

    std::lock_guard<std::recursive_mutex> lock(mStateMutex);
    output << "Displays:\n";
    for (const auto& pair : mDisplays) {
        output << pair.second->dump();
    }

    mDumpString = output.str();
    *outSize = static_cast<uint32_t>(mDumpString.size());
}

hwc2_function_pointer_t Hwc2Device::doGetFunction(FunctionDescriptor descriptor) {
    switch (descriptor) {
        // Device functions
        case FunctionDescriptor::CreateVirtualDisplay:
            return asFP<HWC2_PFN_CREATE_VIRTUAL_DISPLAY>(createVirtualDisplayHook);
        case FunctionDescriptor::DestroyVirtualDisplay:
            return asFP<HWC2_PFN_DESTROY_VIRTUAL_DISPLAY>(destroyVirtualDisplayHook);
        case FunctionDescriptor::Dump:
            return asFP<HWC2_PFN_DUMP>(dumpHook);
        case FunctionDescriptor::GetMaxVirtualDisplayCount:
            return asFP<HWC2_PFN_GET_MAX_VIRTUAL_DISPLAY_COUNT>(
                    getMaxVirtualDisplayCountHook);
        case FunctionDescriptor::RegisterCallback:
            return asFP<HWC2_PFN_REGISTER_CALLBACK>(registerCallbackHook);

        // Display functions
        case FunctionDescriptor::AcceptDisplayChanges:
            return asFP<HWC2_PFN_ACCEPT_DISPLAY_CHANGES>(
                    displayHook<decltype(&Hwc2Display::acceptChanges),
                                &Hwc2Display::acceptChanges>);
        case FunctionDescriptor::CreateLayer:
            return asFP<HWC2_PFN_CREATE_LAYER>(
                    displayHook<decltype(&Hwc2Display::createLayer),
                                &Hwc2Display::createLayer, hwc2_layer_t*>);
        case FunctionDescriptor::DestroyLayer:
            return asFP<HWC2_PFN_DESTROY_LAYER>(
                    displayHook<decltype(&Hwc2Display::destroyLayer),
                                &Hwc2Display::destroyLayer, hwc2_layer_t>);
        case FunctionDescriptor::GetActiveConfig:
            return asFP<HWC2_PFN_GET_ACTIVE_CONFIG>(
                    displayHook<decltype(&Hwc2Display::getActiveConfig),
                                &Hwc2Display::getActiveConfig, hwc2_config_t*>);
        case FunctionDescriptor::GetChangedCompositionTypes:
            return asFP<HWC2_PFN_GET_CHANGED_COMPOSITION_TYPES>(
                    displayHook<decltype(&Hwc2Display::getChangedCompositionTypes),
                                &Hwc2Display::getChangedCompositionTypes, uint32_t*,
                                hwc2_layer_t*, int32_t*>);
        case FunctionDescriptor::GetColorModes:
            return asFP<HWC2_PFN_GET_COLOR_MODES>(
                    displayHook<decltype(&Hwc2Display::getColorModes),
                                &Hwc2Display::getColorModes, uint32_t*, int32_t*>);
        case FunctionDescriptor::GetDisplayAttribute:
            return asFP<HWC2_PFN_GET_DISPLAY_ATTRIBUTE>(getDisplayAttributeHook);
        case FunctionDescriptor::GetDisplayConfigs:
            return asFP<HWC2_PFN_GET_DISPLAY_CONFIGS>(
                    displayHook<decltype(&Hwc2Display::getConfigs), &Hwc2Display::getConfigs,
                                uint32_t*, hwc2_config_t*>);
        case FunctionDescriptor::GetDisplayName:
            return asFP<HWC2_PFN_GET_DISPLAY_NAME>(
                    displayHook<decltype(&Hwc2Display::getName), &Hwc2Display::getName,
                                uint32_t*, char*>);
        case FunctionDescriptor::GetDisplayRequests:
            return asFP<HWC2_PFN_GET_DISPLAY_REQUESTS>(
                    displayHook<decltype(&Hwc2Display::getRequests),
                                &Hwc2Display::getRequests, int32_t*, uint32_t*,
                                hwc2_layer_t*, int32_t*>);
        case FunctionDescriptor::GetDisplayType:
            return asFP<HWC2_PFN_GET_DISPLAY_TYPE>(
                    displayHook<decltype(&Hwc2Display::getType), &Hwc2Display::getType,
                                int32_t*>);
        case FunctionDescriptor::GetDozeSupport:
            return asFP<HWC2_PFN_GET_DOZE_SUPPORT>(
                    displayHook<decltype(&Hwc2Display::getDozeSupport),
                                &Hwc2Display::getDozeSupport, int32_t*>);
        case FunctionDescriptor::GetHdrCapabilities:
            return asFP<HWC2_PFN_GET_HDR_CAPABILITIES>(
                    displayHook<decltype(&Hwc2Display::getHdrCapabilities),
                                &Hwc2Display::getHdrCapabilities, uint32_t*, int32_t*,
                                float*, float*, float*>);
        case FunctionDescriptor::GetReleaseFences:
            return asFP<HWC2_PFN_GET_RELEASE_FENCES>(
                    displayHook<decltype(&Hwc2Display::getReleaseFences),
                                &Hwc2Display::getReleaseFences, uint32_t*, hwc2_layer_t*,
                                int32_t*>);
        case FunctionDescriptor::PresentDisplay:
            return asFP<HWC2_PFN_PRESENT_DISPLAY>(
                    displayHook<decltype(&Hwc2Display::present), &Hwc2Display::present,
                                int32_t*>);
        case FunctionDescriptor::SetActiveConfig:
            return asFP<HWC2_PFN_SET_ACTIVE_CONFIG>(
                    displayHook<decltype(&Hwc2Display::setActiveConfig),
                                &Hwc2Display::setActiveConfig, hwc2_config_t>);
        case FunctionDescriptor::SetClientTarget:
            return asFP<HWC2_PFN_SET_CLIENT_TARGET>(
                    displayHook<decltype(&Hwc2Display::setClientTarget),
                                &Hwc2Display::setClientTarget, buffer_handle_t, int32_t,
                                int32_t, hwc_region_t>);
        case FunctionDescriptor::SetColorMode:
            return asFP<HWC2_PFN_SET_COLOR_MODE>(setColorModeHook);
        case FunctionDescriptor::SetColorTransform:
            return asFP<HWC2_PFN_SET_COLOR_TRANSFORM>(setColorTransformHook);
        case FunctionDescriptor::SetOutputBuffer:
            return asFP<HWC2_PFN_SET_OUTPUT_BUFFER>(
                    displayHook<decltype(&Hwc2Display::setOutputBuffer),
                                &Hwc2Display::setOutputBuffer, buffer_handle_t, int32_t>);
        case FunctionDescriptor::SetPowerMode:
            return asFP<HWC2_PFN_SET_POWER_MODE>(setPowerModeHook);
        case FunctionDescriptor::SetVsyncEnabled:
            return asFP<HWC2_PFN_SET_VSYNC_ENABLED>(setVsyncEnabledHook);
        case FunctionDescriptor::ValidateDisplay:
            return asFP<HWC2_PFN_VALIDATE_DISPLAY>(
                    displayHook<decltype(&Hwc2Display::validate), &Hwc2Display::validate,
                                uint32_t*, uint32_t*>);
        case FunctionDescriptor::GetClientTargetSupport:
            return asFP<HWC2_PFN_GET_CLIENT_TARGET_SUPPORT>(
                    displayHook<decltype(&Hwc2Display::getClientTargetSupport),
                                &Hwc2Display::getClientTargetSupport, uint32_t, uint32_t,
                                int32_t, int32_t>);

        // Layer functions
        case FunctionDescriptor::SetCursorPosition:
            return asFP<HWC2_PFN_SET_CURSOR_POSITION>(
                    layerHook<decltype(&Hwc2Layer::setCursorPosition),
                              &Hwc2Layer::setCursorPosition, int32_t, int32_t>);
        case FunctionDescriptor::SetLayerBuffer:
            return asFP<HWC2_PFN_SET_LAYER_BUFFER>(
                    layerHook<decltype(&Hwc2Layer::setBuffer), &Hwc2Layer::setBuffer,
                              buffer_handle_t, int32_t>);
        case FunctionDescriptor::SetLayerSurfaceDamage:
            return asFP<HWC2_PFN_SET_LAYER_SURFACE_DAMAGE>(
                    layerHook<decltype(&Hwc2Layer::setSurfaceDamage),
                              &Hwc2Layer::setSurfaceDamage, hwc_region_t>);

        // Layer state functions
        case FunctionDescriptor::SetLayerBlendMode:
            return asFP<HWC2_PFN_SET_LAYER_BLEND_MODE>(setLayerBlendModeHook);
        case FunctionDescriptor::SetLayerColor:
            return asFP<HWC2_PFN_SET_LAYER_COLOR>(
                    layerHook<decltype(&Hwc2Layer::setColor), &Hwc2Layer::setColor,
                              hwc_color_t>);
        case FunctionDescriptor::SetLayerCompositionType:
            return asFP<HWC2_PFN_SET_LAYER_COMPOSITION_TYPE>(setLayerCompositionTypeHook);
        case FunctionDescriptor::SetLayerDataspace:
            return asFP<HWC2_PFN_SET_LAYER_DATASPACE>(setLayerDataspaceHook);
        case FunctionDescriptor::SetLayerDisplayFrame:
            return asFP<HWC2_PFN_SET_LAYER_DISPLAY_FRAME>(
                    layerHook<decltype(&Hwc2Layer::setDisplayFrame),
                              &Hwc2Layer::setDisplayFrame, hwc_rect_t>);
        case FunctionDescriptor::SetLayerPlaneAlpha:
            return asFP<HWC2_PFN_SET_LAYER_PLANE_ALPHA>(
                    layerHook<decltype(&Hwc2Layer::setPlaneAlpha),
                              &Hwc2Layer::setPlaneAlpha, float>);
        case FunctionDescriptor::SetLayerSidebandStream:
            return asFP<HWC2_PFN_SET_LAYER_SIDEBAND_STREAM>(
                    layerHook<decltype(&Hwc2Layer::setSidebandStream),
                              &Hwc2Layer::setSidebandStream, const native_handle_t*>);
        case FunctionDescriptor::SetLayerSourceCrop:
            return asFP<HWC2_PFN_SET_LAYER_SOURCE_CROP>(
                    layerHook<decltype(&Hwc2Layer::setSourceCrop),
                              &Hwc2Layer::setSourceCrop, hwc_frect_t>);
        case FunctionDescriptor::SetLayerTransform:
            return asFP<HWC2_PFN_SET_LAYER_TRANSFORM>(setLayerTransformHook);
        case FunctionDescriptor::SetLayerVisibleRegion:
            return asFP<HWC2_PFN_SET_LAYER_VISIBLE_REGION>(
                    layerHook<decltype(&Hwc2Layer::setVisibleRegion),
                              &Hwc2Layer::setVisibleRegion, hwc_region_t>);
        case FunctionDescriptor::SetLayerZOrder:
            return asFP<HWC2_PFN_SET_LAYER_Z_ORDER>(
                    layerHook<decltype(&Hwc2Layer::setZ), &Hwc2Layer::setZ, uint32_t>);

        default:
            ALOGE("doGetFunction: Unknown function descriptor: %d (%s)",
                  static_cast<int32_t>(descriptor), to_string(descriptor).c_str());
            return nullptr;
    }
}

}  // namespace openfde
