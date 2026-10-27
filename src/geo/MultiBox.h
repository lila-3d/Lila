#pragma once

#include "core/Export.h"

#include "geo/BoundingShapes.h"
#include "geo/Mesh.h"

#include <string>
#include <vector>

namespace lila::geo
{
struct MultiBoxOptions
{
    int maxBoxes = 8;
    /** Occupied volume over box volume a chunk should reach before it stops being split and may be merged. */
    double minFill = 0.85;
    /** Smallest extent a split may leave, in scene units; 0 means two voxels. */
    double minBoxSize = 0.0;
    /** Voxels along the longest side of the mesh. */
    int resolution = 48;
    /** Chunks get their own minimum-volume frame; off keeps every box on the world axes. */
    bool oriented = true;
    /** Pushed out on every side of each chunk. */
    double padding = 0.0;
    /** box, hull, or auto (a box unless the chunk's convex hull is clearly smaller). */
    std::string shape = "auto";
    int maxHullPoints = 32;
};

struct BoxChunk
{
    OrientedBox box;
    bool hull = false;
    /** The chunk's convex hull in world space, when hull is set. */
    Mesh hullMesh;
    /** Occupied volume over the chunk's volume, 0..1. */
    double fill = 0.0;
    double volume = 0.0;
};

struct MultiBoxResult
{
    std::vector<BoxChunk> chunks;
    double averageFill = 0.0;
    /** Share of the mesh's occupied volume that lies inside a box. */
    double coverage = 0.0;
};

/**
 * Splits a mesh into chunks that largely conform to boxes. The mesh is voxelized (interior filled by a flood
 * fill from outside plus ray parity, so open and overlapping shells work), the voxels are covered greedily by
 * axis-aligned boxes and merged into regions, then each region is refit, split where it is mostly empty and
 * merged with neighbours where the union is still full. Every point of the mesh lies in some chunk. Deterministic.
 */
LILA_SCENE_API MultiBoxResult multiBoxDecompose(const Mesh& mesh, const MultiBoxOptions& opts = {});
} // namespace lila::geo
