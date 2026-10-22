#pragma once

#include "cmds/Command.h"
#include "geo/Mesh.h"
#include "scene/Components.h"
#include "scene/Node.h"

#include <functional>
#include <span>
#include <string_view>

/** Helpers the uv commands share across files (defined in UvCommands.cpp). */
namespace lila::uv
{
using UvMeshEditFn = std::function<json(geo::Mesh&, const scene::components::Ids& ids, scene::Node&)>;

/**
 * Runs fn on a copy of every target mesh (the component selection converted to kind, or whole selected meshes when
 * allowWhole) and writes it back to inMesh as one undo step. Returns fn's results keyed by mesh name.
 */
json editUvMeshes(const char* label, scene::ComponentMode kind, const UvMeshEditFn& fn, bool allowWhole = true);

/** Direct edits work on the base mesh, so procedural history is baked first. */
void bakeUvHistory(scene::Node& n);

/** Unmarks the seams and welds the uvs across them; with move, each smaller island first travels onto its neighbour. */
json sewUvEdges(geo::Mesh& m, std::span<const geo::Index> edges, std::string_view uvSet, bool move = true);
} // namespace lila::uv
