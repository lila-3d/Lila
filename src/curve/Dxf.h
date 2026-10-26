#pragma once

#include "core/Export.h"
#include "curve/Spline.h"
#include "geo/Mesh.h"

#include <string>
#include <string_view>
#include <vector>

/**
 * AutoCAD DXF, ASCII and binary: curves (lines, polylines with bulges, arcs, circles, ellipses, splines) in as exact
 * splines, faces (3DFACE, polyface and polygon mesh polylines, MESH) in as meshes, blocks expanded at their inserts.
 * Out as R2000 (SPLINE, LWPOLYLINE, polyface meshes) or R12 (polylines only). Coordinates are the file's, Z up.
 */
namespace lila::curve
{
struct DxfLayer
{
    std::string name;
    Splines curves;
    geo::Mesh mesh;
    /** Off or frozen in the file's layer table. */
    bool hidden = false;
};

struct DxfDocument
{
    std::vector<DxfLayer> layers;
    /** Centimetres per drawing unit from $INSUNITS; 0 when the file doesn't say. */
    double unitScale = 0.0;
    std::vector<std::string> warnings;
    std::string error;
    bool ok() const { return error.empty(); }
};

/** Paper space, invisible entities and anything without geometry Lila holds (text, dimensions, hatches, ACIS solids) are skipped and listed in warnings. */
LILA_SCENE_API DxfDocument readDxf(std::string_view data);

/** Centimetres per unit for "mm", "cm", "m", "in", "ft"; 0 for anything else. */
LILA_SCENE_API double dxfUnitScale(std::string_view unit);

enum class DxfVersion : uint8_t
{
    R2000,
    R12,
};

struct DxfWriteOptions
{
    DxfVersion version = DxfVersion::R2000;
    /** Unit named in $INSUNITS: mm, cm, m, in or ft. Coordinates are written as given. */
    std::string unit = "mm";
    /** R2000: curves other than poly as SPLINE entities. Off, or R12, writes them as polylines within tolerance. */
    bool splines = true;
    /** In file units, for curves written as polylines. */
    double tolerance = 0.1;
};

struct DxfObject
{
    std::string layer;
    Splines curves;
    const geo::Mesh* mesh = nullptr;
};

/** One layer per object. Faces with more than four corners are triangulated; meshes above 32767 points are split. */
LILA_SCENE_API std::string writeDxf(const std::vector<DxfObject>& objects, const DxfWriteOptions& o = {});
} // namespace lila::curve
