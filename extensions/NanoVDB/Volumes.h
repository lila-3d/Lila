#pragma once

#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>

namespace nanovdbext
{
/** The geometry-nodes volume a target shows: a gn modifier's, or the top one on a mesh's stack that made one. */
std::shared_ptr<const lila::gn::Volume> volumeOf(lila::scene::Graph& g, lila::scene::Node& target);

lila::gn::NodeDecl readVdbNode();
/** exportVDB. */
void registerVdbCommands(lila::ext::Context& ctx);
} // namespace nanovdbext
