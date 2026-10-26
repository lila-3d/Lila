#pragma once

#include "core/Export.h"
#include "curve/BevelPlus.h"

namespace lila::scene
{
class Node;
}

/**
 * Planar curve tools in the scene: the curveBoolean, offsetRegion, bevelPlus, curveText, roof and curveCenterLine history
 * nodes, their commands, and SVG and DXF import and export.
 */
namespace lila::curve
{
/** The bevelPlus node's settings as options (the custom profile from its profile curve when one is connected). */
LILA_SCENE_API BevelPlusOptions bevelPlusOptionsOf(const scene::Node& n);

void registerPlanarTypes();
void registerPlanarCommands();
void registerDxfCommands();
} // namespace lila::curve
