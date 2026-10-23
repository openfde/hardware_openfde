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

#pragma once

#define HWC2_INCLUDE_STRINGIFICATION
#define HWC2_USE_CPP11
#include <hardware/hwcomposer2.h>
#undef HWC2_INCLUDE_STRINGIFICATION
#undef HWC2_USE_CPP11

#include <cutils/native_handle.h>
#include <pthread.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace openfde {

// Buffer metadata carried by the vendor.openfde.display extension protocol.
// Following the HWC2 model, it is addressed per display (hwc2_display_t) and
// per layer (hwc2_layer_t) instead of the HWC1 layer array index.
struct handle_ext {
    uint32_t format = 0;
    uint32_t stride = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

class Hwc2Device;
class Hwc2Display;

// Per-layer state, addressed by hwc2_layer_t inside its owning display.
class Hwc2Layer {
  public:
    explicit Hwc2Layer(Hwc2Display& display, hwc2_layer_t id);

    hwc2_layer_t getId() const { return mId; }
    Hwc2Display& getDisplay() const { return mDisplay; }

    // HWC2 layer functions
    HWC2::Error setBuffer(buffer_handle_t buffer, int32_t acquireFence);
    HWC2::Error setCursorPosition(int32_t x, int32_t y);
    HWC2::Error setSurfaceDamage(hwc_region_t damage);

    // HWC2 layer state functions
    HWC2::Error setBlendMode(HWC2::BlendMode mode);
    HWC2::Error setColor(hwc_color_t color);
    HWC2::Error setCompositionType(HWC2::Composition type);
    HWC2::Error setDataspace(android_dataspace_t dataspace);
    HWC2::Error setDisplayFrame(hwc_rect_t frame);
    HWC2::Error setPlaneAlpha(float alpha);
    HWC2::Error setSidebandStream(const native_handle_t* stream);
    HWC2::Error setSourceCrop(hwc_frect_t crop);
    HWC2::Error setTransform(HWC2::Transform transform);
    HWC2::Error setVisibleRegion(hwc_region_t visible);
    HWC2::Error setZ(uint32_t z);

    // vendor.openfde.display extension, keyed by hwc2 layer id
    void setName(const std::string& name) { mName = name; }
    const std::string& getName() const { return mName; }
    void setHandleExt(handle_ext ext) { mHandleExt = ext; }
    const handle_ext& getHandleExt() const { return mHandleExt; }

    // Accessors used by the backends while presenting
    buffer_handle_t buffer() const { return mBuffer; }
    HWC2::Composition compositionType() const { return mCompositionType; }
    hwc_rect_t displayFrame() const { return mDisplayFrame; }
    hwc_frect_t sourceCrop() const { return mSourceCrop; }
    HWC2::Transform transform() const { return mTransform; }
    uint32_t z() const { return mZ; }
    float planeAlpha() const { return mPlaneAlpha; }

    // Returns the acquire fence and transfers its ownership to the caller.
    int takeAcquireFence();

  private:
    const hwc2_layer_t mId;
    Hwc2Display& mDisplay;

    buffer_handle_t mBuffer = nullptr;
    int32_t mAcquireFence = -1;
    HWC2::Composition mCompositionType = HWC2::Composition::Invalid;
    hwc_rect_t mDisplayFrame = {0, 0, 0, 0};
    hwc_frect_t mSourceCrop = {0.0f, 0.0f, 0.0f, 0.0f};
    HWC2::Transform mTransform = HWC2::Transform::None;
    HWC2::BlendMode mBlendMode = HWC2::BlendMode::None;
    float mPlaneAlpha = 1.0f;
    hwc_color_t mColor = {0, 0, 0, 0};
    android_dataspace_t mDataspace = HAL_DATASPACE_UNKNOWN;
    uint32_t mZ = 0;
    std::vector<hwc_rect_t> mVisibleRegion;
    std::vector<hwc_rect_t> mSurfaceDamage;
    int32_t mCursorX = 0;
    int32_t mCursorY = 0;

