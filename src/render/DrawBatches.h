#pragma once

#include "core/Export.h"
#include "render/FrameState.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace lila::render
{
/** One placement of a draw, read by the mesh and wire shaders by instance index; matches InstanceData in mesh.slang and wire.slang. */
struct GpuInstance
{
    glm::mat4 world{1.0f};
    glm::vec4 color{0.0f}; // resolved wire colour; in shaded batches the wire the mesh shader draws, alpha 0 for none
};
static_assert(sizeof(GpuInstance) == 80, "matches InstanceData in mesh.slang");

/** Placements of one GPU mesh that share everything else: one instanced draw call per material section. */
struct DrawBatch
{
    const MeshDraw* draw = nullptr; // the first member, which supplies the mesh, materials and per-draw state
    uint32_t first = 0;             // its first entry in DrawBatches::instances
    uint32_t count = 0;
    bool wire = false; // shaded batch whose placements draw their wire in the mesh shader
};

/** A frame's mesh draws grouped for instancing. */
struct DrawBatches
{
    std::vector<GpuInstance> instances;
    std::vector<DrawBatch> shaded;
    /** Wire overlays; within a batch the highlighted placements come last so they win depth ties. */
    std::vector<DrawBatch> wire;
};

/** Field-by-field equality of two material lists, textures by identity. */
LILA_UI_API bool sameMaterials(const std::vector<Material>& a, const std::vector<Material>& b);

/** True when a draw shows a wire overlay. */
LILA_UI_API bool drawsWire(const MeshDraw& d);

/** True when a shaded draw's wire can be drawn by the mesh shader instead of as lines. */
LILA_UI_API bool wireInShader(const MeshDraw& d);

/** The wire colour a draw shows with, from its highlight state, its own colour or the frame's default. */
LILA_UI_API glm::vec4 wireColor(const FrameState& s, const MeshDraw& d);

/**
 * Groups the frame's mesh draws: shaded draws sharing a GPU mesh, material list and vertex colour display become one
 * batch, and so do wire overlays sharing a mesh, width and depth mode. A draw with instances contributes one placement per
 * entry. Draws with no mesh or no triangles are left out of the shaded batches.
 */
LILA_UI_API DrawBatches batchDraws(const FrameState& s, bool shaderWire = false);
} // namespace lila::render
