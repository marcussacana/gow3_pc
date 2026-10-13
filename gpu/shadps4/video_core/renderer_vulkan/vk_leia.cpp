// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#endif

#include "video_core/renderer_vulkan/vk_leia.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "video_core/renderer_vulkan/vk_instance.h"

#ifdef _WIN32
#include "../../../leia_bridge/gow3_leia_bridge.h"
#endif

namespace Vulkan {

#ifdef _WIN32

namespace {

std::string problem = "gow3_leia.dll not loaded yet";

void BridgeLog(const char* message) {
    std::printf("Leia: %s\n", message);
    std::fflush(stdout);
}

std::filesystem::path ExecutableDirectory() {
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path{std::wstring{path, length}}.parent_path();
}

struct SharedImage {
    vk::Image image;
    vk::DeviceMemory memory;
    vk::DeviceSize size = 0;
    HANDLE handle = nullptr;
};

} // namespace

struct LeiaWeaver::Impl {
    HMODULE module = nullptr;
    const Gow3LeiaApi* api = nullptr;
    vk::Device device;
    SharedImage input, output;
    vk::ImageView output_view;
    vk::Semaphore ready, done;
    HANDLE ready_handle = nullptr, done_handle = nullptr;
    vk::Extent2D input_size{}, output_size{};
    vk::Format view_format{};
    bool bgra = false;
    void* window = nullptr;
    bool lens = false;

    void Release() {
        if (!device) {
            return;
        }
        (void)device.waitIdle(); // rare: a size change; the bridge may still read the images
        for (HANDLE h : {input.handle, output.handle, ready_handle, done_handle}) {
            if (h) CloseHandle(h);
        }
        device.destroyImageView(output_view);
        for (auto* shared : {&input, &output}) {
            device.destroyImage(shared->image);
            device.freeMemory(shared->memory);
            *shared = {};
        }
        device.destroySemaphore(ready);
        device.destroySemaphore(done);
        output_view = nullptr;
        ready = done = nullptr;
        ready_handle = done_handle = nullptr;
    }

    bool CreateImage(const Instance& instance, SharedImage& shared, vk::Extent2D size,
                     vk::ImageUsageFlags usage) {
        const vk::ExternalMemoryImageCreateInfo external{
            .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eOpaqueWin32};
        auto [result, image] = device.createImage({
            .pNext = &external,
            .flags = vk::ImageCreateFlagBits::eMutableFormat,
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR8G8B8A8Unorm,
            .extent = {size.width, size.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .usage = usage,
        });
        if (result != vk::Result::eSuccess) {
            return false;
        }
        shared.image = image;
        const auto requirements = device.getImageMemoryRequirements(image);
        const auto& properties = instance.GetPhysicalDevice().getMemoryProperties();
        u32 type = ~0u;
        for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (properties.memoryTypes[i].propertyFlags &
                 vk::MemoryPropertyFlagBits::eDeviceLocal)) {
                type = i;
                break;
            }
        }
        const vk::MemoryDedicatedAllocateInfo dedicated{.image = image};
        const vk::ExportMemoryAllocateInfo export_info{
            .pNext = &dedicated,
            .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eOpaqueWin32};
        auto [alloc_result, memory] = device.allocateMemory({
            .pNext = &export_info,
            .allocationSize = requirements.size,
            .memoryTypeIndex = type,
        });
        if (type == ~0u || alloc_result != vk::Result::eSuccess) {
            return false;
        }
        shared.memory = memory;
        shared.size = requirements.size;
        if (device.bindImageMemory(image, memory, 0) != vk::Result::eSuccess) {
            return false;
        }
        auto [handle_result, handle] = device.getMemoryWin32HandleKHR(
            {.memory = memory, .handleType = vk::ExternalMemoryHandleTypeFlagBits::eOpaqueWin32});
        shared.handle = handle;
        return handle_result == vk::Result::eSuccess;
    }

    bool CreateSemaphore(vk::Semaphore& semaphore, HANDLE& handle) {
        const vk::ExportSemaphoreCreateInfo export_info{
            .handleTypes = vk::ExternalSemaphoreHandleTypeFlagBits::eOpaqueWin32};
        auto [result, created] = device.createSemaphore({.pNext = &export_info});
        if (result != vk::Result::eSuccess) {
            return false;
        }
        semaphore = created;
        auto [handle_result, exported] = device.getSemaphoreWin32HandleKHR(
            {.semaphore = semaphore,
             .handleType = vk::ExternalSemaphoreHandleTypeFlagBits::eOpaqueWin32});
        handle = exported;
        return handle_result == vk::Result::eSuccess;
    }
};

LeiaWeaver* LeiaWeaver::Get() {
    static LeiaWeaver* weaver = [] () -> LeiaWeaver* {
        auto* created = new LeiaWeaver;
        if (!created->impl->api) {
            delete created;
            return nullptr;
        }
        std::atexit([] {
            if (LeiaWeaver* live = Get()) {
                live->SetLens(false);
                live->impl->api->Shutdown();
            }
        });
        return created;
    }();
    return weaver;
}

