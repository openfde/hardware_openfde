/*
 * Copyright © 2021 Openfde Project.
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

#pragma once

#include <stdint.h>
#include <cutils/native_handle.h>

// Vendor-specific gralloc native handles shared by the X11 and Wayland
// backends. Field names, order and packing must match the corresponding
// gralloc implementations exactly.
typedef struct
{
    native_handle_t base;
    int fd[3];
    uint64_t ui64SUnknown;
    int uUnknown;
    int iWidth;
    int iHeight;
    int iFormat;
    unsigned int uiBUnknown;
    int iPUnknown;
    int aiSUnknown[3];
    int aiVSUnknown[3];
    uint64_t aulUnknownO[3];
    unsigned int auiMUnknownU[3];
    unsigned int auiNumUnknownVCs[3];
    unsigned int auiNumPUnknownPCs[3];
    int iNumSUnknownAs;
    int iLunKnown;

} __attribute__((aligned(sizeof(int)), packed)) X100_native_handle_t;

typedef struct {
    native_handle_t nativeHandle;

    /* file descriptors */
    int prime_fd;
    int  fd_runknown;
    int  fd_reunknown[8];

    /* integers */
    int magic;

    int flags;
    int size;
    int offset;
    uint64_t base __attribute__((aligned(8)));
    uint64_t phys __attribute__((aligned(8)));

    int width;
    int height;
    int format;
    int stride; /* the stride in bytes. */
    int bpp;

    int u_unknown;
    int p_unknown;

    uint32_t id;
    uint32_t num_planes;
    uint32_t strides[4];
    uint32_t offsets[4];
    uint32_t sizes[4];
    uint64_t format_modifier;
    uint64_t reserved_region_size;
    uint64_t total_size; /* Total allocation size */
    char name[64];

    uint64_t modifier;    
    uint64_t f_unknown[3] __attribute__((aligned(8)));
    uint64_t s_unknown;

    /* pointer to some bo struct. */
    uint64_t d_unknown __attribute__((aligned(8)));
    uint64_t r_unknown[3];
} gc_private_handle_t;
