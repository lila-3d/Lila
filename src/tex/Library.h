#pragma once

#include "core/Export.h"
#include "core/Registry.h"

#include "tex/Graph.h"
#include "tex/TextureSet.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lila::ext
{
class Context;
}

/**
 * The material library: `.lilamat` files (GraphFiles.h) with a "meta" block, found in tiers. Later tiers win on the same
 * id: built-in (<resources>/materials), extension packs (<package>/resources/materials of loaded extensions, plus
 * registerExtensionMaterial), the user's (<config>/materials), the workspace's (.lila/materials), and the scene's
 * texture sets (id "scene/<set>").
 *
 * An id is the file's path under its tier folder without the extension, with forward slashes ("Metal/steelBare").
 *
 *   "meta": { "name", "category": "Metal/Steel", "tags": [], "physical": "metal|dielectric|organic|emissive|stylised",
 *             "scaleCm": 100, "tileable": true, "seedParam": "Noise.seed", "variationParams": ["Wear.amount"],
 *             "author", "licence", "version", "description" }
 *
 * seedParam and variationParams name the graph's exposed parameters (its top-level param inputs, by name) or values of
 * top-level nodes as "Node.param" (a parameter or an unlinked input). Presets are sets of such values: a file may carry its
 * own ("presets": {name: {key: value}}) and the user saves more (prefs file "materialLibrary").
 */
namespace lila::tex::library
{
inline constexpr const char* kTiers[] = {"builtin", "extension", "user", "workspace", "scene"};
/** The node applyLibraryMaterial inserts before the Material Output to set the material's real-world tiling. */
inline constexpr const char* kTilingNode = "LibraryTiling";
inline constexpr const char* kTilingType = "materialTiling";

struct LILA_SCENE_API Meta
{
    std::string name;
    std::string category;
    std::vector<std::string> tags;
    std::string physical = "dielectric";
    /** Real-world size of one tile of the texture, in cm. */
    double scaleCm = 100.0;
    bool tileable = true;
    std::string seedParam;
    std::vector<std::string> variationParams;
    std::string author;
    std::string licence;
    std::string version;
    std::string description;

    json toJson() const;
    /** Missing or mistyped keys keep their defaults. */
    static Meta fromJson(const json& j);
};

struct LILA_SCENE_API Entry
{
    std::string id;
    Meta meta;
    std::string tier;
    /** The extension a pack material came from. */
    std::string owner;
    /** Empty for scene sets. */
    std::filesystem::path file;
    /** Extension registrations: the document itself (file may be empty). */
    json document;

    json toJson() const;
};

/** Every visible material (the winner per id), by category then name. Scans the folders on first use. */
LILA_SCENE_API std::vector<Entry> all();
LILA_SCENE_API std::optional<Entry> find(std::string_view id);
/** The whole .lilamat document (format, name, graph, settings, meta). Scene sets are made into one. Throws std::invalid_argument. */
LILA_SCENE_API json document(const Entry& e);
LILA_SCENE_API Graph graphOf(const json& document);
/** The document's settings, or the default ones. */
LILA_SCENE_API SetSettings settingsOf(const json& document);

struct Query
{
    /** Words that must each appear in the name, id, category, tags or description (any case). */
    std::string text;
    /** A category or one of its parents ("Metal" matches "Metal/Steel"). */
    std::string category;
    std::string tag;
    std::string physical;
    std::string tier;
    bool favouritesOnly = false;
};
LILA_SCENE_API std::vector<Entry> search(const Query& q);
LILA_SCENE_API std::vector<std::string> categories();
LILA_SCENE_API std::vector<std::string> tags();

/** Rescans every tier on the next lookup. */
LILA_SCENE_API void reload();
/** The folder of a writable tier ("user" or "workspace"); empty when there is none (no workspace open). */
LILA_SCENE_API std::filesystem::path tierDir(std::string_view tier);
/** Files that failed to load on the last scan, "path: why". */
LILA_SCENE_API std::vector<std::string> errors();

/** Owner-tagged materials extensions registered; each carries its document. */
LILA_SCENE_API Registry<Entry>& extensionMaterials();
LILA_SCENE_API uint64_t registerExtensionMaterial(ext::Context& ctx, Entry e);
/** Registers every .lilamat under dir. Returns the count. */
LILA_SCENE_API size_t registerExtensionDir(ext::Context& ctx, const std::filesystem::path& dir);

LILA_SCENE_API bool isFavourite(std::string_view id);
LILA_SCENE_API void setFavourite(const std::string& id, bool on);
LILA_SCENE_API std::vector<std::string> favourites();

/** The material's presets by name: the file's own, then the user's (which win). */
LILA_SCENE_API json presets(const std::string& id);
LILA_SCENE_API void savePreset(const std::string& id, const std::string& name, const json& values);
LILA_SCENE_API bool deletePreset(const std::string& id, const std::string& name);
/** Sets exposed parameters (by name: their default and their targets) and "Node.param" values. Returns the keys that matched nothing. */
LILA_SCENE_API std::vector<std::string> applyValues(Graph& g, const json& values);
/** The current values of the meta's seed and variation parameters in g, as a preset. */
LILA_SCENE_API json captureValues(const Graph& g, const Meta& m);

/** The tiles per uv unit that put one tile of scaleCm on a surface of density uvPerCm (uv units per cm). 1 when unknown. */
LILA_SCENE_API double tilesFor(double scaleCm, double uvPerCm);
/**
 * Puts a materialTiling node named kTilingNode between the graph's Material Output and what feeds it (moving direct
 * channel links onto a Material node), or updates the one there.
 */
LILA_SCENE_API void setTiling(Graph& g, double tilesX, double tilesY);

/** materialLibrary, saveLibraryMaterial, applyLibraryMaterial, materialFavourite, materialPresets, materialLint. */
LILA_SCENE_API void registerCommands();
} // namespace lila::tex::library
