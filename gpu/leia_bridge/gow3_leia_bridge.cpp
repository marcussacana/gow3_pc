// SPDX-FileCopyrightText: Copyright 2026 Marcus André (Leia3DBridge), gow3 contributors
// SPDX-License-Identifier: MIT
//
// gow3_leia.dll: the Leia SR OpenGL weaver behind a C interface (gow3_leia_bridge.h). Adapted
// from Leia3DBridge (NewAxis), which weaves an OpenGL texture into an OpenGL window; here the
// input and output are Vulkan images imported into a hidden OpenGL context, and the port
// presents the woven image itself.

#include "gow3_leia_bridge.h"

#include <windows.h>

#include <GL/gl.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <string>

#include "sr/management/srcontext.h"
#include "sr/sense/display/switchablehint.h"
#include "sr/sense/eyetracker/eyetracker.h"
#include "sr/weaver/glweaver.h"
#include "sr/world/display/display.h"

namespace {

// OpenGL 3.0 framebuffers and EXT_memory_object(_win32) / EXT_semaphore(_win32), loaded with
// wglGetProcAddress (opengl32.lib exports OpenGL 1.1 only).
using GLuint64 = unsigned long long;
constexpr GLenum GL_RGBA8_ = 0x8058;
constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
constexpr GLenum GL_COLOR_ATTACHMENT0 = 0x8CE0;
constexpr GLenum GL_FRAMEBUFFER_COMPLETE = 0x8CD5;
constexpr GLenum GL_READ_FRAMEBUFFER = 0x8CA8;
constexpr GLenum GL_DRAW_FRAMEBUFFER = 0x8CA9;
constexpr GLenum GL_HANDLE_TYPE_OPAQUE_WIN32_EXT = 0x9587;
constexpr GLenum GL_TEXTURE_TILING_EXT = 0x9580;
constexpr GLenum GL_OPTIMAL_TILING_EXT = 0x9584;
constexpr GLenum GL_LAYOUT_COLOR_ATTACHMENT_EXT = 0x958E;
constexpr GLenum GL_LAYOUT_SHADER_READ_ONLY_EXT = 0x9591;
constexpr GLenum GL_TEXTURE_SWIZZLE_R = 0x8E42;
constexpr GLenum GL_TEXTURE_SWIZZLE_B = 0x8E44;

#define GL_FUNCTIONS(X)                                                                            \
    X(void, glCreateMemoryObjectsEXT, GLsizei, GLuint*)                                            \
    X(void, glDeleteMemoryObjectsEXT, GLsizei, const GLuint*)                                      \
    X(void, glImportMemoryWin32HandleEXT, GLuint, GLuint64, GLenum, void*)                         \
    X(void, glTexStorageMem2DEXT, GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64)     \
    X(void, glGenSemaphoresEXT, GLsizei, GLuint*)                                                  \
    X(void, glDeleteSemaphoresEXT, GLsizei, const GLuint*)                                         \
    X(void, glImportSemaphoreWin32HandleEXT, GLuint, GLenum, void*)                                \
    X(void, glWaitSemaphoreEXT, GLuint, GLuint, const GLuint*, GLuint, const GLuint*,              \
      const GLenum*)                                                                               \
    X(void, glSignalSemaphoreEXT, GLuint, GLuint, const GLuint*, GLuint, const GLuint*,            \
      const GLenum*)                                                                               \
    X(void, glGenFramebuffers, GLsizei, GLuint*)                                                   \
    X(void, glDeleteFramebuffers, GLsizei, const GLuint*)                                          \
    X(void, glBindFramebuffer, GLenum, GLuint)                                                     \
    X(void, glFramebufferTexture2D, GLenum, GLenum, GLenum, GLuint, GLint)                         \
    X(GLenum, glCheckFramebufferStatus, GLenum)                                                        X(void, glBlitFramebuffer, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield,        GLenum)

#define DECLARE(ret, name, ...) using name##_t = ret(APIENTRY*)(__VA_ARGS__); name##_t name = nullptr;
GL_FUNCTIONS(DECLARE)
#undef DECLARE

Gow3LeiaLogFn g_log = nullptr;

void Log(const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (g_log) {
        g_log(text);
    }
}

SR::SRContext* g_context = nullptr;
SR::Display* g_display = nullptr;
SR::EyeTracker* g_eye_tracker = nullptr;
SR::SwitchableLensHint* g_lens = nullptr;
SR::IGLWeaver1* g_weaver = nullptr;
bool g_context_initialized = false;

// The OpenGL side: a hidden window for the context, the shared images and semaphores.
HWND g_gl_window = nullptr;
HDC g_gl_dc = nullptr;
HGLRC g_gl_context = nullptr;
GLuint g_memory[2] = {};
GLuint g_texture[2] = {}; ///< input (side by side), output (woven)
GLuint g_semaphore[2] = {}; ///< ready, done
// OpenGL counts rows from the bottom, Vulkan from the top: the shared images are upside down
// for OpenGL. The weaver's lens pattern follows gl_FragCoord, so it weaves upright copies
// (local textures) and the result is flipped back: rows match the panel's physical rows.
// GOW3_LEIA_FLIP (tests): bit 0 flips the input, bit 1 the output (default 3).
enum Local { FlippedInput, Woven };
GLuint g_local[2] = {};
GLuint g_framebuffer = 0;           ///< the shared output
GLuint g_local_framebuffer[2] = {}; ///< the local textures
GLuint g_input_framebuffer = 0;     ///< the shared input (blit source)
int g_flip = 3;
Gow3LeiaImage g_input{}, g_output{};

bool CreateGlContext() {
    if (g_gl_context) {
        return wglMakeCurrent(g_gl_dc, g_gl_context) != FALSE;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"gow3_leia_gl";
    RegisterClassW(&wc);
    g_gl_window = CreateWindowW(wc.lpszClassName, L"gow3 Leia", WS_OVERLAPPEDWINDOW, 0, 0, 1, 1,
                                nullptr, nullptr, wc.hInstance, nullptr);
    g_gl_dc = g_gl_window ? GetDC(g_gl_window) : nullptr;
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    const int format = g_gl_dc ? ChoosePixelFormat(g_gl_dc, &pfd) : 0;
    if (!format || !SetPixelFormat(g_gl_dc, format, &pfd) ||
        !(g_gl_context = wglCreateContext(g_gl_dc)) || !wglMakeCurrent(g_gl_dc, g_gl_context)) {
        Log("OpenGL context creation failed (error %lu)", GetLastError());
        return false;
    }
    bool missing = false;
#define LOAD(ret, name, ...)                                                                       \
    name = reinterpret_cast<name##_t>(wglGetProcAddress(#name));                                   \
    if (!name) {                                                                                   \
        Log("OpenGL function %s unavailable", #name);                                              \
        missing = true;                                                                            \
    }
    GL_FUNCTIONS(LOAD)
#undef LOAD
    Log("OpenGL %s on %s", reinterpret_cast<const char*>(glGetString(GL_VERSION)),
        reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    return !missing;
}

void ReleaseImages() {
    if (g_framebuffer) glDeleteFramebuffers(1, &g_framebuffer);
    if (g_input_framebuffer) glDeleteFramebuffers(1, &g_input_framebuffer);
    if (g_local_framebuffer[0]) glDeleteFramebuffers(2, g_local_framebuffer);
    if (g_local[0]) glDeleteTextures(2, g_local);
    g_input_framebuffer = g_local_framebuffer[0] = g_local_framebuffer[1] = 0;
    g_local[0] = g_local[1] = 0;
    if (g_texture[0]) glDeleteTextures(2, g_texture);
    if (g_memory[0]) glDeleteMemoryObjectsEXT(2, g_memory);
    if (g_semaphore[0]) glDeleteSemaphoresEXT(2, g_semaphore);
    g_framebuffer = 0;
    g_texture[0] = g_texture[1] = g_memory[0] = g_memory[1] = 0;
    g_semaphore[0] = g_semaphore[1] = 0;
}

bool Import(int index, const Gow3LeiaImage& image) {
    glCreateMemoryObjectsEXT(1, &g_memory[index]);
    glImportMemoryWin32HandleEXT(g_memory[index], image.size, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT,
                                 image.memory);
    glGenTextures(1, &g_texture[index]);
    glBindTexture(GL_TEXTURE_2D, g_texture[index]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
    glTexStorageMem2DEXT(GL_TEXTURE_2D, 1, GL_RGBA8_, GLsizei(image.width), GLsizei(image.height),
                         g_memory[index], 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        Log("Importing image %d (%ux%u) failed: GL error %#x", index, image.width, image.height,
            error);
        return false;
    }
    return true;
}

int Initialize(Gow3LeiaLogFn log) {
    g_log = log;
    if (g_context) {
        return 1;
    }
    try {
        g_context = SR::SRContext::create(false, SR::SRContext::StandaloneMode);
        if (!g_context) {
            Log("No SR context (is the Leia SR platform installed and running?)");
            return 0;
        }
        for (int tries = 0; tries < 20 && !g_display; ++tries) {
            g_display = SR::Display::create(*g_context);
            if (!g_display) Sleep(100);
        }
        if (!g_display) {
            Log("No SR display found");
            SR::SRContext::deleteSRContext(g_context);
            g_context = nullptr;
            return 0;
        }
        Log("SR display: recommended view %dx%d", g_display->getRecommendedViewsTextureWidth(),
            g_display->getRecommendedViewsTextureHeight());
        return 1;
    } catch (...) {
        Log("SR initialization threw an exception");
        g_context = nullptr;
        g_display = nullptr;
        return 0;
    }
}

void RecommendedView(uint32_t* width, uint32_t* height) {
    *width = g_display ? uint32_t(g_display->getRecommendedViewsTextureWidth()) : 0;
    *height = g_display ? uint32_t(g_display->getRecommendedViewsTextureHeight()) : 0;
}

int Attach(void* window, const Gow3LeiaImage* input, int input_bgra, const Gow3LeiaImage* output,
           void* ready_semaphore, void* done_semaphore) {
    if (!g_context || !CreateGlContext()) {
        return 0;
    }
    ReleaseImages();
    g_input = *input;
    g_output = *output;
    if (!Import(0, *input) || !Import(1, *output)) {
        return 0;
    }
    if (const char* flip = std::getenv("GOW3_LEIA_FLIP")) {
        g_flip = std::atoi(flip) & 3;
    }
    glGenTextures(2, g_local);
    const Gow3LeiaImage* sizes[2] = {input, output};
    for (int i = 0; i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, g_local[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8_, GLsizei(sizes[i]->width),
                     GLsizei(sizes[i]->height), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    if (input_bgra) {
        // The weaver samples the local copy (a blit ignores swizzles).
        glBindTexture(GL_TEXTURE_2D, g_local[FlippedInput]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(2, g_local_framebuffer);
    glGenFramebuffers(1, &g_input_framebuffer);
    const std::pair<GLuint, GLuint> attachments[] = {
        {g_local_framebuffer[0], g_local[0]},
        {g_local_framebuffer[1], g_local[1]},
        {g_input_framebuffer, g_texture[0]},
    };
    for (const auto& [framebuffer, texture] : attachments) {
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            Log("Local framebuffer incomplete");
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            return 0;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glGenSemaphoresEXT(2, g_semaphore);
    glImportSemaphoreWin32HandleEXT(g_semaphore[0], GL_HANDLE_TYPE_OPAQUE_WIN32_EXT,
                                    ready_semaphore);
    glImportSemaphoreWin32HandleEXT(g_semaphore[1], GL_HANDLE_TYPE_OPAQUE_WIN32_EXT,
                                    done_semaphore);
    glGenFramebuffers(1, &g_framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, g_framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_texture[1], 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        Log("Output framebuffer incomplete: %#x", status);
        return 0;
    }
    try {
        if (!g_weaver) {
            const WeaverErrorCode result =
                SR::CreateGLWeaver(*g_context, static_cast<HWND>(window), &g_weaver);
            if (result != WeaverSuccess) {
                Log("CreateGLWeaver failed: %d", int(result));
                g_weaver = nullptr;
                return 0;
            }
            // The port's frames are the game's sRGB-encoded bytes, read and written as stored.
            g_weaver->setShaderSRGBConversion(false, false);
            g_weaver->enableLateLatching(true);
        } else {
            g_weaver->setWindowHandle(static_cast<HWND>(window));
        }
        if (!g_context_initialized) {
            g_eye_tracker = SR::EyeTracker::create(*g_context);
            if (g_eye_tracker) {
                g_eye_tracker->start();
            } else {
                Log("No eye tracker: weaving for a fixed viewer position");
            }
            g_context->initialize();
            g_lens = SR::SwitchableLensHint::create(*g_context);
            g_context_initialized = true;
        }
    } catch (...) {
        Log("SR weaver setup threw an exception");
        return 0;
    }
    g_weaver->setInputViewTexture(g_local[FlippedInput], int(input->width), int(input->height),
                                  GL_RGBA8_);
    Log("Weaving %ux%u side by side into %ux%u (flip %d)", input->width, input->height,
        output->width, output->height, g_flip);
    return 1;
}

int Weave() {
    if (!g_weaver || !g_framebuffer) {
        return 0;
    }
    const GLenum layouts[2] = {GL_LAYOUT_SHADER_READ_ONLY_EXT, GL_LAYOUT_COLOR_ATTACHMENT_EXT};
    const GLenum write_layout = GL_LAYOUT_COLOR_ATTACHMENT_EXT;
    glWaitSemaphoreEXT(g_semaphore[0], 0, nullptr, 2, g_texture, layouts);
    const auto copy = [](GLuint from, GLuint to, const Gow3LeiaImage& size, bool flip) {
        const GLint w = GLint(size.width), h = GLint(size.height);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, from);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, to);
        glBlitFramebuffer(0, 0, w, h, 0, flip ? h : 0, w, flip ? 0 : h, GL_COLOR_BUFFER_BIT,
                          GL_NEAREST);
    };
    copy(g_input_framebuffer, g_local_framebuffer[FlippedInput], g_input, g_flip & 1);
    glBindFramebuffer(GL_FRAMEBUFFER, g_local_framebuffer[Woven]);
    glViewport(0, 0, GLsizei(g_output.width), GLsizei(g_output.height));
    g_weaver->weave();
    copy(g_local_framebuffer[Woven], g_framebuffer, g_output, g_flip & 2);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glSignalSemaphoreEXT(g_semaphore[1], 0, nullptr, 1, &g_texture[1], &write_layout);
    glFlush();
    return 1;
}

void SetLens(int enable) {
    if (!g_lens) {
        return;
    }
    try {
        if (enable) {
            g_lens->enable();
        } else {
            g_lens->disable();
        }
    } catch (...) {
        Log("Switching the lenses failed");
    }
}

void Shutdown() {
    SetLens(0);
    g_lens = nullptr;
    if (g_gl_context && wglMakeCurrent(g_gl_dc, g_gl_context)) {
        ReleaseImages();
    }
    if (g_weaver) {
        g_weaver->destroy();
        g_weaver = nullptr;
    }
    if (g_eye_tracker) {
        g_eye_tracker->stop();
        g_eye_tracker = nullptr;
    }
    if (g_context) {
        SR::SRContext::deleteSRContext(g_context);
        g_context = nullptr;
    }
    g_display = nullptr;
    g_context_initialized = false;
    if (g_gl_context) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(g_gl_context);
        g_gl_context = nullptr;
    }
    if (g_gl_window) {
        ReleaseDC(g_gl_window, g_gl_dc);
        DestroyWindow(g_gl_window);
        g_gl_window = nullptr;
    }
}

constexpr Gow3LeiaApi api = {GOW3_LEIA_BRIDGE_ABI, Initialize, RecommendedView, Attach,
                             Weave,                SetLens,    Shutdown};

} // namespace

extern "C" __declspec(dllexport) const Gow3LeiaApi* Gow3LeiaGetApi() {
    return &api;
}