    std::string mName;
    handle_ext mHandleExt;
};

// Backend implemented by the windowing-system specific code (wayland/ x11/).
// The common HWC2 device owns all protocol state and forwards the actual
// composition work to the backend, always per display.
class Hwc2Backend {
  public:
    virtual ~Hwc2Backend() = default;

    void setDevice(Hwc2Device* device) { mDevice = device; }
    Hwc2Device* getDevice() const { return mDevice; }

    // Display configuration, queried per display
    virtual std::vector<hwc2_config_t> getConfigs(Hwc2Display& display) = 0;
    virtual HWC2::Error getActiveConfig(Hwc2Display& display, hwc2_config_t* outConfig) = 0;
    virtual HWC2::Error setActiveConfig(Hwc2Display& display, hwc2_config_t config) = 0;
    virtual HWC2::Error getAttribute(Hwc2Display& display, hwc2_config_t config,
                                     HWC2::Attribute attribute, int32_t* outValue) = 0;

    // Composition; the backend records composition type changes through
    // Hwc2Display::requestCompositionChange() during validate().
    virtual HWC2::Error validate(Hwc2Display& display) = 0;
    virtual HWC2::Error present(Hwc2Display& display) = 0;

    virtual HWC2::Error setPowerMode(Hwc2Display& /*display*/,
                                     HWC2::PowerMode /*mode*/) {
        return HWC2::Error::None;
    }

    virtual std::string dump(Hwc2Display& /*display*/) { return std::string(); }

    /*
     * Displays beyond the primary one, if the backend has any (e.g. the x11
     * external display). Called once from Hwc2Device's constructor, before
     * any client request; the backend reports each extra display with
     * Hwc2Device::createExternalDisplay(), which in turn emits the hotplug
     * event required by the HWC2 hotplug rules.
     */
    virtual void createExternalDisplays(Hwc2Device& /*device*/) {}

    // Tears down a display previously reported through
    // Hwc2Device::createExternalDisplay().
    virtual void destroyExternalDisplay(void* /*backendData*/) {}

  private:
    Hwc2Device* mDevice = nullptr;
};

// Per-display state and HWC2 display functions.
class Hwc2Display {
  public:
    Hwc2Display(Hwc2Device& device, hwc2_display_t id, HWC2::DisplayType type);

    // The primary display always uses id 0, as in the hwc2on1adapter
    static constexpr hwc2_display_t kPrimaryDisplayId = 0;

    hwc2_display_t getId() const { return mId; }
    Hwc2Device& getDevice() const { return mDevice; }
    HWC2::DisplayType getDisplayType() const { return mType; }
    bool isPrimary() const { return mId == kPrimaryDisplayId; }

    // Opaque backend data for this display (e.g. the backend's struct
    // display*). Backends use it to route a composer display to the right
    // windowing-system resources, which is how the external display is
    // supported: one backend display per composer display.
    void setBackendData(void* data) { mBackendData = data; }
    void* backendData() const { return mBackendData; }

