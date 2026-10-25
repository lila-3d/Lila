#pragma once

#include "scene/Deformers.h"

/**
 * Surface deformers (SurfaceDeformers.cpp): shrinkwrap, surfaceDeform, wrap (proximity wrap with several drivers),
 * collision, rigidify and skinSlide, plus the surfacePin node that rides a mesh after its stack.
 */
namespace lila::scene::deformers
{
void registerSurfaceTypes();
void registerSurfaceCommands();

/** Re-captures the bind-time state of a deformer from this file. False for types it does not own. */
bool rebindSurface(Graph& g, DeformerNode& d);
} // namespace lila::scene::deformers
