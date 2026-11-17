#include "OpenfdeLauncherAidl.h"

#include <android-base/logging.h>
#include <log/log.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fstream>
#include <sstream>
#include <vector>

namespace aidl {
namespace vendor {
namespace openfde {
namespace launcher {

namespace {

// 去掉 .desktop Exec 里的参数占位符（%U/%F/%f 等），和 fde_ctrl 的
// removeDesktopArgs() 行为保持一致：只保留可执行文件本身和普通参数。
std::string removeDesktopArgs(const std::string& path) {
    std::istringstream iss(path);
    std::vector<std::string> fields;
    std::string token;
    while (iss >> token) {
        fields.push_back(token);
    }
    while (!fields.empty() && fields.back().size() >= 2 && fields.back()[0] == '%') {
        fields.pop_back();
    }
    std::string out;
    for (const auto& f : fields) {
        if (!out.empty()) out += " ";
        out += f;
    }
    return out;
}

bool writeScript(const std::string& file, const std::string& content) {
    std::ofstream ofs(file, std::ios::out | std::ios::trunc);
    if (!ofs.is_open()) {
        return false;
    }
    ofs << content;
    ofs.close();
    chmod(file.c_str(), 0777);
    return true;
}

}  // namespace

OpenfdeLauncherAidl::OpenfdeLauncherAidl() = default;

ndk::ScopedAStatus OpenfdeLauncherAidl::launchLinuxApp(
        const std::string& app, const std::string& path, const std::string& display,
        const std::string& workingPath, bool withOutTheme, int32_t* _aidl_return) {
    if (_aidl_return == nullptr) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_NULL_POINTER);
    }
    *_aidl_return = -1;

    if (app.empty() || path.empty()) {
        ALOGE("launchLinuxApp: empty app or path");
        return ndk::ScopedAStatus::ok();
    }

    const std::string execLine = removeDesktopArgs(path);
    const std::string outDisplay = display.empty() ? ":1001" : display;

    std::ostringstream script;
    script << "#!/bin/bash\n";
    script << "export GDK_BACKEND=x11\n";
    script << "export QT_QPA_PLATFORM=xcb\n";
    if (!withOutTheme) {
        script << "export QT_QPA_PLATFORMTHEME=ukui\n";
    }
    script << "export DISPLAY=" << outDisplay << "\n";
    if (!workingPath.empty()) {
        script << "cd " << workingPath << " || true\n";
    }
    script << execLine << "\n";

    const std::string scriptPath = "/tmp/xserver_" + app;
    if (!writeScript(scriptPath, script.str())) {
        ALOGE("launchLinuxApp: write %s failed", scriptPath.c_str());
        return ndk::ScopedAStatus::ok();
    }

    pid_t pid = fork();
    if (pid < 0) {
        ALOGE("launchLinuxApp: fork failed");
        return ndk::ScopedAStatus::ok();
    }
    if (pid == 0) {
        setsid();
        execl("/bin/bash", "bash", scriptPath.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    ALOGI("launchLinuxApp: app=%s pid=%d display=%s", app.c_str(), pid, outDisplay.c_str());
    *_aidl_return = pid;
    return ndk::ScopedAStatus::ok();
}

}  // namespace launcher
}  // namespace openfde
}  // namespace vendor
}  // namespace aidl