    // HWC2 display functions
    HWC2::Error acceptChanges();
    HWC2::Error createLayer(hwc2_layer_t* outLayerId);
    HWC2::Error destroyLayer(hwc2_layer_t layerId);
    HWC2::Error getActiveConfig(hwc2_config_t* outConfigId);
    HWC2::Error getAttribute(hwc2_config_t configId, HWC2::Attribute attribute,
                             int32_t* outValue);
    HWC2::Error getChangedCompositionTypes(uint32_t* outNumElements,
                                           hwc2_layer_t* outLayers, int32_t* outTypes);
    HWC2::Error getColorModes(uint32_t* outNumModes, int32_t* outModes);
    HWC2::Error getConfigs(uint32_t* outNumConfigs, hwc2_config_t* outConfigIds);
    HWC2::Error getDozeSupport(int32_t* outSupport);
    HWC2::Error getHdrCapabilities(uint32_t* outNumTypes, int32_t* outTypes,
                                   float* outMaxLuminance, float* outMaxAverageLuminance,
                                   float* outMinLuminance);
    HWC2::Error getName(uint32_t* outSize, char* outName);
    HWC2::Error getReleaseFences(uint32_t* outNumElements, hwc2_layer_t* outLayers,
                                 int32_t* outFences);
    HWC2::Error getRequests(int32_t* outDisplayRequests, uint32_t* outNumElements,
                            hwc2_layer_t* outLayers, int32_t* outLayerRequests);
    HWC2::Error getType(int32_t* outType);
    HWC2::Error present(int32_t* outRetireFence);
    HWC2::Error setActiveConfig(hwc2_config_t configId);
    HWC2::Error setClientTarget(buffer_handle_t target, int32_t acquireFence,
                                int32_t dataspace, hwc_region_t damage);
    HWC2::Error setColorMode(android_color_mode_t mode);
    HWC2::Error setColorTransform(android_color_transform_t hint);
    HWC2::Error setOutputBuffer(buffer_handle_t buffer, int32_t releaseFence);
    HWC2::Error setPowerMode(HWC2::PowerMode mode);
    HWC2::Error setVsyncEnabled(HWC2::Vsync enabled);
    HWC2::Error validate(uint32_t* outNumTypes, uint32_t* outNumRequests);
    HWC2::Error getClientTargetSupport(uint32_t width, uint32_t height, int32_t format,
                                       int32_t dataspace);

    Hwc2Layer* getLayer(hwc2_layer_t layerId) const;

    // Layers sorted by ascending z order, used by the backends while
    // validating and presenting.
    std::vector<Hwc2Layer*> getLayersByZ() const;

    // Called by the backend during validate() to ask the client for a
    // composition type change, following the HWC2 validate/accept protocol.
    void requestCompositionChange(Hwc2Layer& layer, HWC2::Composition type);

    // Client target (replaces the HWC1 HWC_FRAMEBUFFER_TARGET layer)
    buffer_handle_t clientTarget() const { return mClientTarget; }
    int takeClientTargetAcquireFence();
    void setClientTargetExt(handle_ext ext) { mClientTargetExt = ext; }
    const handle_ext& clientTargetExt() const { return mClientTargetExt; }

    void markGeometryChanged() { mGeometryChanged = true; }
    bool geometryChanged() const { return mGeometryChanged; }
    void resetGeometryChanged() { mGeometryChanged = false; }

    bool colorTransformForced() const { return mColorTransform != HAL_COLOR_TRANSFORM_IDENTITY; }

    std::string dump();

  private:
    Hwc2Device& mDevice;
    const hwc2_display_t mId;
    const HWC2::DisplayType mType;
    const std::string mName;

    // Backend resources backing this display
    void* mBackendData = nullptr;

    std::map<hwc2_layer_t, std::shared_ptr<Hwc2Layer>> mLayers;
    hwc2_layer_t mNextLayerId = 1;

    // Composition type changes requested during validateDisplay()
    std::map<hwc2_layer_t, HWC2::Composition> mTypeChanges;
    bool mValidated = false;

    buffer_handle_t mClientTarget = nullptr;
    int32_t mClientTargetAcquireFence = -1;
    handle_ext mClientTargetExt;

    android_color_mode_t mColorMode = HAL_COLOR_MODE_NATIVE;
    android_color_transform_t mColorTransform = HAL_COLOR_TRANSFORM_IDENTITY;
    HWC2::PowerMode mPowerMode = HWC2::PowerMode::On;

    bool mGeometryChanged = false;
};

// HWC2 device. Unlike the former HWC1 implementation, every display function
// is addressed per display through hwc2_display_t, as required by hwcomposer2.h.
class Hwc2Device : public hwc2_device_t {
  public:
    // primaryBackendData is the opaque backend data of the primary display,
    // see Hwc2Display::setBackendData().
    Hwc2Device(std::unique_ptr<Hwc2Backend> backend, void* primaryBackendData);
    ~Hwc2Device();
    Hwc2Device(const Hwc2Device&) = delete;
    Hwc2Device& operator=(const Hwc2Device&) = delete;

    // Starts the vsync thread and the vendor display extension service.
    // Must be called once, after construction, from hwc_open().
    void startServices();

    Hwc2Backend& backend() { return *mBackend; }

