#pragma once

#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Transform.h"

#include <glm/glm.hpp>

#include <string>
#include <variant>

/** Argument and scene lookups the retopology and topology commands share. */
namespace lila::retopo
{
/** The mesh node called name in the current graph; a CommandError naming cmd otherwise. */
inline scene::Node& meshArg(const std::string& name, const char* cmd)
{
    scene::Node* n = scene::Graph::current().find(name);
    if (!n || !n->type().isA("mesh"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a mesh");
    return *n;
}

/** The mesh stored on p after evaluating the current graph, empty when none is. */
inline geo::Mesh baseMesh(const scene::PlugRef& p)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    const scene::MeshPtr& m = std::get<scene::MeshPtr>(g.storedValue(p));
    return m ? *m : geo::Mesh();
}

inline glm::dmat4 worldOf(const scene::Node& n) { return n.as<glm::dmat4>(scene::Transform::kWorldMatrix); }
} // namespace lila::retopo
