#ifndef VENDOR_OPENFDE_DISPLAY_H
#define VENDOR_OPENFDE_DISPLAY_H

#include <stdint.h>
#include <map>

struct display_base {
    int width;
    int height;
    std::map<uint32_t, std::string> layer_names;
    std::map<uint32_t, struct handleExt> layer_handles_ext;
    struct handleExt target_layer_handle_ext;
};
#endif
