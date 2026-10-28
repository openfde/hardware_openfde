/*
 * Copyright (C) 2026 Openfde Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#include "screen_config.h"

#include <new>

#include <log/log.h>

// Preset resolutions offered in addition to the real one.
static const ScreenConfig presetScreenConfigs[] = {
    {1280,720,160},
    {1920,1080,160}, // Full HD
    {2560,1440,256},  // 2K
    {2560,1600,256}
};
static const int presetScreenConfigCount =
    sizeof(presetScreenConfigs) / sizeof(presetScreenConfigs[0]);

ScreenConfig *screenConfigs = nullptr;
int gScreenConfigCount = 0;

static int screen_config_density_for_width(int width) {
    if (width >= 2560) {
        return 256;
    } else if (width <= 1920) {
        return 160;
    }
    return 0;
}

void screen_configs_init(int realWidth, int realHeight) {
    screen_configs_destroy();

    int count = 1; // slot 0 is always the real resolution
    for (int i = 0; i < presetScreenConfigCount; i++) {
        if (presetScreenConfigs[i].Width <= realWidth &&
            presetScreenConfigs[i].Height <= realHeight &&
            (presetScreenConfigs[i].Width != realWidth ||
             presetScreenConfigs[i].Height != realHeight)) {
            count++;
        }
    }

    screenConfigs = new (std::nothrow) ScreenConfig[count];
    if (!screenConfigs) {
        ALOGE("failed to allocate screen configs");
        return;
    }

    screenConfigs[defaultConfigIndex] = {realWidth, realHeight,
                                         screen_config_density_for_width(realWidth)};

    int slot = 1;
    for (int i = 0; i < presetScreenConfigCount; i++) {
        if (presetScreenConfigs[i].Width == realWidth &&
            presetScreenConfigs[i].Height == realHeight) {
            ALOGI("skip preset %dx%d: same as real resolution",
                  presetScreenConfigs[i].Width, presetScreenConfigs[i].Height);
            continue;
        }
        if (presetScreenConfigs[i].Width > realWidth ||
            presetScreenConfigs[i].Height > realHeight) {
            ALOGI("skip preset %dx%d: exceeds real resolution %dx%d",
                  presetScreenConfigs[i].Width, presetScreenConfigs[i].Height,
                  realWidth, realHeight);
            continue;
        }
        screenConfigs[slot++] = presetScreenConfigs[i];
    }
    gScreenConfigCount = count;
    ALOGI("screen configs initialized: count=%d real=%dx%d",
          gScreenConfigCount, realWidth, realHeight);
}

void screen_configs_destroy() {
    if (screenConfigs) {
        delete[] screenConfigs;
        screenConfigs = nullptr;
    }
    gScreenConfigCount = 0;
}

int screen_config_get_index(int width, int height){
    int index = -1;
    for(int i = 0; i < gScreenConfigCount; i++){
        if(screenConfigs[i].Width == width && screenConfigs[i].Height == height){
            index = i;
            break;
        }
    }
    return index;
}

int screen_config_set_and_get_index(int width, int height) {
    int index = screen_config_get_index(width, height);
    if(index >= 0){
        return index;
    }else{
        if (gScreenConfigCount <= 0) {
            ALOGE("screen configs not initialized");
            return -1;
        }
        screenConfigs[defaultConfigIndex].Width = width;
        screenConfigs[defaultConfigIndex].Height = height;
        screenConfigs[defaultConfigIndex].Density = screen_config_density_for_width(width);
        return defaultConfigIndex;
    }
}

bool screen_config_is_active(bool isPrimary, int active_config, int config) {
    if (!isPrimary) {
        // External displays only have the single fixed config
        return config == defaultConfigIndex;
    }
    int active = active_config;
    if (active < 0 || active >= gScreenConfigCount) {
        // Table not ready yet: config 0 (the real resolution) is active
        active = defaultConfigIndex;
    }
    return config == active;
}
