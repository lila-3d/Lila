#pragma once

#include "core/Export.h"

#include "render/RenderMesh.h"
#include "scene/Plug.h"

#include <functional>
#include <optional>

namespace lila::geo
{
struct RenderMesh;
}

namespace lila::ui
{
/**
 * GPU-ready copy of a scene mesh, built once per mesh snapshot and shared by every viewport, triangles grouped by
 * material slot. tangents adds MikkTSpace tangents (for normal maps); a copy that has them serves both.
 */
LILA_UI_API std::shared_ptr<const render::RenderMesh> renderMeshFor(const scene::MeshPtr& mesh, bool tangents = false);

/** The mesh subdivided for smooth preview, cached per snapshot and level. Level 0 returns the mesh itself. */
LILA_UI_API scene::MeshPtr smoothedFor(const scene::MeshPtr& mesh, int level);

/** Triangle-to-face, vertex-to-point and line-to-edge maps for the same render copy, used to pick components. */
LILA_UI_API std::shared_ptr<const geo::RenderMesh> pickDataFor(const scene::MeshPtr& mesh);

/**
 * Gives `to` the render copy of `from` with its vertices passed through edit, for a snapshot that differs from `from`
 * only in vertex colors, so it needn't be rebuilt. False when `from` has no copy yet.
 */
LILA_UI_API bool reuseRenderMesh(const scene::MeshPtr& from, const scene::MeshPtr& to, const std::function<void(std::vector<render::MeshVertex>&)>& edit);

struct RayHit
{
    double distance = 0.0; // world units
    uint32_t triangle = 0;
};

/** Nearest hit along a world-space ray, or nullopt. Triangles come from the render copy. */
LILA_UI_API std::optional<RayHit> raycast(const render::RenderMesh& mesh, const glm::dmat4& world, const glm::dvec3& origin, const glm::dvec3& dir);
} // namespace lila::ui
