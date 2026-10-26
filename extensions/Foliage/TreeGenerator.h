#pragma once

#include <lila/Extension.h>
#include <lila/Graph.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

/** Node types, attribute names and the shared entry points of the Tree Generator's scene and UI halves. */
namespace treegen
{
using json = nlohmann::json;

/** A generated tree, canopy group or leaf group: a transform carrying the settings it was built with. */
inline constexpr const char* kTreeType = "treeGenerator";
/** A generated trunk, blob or leaf mesh: a mesh carrying the same attributes. */
inline constexpr const char* kTreeMeshType = "treeMesh";

inline constexpr const char* kAttrSettings = "treeGenerator_settings";
inline constexpr const char* kAttrSources = "treeGenerator_sources";
inline constexpr const char* kAttrPreset = "treeGenerator_preset";
inline constexpr const char* kAttrVersion = "treeGenerator_version";
inline constexpr const char* kAttrPart = "treeGenerator_part";
inline constexpr const char* kAttrParts = "treeGenerator_parts";
inline constexpr const char* kAttrOwner = "treeGenerator_tree";
inline constexpr const char* kAttrLod = "treeGenerator_lod";

/** The preference file the presets, the remembered preset and the export block live in. */
inline constexpr const char* kPrefsFile = "treeGenerator";

inline constexpr const char* kHighSuffix = "_Hi";
inline constexpr const char* kLowSuffix = "_Lo";
inline constexpr const char* kLeafSuffix = "_Leaf";
inline constexpr const char* kSidecarSuffix = "_tree.json";
inline constexpr const char* kPaletteSuffix = "_palette.json";
inline constexpr const char* kOpacitySuffix = "_opacity.png";

bool registerScene(lila::ext::Context& ctx);
void registerUi(lila::ext::Context& ctx);

/** {name: settings} saved in the preference file. */
json userPresets();

/** The tree behind the selection (wherever its settings live), or kNoNode. */
lila::scene::NodeId treeInSelection(lila::scene::Graph& g);
/** {values, sources, preset} stored on a generated node; null when it carries none. */
json storedSettings(lila::scene::Graph& g, lila::scene::NodeId id);

/**
 * What a bake project needs from a tree's export, without running one: the high and low FBX paths the export writes
 * (or wrote), the sidecar and palette json, the opacity map of a leaf export, the material names the baker names its
 * texture sets after, the face counts per part and the measured high to low ray distances per part. values are the
 * full settings (the export block is read for the paths). Throws lila::CommandError.
 */
json bakeHandoff(lila::scene::Graph& g, lila::scene::NodeId tree, const json& values);
} // namespace treegen
