#include "OpenfdeDisplayAidl.h"

#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android-base/logging.h>
#include <log/log.h>

using aidl::vendor::openfde::display::OpenfdeDisplayAidl;

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(4);
    ABinderProcess_startThreadPool();

    std::shared_ptr<OpenfdeDisplayAidl> service =
        ndk::SharedRefBase::make<OpenfdeDisplayAidl>();

    const std::string instance =
        std::string(OpenfdeDisplayAidl::descriptor) + "/default";
    binder_status_t status =
        AServiceManager_addService(service->asBinder().get(), instance.c_str());
    CHECK(status == STATUS_OK);

    ALOGI("OpenfdeDisplay AIDL service started");
    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;
}