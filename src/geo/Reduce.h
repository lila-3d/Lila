#pragma once

#include "geo/Mesh.h"

namespace lila::geo
{
struct ReduceOptions
{
    /** Fraction of triangles to keep, 0 to 1. */
    double ratio = 0.5;
    /** Triangles to keep; positive overrides ratio. */
    Index targetTriangles = 0;
    /** Open borders stay exactly where they are. */
    bool keepBorders = true;
    /**
     * Hard edges stay intact like UV seams, material borders and colour splits always do: points slide along them but
     * never off them, and their corners stay. Off lets hard edges collapse for a lower count.
     */
    bool keepHardEdges = true;
    /** -1 off, 0/1/2 to reduce the +x/+y/+z half and mirror it, so the result stays exactly symmetric. */
    int symmetryAxis = -1;
    /** Points this close to the symmetry plane count as on it. */
    double symmetryTolerance = 1e-4;
    /** Join triangle pairs back into quads where they make good ones. */
    bool quads = false;
};

struct ReduceResult
{
    Mesh mesh;
    /** Shape error reached, as a fraction of the mesh's size. */
    double error = 0.0;
    /** Symmetry was asked for but faces straddle the plane, so the mesh was reduced whole. */
    bool symmetryIgnored = false;
};

/**
 * Quadric edge-collapse simplification. Surviving points stay where they were with all their attributes, and every
 * corner keeps its own UVs, normals and colours, so textures don't swim. No collapse may pinch an edge, turn a
 * triangle over (in space or in UV) or leave a sliver. The result is triangles unless quads is set; a mesh that runs
 * out of legal collapses stops above the target.
 */
ReduceResult reduceMesh(const Mesh& mesh, const ReduceOptions& opts = {});
} // namespace lila::geo
