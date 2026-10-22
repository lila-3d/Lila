#pragma once

#include "render/FrameState.h"

#include <memory>
#include <string>
#include <vector>

namespace lila::render
{
using ViewportId = int;
struct EnvProbe;

/**
 * Owns the GPU and a render thread. The main thread hands each viewport a FrameState and the render thread draws
 * it when it gets to it; nothing on the main thread ever waits for the GPU. Frames are only drawn on demand.
 */
class Renderer
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
    };

    bool start();
    void stop();
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

    /** A CPU copy of the environment built for path (as FrameState::envPath names it), null until it is built. */
    std::shared_ptr<const EnvProbe> environmentProbe(const std::string& path) const;

    ~Renderer();

private:
    Renderer();
    struct Impl;
    std::unique_ptr<Impl> m;
};
} // namespace lila::render
