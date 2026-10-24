#pragma once

#include "bake/Backend.h"
#include "render/GpuImage.h"

#include <memory>
#include <span>
#include <vector>

namespace lila::render
{
/**
 * A device-resident bake whose textures' accumulators stay on the device between updates, for live mesh maps: re-trace
 * the texel tiles an edit touched, then finish whole textures straight into GPU images (no read back). Call from one
 * thread that is not the render thread; GPU work goes through Renderer::runTask slices.
 */
class DeviceBake
{
public:
    virtual ~DeviceBake() = default;
    /** Follows inputs the backend was just prepared with: drops what was made from the old ones, keeps the accumulators. */
    virtual void rebind(const bake::BakeInputs& inputs) = 0;
    virtual int bandRows(int width, int samplesPerTexel) const = 0;
    virtual bake::ResidentStats bakeTile(const bake::ResidentTile& tile) = 0;
    /**
     * Zeroes a texture's accumulators in the listed texel tiles (ty * columns + tx, tile x tile texels each), or all of
     * them when tiles is empty.
     */
    virtual void clearTiles(const bake::TextureBuffer& tex, std::span<const uint32_t> tiles, int tile) = 0;
    /**
     * Resolves, finalizes and dilates a copy of the texture's accumulators into image (the texture's size; RGBA16F, R16F
     * or RGBA8, written raw), with the finalize's decode data in tex.meta. Throws for maps that finalize on the CPU.
     */
    virtual void finishInto(bake::TextureBuffer& tex, int padding, const GpuImageRef& image) = 0;
    /** Frees a texture's accumulators. */
    virtual void drop(const bake::TextureBuffer& tex) = 0;
};

/**
 * A DeviceBake of maps (each backend.residentSupports) on a GPU backend already prepared with inputs; null when the
 * backend is not a GPU one or has failed. sliceMs is the GPU time each render-thread slice aims at.
 */
std::unique_ptr<DeviceBake> beginDeviceBake(bake::Backend& backend, const bake::BakeInputs& inputs, std::vector<bake::ResidentMap> maps,
                                            const bake::OutputSettings& output, double sliceMs);
} // namespace lila::render
