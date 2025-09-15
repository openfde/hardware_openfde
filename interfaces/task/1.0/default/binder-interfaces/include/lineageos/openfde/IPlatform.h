#ifndef AIDL_GENERATED_LINEAGEOS_WAYDROID_I_PLATFORM_H_
#define AIDL_GENERATED_LINEAGEOS_WAYDROID_I_PLATFORM_H_

#include <binder/IBinder.h>
#include <binder/IInterface.h>
#include <binder/Status.h>
#include <utils/String16.h>
#include <utils/StrongPointer.h>

namespace lineageos {

namespace openfde {

class IPlatform : public ::android::IInterface {
public:
  DECLARE_META_INTERFACE(Platform)
  virtual ::android::binder::Status getAppName(const ::android::String16& packageName, ::android::String16* _aidl_return) = 0;
  virtual ::android::binder::Status commitText(const ::android::String16& text) = 0;
  virtual ::android::binder::Status sendKeyEvent(int32_t action, int32_t code) = 0;
};  // class IPlatform

class IPlatformDefault : public IPlatform {
public:
  ::android::IBinder* onAsBinder() override;
  ::android::binder::Status getAppName(const ::android::String16& packageName, ::android::String16* _aidl_return) override;
  ::android::binder::Status commitText(const ::android::String16& text) override;
  ::android::binder::Status sendKeyEvent(int32_t action, int32_t code) override;

};

}  // namespace openfde

}  // namespace lineageos

#endif  // AIDL_GENERATED_LINEAGEOS_WAYDROID_I_PLATFORM_H_
