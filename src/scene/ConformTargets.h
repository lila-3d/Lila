#pragma once

#include "core/Export.h"
#include "geo/Mesh.h"

#include <functional>
#include <optional>
#include <string>

namespace lila::scene
{
class Graph;
class Node;

/**
 * Non-mesh nodes polyConformToObject can conform to. A provider turns such a node into world-space geometry; a target
 * without faces is used through its edges (a curve as a chain of edges: nearest point, or loop mode on a closed one).
 */
namespace conformTargets
{
using Provider = std::function<std::optional<geo::Mesh>(Graph& g, Node& n)>;
LILA_SCENE_API void add(std::string nodeType, Provider p);
/** The node's world-space geometry, or nullopt when no provider takes its type. */
LILA_SCENE_API std::optional<geo::Mesh> worldGeometry(Graph& g, Node& n);
} // namespace conformTargets
} // namespace lila::scene
