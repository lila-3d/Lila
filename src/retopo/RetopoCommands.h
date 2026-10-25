#pragma once

#include "core/Export.h"

#include "ops/Command.h"
#include "scene/Graph.h"

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <vector>

namespace lila::retopo
{
/**
 * quadRemesh, meshRepair, relaxMesh, shrinkwrap, plus the retopoDraw* edit commands the Retopo Draw tool runs (so its edits
 * undo and echo like any other).
 */
LILA_SCENE_API void registerCommands();

/** What Retopo Draw works on: the reference surface and the retopo mesh's name (which may not exist yet). */
struct RetopoDrawTargets
{
    scene::NodeId reference = scene::kNoNode;
    std::string retopo;
};

/**
 * Retopo Draw's targets for a selection. A selected mesh already drawn over a reference (its retopoDrawReference
 * connection) resumes on that pair. Otherwise the first selected mesh is the reference and the retopo mesh is the second
 * selected one, else a mesh already drawn over the reference, else "<reference>_retopo". nullopt without a mesh selected.
 */
LILA_SCENE_API std::optional<RetopoDrawTargets> resolveRetopoDraw(const scene::Graph& g, const std::vector<scene::NodeId>& selection);

/** The mesh connected as a retopo mesh's Retopo Draw reference, or kNoNode. */
LILA_SCENE_API scene::NodeId retopoDrawReference(const scene::Graph& g, scene::NodeId retopo);

/**
 * The retopo mesh a retopoDraw* or topology command names with mesh=, created when create is set and missing; with
 * reference=, that mesh is connected as its surface. Throws CommandError.
 */
LILA_SCENE_API scene::Node& retopoTargetNode(const Args& a, const char* cmd, bool create);

/** A retopo mesh's stored Retopo Draw dots, in world space. */
LILA_SCENE_API std::vector<glm::dvec3> retopoDrawDots(const scene::Node& retopo);
} // namespace lila::retopo
