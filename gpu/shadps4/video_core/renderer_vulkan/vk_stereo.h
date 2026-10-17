// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: stereo 3D side by side (docs/stereo.md). Each draw whose targets are all screen-sized
// is recorded twice: the left eye into the guest's images, the right eye into twins kept
// here. The eye offset is applied in the vertex shaders (PushData::stereo_sep). Twins never
// go back to guest memory; a write to the guest image by anything else makes the twin stale,
// and it is refilled from the guest image when next used.
#pragma once

#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "video_core/texture_cache/image.h"

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {
class Instance;
class Runtime;
class Scheduler;

namespace Stereo {
enum Eye : u32 { Left = 0, Right = 1 };

/// Screen-sized: the game's image or a power-of-two reduction of it (1920x1080 down to
/// 120x67, and 1920x1079). Shadow maps and other render targets are shared by both eyes.
constexpr bool EyeSized(u32 width, u32 height) {
    // The game's own size and the 4K render patch, halved down to 1/16.
    for (const u32 w : {1920u, 3840u}) {
        const u32 h = w * 9 / 16;
        for (u32 k = 0; k <= 4; ++k) {
            const u32 ew = w >> k, eh = h >> k;
            if (width + 1 >= ew && width <= ew + 1 && height + 1 >= eh && height <= eh + 1) {
                return true;
            }
        }
    }
    return false;
}

/// Clip-space shift of an eye: x += sep * (w - conv). `separation`: parallax at infinity in
/// percent of the image width (the clip range is 2 wide, each eye takes half).
constexpr float EyeSeparation(Eye eye, float separation_percent) {
    // Parallax at infinity between the eyes, in NDC (2 wide): 2 * sep = 2 * percent / 100.
    const float half = separation_percent / 100.f;
    return eye == Right ? half : -half;
}

static_assert(EyeSized(1920, 1080) && EyeSized(1920, 1079) && EyeSized(960, 540) &&
              EyeSized(120, 67) && EyeSized(3840, 2160));
static_assert(!EyeSized(2048, 2048) && !EyeSized(1, 1) && !EyeSized(64, 32) &&
              !EyeSized(512, 256) && !EyeSized(8, 9));
static_assert(EyeSeparation(Right, 3.f) == 0.03f && EyeSeparation(Left, 3.f) == -0.03f);

/// The screen-space shadow mask (docs/stereo.md, Research) rebuilds positions with the center
/// camera from its 24 constants: v = (d * ndc.x, d * ndc.y, z', w'), z' = -c20 d + c22,
/// w' = -c21 d + c23, shadow = v.x c[0..3] + v.y c[4..7] + z' c[8..11] + w' c[12..15].
/// An eye sees d * ndc_center.x = d * ndc.x - sep d + sep conv (EmitStereo), affine in d: it
/// folds into the z' and w' rows. False if z' and w' do not span d (left as they are).
/// shortcut: ignores the pop-out limit (pixels nearer than conv / (1 + popout / 2)), fine for
/// floors.
constexpr u64 ShadowMaskShader = 0x3af97853811d0c07ull;
constexpr bool PatchShadowMask(float* c, float sep, float conv) {
    const float a = -sep, b = sep * conv; // solve alpha z' + beta w' = a d + b
    const float det = c[21] * c[22] - c[20] * c[23];
    if (det == 0.f) {
        return false;
    }
    const float alpha = (a * c[23] + c[21] * b) / det;
    const float beta = -(c[20] * b + c[22] * a) / det;
    for (int k = 0; k < 4; ++k) {
        c[8 + k] += alpha * c[k];
        c[12 + k] += beta * c[k];
    }
    return true;
}

constexpr bool ShadowMaskSelfCheck() {
    // The x row of a captured mask (first level, gameplay).
    float c[24] = {7.13351f, 0, 0, 0, 0, 0, 0, 0, -3798.8f, 0, 0, 0, 3764.53f, 0, 0, 0,
                   0, 0, 0, 0, -1.00007f, -1.f, -0.500018f, 0.f};
    const auto shadow_x = [&](float d, float ndc_x) {
        return d * ndc_x * c[0] + (-c[20] * d + c[22]) * c[8] + (-c[21] * d + c[23]) * c[12];
    };
    const float d = 50.f, sep = 0.03f, conv = 20.f;
    const float center = shadow_x(d, 0.25f);
    const float eye_ndc = 0.25f + sep * (1.f - conv / d);
    const float unpatched = shadow_x(d, eye_ndc);
    PatchShadowMask(c, sep, conv);
    const float error = shadow_x(d, eye_ndc) - center, before = unpatched - center;
    return error * error < 0.25f && before * before > 1.f; // (cancellation of ~2e5 terms)
}
static_assert(ShadowMaskSelfCheck());
} // namespace Stereo

class EyeTargets {
public:
    EyeTargets(const Instance&, Scheduler&, Runtime&, VideoCore::TextureCache&);
    ~EyeTargets();

    struct Target {
        vk::Image image;
        vk::ImageView view;
        vk::ImageLayout layout;
    };

    /// Whether the image can have a twin (screen-sized, single level and layer; MSAA too).
    bool Eligible(const VideoCore::Image&) const;
    /// Before the left eye writes a target of a duplicated draw: the twin holds the guest
    /// image's current content (else the right eye would start from the left eye's result).
    void Prepare(VideoCore::ImageId);
    /// After the guest's MSAA resolve of `src` into `dst`: the same between their twins.
    void Resolve(VideoCore::Image& src, VideoCore::Image& dst);
    /// The right eye's attachment for `id` (created and filled from the guest image as needed).
    Target Attachment(VideoCore::ImageId, const VideoCore::ImageViewInfo&);
    /// The right eye's image to sample instead of `image`, when it has a current twin.
    std::optional<Target> Sample(const VideoCore::Image&, const VideoCore::ImageViewInfo&);
    /// The right eye's copy of a finished frame for presentation, or null: `layout` is set to
    /// the layout it is left in (TransferSrc).
    vk::Image PresentSource(VideoCore::ImageId, vk::ImageLayout& layout);
    /// Runtime::Transit: a guest image is about to be accessed; writes make its twin stale,
    /// except while a duplicated draw is recorded (both eyes write their own image).
    void NativeAccess(const VideoCore::Image&, vk::AccessFlags2);

    bool duplicating = false;

private:
    struct Entry {
        u64 uid = 0;
        VideoCore::UniqueImage image;
        std::vector<std::pair<VideoCore::ImageViewInfo, vk::UniqueImageView>> views;
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;
        bool valid = false;
    };
    Entry& Get(VideoCore::Image&, bool fill = true);
    void Fill(Entry&, VideoCore::Image&);
    void Transition(Entry&, vk::ImageAspectFlags, vk::ImageLayout, vk::PipelineStageFlags2,
                    vk::AccessFlags2);
    vk::ImageView View(Entry&, const VideoCore::Image&, const VideoCore::ImageViewInfo&);

    const Instance& instance;
    Scheduler& scheduler;
    Runtime& runtime;
    VideoCore::TextureCache& texture_cache;
    std::unordered_map<u64, std::unique_ptr<Entry>> entries; ///< by guest image uid
};

} // namespace Vulkan
