#pragma once

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
    };
    /** Each node (relative to ns) right after loading. */
    std::map<std::string, Baseline> baseline;
    std::set<std::pair<std::string, std::string>> baselineConnections;
    /** Edits held while unloaded, and scene nodes parented under referenced ones as [child, parent]. */
    nlohmann::json edits = nlohmann::json::array();
    nlohmann::json children = nlohmann::json::array();
};

/** Reads any scene file format into the scene document. */
nlohmann::json readSceneFile(const std::filesystem::path& path, std::vector<std::string>* warnings);
/** The document without its large arrays for a binary file (see decodeBinaryHeader); a text file reads whole. */
nlohmann::json readSceneHeader(const std::filesystem::path& path);

/**
 * Creates a document's nodes and internal connections in the current graph, names prefixed with `ns:` and tagged with
 * reference `refId` (0 = owned by the scene). Nested references in the document load under `ns:<theirs>` as part of
 * the same reference. `baseDir` resolves the document's relative reference paths.
 */
std::vector<scene::NodeId> instantiate(const nlohmann::json& doc, const std::string& ns, uint32_t refId,
                                       const std::filesystem::path& baseDir, std::vector<std::string>& warnings, int depth = 0);

namespace refs
{
std::vector<Reference>& all();
Reference* find(const std::string& ns);
void clear();

/** Resolves a stored path: absolute, else next to the scene, else under the workspace. */
std::filesystem::path resolve(const std::string& stored, const std::filesystem::path& sceneDir);

Reference& create(const std::filesystem::path& path, std::string ns, std::vector<std::string>& warnings);
void load(Reference& r, std::vector<std::string>& warnings);
void unload(Reference& r);
void reload(Reference& r, std::vector<std::string>& warnings);
void remove(const std::string& ns);
/** Makes the nodes ordinary scene nodes and forgets the reference. */
void importNodes(const std::string& ns);

nlohmann::json currentEdits(const Reference& r);
/** The same with file paths written relative to sceneDir, where the scene document being made will live. */
nlohmann::json currentEdits(const Reference& r, const std::filesystem::path& sceneDir);
void applyEdits(const Reference& r, const nlohmann::json& edits, std::vector<std::string>& warnings);

/** Records for the scene document, paths relative to `sceneDir` where possible. */
nlohmann::json serialize(const std::filesystem::path& sceneDir);
/** Loads the "references" section of a scene document. span: its share of the caller's progress. */
void deserialize(const nlohmann::json& records, const std::filesystem::path& sceneDir, std::vector<std::string>& warnings,
                 double span = 1.0);
/** Total size of the files the records load, for weighing progress. */
double loadedBytes(const nlohmann::json& records, const std::filesystem::path& sceneDir);

/** Parent edits that named a scene node not loaded yet (references load before the scene's own nodes). */
void resolvePendingParents(std::vector<std::string>& warnings);

/** True for a connection that is part of a reference's own file (so the scene shouldn't save it). */
bool isBaselineConnection(const std::string& src, const std::string& dst);

void registerCommands();
} // namespace refs
} // namespace lila::io
