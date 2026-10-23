#pragma once

#include "core/Export.h"

#include "scene/Graph.h"

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <vector>

namespace lila::retopo
{
/**
 * quadRemesh, meshRepair, relaxMesh, shrinkwrap, plus the quadDraw* edit commands the Quad Draw tool runs (so its edits
 * undo and echo like any other).
 */
LILA_SCENE_API void registerCommands();

/** What Quad Draw works on: the reference surface and the retopo mesh's name (which may not exist yet). */
struct QuadDrawTargets
{
    scene::NodeId reference = scene::kNoNode;
    std::string retopo;
};

/**
 * Quad Draw's targets for a selection. A selected mesh already drawn over a reference (its quadDrawReference
 * connection) resumes on that pair. Otherwise the first selected mesh is the reference and the retopo mesh is the second
 * selected one, else a mesh already drawn over the reference, else "<reference>_retopo". nullopt without a mesh selected.
 */
LILA_SCENE_API std::optional<QuadDrawTargets> resolveQuadDraw(const scene::Graph& g, const std::vector<scene::NodeId>& selection);

/** The mesh connected as a retopo mesh's Quad Draw reference, or kNoNode. */
LILA_SCENE_API scene::NodeId quadDrawReference(const scene::Graph& g, scene::NodeId retopo);

/** A retopo mesh's stored Quad Draw dots, in world space. */
LILA_SCENE_API std::vector<glm::dvec3> quadDrawDots(const scene::Node& retopo);
} // namespace lila::retopo
