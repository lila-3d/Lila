#pragma once

#include "core/Export.h"

#include "scene/Isolation.h"
#include "ui/MeshCache.h"

#include <memory>
#include <optional>

/**
 * What a view's isolation leaves of a node for drawing and picking. A null or inactive set hides nothing, so callers
 * pass scene::isolation::active() when they act in the active view rather than one view of their own.
 */
namespace lila::ui::isolate
{
using scene::isolation::Set;

LILA_UI_API bool shows(const Set* set, scene::NodeId id);
/**
 * The subset of renderMeshFor(drawn) to draw and raycast, or null when every face of the node shows. drawn is the node's
 * cage, its outMesh, or its outMesh after smooth levels of smooth preview; meshes whose faces no longer match the
 * cage's ids (topology-changing modifiers) show whole.
 */
LILA_UI_API std::shared_ptr<const MaskedMesh> masked(const Set* set, scene::NodeId id, const scene::MeshPtr& drawn, int smooth = 0, bool tangents = false);
/** Which components of the node's cage show, cached per isolation change. */
LILA_UI_API std::shared_ptr<const scene::isolation::Visibility> visibility(const Set* set, scene::NodeId id, const scene::MeshPtr& cage);
/** raycast against what shows of the node's mesh; nullopt for a node the set hides. */
LILA_UI_API std::optional<RayHit> raycast(const Set* set, scene::NodeId id, const scene::MeshPtr& mesh, const glm::dmat4& world, const glm::dvec3& origin,
                                          const glm::dvec3& dir);
} // namespace lila::ui::isolate
