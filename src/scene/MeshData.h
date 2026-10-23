#pragma once

#include "core/Export.h"

#include "scene/Plug.h"

namespace lila::scene
{
/**
 * Mesh <-> scene document. Points, faces (sizes + flat indices), material names, and every attribute layer; corner
 * layers follow face order, edge layers come with their point pairs so they survive edge renumbering.
 */
LILA_SCENE_API json meshToJson(const geo::Mesh& mesh);
/** Same document with its numeric arrays as packed::make arrays, for writing straight into a binary scene file. */
LILA_SCENE_API json meshToJsonPacked(const geo::Mesh& mesh);
LILA_SCENE_API MeshPtr meshFromJson(const json& j);
} // namespace lila::scene
