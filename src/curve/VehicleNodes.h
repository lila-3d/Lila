#pragma once

#include "core/Export.h"
#include "curve/Blend.h"
#include "curve/BorderBlend.h"

#include <span>
#include <string>
#include <vector>

namespace lila::scene
{
class Node;
}

/**
 * Hard-surface curve tools in the scene: the history nodes "curveBlend" (a blend curve between two curves or a curve and
 * a mesh border), "curveNetwork" (a quad surface through crossing curves) and "meshBorderBlend" (a strip between two mesh
 * borders), and the commands blendCurve, matchCurve, trimCurve, extendCurve, splitCurves, fairCurve, curveNetwork and
 * borderBlend.
 */
namespace lila::curve
{
/** The continuity choices of a blend's ends: position, tangent, curvature, curvature rate. */
inline const std::vector<std::string> kContinuity = {"g0", "g1", "g2", "g3"};

/** Edge ids as the nodes store them: a JSON list. */
LILA_SCENE_API std::string edgeListToJson(std::span<const geo::Index> edges);
LILA_SCENE_API std::vector<geo::Index> edgeListFromJson(const std::string& text);

/** The blend's ends as a curveBlend node sets them up, world space: what the interactive tool previews. */
struct BlendSetup
{
    BlendEnd a, b;
};
/** Evaluates a curveBlend node's ends from its current inputs. Throws std::invalid_argument when an input is missing. */
LILA_SCENE_API BlendSetup blendSetupOf(const scene::Node& n);

void registerVehicleTypes();
void registerVehicleCommands();
} // namespace lila::curve
