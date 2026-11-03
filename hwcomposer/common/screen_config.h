/*
 * Copyright (C) 2026 Openfde Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

// Shared screen-configuration table for the HWC 1.4 multi-resolution API.
//
// Both backends (x11-hwc.cpp and wayland-hwc.cpp) carried a byte-identical
// copy of this table and the helpers that build/query it; it now lives here
// so there is a single definition of "which resolutions do we advertise".
//
// The table is process-wide state: slot 0 always holds the real (native)
// resolution, the remaining slots hold the presets that neither duplicate
// nor exceed it. hwc_composer.cpp reads it to answer
// getDisplayConfigs()/getDisplayAttributes()/setActiveConfig().

#pragma once

struct ScreenConfig {
    int Width;
    int Height;
    int Density;
};

const int defaultConfigIndex = 0;

extern int gScreenConfigCount;
extern ScreenConfig *screenConfigs;

// (Re)build the table for the given real resolution.
void screen_configs_init(int realWidth, int realHeight);
void screen_configs_destroy();

// Lookup helpers.
int screen_config_get_index(int width, int height);
// Returns the slot matching width/height, recording it as the real
// resolution (slot 0) when it is not a known preset.
int screen_config_set_and_get_index(int width, int height);

// Whether the queried config is the one currently in effect on the display.
// External displays only ever have the single fixed config (slot 0); primary
// displays track their active slot in active_config, which falls back to the
// real resolution until the table is ready.
bool screen_config_is_active(bool isPrimary, int active_config, int config);
