#pragma once

#include <aidl/vendor/openfde/display/BnOpenfdeDisplay.h>
#include <vendor/openfde/display/1.2/IOpenfdeDisplay.h>

namespace aidl {
namespace vendor {
namespace openfde {
namespace display {

class OpenfdeDisplayAidl : public BnOpenfdeDisplay {
public:
    OpenfdeDisplayAidl();

    ndk::ScopedAStatus openExternalDisplay(bool* _aidl_return) override;
    ndk::ScopedAStatus closeExternalDisplay(bool* _aidl_return) override;
    ndk::ScopedAStatus getExternalDisplayState(bool* _aidl_return) override;
    ndk::ScopedAStatus isExternalDisplaySupported(bool* _aidl_return) override;

private:
    // 1.2 版本继承自 1.1，持有 1.2 即可调用所有方法
    android::sp<::vendor::openfde::display::V1_2::IOpenfdeDisplay> mHidlDisplay;
};

}  // namespace display
}  // namespace openfde
}  // namespace vendor
}  // namespace aidl