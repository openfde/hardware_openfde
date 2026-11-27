/*
 * Copyright © 2022 Waydroid Project.
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

#include "egl-tools.h"
#include "../common/egl-tools-common.h"

#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gralloc_handle.h>

#include <log/log.h>
#include <semaphore.h>
#include <ui/GraphicBuffer.h>

static void x11_egl_on_context_created(struct display* display, EGLContext context, EGLSurface surface) {
    display->egl_ctx = context;
    display->egl_surface = surface;
}

static bool x11_egl_should_exit(struct display* display) {
    return display->egl_exit;
}

static void x11_egl_on_loop_exit(struct display* display) {
    // Tear down the per-display EGL resources. This runs on the EGL worker
    // thread (the one the context is current on) while the X connection is
    // still alive: destroy_display() joins this thread BEFORE it closes the X
    // connection, so the pbuffer surface and GL objects are still valid here.
    // egl_dpy is the shared default display, so it must NOT be eglTerminate()'d
    // (the primary display uses the same one).
    ALOGI("egl_loop: tearing down per-display EGL resources (ctx=%p surface=%p)",
          (void*)display->egl_ctx, (void*)display->egl_surface);
    if (display->gl_initialized) {
        glDeleteProgram(display->shader_program);
        glDeleteVertexArraysOES(1, &display->VAO);
        glDeleteBuffers(1, &display->VBO);
        glDeleteBuffers(1, &display->EBO);
        display->shader_program = 0;
        display->VAO = display->VBO = display->EBO = 0;
        display->gl_initialized = false;
    }
    eglMakeCurrent(display->egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (display->egl_surface != EGL_NO_SURFACE) {
        eglDestroySurface(display->egl_dpy, display->egl_surface);
        display->egl_surface = EGL_NO_SURFACE;
    }
    if (display->egl_ctx != EGL_NO_CONTEXT) {
        eglDestroyContext(display->egl_dpy, display->egl_ctx);
        display->egl_ctx = EGL_NO_CONTEXT;
    }
    eglReleaseThread();
}

static const EglToolsHooks kEglToolsHooks = {
    x11_egl_on_context_created,
    x11_egl_should_exit,
    x11_egl_on_loop_exit,
};

void* egl_loop(void* data) {
    return egl_common_loop(data, &kEglToolsHooks);
}

static const char* vertex_shader_source =
    "#version 300 es\n"
    "layout (location = 0) in vec2 aPos;\n"
    "layout (location = 1) in vec2 aTexCoord;\n"
    "out vec2 TexCoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "    TexCoord = aTexCoord;\n"
    "}\0";

static const char* fragment_shader_source =
    "#version 300 es\n"
    "precision mediump float;\n"
    "out vec4 FragColor;\n"
    "in vec2 TexCoord;\n"
    "uniform sampler2D ourTexture;\n"
    "void main() {\n"
    "    FragColor = texture(ourTexture, TexCoord);\n"
    "}\0";

// 编译着色器
static GLuint compile_shader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, NULL, infoLog);
        ALOGE("Shader compilation failed: %s\n", infoLog);
        return 0;
    }
    return shader;
}

static GLuint create_shader_program() {
    GLuint vertexShader = compile_shader(GL_VERTEX_SHADER, vertex_shader_source);
    GLuint fragmentShader = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_source);

    if (!vertexShader || !fragmentShader) {
        return 0;
    }

    GLuint shaderProgram = glCreateProgram();
    glAttachShader(shaderProgram, vertexShader);
    glAttachShader(shaderProgram, fragmentShader);
    glLinkProgram(shaderProgram);

    GLint success;
    glGetProgramiv(shaderProgram, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(shaderProgram, 512, NULL, infoLog);
        ALOGE("Shader program linking failed: %s\n", infoLog);
        return 0;
    }

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    return shaderProgram;
}

void egl_convert_buffer_to_BGRA_8888(struct display* display, android::sp<android::GraphicBuffer> src_buffer, android::sp<android::GraphicBuffer> dst_buffer) {
    //static GLuint shader_program = 0;
    //static GLuint VAO = 0, VBO = 0, EBO = 0;
    //static int gl_initialized = 0;

    if (!display->gl_initialized) {
        // 创建着色器程序
        display->shader_program = create_shader_program();
        if (!display->shader_program) {
            return;
        }

        // 设置顶点数据（全屏四边形）
        float vertices[] = {
            // 位置      // 纹理坐标
            -1.0f, -1.0f, 0.0f, 0.0f,
             1.0f, -1.0f, 1.0f, 0.0f,
             1.0f,  1.0f, 1.0f, 1.0f,
            -1.0f,  1.0f, 0.0f, 1.0f
        };
        unsigned int indices[] = {
            0, 1, 2,
            2, 3, 0
        };

        glGenVertexArraysOES(1, &display->VAO);
        glGenBuffers(1, &display->VBO);
        glGenBuffers(1, &display->EBO);

        glBindVertexArrayOES(display->VAO);
        glBindBuffer(GL_ARRAY_BUFFER, display->VBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, display->EBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(1);

        display->gl_initialized = 1;
    }

    EGLint image_attrs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };

    // Create EGLImage for source buffer
    auto src_image = eglCreateImageKHR(display->egl_dpy, EGL_NO_CONTEXT,
                                    EGL_NATIVE_BUFFER_ANDROID, (EGLClientBuffer) src_buffer->getNativeBuffer(),
                                    image_attrs);
    if (src_image == EGL_NO_IMAGE_KHR) {
        ALOGE("Failed to create EGLImage from source buffer: 0x%x", eglGetError());
        return;
    }

    // Create EGLImage for destination buffer
    auto dst_image = eglCreateImageKHR(display->egl_dpy, EGL_NO_CONTEXT,
                                    EGL_NATIVE_BUFFER_ANDROID, (EGLClientBuffer) dst_buffer->getNativeBuffer(),
                                    image_attrs);
    if (dst_image == EGL_NO_IMAGE_KHR) {
        ALOGE("Failed to create EGLImage from destination buffer: 0x%x", eglGetError());
        eglDestroyImageKHR(display->egl_dpy, src_image);
        return;
    }

    // Create texture for source buffer
    GLuint src_texture;
    glGenTextures(1, &src_texture);
    glBindTexture(GL_TEXTURE_2D, src_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, src_image);

    // Create texture for destination buffer
    GLuint dst_texture;
    glGenTextures(1, &dst_texture);
    glBindTexture(GL_TEXTURE_2D, dst_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, dst_image);

    // Create framebuffer and bind destination texture
    GLuint framebuffer;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst_texture, 0);

    // Check framebuffer completeness
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        ALOGE("Framebuffer not complete");
	    glBindFramebuffer(GL_FRAMEBUFFER, 0);
	    glDeleteFramebuffers(1, &framebuffer);
	    glDeleteTextures(1, &src_texture);
	    glDeleteTextures(1, &dst_texture);
	    eglDestroyImageKHR(display->egl_dpy, src_image);
	    eglDestroyImageKHR(display->egl_dpy, dst_image);
	return ;
    }

    // Set viewport and use shader program
    glViewport(0, 0, src_buffer->getWidth(), src_buffer->getHeight());
    glUseProgram(display->shader_program);

    // Set uniform variable
    GLint texture_location = glGetUniformLocation(display->shader_program, "ourTexture");
    glUniform1i(texture_location, 0);

    // Bind source texture and render
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src_texture);

    glBindVertexArrayOES(display->VAO);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    glFinish();

    // Cleanup
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &src_texture);
    glDeleteTextures(1, &dst_texture);
    eglDestroyImageKHR(display->egl_dpy, src_image);
    eglDestroyImageKHR(display->egl_dpy, dst_image);
}
