#pragma once

#include "core/Export.h"
#include "curve/Groom.h"
#include "curve/HairCards.h"

#include <string>

namespace lila::scene
{
class Node;
}

/** Grooming in the scene: the "hairCards" history node and the curve sculpting, groom surface, guide and card commands. */
namespace lila::curve
{
/** The mesh a curve's roots grow from (its groomSurface attr); empty for none. */
LILA_SCENE_API std::string groomSurfaceOf(const scene::Node& curve);
/** The hairCards node's settings as card options. */
LILA_SCENE_API CardOptions cardOptionsOf(const scene::Node& n);

void registerGroomType();
void registerGroomCommands();
} // namespace lila::curve
