// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: output for Leia SR 3D monitors (docs/stereo.md). The side-by-side frame is woven for
// the display's lenses by gow3_leia.dll (gpu/leia_bridge, the Leia SR OpenGL weaver), loaded
// at run time when it is next to gow3-probe.exe. The input and output images and the two
// semaphores around each weave are exportable Vulkan objects the bridge imports into OpenGL.

#pragma once

#include <memory>

#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;

class LeiaWeaver {
public:
    /// The weaver when gow3_leia.dll is present and an SR display answers, else null (Problem()).
    static LeiaWeaver* Get();
    /// Why the Leia output is unavailable, or null.
    static const char* Problem();

    ~LeiaWeaver();

    /// (Re)creates the shared images for a side-by-side input of `input` and a woven output
    /// of `output` (the window's size: weaving is pixel exact), read by the presenter through
    /// `view_format`. `bgra`: the input's texels are B, G, R, A.
    bool Prepare(const Instance& instance, vk::Extent2D input, vk::Extent2D output,
                 vk::Format view_format, bool bgra, void* window);

    [[nodiscard]] vk::Image Input() const;
    [[nodiscard]] vk::Image Output() const;
    [[nodiscard]] vk::ImageView OutputView() const;
    [[nodiscard]] vk::Semaphore Ready() const;
    [[nodiscard]] vk::Semaphore Done() const;

    /// After a submission signalled Ready(): weaves; the next submission waits for Done().
    bool Weave();
    /// The display's lenses (3D mode): on while frames are woven.
    void SetLens(bool enable);

private:
    LeiaWeaver();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan
