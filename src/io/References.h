#pragma once

#include "core/Export.h"

#include "scene/Plug.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace lila::io
{
/**
 * A scene file brought in by reference (Maya style). Its nodes live under "<ns>:" and keep their structure from the
 * file; local changes (attribute values, locks, reparenting, children added under them) are stored as edits, so reloading
 * the file picks up upstream changes and re-applies them.
 */
struct Reference
{
    uint32_t id = 0;
    std::string ns;
    std::filesystem::path path; // absolute
    bool loaded = true;
    struct Baseline
    {
        /** Storable, unconnected input values by attribute index. Meshes are shared, not copied. */
        std::map<int, scene::PlugValue> attrs;
        std::optional<std::string> parent;
        /** Nonzero lock masks by attribute index (see Node::lockMask). */
        std::map<int, uint8_t> locks;
        /** Storable arrays' elements by attribute index. */
        std::map<int, std::vector<std::pair<int, scene::PlugValue>>> elements;
    };
    /** Each node right after loading, by its path through the reference's nodes with ns stripped ("Body|Arm"). */
    std::map<std::string, Baseline> baseline;
    /** The baseline key each loaded node had, so edits keep their key when the node is reparented. */
    std::map<scene::NodeId, std::string> baselineKeys;
    std::set<std::pair<std::string, std::string>> baselineConnections;
    /** Edits held while unloaded, and scene nodes parented under referenced ones as [child, parent] paths. */
    nlohmann::json edits = nlohmann::json::array();
    nlohmann::json children = nlohmann::json::array();
};

/** Reads any scene file format into the scene document. */
LILA_SCENE_API nlohmann::json readSceneFile(const std::filesystem::path& path, std::vector<std::string>* warnings);
/** The document without its large arrays for a binary file (see decodeBinaryHeader); a text file reads whole. */
LILA_SCENE_API nlohmann::json readSceneHeader(const std::filesystem::path& path);

/**
 * Creates a document's nodes and internal connections in the current graph, names prefixed with `ns:` and tagged with
 * reference `refId` (0 = owned by the scene). Nested references in the document load under `ns:<theirs>` as part of
 * the same reference. `baseDir` resolves the document's relative reference paths.
 */
LILA_SCENE_API std::vector<scene::NodeId> instantiate(const nlohmann::json& doc, const std::string& ns, uint32_t refId,
                                       const std::filesystem::path& baseDir, std::vector<std::string>& warnings, int depth = 0);

namespace refs
{
LILA_SCENE_API std::vector<Reference>& all();
LILA_SCENE_API Reference* find(const std::string& ns);
LILA_SCENE_API void clear();

/** Resolves a stored path: absolute, else next to the scene, else under the workspace. */
LILA_SCENE_API std::filesystem::path resolve(const std::string& stored, const std::filesystem::path& sceneDir);

LILA_SCENE_API Reference& create(const std::filesystem::path& path, std::string ns, std::vector<std::string>& warnings);
LILA_SCENE_API void load(Reference& r, std::vector<std::string>& warnings);
LILA_SCENE_API void unload(Reference& r);
LILA_SCENE_API void reload(Reference& r, std::vector<std::string>& warnings);
LILA_SCENE_API void remove(const std::string& ns);
/** Makes the nodes ordinary scene nodes and forgets the reference. */
LILA_SCENE_API void importNodes(const std::string& ns);

LILA_SCENE_API nlohmann::json currentEdits(const Reference& r);
/** The same with file paths written relative to sceneDir, where the scene document being made will live. */
LILA_SCENE_API nlohmann::json currentEdits(const Reference& r, const std::filesystem::path& sceneDir);
LILA_SCENE_API void applyEdits(const Reference& r, const nlohmann::json& edits, std::vector<std::string>& warnings);

/** Records for the scene document, paths relative to `sceneDir` where possible. */
LILA_SCENE_API nlohmann::json serialize(const std::filesystem::path& sceneDir);
/** Loads the "references" section of a scene document. span: its share of the caller's progress. */
LILA_SCENE_API void deserialize(const nlohmann::json& records, const std::filesystem::path& sceneDir, std::vector<std::string>& warnings,
                 double span = 1.0);
/** Total size of the files the records load, for weighing progress. */
LILA_SCENE_API double loadedBytes(const nlohmann::json& records, const std::filesystem::path& sceneDir);

/** Parent edits that named a scene node not loaded yet (references load before the scene's own nodes). */
LILA_SCENE_API void resolvePendingParents(std::vector<std::string>& warnings);

/** True for a connection that is part of a reference's own file (so the scene shouldn't save it). */
LILA_SCENE_API bool isBaselineConnection(const std::string& src, const std::string& dst);

LILA_SCENE_API void registerCommands();
} // namespace refs
} // namespace lila::io
