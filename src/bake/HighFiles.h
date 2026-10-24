#pragma once

#include "core/Export.h"

#include "bake/BakeTypes.h"
#include "scene/Node.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

/**
 * High-poly meshes streamed from files (HighEntry::file): FBX, OBJ or one Alembic frame, read only while a bake runs, so
 * a 50-200M triangle high never enters the scene, the undo stack or the saved file. Reads are cached in memory by a hash
 * of the resolved path, its size and time stamp, the object filter and the frame, so rebakes of an unchanged file skip
 * the read.
 */
namespace lila::bake
{
struct FileMesh
{
    /** "<file>:<object>" for reports and object ID maps. */
    std::string name;
    std::shared_ptr<const geo::Mesh> mesh;
    /** Object to world (bake) space. */
    glm::dmat4 world{1.0};
};

/** Relative paths resolve under the workspace root, else the current folder. */
LILA_SCENE_API std::filesystem::path resolveHighFile(const std::string& file);

/** The cache key of an entry: changes whenever the file, its filter or its frame does. Empty when the file is missing. */
LILA_SCENE_API std::string highFileKey(const HighEntry& entry);

/**
 * The meshes of a file high, from the cache when the key matches. Main thread (FBX imports into a private graph).
 * Throws std::runtime_error when the file is missing, unreadable, of an unknown type, or holds no mesh.
 */
LILA_SCENE_API std::vector<FileMesh> loadHighFile(const HighEntry& entry);

/** Reads served from the cache and from disk since the last clear (tests, bake reports). */
struct HighFileStats
{
    size_t hits = 0;
    size_t reads = 0;
    size_t entries = 0;
};
LILA_SCENE_API HighFileStats highFileStats();
LILA_SCENE_API void clearHighFileCache();
} // namespace lila::bake
