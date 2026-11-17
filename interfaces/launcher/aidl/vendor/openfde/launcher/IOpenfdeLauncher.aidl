/*
 * Copyright (C) 2025 The OpenFDE Project
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

package vendor.openfde.launcher;

/**
 * 在宿主机上启动 Linux（X11）程序，替代原来的 HTTP -> fde_ctrl 路径。
 *
 * 参数与 fde_ctrl 的 POST /v1/xserver 一一对应：
 *   app          应用名（.desktop 的 Name / linuxInfo.name），用于脚本名和日志
 *   path         .desktop 的 Exec 命令行（可带参数，%U 之类会被去掉）
 *   display      X display，例如 ":1001"
 *   workingPath  启动前 cd 的目录，可为空
 *   withOutTheme 是否去掉 QT_QPA_PLATFORMTHEME / XDG_CURRENT_DESKTOP
 *
 * 返回启动进程的 pid；失败返回负数。
 */
@VintfStability
interface IOpenfdeLauncher {
    int launchLinuxApp(in String app, in String path, in String display,
                       in String workingPath, in boolean withOutTheme);
}
