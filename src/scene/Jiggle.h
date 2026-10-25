#pragma once

#include "core/Export.h"

/**
 * The jiggle deformer: every point trails its input on a spring-damper (stiffness, damping, gravity, weighted by the
 * deformer's weights), stepped and cached by the simulation framework (scene/Simulation.h), so scrubbing and playing
 * agree and looping takes loop. It reaches Unreal through bakeSkinWeights source="animation" helpers=N (keyed helper
 * joints), a geometry cache, or ML Deformer training data.
 */
namespace lila::scene::jiggle
{
LILA_SCENE_API void registerTypes();
LILA_SCENE_API void registerCommands();
} // namespace lila::scene::jiggle
