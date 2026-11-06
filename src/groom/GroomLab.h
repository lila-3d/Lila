#pragma once

#include "core/Export.h"
#include "curve/Spline.h"

#include <string>
#include <vector>

namespace lila::geo
{
class Mesh;
}

/**
 * The groom stack's engine: a GroomLab node graph (src/groom/groomlab) evaluated on a Lila scalp. Guides go in as
 * curves, strands come out as poly curves with a per-point "width" and a per-curve "id".
 */
namespace lila::groom
{
struct GrownStrands
{
    Splines strands;
    /** What the graph reported while evaluating (unknown nodes, bad params); empty when it ran clean. */
    std::vector<std::string> warnings;
};

/**
 * Evaluates graphJson (GroomLab's graph schema; empty for its default graph) on scalp, both in world space. guides are
 * rooted on the scalp at their first point and read by the graph's Guides nodes. seconds is the time expressions and
 * motion nodes see. Throws std::runtime_error when the graph can't load or its output isn't strands or guides.
 */
LILA_SCENE_API GrownStrands growStrands(const geo::Mesh& scalp, const Splines& guides, const std::string& graphJson,
                                        double seconds = 0.0);

void registerGroomLabType();
} // namespace lila::groom
