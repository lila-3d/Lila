#pragma once

#include "TileCore.h"

#include <lila/Extension.h>

#include <string>

/** The scene half (node type, commands, export and bakes) and the UI half of the Ground Tiles. */
namespace groundtiles
{
inline constexpr const char* kGroupType = "groundTiles";
inline constexpr const char* kVersion = "1.0.0";
/** The PrefStore file holding user presets, the ID palette, export settings and panel options. */
inline constexpr const char* kPrefsFile = "groundTiles";

inline constexpr const char* kAttrSettings = "groundTiles_settings";
inline constexpr const char* kAttrSources = "groundTiles_sources";
inline constexpr const char* kAttrPreset = "groundTiles_preset";
inline constexpr const char* kAttrVersion = "groundTiles_version";
inline constexpr const char* kAttrPalette = "groundTiles_palette";
inline constexpr const char* kAttrIds = "groundTiles_objectIds";
inline constexpr const char* kAttrExport = "groundTiles_export";
inline constexpr const char* kAttrMeasured = "groundTiles_heightMeasured";

inline constexpr const char* kAttrMinHeight = "groundTiles_minHeight";
inline constexpr const char* kAttrMaxHeight = "groundTiles_maxHeight";
inline constexpr const char* kAttrHeightRange = "groundTiles_heightRange";
inline constexpr const char* kAttrPlaneHeight = "groundTiles_bakePlaneHeight";
inline constexpr const char* kAttrAbovePlane = "groundTiles_heightAbovePlane";
inline constexpr const char* kAttrHeightP50 = "groundTiles_heightP50";
inline constexpr const char* kAttrHeightP99 = "groundTiles_heightP99";
inline constexpr const char* kAttrHeightTop = "groundTiles_heightOverTile";

/** The user's prefs document, filled with the keys every reader expects: Presets, Palette, _default, Export, Window. */
json loadPrefs();
void savePrefs(const json& prefs);

/**
 * What a bake project needs from an exported tile: the high and low FBX paths, the bake plane, the measured height
 * range and the ID palette. Empty paths until the tile has been exported.
 */
struct BakeHandoff
{
    std::string group;
    std::string highPath, lowPath, idsPath, coloursPath, idMapPath;
    std::string bakePlane, bakeMaterial;
    std::string upAxis;
    double tileSize = 0.0;
    double minHeight = 0.0, maxHeight = 0.0, bakePlaneHeight = 0.0, heightAbovePlane = 0.0;
    bool measured = false;
    double heightP50 = 0.0, heightP99 = 0.0, heightOverTile = 0.0;
    /** {"<id>": {"colour": [r, g, b], "sources": [...]}}, the exact values the ID map bakes. */
    json legend = json::object();

    json toJson() const;
};

/** The handoff for a generated group (a name or path). Throws lila::CommandError when it is not one. */
BakeHandoff bakeHandoff(const std::string& group);

/**
 * A native texture set for a generated group (groundTileTextureSet): a bake project from the exported tile when there is one
 * (ID map from the palette, terrainMasks vertex colours, AO, height over the measured range), and a layer stack of a ground
 * fill, the baked height, and per Object ID a folder masked by ID Select holding the colour recipe (base, root, per instance,
 * tip, occlusion) masked by the terrainMasks channels and AO.
 */
json createTextureSet(lila::scene::Graph& g, lila::scene::Node& grp, const lila::Args& a);

bool registerScene(lila::ext::Context& ctx);
void registerUi(lila::ext::Context& ctx);
} // namespace groundtiles
