#pragma once

#include <pxr/pxr.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>

PXR_NAMESPACE_OPEN_SCOPE
class Hgi;
class HgiTexture;
PXR_NAMESPACE_CLOSE_SCOPE

namespace lila::usd::hydra
{
/** A Lila GpuImage's shared memory, as render::SharedImageMemory gives it, with its Vulkan enums as integers. */
struct SharedTarget
{
    void* handle = nullptr;
    uint64_t size = 0;
    uint32_t format = 0;
    uint32_t flags = 0;
    uint32_t usage = 0;
    int width = 0;
    int height = 0;
};

/**
 * Copies a render delegate's Vulkan AOV textures into images Lila's device shares, on HgiVulkan's own device and queue,
 * so frames reach the viewport without leaving the GPU. Kept apart from the Hydra session because HgiVulkan calls the
 * Vulkan loader directly while Lila's headers bring in volk. Main thread (HgiVulkan's), except wait.
 */
class HgiInterop
{
public:
    /** Null, with why, when hgi isn't HgiVulkan, drives another GPU than the one with deviceUuid, or can't import memory. */
    static std::unique_ptr<HgiInterop> create(pxr::Hgi* hgi, const std::array<uint8_t, 16>& deviceUuid, std::string& why);
    ~HgiInterop();

    /** Imports a colour (RGBA16F) and depth (R32F) target pair as slot n; false when either fails. */
    bool setSlot(size_t n, const SharedTarget& color, const SharedTarget& depth);
    /** Drops every slot once its copies finish. */
    void clearSlots();
    /** Whether texture can be copied into a slot: a single-sample Vulkan texture, colour or a 32-bit float depth. */
    bool copyable(pxr::HgiTexture* texture, bool color) const;
    /**
     * Submits copies of color and depth (when not null) into slot n, rows unchanged, after everything Hydra submitted;
     * the targets end SHADER_READ_ONLY_OPTIMAL, released to VK_QUEUE_FAMILY_EXTERNAL. False, submitting nothing, when
     * a texture can't be copied.
     */
    bool copy(size_t n, pxr::HgiTexture* color, pxr::HgiTexture* depth);
    /** Any thread: blocks until slot n's last copy has finished. */
    void wait(size_t n);

private:
    HgiInterop();
    struct Impl;
    std::unique_ptr<Impl> m;
};
/**
 * Notices when HgiVulkan's device is lost, as a GPU fault in a delegate's work leaves it: Hgi carries on, every submit
 * failing quietly, and its AOVs keep the last frame drawn. Checks with an empty submit and fence per frame. Main thread.
 */
class HgiDeviceWatch
{
public:
    /** Null when hgi isn't HgiVulkan. */
    static std::unique_ptr<HgiDeviceWatch> create(pxr::Hgi* hgi);
    ~HgiDeviceWatch();
    /** Call after a frame's work is submitted. True once the device is lost, or has not finished a frame for many seconds. */
    bool lost();

private:
    HgiDeviceWatch();
    struct Impl;
    std::unique_ptr<Impl> m;
};
} // namespace lila::usd::hydra
