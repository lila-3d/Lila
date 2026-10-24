#pragma once

#include "core/Export.h"

#include <memory>
#include <string>

namespace lila::bake
{
class Backend;
}

/**
 * The GPU bake backend. Built-in maps bake device-resident (Backend::beginResident): per band the UV raster, the
 * projection, every map's kernel and the per-texel accumulation run as compute, textures resolve, finalize and dilate
 * there, and only finished textures are read back. Extension maps run their CPU kernels per band on GPU hits. Rays trace
 * with ray query against a BLAS per high (split into chunks) and a TLAS per group when the device has it, else by
 * compute traversal of each high's flat MeshBvh. Highs stay on the device between bakes, keyed by content.
 *
 * Every GPU step is a render-thread task sliced to about 40 ms, so viewports keep drawing and the driver never times
 * out. Band sizes follow the device's memory budget; a GPU failure mid-bake falls back to the CPU for the rest.
 * LILA_BAKE_GPU_RESIDENT=0 keeps every map on the per-band path.
 */
namespace lila::render
{
enum class GpuTrace
{
    /** Ray query when the device has it, else compute traversal. LILA_BAKE_GPU_TRACE=sw forces compute. */
    Auto,
    Hardware,
    Software,
};

/**
 * Registers the "gpu" bake backend. Its factory starts the renderer headless when it is not running (lilabatch) and
 * throws std::runtime_error when there is no Vulkan 1.3 device.
 */
LILA_UI_API void registerGpuBakeBackend();

/** A GPU backend (see registerGpuBakeBackend). Hardware throws when the device lacks ray query. */
LILA_UI_API std::unique_ptr<bake::Backend> createGpuBakeBackend(GpuTrace trace = GpuTrace::Auto);

/** How a backend traces: "rt" (ray query), "compute", or empty when it is not a GPU backend. */
LILA_UI_API std::string gpuBakeMode(const bake::Backend& backend);
} // namespace lila::render
