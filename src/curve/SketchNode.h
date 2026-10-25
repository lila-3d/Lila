#pragma once

#include "core/Export.h"
#include "curve/Sketch.h"

namespace lila::scene
{
class Node;
}

/** Sketch constraints on curve nodes: the stored sketch, drag solving for point moves, and the sketch commands. */
namespace lila::curve
{
/** The curve's sketch (its "sketch" attr); empty when it has none or the stored text is unreadable (logged). */
LILA_SCENE_API sketch::Sketch sketchOf(const scene::Node& curve);
LILA_SCENE_API bool hasSketch(const scene::Node& curve);

/**
 * The splines with point moves (component id to object-space position) applied. On a sketch the moved points are
 * dragged and the rest re-solved so the constraints keep holding; a moved point the sketch pins goes back.
 */
LILA_SCENE_API Splines sketchMoved(const scene::Node& curve, const Splines& stored, const std::vector<std::pair<uint32_t, glm::dvec3>>& moves);

void registerSketchCommands();
} // namespace lila::curve
