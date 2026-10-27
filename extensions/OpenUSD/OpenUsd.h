#pragma once

// Shared by the OpenUSD front's files. Nothing here touches Pixar's headers.

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Plug.h>

#include <nlohmann/json.hpp>

namespace lila::usd
{
/** Runs a command in OpenUSDCore, loading it first. Throws CommandError with its message. */
nlohmann::json runBackend(const char* command, const Args& a);
/** True once OpenUSDCore is loaded; checking never loads it. */
bool backendLoaded();
/** An entry point of OpenUSDCore, loading it first; null when it has none. Throws CommandError when it fails to load. */
void* backendSymbol(const char* name);
/** True when node, or one of its ancestors when above, is a usdStage. */
bool isStageNode(scene::NodeId node);
scene::NodeId stageAbove(scene::NodeId node);

/** The Attribute Editor sections of stages and the objects in them. */
void registerUi(ext::Context& ctx);
/** A viewport renderer per Hydra render delegate on the plugin path ("Hydra: Storm", ...). */
void registerHydra(ext::Context& ctx);
} // namespace lila::usd
