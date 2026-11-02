#pragma once

#include "core/Export.h"
#include "curve/Spline.h"

#include <vector>

namespace lila::scene
{
class Node;
struct NodeType;
} // namespace lila::scene

/**
 * Curve controls: transforms whose world matrices drive a curve's points and Bezier handles. The curve node holds one
 * element per driven component in its `controls` group: `controls.matrix` (the control's world matrix, connected),
 * `controls.point` (the component id, see componentId) and `controls.offset` (the component in the control's space).
 */
namespace lila::curve
{
/** Adds the controls group to the curve type. */
void addControlAttrs(scene::NodeType& curveType);

/** Whether any of the curve's components follow a control. */
LILA_SCENE_API bool hasControls(const scene::Node& curve);

/**
 * The splines with every controlled component moved where its control puts it, object space. Driven handles act as free
 * handles; auto handles of driven points are recomputed from their neighbours.
 */
LILA_SCENE_API Splines drivenSplines(const scene::Node& curve, Splines s);

void registerCurveControlCommands();
} // namespace lila::curve
