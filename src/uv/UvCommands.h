#pragma once

#include "core/Export.h"
#include "scene/Plug.h"

#include <span>
#include <vector>

namespace lila::scene
{
class Graph;
}

namespace lila::uv
{
/** Registers the uv* commands (uvUnwrap, uvProject, uvAuto, uvPack, ...). */
LILA_SCENE_API void registerCommands();

/** Registers the live solver commands (uvLiveRelax, uvPin, uvCut, uvSew, ...); called by registerCommands. */
LILA_SCENE_API void registerLiveCommands();

/** Registers BFF, optimise, mosaic, symmetric unwrap and edge straighten/align commands; called by registerCommands. */
LILA_SCENE_API void registerOptimizeCommands();

/** Every mesh at or under the roots (each root, then its descendants), each once, in that order. */
LILA_SCENE_API std::vector<scene::NodeId> meshesUnder(const scene::Graph& g, std::span<const scene::NodeId> roots);

/**
 * Meshes the uv editor shows and whole-mesh uv tools work on: those at or under the object selection. With nothing
 * selected, the meshes with components selected in the current component mode.
 */
LILA_SCENE_API std::vector<scene::NodeId> shownMeshes();
} // namespace lila::uv
