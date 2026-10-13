// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_stereo.h"

#include <cmath>
#include <cstdio>

#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

EyeTargets::EyeTargets(const Instance& i, Scheduler& s, Runtime& r, VideoCore::TextureCache& t)
    : instance{i}, scheduler{s}, runtime{r}, texture_cache{t} {
    runtime.eye_targets = this;
}

EyeTargets::~EyeTargets() {
    scheduler.Finish();
    runtime.eye_targets = nullptr;
}

bool EyeTargets::Eligible(const VideoCore::Image& image) const {
    const auto& i = image.info;
    // The scene is 2x MSAA (color and depth); twins copy the samples as they are.
    return Stereo::EyeSized(i.size.width, i.size.height) && i.size.depth == 1 &&
           i.resources.layers == 1 && i.resources.levels == 1 && !i.props.is_block &&
           image.backing && image.backing->num_samples == i.num_samples;
}

void EyeTargets::Transition(Entry& e, vk::ImageAspectFlags aspect, vk::ImageLayout layout,
                            vk::PipelineStageFlags2 stages, vk::AccessFlags2 access) {
    if (e.layout == layout) {
        return;
    }
    scheduler.EndRendering();
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = stages,
        .dstAccessMask = access,
        .oldLayout = e.layout,
        .newLayout = layout,
        .image = e.image,
        .subresourceRange = {aspect, 0, 1, 0, 1},
    };
    scheduler.Record([barrier](vk::CommandBuffer cmd) {
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    });
    e.layout = layout;
}

