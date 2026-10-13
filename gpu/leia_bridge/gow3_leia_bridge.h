// SPDX-FileCopyrightText: Copyright 2026 Marcus André (Leia3DBridge), gow3 contributors
// SPDX-License-Identifier: MIT
//
// C interface between gow3 and gow3_leia.dll, the weaver for Leia SR 3D monitors (Samsung
// Odyssey 3D and others). The port contains no Leia SR code: it loads the bridge at run time
// when it is present next to gow3-probe.exe. The bridge weaves with the SR OpenGL weaver in a
// context of its own: the port shares the side-by-side image and the output image with it as
// Win32 handles to exportable Vulkan memory (GL_EXT_memory_object_win32), with exportable
// semaphores around each weave (GL_EXT_semaphore_win32).

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GOW3_LEIA_BRIDGE_ABI 1

typedef void (*Gow3LeiaLogFn)(const char* message);

typedef struct Gow3LeiaImage {
    void* memory;   ///< HANDLE of the image's memory (VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32)
    uint64_t size;  ///< allocation size in bytes
    uint32_t width;
    uint32_t height;
} Gow3LeiaImage;

typedef struct Gow3LeiaApi {
    uint32_t abi;
    /// Connects to the SR service: context, display, eye tracking. 0 when no SR display.
    int (*Initialize)(Gow3LeiaLogFn log);
    /// The display's recommended size of one view (each half of the side-by-side image).
    void (*RecommendedView)(uint32_t* width, uint32_t* height);
    /// On the thread that calls Weave: an OpenGL context of the bridge's own, the images and
    /// semaphores imported, the weaver for `window` (its position on the display sets the
    /// lens phase). Both images hold 8-bit RGBA texels; `input_bgra`: the input's bytes are in
    /// B, G, R, A order (swizzled when sampled). Called again when sizes change. 0 on failure.
    int (*Attach)(void* window, const Gow3LeiaImage* input, int input_bgra,
                  const Gow3LeiaImage* output, void* ready_semaphore, void* done_semaphore);
    /// Waits for `ready` (input in SHADER_READ_ONLY layout, output in COLOR_ATTACHMENT), weaves
    /// the side-by-side input into the output (left COLOR_ATTACHMENT), signals `done`.
    int (*Weave)(void);
    /// Switches the display's lenses (3D mode) on or off.
    void (*SetLens)(int enable);
    void (*Shutdown)(void);
} Gow3LeiaApi;

typedef const Gow3LeiaApi* (*Gow3LeiaGetApiFn)(void);

#ifdef __cplusplus
}
#endif
