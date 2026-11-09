#include "OpenfdeDisplayAidl.h"

#include <android/binder_manager.h>
#include <log/log.h>

#include <vendor/openfde/display/1.3/IOpenfdeDisplayCallback.h>

// Error 类型来自 composer 2.1
using ::android::hardware::graphics::composer::V2_1::Error;

namespace aidl {
namespace vendor {
namespace openfde {
namespace display {

namespace {

// HIDL 回调：hwcomposer 进程通过它把外部屏事件转发出来，这里再转给 SystemUI
// 注册的 AIDL 回调（跨进程 oneway，不会阻塞）。
class HidlExternalDisplayCallback
    : public ::vendor::openfde::display::V1_3::IOpenfdeDisplayCallback {
public:
    explicit HidlExternalDisplayCallback(
        std::shared_ptr<::aidl::vendor::openfde::display::IOpenfdeDisplayCallback> cb)
        : mAidlCb(std::move(cb)) {}

    ::android::hardware::Return<void> onExternalDisplayEvent(int32_t event) override {
        if (mAidlCb) {
            mAidlCb->onExternalDisplayEvent(event);
        }
        return ::android::hardware::Void();
    }

private:
    std::shared_ptr<::aidl::vendor::openfde::display::IOpenfdeDisplayCallback> mAidlCb;
};

}  // namespace

OpenfdeDisplayAidl::OpenfdeDisplayAidl() {
    // 注意：不能用 IOpenfdeDisplay::getService()，
    // 因为名字和 AIDL 的 IOpenfdeDisplay 冲突了，且这里要的是 HIDL 的。
    mHidlDisplay = ::vendor::openfde::display::V1_2::IOpenfdeDisplay::getService();
    if (mHidlDisplay == nullptr) {
        ALOGE("Failed to get HIDL IOpenfdeDisplay@1.2 service");
    }
    mHidlDisplayV3 = ::vendor::openfde::display::V1_3::IOpenfdeDisplay::getService();
    if (mHidlDisplayV3 == nullptr) {
        ALOGE("Failed to get HIDL IOpenfdeDisplay@1.3 service");
    }
}

ndk::ScopedAStatus OpenfdeDisplayAidl::openExternalDisplay(bool* _aidl_return) {
    ALOGE("OpenfdeDisplayAidl::openExternalDisplay called");
    if (mHidlDisplay == nullptr) {
        ALOGE("HIDL IOpenfdeDisplay is null");
        *_aidl_return = false;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }

    // Guard: only open when the backend reports an external display can be
    // hosted (e.g. x11 requires >=2 monitors in extended, non-mirror mode).
    // If not supported, do not attempt to open and return false early.
    auto sup = mHidlDisplay->isExternalDisplaySupported();
    if (!sup.isOk() || !sup.withDefault(false)) {
        ALOGI("openExternalDisplay skipped: external display not supported");
        *_aidl_return = false;
        return ndk::ScopedAStatus::ok();
    }

    auto ret = mHidlDisplay->openExternalDisplay();
    if (!ret.isOk()) {
        ALOGE("HIDL openExternalDisplay transaction failed");
        *_aidl_return = false;
        return ndk::ScopedAStatus::fromExceptionCode(EX_TRANSACTION_FAILED);
    }

    Error err = ret.withDefault(Error::BAD_PARAMETER);
    *_aidl_return = (err == Error::NONE);

    if (*_aidl_return) {
        ALOGI("HIDL openExternalDisplay success");
    } else {
        ALOGE("HIDL openExternalDisplay error=%d", static_cast<int>(err));
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus OpenfdeDisplayAidl::closeExternalDisplay(bool* _aidl_return) {
    ALOGE("OpenfdeDisplayAidl::closeExternalDisplay called");
    if (mHidlDisplay == nullptr) {
        ALOGE("HIDL IOpenfdeDisplay is null");
        *_aidl_return = false;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }

    auto ret = mHidlDisplay->closeExternalDisplay();
    if (!ret.isOk()) {
        ALOGE("HIDL closeExternalDisplay transaction failed");
        *_aidl_return = false;
        return ndk::ScopedAStatus::fromExceptionCode(EX_TRANSACTION_FAILED);
    }

    Error err = ret.withDefault(Error::BAD_PARAMETER);
    *_aidl_return = (err == Error::NONE);

    if (*_aidl_return) {
        ALOGI("HIDL closeExternalDisplay success");
    } else {
        ALOGE("HIDL closeExternalDisplay error=%d", static_cast<int>(err));
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus OpenfdeDisplayAidl::getExternalDisplayState(bool* _aidl_return) {
    ALOGE("OpenfdeDisplayAidl::getExternalDisplayState called");
    if (mHidlDisplay == nullptr) {
        ALOGE("HIDL IOpenfdeDisplay is null");
        *_aidl_return = false;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }

    auto ret = mHidlDisplay->getExternalDisplayState();
    if (!ret.isOk()) {
        ALOGE("HIDL getExternalDisplayState transaction failed");
        *_aidl_return = false;
        return ndk::ScopedAStatus::fromExceptionCode(EX_TRANSACTION_FAILED);
    }

    bool state = ret.withDefault(false);

    ALOGI("HIDL getExternalDisplayState returned: %s", state ? "true" : "false");
    *_aidl_return = state;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus OpenfdeDisplayAidl::isExternalDisplaySupported(bool* _aidl_return) {
    ALOGI("OpenfdeDisplayAidl::isExternalDisplaySupported called");
    if (mHidlDisplay == nullptr) {
        ALOGE("HIDL IOpenfdeDisplay is null");
        *_aidl_return = false;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }

    auto ret = mHidlDisplay->isExternalDisplaySupported();
    if (!ret.isOk()) {
        ALOGE("HIDL isExternalDisplaySupported transaction failed");
        *_aidl_return = false;
        return ndk::ScopedAStatus::fromExceptionCode(EX_TRANSACTION_FAILED);
    }

    bool supported = ret.withDefault(false);
    ALOGI("HIDL isExternalDisplaySupported returned: %s", supported ? "true" : "false");
    *_aidl_return = supported;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus OpenfdeDisplayAidl::registerCallback(
        const std::shared_ptr<IOpenfdeDisplayCallback>& cb) {
    ALOGI("OpenfdeDisplayAidl::registerCallback called");
    if (mHidlDisplayV3 == nullptr) {
        ALOGE("HIDL IOpenfdeDisplay@1.3 is null, cannot register event callback");
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }

    // Forward HIDL events coming from the hwcomposer process to the AIDL
    // callback that SystemUI registered with us.
    ::android::sp<HidlExternalDisplayCallback> hidlCb =
        new HidlExternalDisplayCallback(cb);
    auto ret = mHidlDisplayV3->registerCallback(hidlCb);
    if (!ret.isOk()) {
        ALOGE("HIDL registerCallback transaction failed");
        return ndk::ScopedAStatus::fromExceptionCode(EX_TRANSACTION_FAILED);
    }

    ALOGI("OpenfdeDisplayAidl registered external display event callback");
    return ndk::ScopedAStatus::ok();
}

}  // namespace display
}  // namespace openfde
}  // namespace vendor
}  // namespace aidl