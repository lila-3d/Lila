// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
#pragma once

// A simulation baked to a file: a Simulate node's frames (GroomSession's cache, simulation.h)
// as an Alembic guide cache, and read back as a bake the session takes (BakedSimulation). The
// workflow every host documents: simulate, bake to a cache, render (the groom file names the
// bake, "simCaches", so the Arnold procedural, the CLI and a farm read the baked frames rather
// than simulating again).
//
// The file is the guide caches' layout (AlembicCurvesWriter): one curves object, a curve per
// guide with its ID as "strandId", world space, a sample per frame at framesPerSecond. Maya's
// nHair guide caches have it too, so either reads as a guide cache elsewhere. A wet simulation
// (water or rain) adds its "wetness" per vertex, read back as the guides' wetness layer.

#include <cstdint>
#include <functional>
#include <string>

#include "groomlab/simulation.h"

namespace groomlab {

// A bake read back: the guides at any time within the file's samples (blended between two, the
// reader's own way), its span, and `stamp` (the settings it was made with: the session ignores it
// for other settings). The file stays open while the bake lives. Throws AlembicError.
BakedSimulationPtr readSimulationCache(const std::string& path, const std::string& object = {}, std::uint64_t stamp = 0);

}  // namespace groomlab
