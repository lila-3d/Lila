#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <string>
#include <string_view>
#include <vector>

namespace lila::geo
{
struct PlyReadOptions
{
    /** Keep file normals as the corner "normal" attribute (point "normal" on a point cloud). */
    bool keepNormals = true;
};

struct PlyReadResult
{
    /**
     * Polygons with corner normals, uv0 and color sets ("colorSet" from vertex colors, "faceColor" from face colors).
     * Other numeric vertex and face properties become float attributes of the same name. A file without faces is a
     * point cloud: loose points whose normals, colors and uvs are point attributes.
     */
    Mesh mesh;
    bool pointCloud = false;
    /** From "comment TextureFile" lines (MeshLab), as written. */
    std::vector<std::string> textureFiles;
    /** Elements and properties that had no Lila equivalent. */
    std::vector<std::string> skipped;
    std::string error;
    bool ok() const { return error.empty(); }
};

/** ASCII, binary little endian and binary big endian. */
LILA_SCENE_API PlyReadResult readPly(std::string_view data, const PlyReadOptions& opts = {});
LILA_SCENE_API PlyReadResult readPlyFile(const std::string& path, const PlyReadOptions& opts = {});

struct PlyWriteOptions
{
    bool binary = true;
    bool normals = true;
    /** uv0 as per-vertex s, t. */
    bool uvs = true;
    /** The current color set as 8-bit sRGB red, green, blue, alpha. */
    bool colors = true;
    /** ASCII significant digits; 0 writes the shortest text that reads back exactly. */
    int precision = 0;
};

/**
 * Points are split where the written corner attributes differ (UV seams, hard edges when writing normals), since PLY
 * holds them per vertex. Loose points and edges are written too, so point clouds and wires round-trip.
 */
LILA_SCENE_API std::string writePly(const Mesh& mesh, const PlyWriteOptions& opts = {});
} // namespace lila::geo
