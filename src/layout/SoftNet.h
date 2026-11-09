#pragma once

#include "core/Export.h"

/**
 * Soft Net (softNet deformer): a Panel Layout's lines as curves that shape the surface after the skin. The lines are
 * bound to the mesh as it is, each sample a barycentric soft constraint on its uncut triangles; every junction is a knot
 * with a keyable control in a frame riding the surface under the net. Moving, turning or scaling a knot carries the
 * lines through it, and the surface follows the lines with a smooth displacement that fades over the net's reach.
 */
namespace lila::layout
{
inline constexpr const char* kSoftNetType = "softNet";

LILA_SCENE_API void registerSoftNetTypes();
LILA_SCENE_API void registerSoftNetCommands();
} // namespace lila::layout
