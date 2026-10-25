#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lila::geo
{
struct StlReadOptions
{
    /** Merge corners at bit-identical positions into shared points, so the mesh is connected. */
    bool weld = true;
    /** Edges between faces bent more than this many degrees are hard; negative leaves every edge soft. */
    double hardAngle = 30.0;
};

struct StlSolid
{
    std::string name;
    Mesh mesh;
};

struct StlReadResult
{
    /** One per ASCII "solid"; a binary file is always one. */
    std::vector<StlSolid> solids;
    bool binary = false;
    /** Binary files whose triangles carry VisCAM or Materialise colors, read into a color set. */
    bool colors = false;
    std::string error;
    bool ok() const { return error.empty(); }
};

/** Detects binary from the size the triangle count implies, so binary files whose header starts "solid" still read. */
LILA_SCENE_API StlReadResult readStl(std::string_view data, const StlReadOptions& opts = {});
LILA_SCENE_API StlReadResult readStlFile(const std::string& path, const StlReadOptions& opts = {});

struct StlWriteOptions
{
    bool binary = true;
    /** Binary only: the current color set, averaged per triangle, in the VisCAM 15-bit convention. */
    bool colors = false;
    /** ASCII significant digits; 0 writes the shortest text that reads back exactly. */
    int precision = 0;
};

/** Faces are triangulated; each solid becomes one ASCII "solid", and binary files merge them. */
LILA_SCENE_API std::string writeStl(std::span<const StlSolid> solids, const StlWriteOptions& opts = {});
} // namespace lila::geo