void EyeTargets::Fill(Entry& e, VideoCore::Image& original) {
    // The guest image (the left eye's, or content written for both) becomes the twin's start.
    const bool was_duplicating = duplicating;
    duplicating = true; // the transfer read below is not a write
    scheduler.EndRendering();
    runtime.Transit(&original, vk::ImageLayout::eTransferSrcOptimal,
                    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
    runtime.FlushBarriers();
    duplicating = was_duplicating;
    Transition(e, original.aspect_mask, vk::ImageLayout::eTransferDstOptimal,
               vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
    std::vector<vk::ImageCopy> regions;
    for (const auto aspect : {vk::ImageAspectFlagBits::eColor, vk::ImageAspectFlagBits::eDepth,
                              vk::ImageAspectFlagBits::eStencil}) {
        if (original.aspect_mask & aspect) {
            regions.push_back({
                .srcSubresource = {aspect, 0, 0, 1},
                .dstSubresource = {aspect, 0, 0, 1},
                .extent = e.image.image_ci.extent,
            });
        }
    }
    scheduler.Record([src = original.GetImage(), dst = vk::Image(e.image),
                      regions](vk::CommandBuffer cmd) {
        cmd.copyImage(src, vk::ImageLayout::eTransferSrcOptimal, dst,
                      vk::ImageLayout::eTransferDstOptimal, regions);
    });
    e.valid = true;
}

EyeTargets::Entry& EyeTargets::Get(VideoCore::Image& original, bool fill) {
    auto& entry = entries[original.image_uid];
    if (!entry || entry->image.image_ci.extent != original.backing->image.image_ci.extent ||
        entry->image.image_ci.format != original.backing->image.image_ci.format) {
        if (entry) {
            // A new guest image at a reused uid: the old twin's views may still be in flight.
            scheduler.Finish();
        }
        entry = std::make_unique<Entry>();
        entry->uid = original.image_uid;
        entry->image = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
        auto ci = original.backing->image.image_ci;
        ci.pNext = nullptr;
        ci.usage |= vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
        entry->image.Create(ci);
    }
    if (!entry->valid && fill) {
        Fill(*entry, original);
    }
    return *entry;
}

void EyeTargets::Prepare(VideoCore::ImageId id) {
    Get(texture_cache.GetImage(id));
}

void EyeTargets::Resolve(VideoCore::Image& src, VideoCore::Image& dst) {
    if (!Eligible(src) || !Eligible(dst) || (src.aspect_mask & ~vk::ImageAspectFlagBits::eColor)) {
        return;
    }
    const auto it = entries.find(src.image_uid);
    if (it == entries.end() || !it->second->valid) {
        return; // the right eye never drew it: dst's twin stays stale (refilled from the guest)
    }
    auto& s = *it->second;
    auto& d = Get(dst, false); // fully written below
    Transition(s, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eTransferSrcOptimal,
               vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
    Transition(d, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eTransferDstOptimal,
               vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
    const vk::ImageResolve region{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .extent = d.image.image_ci.extent,
    };
    scheduler.Record([src_image = vk::Image(s.image), dst_image = vk::Image(d.image),
                      region](vk::CommandBuffer cmd) {
        cmd.resolveImage(src_image, vk::ImageLayout::eTransferSrcOptimal, dst_image,
                         vk::ImageLayout::eTransferDstOptimal, region);
    });
    d.valid = true;
}

vk::ImageView EyeTargets::View(Entry& e, const VideoCore::Image& original,
                               const VideoCore::ImageViewInfo& info) {
    for (const auto& [key, view] : e.views) {
        if (key == info) {
            return *view;
        }
    }
    const auto aspect = original.info.props.is_depth
        ? (info.is_storage ? original.aspect_mask
                           : vk::ImageAspectFlags(vk::ImageAspectFlagBits::eDepth))
        : vk::ImageAspectFlags(vk::ImageAspectFlagBits::eColor);
    const auto format =
        original.info.props.is_depth ? original.backing->image.image_ci.format : info.format;
    auto view = Check(instance.GetDevice().createImageViewUnique({
        .image = e.image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .components = info.mapping,
        .subresourceRange = {aspect, 0, 1, 0, 1},
    }));
    const auto result = *view;
    e.views.emplace_back(info, std::move(view));
    return result;
}

EyeTargets::Target EyeTargets::Attachment(VideoCore::ImageId id,
                                          const VideoCore::ImageViewInfo& info) {
    auto& original = texture_cache.GetImage(id);
    auto& e = Get(original);
    const bool depth = original.info.props.is_depth;
    Transition(e, original.aspect_mask,
               depth ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                     : vk::ImageLayout::eColorAttachmentOptimal,
               depth ? vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                           vk::PipelineStageFlagBits2::eLateFragmentTests
                     : vk::PipelineStageFlags2(vk::PipelineStageFlagBits2::eColorAttachmentOutput),
               depth ? vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                           vk::AccessFlagBits2::eDepthStencilAttachmentWrite
                     : vk::AccessFlagBits2::eColorAttachmentRead |
                           vk::AccessFlagBits2::eColorAttachmentWrite);
    auto attachment_info = info;
    attachment_info.is_storage = true; // include stencil in attachment views
    return {e.image, View(e, original, attachment_info), e.layout};
}

std::optional<EyeTargets::Target> EyeTargets::Sample(const VideoCore::Image& image,
                                                     const VideoCore::ImageViewInfo& info) {
    const auto it = entries.find(image.image_uid);
    if (it == entries.end() || !it->second->valid || info.range.base.level != 0) {
        return std::nullopt;
    }
    auto& e = *it->second;
    Transition(e, image.aspect_mask, vk::ImageLayout::eShaderReadOnlyOptimal,
               vk::PipelineStageFlagBits2::eVertexShader |
                   vk::PipelineStageFlagBits2::eFragmentShader |
                   vk::PipelineStageFlagBits2::eComputeShader,
               vk::AccessFlagBits2::eShaderSampledRead);
    return Target{e.image, View(e, image, info), e.layout};
}

vk::Image EyeTargets::PresentSource(VideoCore::ImageId id, vk::ImageLayout& layout) {
    auto& original = texture_cache.GetImage(id);
    const auto it = entries.find(original.image_uid);
    if (it == entries.end() || !it->second->valid) {
        return {};
    }
    auto& e = *it->second;
    Transition(e, original.aspect_mask, vk::ImageLayout::eTransferSrcOptimal,
               vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
    layout = e.layout;
    return e.image;
}

void EyeTargets::NativeAccess(const VideoCore::Image& image, vk::AccessFlags2 access) {
    constexpr auto writes = vk::AccessFlagBits2::eShaderWrite |
                            vk::AccessFlagBits2::eTransferWrite |
                            vk::AccessFlagBits2::eColorAttachmentWrite |
                            vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                            vk::AccessFlagBits2::eMemoryWrite;
    if (duplicating || !(access & writes)) {
        return;
    }
    if (const auto it = entries.find(image.image_uid); it != entries.end()) {
        it->second->valid = false;
    }
}

} // namespace Vulkan
