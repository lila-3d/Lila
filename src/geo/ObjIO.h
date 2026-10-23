#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <string>
#include <string_view>
#include <vector>

namespace lila::geo
{
struct ObjReadOptions
{
    /** One mesh per "o" statement; otherwise everything goes into one mesh. */
    bool splitObjects = true;
    /** Turn smoothing groups into hard edges (when the file has "s" statements). */
    bool smoothingGroupsToHardEdges = true;
    /** Keep file normals as the corner "normal" attribute. */
    bool keepNormals = true;
};

struct ObjObject
{
    std::string name;
    Mesh mesh;
    /** Names for the face "group" int attribute, from "g" statements. */
    std::vector<std::string> groupNames;
};

struct ObjReadResult
{
    std::vector<ObjObject> objects;
    std::vector<std::string> mtllibs;
    std::string error;
    bool ok() const { return error.empty(); }
};

/** Parses in parallel chunks. Faces referencing missing data are skipped. */
LILA_SCENE_API ObjReadResult readObj(std::string_view text, const ObjReadOptions& opts = {});
LILA_SCENE_API ObjReadResult readObjFile(const std::string& path, const ObjReadOptions& opts = {});

struct ObjWriteOptions
{
    bool normals = true;
    bool uvs = true;
    /** Write "s" statements derived from hard edges. */
    bool smoothingGroups = true;
    /** Significant digits; 0 writes the shortest text that reads back exactly. */
    int precision = 0;
};

/** Writes one or more meshes, each as an "o" object, sharing one vertex pool. */
LILA_SCENE_API std::string writeObj(std::span<const ObjObject> objects, const ObjWriteOptions& opts = {});
LILA_SCENE_API std::string writeObj(const Mesh& mesh, const ObjWriteOptions& opts = {});
LILA_SCENE_API bool writeObjFile(const std::string& path, const Mesh& mesh, const ObjWriteOptions& opts = {});

/**
 * Smoothing group per face from hard edges: faces joined by soft edges share a group, faces with
 * only hard edges get 0. Groups are numbered from 1.
 */
LILA_SCENE_API std::vector<int32_t> smoothingGroupsFromHardEdges(const Mesh& mesh);

/** Sets "hard" on edges whose faces have different smoothing groups, or group 0. */
LILA_SCENE_API void hardEdgesFromSmoothingGroups(Mesh& mesh, std::span<const int32_t> groups);
} // namespace lila::geo
