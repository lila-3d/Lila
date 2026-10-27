#pragma once

#include "core/Export.h"
#include "scene/Simulation.h"

namespace lila::ui
{
/**
 * The frame a simulation's viewport preview draws at the current time, without making edits wait on the solver: the
 * cached frame, or one simulated now when that is cheap (playback, or a frame or two past the cache). Otherwise null,
 * and the simulation runs once edits pause, then the viewports redraw. A drag re-simulates once when it stops.
 */
LILA_UI_API std::shared_ptr<const scene::sim::Frame> simPreviewFrame(scene::Graph& g, scene::NodeId node);
} // namespace lila::ui
