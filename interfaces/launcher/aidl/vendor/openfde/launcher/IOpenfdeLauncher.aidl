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
 * Launch a Linux (X11) program on the host, replacing the HTTP -> fde_ctrl path.
 *
 * Parameters match fde_ctrl's POST /v1/xserver:
 *   app          application name (.desktop Name / linuxInfo.name)
 *   path         .desktop Exec line (args like %U are stripped)
 *   display      X display, e.g. ":1001"
 *   workingPath  directory to cd into before launch, may be empty
 *   withOutTheme drop QT_QPA_PLATFORMTHEME / XDG_CURRENT_DESKTOP when true
 *
 * Returns the pid of the launched process, or a negative value on failure.
 *
 * Note: the aidl_interface is currently unstable (not versioned), so this
 * interface must NOT carry @VintfStability. Add it back once the interface is
 * frozen with stability: "vintf".
 */
interface IOpenfdeLauncher {
    int launchLinuxApp(in String app, in String path, in String display,
                       in String workingPath, in boolean withOutTheme);
}
