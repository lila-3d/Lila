#pragma once

#include "core/Export.h"
#include "curve/Strap.h"
#include "scene/Components.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string_view>
#include <vector>

/** The "curveStrapPath" and "curveStrap" history nodes and the strap commands. */
namespace lila::curve
{
/** Attr values a preset sets: "strap", "rope", "tape", "metalBand" or "zipTie". Null for an unknown name. */
LILA_SCENE_API nlohmann::json strapPreset(std::string_view name);
/** Strap options from attr values by name (missing ones at the node's defaults). */
LILA_SCENE_API StrapOptions strapOptionsFrom(const nlohmann::json& values);

/** A mesh to strap and the faces of it that count; none means every face. */
struct StrapTarget
{
    scene::NodeId node = scene::kNoNode;
    std::optional<scene::components::Ids> faces;
};

/**
 * What a strap goes round: meshes with components selected (just the faces those touch), selected meshes (whole), or with
 * nothing selected every visible mesh. Hidden meshes, and faces the active view's isolation hides, never count.
 */
LILA_SCENE_API std::vector<StrapTarget> strapTargets(scene::Graph& g);
/** The mesh with only the given faces, or the mesh itself when every face counts. */
LILA_SCENE_API scene::MeshPtr strapFaces(const scene::MeshPtr& mesh, const std::optional<scene::components::Ids>& faces);

void registerStrapTypes();
void registerStrapCommands();
} // namespace lila::curve
