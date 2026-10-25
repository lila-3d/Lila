#pragma once

#include "core/Export.h"
#include "curve/Pipe.h"

#include <nlohmann/json.hpp>

namespace lila::scene
{
class Node;
}

/** The "curvePipe" history node (path curves to a pipe mesh with parts) and the pipe commands. */
namespace lila::curve
{
/** The node's settings as makePipe options (mesh inputs point into the node's values). */
LILA_SCENE_API PipeOptions pipeOptionsOf(const scene::Node& n);
/** Attr values a preset sets: "pipe", "hose", "cable" or "conduit". Empty for an unknown name. */
LILA_SCENE_API nlohmann::json pipePreset(std::string_view name);

void registerPipeType();
void registerPipeCommands();
} // namespace lila::curve
