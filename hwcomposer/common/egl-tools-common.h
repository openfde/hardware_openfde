/*
 * Copyright © 2026 Openfde Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#pragma once

#include <EGL/egl.h>

struct display;
struct buffer;

const char* eglStrError(EGLint err);
void egl_render_to_pixels(struct display* display, struct buffer* buf);

// Backend-specific points in the shared EGL worker lifecycle. A backend may
// pass NULL for hooks it does not need.
struct EglToolsHooks {
    void (*onContextCreated)(struct display* display, EGLContext context, EGLSurface surface);
    bool (*shouldExit)(struct display* display);
    void (*onLoopExit)(struct display* display);
};

void egl_common_init(struct display* display, const EglToolsHooks* hooks);
void* egl_common_loop(void* data, const EglToolsHooks* hooks);
