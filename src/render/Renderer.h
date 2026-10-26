#pragma once

#include "core/Export.h"

#include "render/FrameState.h"

#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace lila::render
{
using ViewportId = int;
struct EnvProbe;
class VulkanContext;

/**
 * Work for the render thread, run a slice at a time between frames: each call records, submits and waits for one slice
 * (keep it near 50 ms of GPU time so viewports stay live and the driver never times out) and returns true when done.
 */
using GpuTask = std::function<bool(VulkanContext& vk)>;

/**
 * Owns the GPU and a render thread. The main thread hands each viewport a FrameState and the render thread draws
 * it when it gets to it; nothing on the main thread ever waits for the GPU. Frames are only drawn on demand.
 */
class LILA_UI_API Renderer
{
public:
    static Renderer& get();

    /** What the GPU spent on a viewport's last finished frame, from timestamp queries; zero where unsupported. */
    struct FrameStats
    {
        uint64_t frames = 0; // frames actually drawn, not submitted states
        double gpuMs = 0.0;
        double aoMs = 0.0; // depth/normal prepass, GTAO and its blur; 0 when ambient occlusion did not run
        double cpuMs = 0.0;   // render thread recording the frame's commands
        double aoCpuMs = 0.0; // the ambient occlusion share of cpuMs
        uint64_t meshDrawCalls = 0; // shaded mesh draw calls, prepass included; each draws one batch's placements
        uint64_t meshInstances = 0; // placements those calls drew
    };

    bool start();
    void stop();
    /** Runs fn in stop(), once the render thread is gone and before the device is: frees what others keep on it. */
    void addStopHook(std::function<void(VulkanContext& vk)> fn);
    bool running() const;

    ViewportId addViewport(uintptr_t nativeWindow, int width, int height);
    /** Blocks until the render thread has let go of the window. */
    void removeViewport(ViewportId id);
    void resize(ViewportId id, int width, int height);
    void submit(ViewportId id, FrameState state);
    FrameStats stats(ViewportId id) const;

    /**
     * Draws one frame into an offscreen target of the given pixel size and waits for it, with no window involved. Pixels
     * are 8-bit BGRA, display-encoded (sRGB), top row first. False (with err) if the renderer is not running or the GPU
     * failed. repeat draws the frame that many times, for timing: the median frame's stats go to timing.
     */
    bool renderOffscreen(FrameState state, int width, int height, std::vector<uint8_t>& bgra, std::string* err = nullptr,
                         FrameStats* timing = nullptr, int repeat = 1);

    std::string deviceName() const;

    struct GpuInfo
    {
        std::string name; // empty while the renderer is not running
        bool discrete = false;
        uint64_t vramBytes = 0; // device-local heaps
    };
    GpuInfo gpuInfo() const;

    /**
     * Queues a task on the render thread. Slices of queued tasks interleave round robin with frames drawn in between.
     * The future holds what the task threw, or a std::runtime_error when the renderer stops first.
     */
    std::future<void> runTask(GpuTask task);
    /** The device, for allocating from other threads (VMA is thread safe); null when not running. Submit only from tasks. */
    VulkanContext* context();

    /** A CPU copy of the environment built for path (as FrameState::envPath names it), null until it is built. */
    std::shared_ptr<const EnvProbe> environmentProbe(const std::string& path) const;

    ~Renderer();

private:
    Renderer();
    struct Impl;
    std::unique_ptr<Impl> m;
};
} // namespace lila::render
