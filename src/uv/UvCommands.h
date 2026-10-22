#pragma once

namespace lila::uv
{
/** Registers the uv* commands (uvUnwrap, uvProject, uvAuto, uvPack, ...). */
void registerCommands();

/** Registers the live solver commands (uvLiveRelax, uvPin, uvCut, uvSew, ...); called by registerCommands. */
void registerLiveCommands();

/** Registers BFF, optimise, mosaic, symmetric unwrap and edge straighten/align commands; called by registerCommands. */
void registerOptimizeCommands();
} // namespace lila::uv
