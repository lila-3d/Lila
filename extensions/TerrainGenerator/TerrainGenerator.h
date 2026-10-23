#pragma once

#include "TerrainCore.h"

#include <lila/Extension.h>

#include <string>

/** The scene half (node type, commands, export and bakes) and the UI half of the Terrain Generator. */
namespace terrain
{
inline constexpr const char* kGroupType = "terrainGenerator";
inline constexpr const char* kVersion = "1.0.0";
/** The PrefStore file holding user presets, the ID palette, export settings and panel options. */
inline constexpr const char* kPrefsFile = "terrainGenerator";

inline constexpr const char* kAttrSettings = "terrainGenerator_settings";
inline constexpr const char* kAttrSources = "terrainGenerator_sources";
inline constexpr const char* kAttrPreset = "terrainGenerator_preset";
inline constexpr const char* kAttrVersion = "terrainGenerator_version";
inline constexpr const char* kAttrPalette = "terrainGenerator_palette";
inline constexpr const char* kAttrIds = "terrainGenerator_objectIds";
inline constexpr const char* kAttrExport = "terrainGenerator_export";
inline constexpr const char* kAttrMeasured = "terrainGenerator_heightMeasured";

inline constexpr const char* kAttrMinHeight = "terrainGenerator_minHeight";
inline constexpr const char* kAttrMaxHeight = "terrainGenerator_maxHeight";
inline constexpr const char* kAttrHeightRange = "terrainGenerator_heightRange";
inline constexpr const char* kAttrPlaneHeight = "terrainGenerator_bakePlaneHeight";
inline constexpr const char* kAttrAbovePlane = "terrainGenerator_heightAbovePlane";
inline constexpr const char* kAttrHeightP50 = "terrainGenerator_heightP50";
inline constexpr const char* kAttrHeightP99 = "terrainGenerator_heightP99";
inline constexpr const char* kAttrHeightTop = "terrainGenerator_heightOverTile";

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

bool registerScene(lila::ext::Context& ctx);
void registerUi(lila::ext::Context& ctx);
} // namespace terrain
