#pragma once

#include "core/Export.h"

#include "clay/Clay.h"
#include "scene/Node.h"
#include "scene/Plug.h"

namespace lila::clay
{
/** A clay primitive's or group's contribution as it flows along outClay into its parent. */
struct ItemData final : scene::Data
{
    explicit ItemData(Item i) : item(std::move(i)) {}

    Item item;

    const char* kind() const override { return "clayItem"; }
    nlohmann::json toJson() const override { return {{"kind", "clayItem"}}; }
};

/** The claySurface that meshes a clay object, or null. */
LILA_SCENE_API scene::Node* surfaceOf(const scene::Node& object);
/** The clay object a node is part of (itself or its nearest clay ancestor), or null. */
LILA_SCENE_API scene::Node* objectOf(scene::NodeId id);

/** claySurface (meshes the tree), clayPrimitive and clayGroup (transforms that feed it). */
LILA_SCENE_API void registerTypes();
/** createClay, addClayPrimitive, groupClay, refreshClay, convertClayToMesh. */
LILA_SCENE_API void registerCommands();
} // namespace lila::clay