    Hwc2Display* getDisplay(hwc2_display_t id) const;

    // Backend-facing display management. Allocates a display id for the given
    // backend data and reports the display to the client with a hotplug event.
    Hwc2Display* createExternalDisplay(void* backendData);
    HWC2::Error destroyExternalDisplay(hwc2_display_t displayId);

    // Recursion is required since present()/validate() call back into the
    // backend, which may query display state that takes the same lock.
    std::recursive_mutex& stateMutex() { return mStateMutex; }

    // Vsync clock, shared with the backend (e.g. wayland presentation
    // feedback updates the last vsync timestamp).
    void setVsyncTimestamp(int64_t timestampNs);
    void setVsyncPeriod(int32_t periodNs);
    void setVsyncEnabled(hwc2_display_t displayId, bool enabled);

    // Creates a present fence on the sw_sync timeline.
    int createPresentFence();

    static inline Hwc2Device* fromDevice(hwc2_device_t* device) {
        return static_cast<Hwc2Device*>(device);
    }

    // hwc2_device_t entry points and hook helpers, also used by the static
    // hook functions in hwc2_device.cpp
    HWC2::Error registerCallback(HWC2::Callback descriptor,
                                 hwc2_callback_data_t callbackData,
                                 hwc2_function_pointer_t pointer);
    void dump(uint32_t* outSize, char* outBuffer);

    template <typename... Args>
    static int32_t callDisplayFunction(hwc2_device_t* device, hwc2_display_t displayId,
                                       HWC2::Error (Hwc2Display::*member)(Args...),
                                       Args... args);

    template <typename MF, MF memFunc, typename... Args>
    static int32_t displayHook(hwc2_device_t* device, hwc2_display_t displayId,
                               Args... args);

    template <typename... Args>
    static int32_t callLayerFunction(hwc2_device_t* device, hwc2_display_t displayId,
                                     hwc2_layer_t layerId,
                                     HWC2::Error (Hwc2Layer::*member)(Args...),
                                     Args... args);

    template <typename MF, MF memFunc, typename... Args>
    static int32_t layerHook(hwc2_device_t* device, hwc2_display_t displayId,
                             hwc2_layer_t layerId, Args... args);

  private:
    void doGetCapabilities(uint32_t* outCount, int32_t* outCapabilities);
    hwc2_function_pointer_t doGetFunction(HWC2::FunctionDescriptor descriptor);

    static void getCapabilitiesHook(hwc2_device_t* device, uint32_t* outCount,
                                    int32_t* outCapabilities);
    static hwc2_function_pointer_t getFunctionHook(hwc2_device_t* device,
                                                   int32_t intDescriptor);
    static int closeHook(hw_device_t* device);

    // vsync
    static void* vsyncThreadEntry(void* data);
    void vsyncThread();
    void hotplugEvent(hwc2_display_t displayId, bool connected);

    // vendor.openfde.display extension service
    static void* extensionThreadEntry(void* data);

    std::unique_ptr<Hwc2Backend> mBackend;

    mutable std::recursive_mutex mStateMutex;
    std::map<hwc2_display_t, std::shared_ptr<Hwc2Display>> mDisplays;

    struct CallbackInfo {
        hwc2_callback_data_t data = nullptr;
        hwc2_function_pointer_t pointer = nullptr;
    };
    std::map<HWC2::Callback, CallbackInfo> mCallbacks;

    // Hotplug events that happened before the client registered the callback
    std::vector<std::pair<hwc2_display_t, int32_t>> mPendingHotplugs;

    // vsync state, protected by mVsyncLock
    pthread_mutex_t mVsyncLock;
    pthread_t mVsyncThread{};
    std::atomic<bool> mVsyncThreadExit{false};
    std::map<hwc2_display_t, bool> mVsyncEnabled;
    int32_t mVsyncPeriodNs = 1000 * 1000 * 1000 / 60;
    int64_t mLastVsyncNs = 0;

    pthread_t mExtensionThread{};

    int mTimelineFd = -1;
    int mNextSyncPoint = 1;

    std::string mDumpString;
};

}  // namespace openfde
