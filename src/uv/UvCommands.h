#pragma once

#include "core/Export.h"

namespace lila::uv
{
/** Registers the uv* commands (uvUnwrap, uvProject, uvAuto, uvPack, ...). */
LILA_SCENE_API void registerCommands();

/** Registers the live solver commands (uvLiveRelax, uvPin, uvCut, uvSew, ...); called by registerCommands. */
LILA_SCENE_API void registerLiveCommands();

/** Registers BFF, optimise, mosaic, symmetric unwrap and edge straighten/align commands; called by registerCommands. */
LILA_SCENE_API void registerOptimizeCommands();
} // namespace lila::uv