const char* LeiaWeaver::Problem() {
    return Get() ? nullptr : problem.c_str();
}

LeiaWeaver::LeiaWeaver() : impl{std::make_unique<Impl>()} {
    impl->module = LoadLibraryW((ExecutableDirectory() / L"gow3_leia.dll").c_str());
    const auto get_api = impl->module ? reinterpret_cast<Gow3LeiaGetApiFn>(
                                            GetProcAddress(impl->module, "Gow3LeiaGetApi"))
                                      : nullptr;
    const Gow3LeiaApi* api = get_api ? get_api() : nullptr;
    if (!api || api->abi != GOW3_LEIA_BRIDGE_ABI) {
        problem = impl->module ? "gow3_leia.dll is from another version"
                               : "gow3_leia.dll or the Leia SR platform is missing";
        return;
    }
    if (!api->Initialize(BridgeLog)) {
        problem = "no Leia SR display found";
        return;
    }
    impl->api = api;
    problem.clear();
}

LeiaWeaver::~LeiaWeaver() {
    impl->Release();
}

bool LeiaWeaver::Prepare(const Instance& instance, vk::Extent2D input, vk::Extent2D output,
                         vk::Format view_format, bool bgra, void* window) {
    auto& i = *impl;
    if (i.input.image && i.input_size == input && i.output_size == output &&
        i.view_format == view_format && i.bgra == bgra && i.window == window) {
        return true;
    }
    if (!instance.IsExternalWin32Supported()) {
        problem = "the Vulkan driver cannot share memory with OpenGL";
        return false;
    }
    i.device = instance.GetDevice();
    i.Release();
    i.input_size = input;
    i.output_size = output;
    i.view_format = view_format;
    i.bgra = bgra;
    i.window = window;
    const bool created =
        i.CreateImage(instance, i.input, input,
                      vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled) &&
        i.CreateImage(instance, i.output, output,
                      vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled) &&
        i.CreateSemaphore(i.ready, i.ready_handle) && i.CreateSemaphore(i.done, i.done_handle);
    if (created) {
        auto [result, view] = i.device.createImageView({
            .image = i.output.image,
            .viewType = vk::ImageViewType::e2D,
            // The bridge wrote R, G, B, A (it swizzles a BGRA input): the RGBA format of the
            // presenter's encoding.
            .format = view_format == vk::Format::eR8G8B8A8Srgb ||
                              view_format == vk::Format::eB8G8R8A8Srgb
                          ? vk::Format::eR8G8B8A8Srgb
                          : vk::Format::eR8G8B8A8Unorm,
            .components = {vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
                           vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eOne},
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        });
        i.output_view = view;
    }
    const Gow3LeiaImage in{i.input.handle, i.input.size, input.width, input.height};
    const Gow3LeiaImage out{i.output.handle, i.output.size, output.width, output.height};
    if (!created || !i.output_view ||
        !i.api->Attach(window, &in, bgra ? 1 : 0, &out, i.ready_handle, i.done_handle)) {
        problem = "sharing the images with the Leia weaver failed (see the log)";
        i.Release();
        i.input_size = {};
        return false;
    }
    return true;
}

vk::Image LeiaWeaver::Input() const {
    return impl->input.image;
}
vk::Image LeiaWeaver::Output() const {
    return impl->output.image;
}
vk::ImageView LeiaWeaver::OutputView() const {
    return impl->output_view;
}
vk::Semaphore LeiaWeaver::Ready() const {
    return impl->ready;
}
vk::Semaphore LeiaWeaver::Done() const {
    return impl->done;
}

bool LeiaWeaver::Weave() {
    return impl->api->Weave() != 0;
}

void LeiaWeaver::SetLens(bool enable) {
    if (impl->lens != enable) {
        impl->lens = enable;
        impl->api->SetLens(enable ? 1 : 0);
    }
}

#else // the bridge is Windows-only

struct LeiaWeaver::Impl {};
LeiaWeaver* LeiaWeaver::Get() {
    return nullptr;
}
const char* LeiaWeaver::Problem() {
    return "Leia SR monitors are supported on Windows only";
}
LeiaWeaver::LeiaWeaver() = default;
LeiaWeaver::~LeiaWeaver() = default;
bool LeiaWeaver::Prepare(const Instance&, vk::Extent2D, vk::Extent2D, vk::Format, bool, void*) {
    return false;
}
vk::Image LeiaWeaver::Input() const { return {}; }
vk::Image LeiaWeaver::Output() const { return {}; }
vk::ImageView LeiaWeaver::OutputView() const { return {}; }
vk::Semaphore LeiaWeaver::Ready() const { return {}; }
vk::Semaphore LeiaWeaver::Done() const { return {}; }
bool LeiaWeaver::Weave() { return false; }
void LeiaWeaver::SetLens(bool) {}

#endif

} // namespace Vulkan
