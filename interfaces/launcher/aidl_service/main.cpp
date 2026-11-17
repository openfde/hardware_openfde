#include "OpenfdeLauncherAidl.h"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <log/log.h>

using aidl::vendor::openfde::launcher::OpenfdeLauncherAidl;

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(2);
    ABinderProcess_startThreadPool();

    std::shared_ptr<OpenfdeLauncherAidl> service =
        ndk::SharedRefBase::make<OpenfdeLauncherAidl>();

    const std::string instance =
        std::string(OpenfdeLauncherAidl::descriptor) + "/default";
    binder_status_t status =
        AServiceManager_addService(service->asBinder().get(), instance.c_str());
    CHECK(status == STATUS_OK);

    ALOGI("OpenfdeLauncher AIDL service started");
    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;
}
