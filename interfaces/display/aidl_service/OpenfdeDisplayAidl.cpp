#include "OpenfdeDisplayAidl.h"

#include <android/binder_manager.h>
#include <log/log.h>

// 1.2 的 HIDL 代理
using ::vendor::openfde::display::V1_2::IOpenfdeDisplay;
// Error 类型来自 composer 2.1
using ::android::hardware::graphics::composer::V2_1::Error;

namespace aidl {
namespace vendor {
namespace openfde {
namespace display {

OpenfdeDisplayAidl::OpenfdeDisplayAidl() {
    // 注意：不能用 IOpenfdeDisplay::getService()，
    // 因为名字和 AIDL 的 IOpenfdeDisplay 冲突了，且这里要的是 HIDL 的。
    mHidlDisplay = ::vendor::openfde::display::V1_2::IOpenfdeDisplay::getService();
    if (mHidlDisplay == nullptr) {
        ALOGE("Failed to get HIDL IOpenfdeDisplay@1.2 service");
    }
}

ndk::ScopedAStatus OpenfdeDisplayAidl::openExternalDisplay(bool* _aidl_return) {
    ALOGE("OpenfdeDisplayAidl::openExternalDisplay called");
    if (mHidlDisplay == nullptr) {
        ALOGE("HIDL IOpenfdeDisplay is null");
        *_aidl_return = false;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
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

}  // namespace display
}  // namespace openfde
}  // namespace vendor
}  // namespace aidl