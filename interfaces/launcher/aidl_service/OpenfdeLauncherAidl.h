#pragma once

#include <aidl/vendor/openfde/launcher/BnOpenfdeLauncher.h>

namespace aidl {
namespace vendor {
namespace openfde {
namespace launcher {

class OpenfdeLauncherAidl : public BnOpenfdeLauncher {
public:
    OpenfdeLauncherAidl();

    ndk::ScopedAStatus launchLinuxApp(const std::string& app,
                                      const std::string& path,
                                      const std::string& display,
                                      const std::string& workingPath,
                                      bool withOutTheme,
                                      int32_t* _aidl_return) override;
};

}  // namespace launcher
}  // namespace openfde
}  // namespace vendor
}  // namespace aidl
