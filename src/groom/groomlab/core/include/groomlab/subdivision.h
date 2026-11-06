// SPDX-License-Identifier: Apache-2.0
#pragma once

// Catmull-Clark subdivision of a scalp or collider mesh, so hair grows on (and collides with) the
// surface a renderer draws: Arnold refines a mesh with aiSubdivType catclark by its iterations,
// Maya's smooth mesh preview by its level. Written from the published rules (Catmull and Clark
// 1978; DeRose, Kass and Truong 1998 for creases), to give what OpenSubdiv gives at the same level:
//
// - the host's polygons (ScalpMesh::polygons: quads, n-gons, triangles) are refined, not their
//   triangles; every level makes one quad per face corner;
// - boundaries: OpenSubdiv's "edge and corner" (Arnold's and Maya's default): boundary and
//   non-manifold edges are sharp creases, a boundary vertex with one face is a sharp corner;
// - UVs face-varying, smoothed with their corners pinned (Arnold's default subdiv_uv_smoothing
//   "pin_corners", OpenSubdiv's "linear corners only"): UV seams and borders are creases of the
//   UV layout, a UV with one face keeps its place;
// - color sets (painted vertex maps among them) face-varying and linear: each child corner the
//   bilinear mix of its face's corners, so a map reads on the refined surface as on the cage.
//
// The refined mesh keeps the level's control points (as Arnold and Maya draw them), not the
// limit surface. Its quads are split into two triangles each (a, b, c) and (a, c, d).
//
// The topology work (edges, rules, the faces of every level) is a SubdivisionPlan, built once per
// cage topology, UV layout and level and kept (SubdivisionPlan::of); a deforming cage only runs
// the plan's weights over its new points (refinePoints, subdivideScalp with the last result).
//
// Several scalps on one groom (scalp_pieces.h): a cage holding them refines each scalp at its own
// level (ScalpPiece::subdivision; -1 the level asked for), one after the other: the refined mesh
// holds each scalp's refinement in turn (a scalp at 0 as it is), so a scalp's triangles of it are
// its own refinement's, in its order. Its subdivisionLevel() is the level asked for (the session
// asks for the most of the scalps').

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "groomlab/scalp_mesh.h"

namespace groomlab {

using MeshPtr = std::shared_ptr<const ScalpMesh>;  // as graph.h

constexpr int kMaxSubdivisionLevels = 3;

// The levels a node's `subdivision` param asks for: "off" 0, "1" to "3", "matchRender" (or
// anything else) the mesh's renderSubdivision, clamped to 0..kMaxSubdivisionLevels.
int subdivisionLevels(const std::string& param, int matchRender);

class SubdivisionPlan {
public:
    struct Step;
    ~SubdivisionPlan();

    // The plan for `cage` at `levels` (1 to kMaxSubdivisionLevels), from a small cache keyed by
    // its topology, polygons, UVs, scalps and the level: the same object for every pose of the
    // cage. A cage of several scalps with levels of their own refines each at its own.
    static std::shared_ptr<const SubdivisionPlan> of(const ScalpMesh& cage, int levels);
    // The same, built anew (tests and benchmarks time it).
    static std::shared_ptr<const SubdivisionPlan> build(const ScalpMesh& cage, int levels);

    int levels() const { return levels_; }
    std::size_t cagePointCount() const { return cagePoints_; }
    std::size_t cageFaceCount() const { return cageFaces_; }
    std::size_t pointCount() const { return points_; }
    const std::vector<ScalpMesh::Triangle>& triangles() const { return triangles_; }

    // The refined points from the cage's (the vertex rules); `cagePoints` has cagePointCount().
    std::vector<Vec3f> refinePoints(const std::vector<Vec3f>& cagePoints) const;
    // Values per cage vertex (`channels` each) refined linearly: a vertex keeps its value, an edge
    // point takes its two ends' mean, a face point its corners' (painted vertex maps shown on the
    // refined scalp).
    std::vector<float> refineVertexValues(const std::vector<float>& values, int channels) const;
    // The refined mesh: points, triangles, UVs, color sets and rest pose; subdivisionLevel() set.
    ScalpMesh refine(const ScalpMesh& cage) const;

    // A point of the cage (a cage triangle and barycentrics) on the refined mesh, and back: the
    // same place in the surface's parametrization, whatever the pose. Each cage face is laid out
    // as a regular polygon (its triangles cover it), split into a quad per corner as level 1
    // splits it (corner, next edge's middle, center, previous edge's middle), and each level
    // splits a quad into its four quarters; the refined triangles cover their quads' halves. Both
    // ways are piecewise linear and continuous, and each undoes the other (to rounding): data
    // stored bound to the cage (parts, flow) shows on the refined mesh where it was put, and a
    // point picked on the refined mesh is stored on the cage where it shows. A cage point lies on
    // the refined surface where it refines to, not where it sits in space: Catmull-Clark draws
    // the cage in (by the curvature, a little at each level) and evens out its spacing.
    // Face -1 for a root off the cage (refinedRoot) or off the refined mesh (cageRoot).
    StrandRoot refinedRoot(const StrandRoot& onCage) const;
    StrandRoot cageRoot(const StrandRoot& onRefined) const;

private:
    // One scalp of a cage holding several, refined at its own level (scalp_pieces.h): its triangles
    // and points on the cage and on the refined mesh, and its own plan (null: kept as it is).
    struct Part {
        std::size_t firstTriangle = 0, triangleCount = 0, firstPoint = 0, pointCount = 0;
        std::size_t refinedFirstTriangle = 0, refinedTriangleCount = 0, refinedFirstPoint = 0, refinedPointCount = 0;
        std::shared_ptr<const SubdivisionPlan> plan;
    };

    SubdivisionPlan() = default;
    static std::shared_ptr<const SubdivisionPlan> buildParts(const ScalpMesh& cage, int levels, const std::vector<int>& partLevels);
    const Part* partOfCageTriangle(std::int32_t face) const;
    const Part* partOfRefinedTriangle(std::int32_t face) const;
    std::vector<ScalpMesh::Color> refineCorners(const std::vector<ScalpMesh::Color>& triangleCorners) const;

    int levels_ = 0;
    std::size_t cagePoints_ = 0, cageFaces_ = 0, points_ = 0;
    // Per cage triangle corner: its corner of the cage face (an index into the faces' corners,
    // as cageFaceOffsets_ counts them); per cage face, its triangles (offsets, then the list).
    std::vector<std::int32_t> cageTriangleCorners_;
    std::vector<std::int32_t> faceTriangleOffsets_, faceTriangles_;
    std::vector<std::unique_ptr<Step>> vertexSteps_;  // one per level
    std::vector<std::unique_ptr<Step>> uvSteps_;      // the UV layout's, when the cage has UVs
    std::vector<std::int32_t> cageCornerSource_;      // per cage face corner: its triangle corner
    std::vector<std::int32_t> cageFaceOffsets_;       // the cage's faces (corners per face)
    std::vector<ScalpMesh::Triangle> triangles_;
    std::vector<ScalpMesh::UV> cornerUVs_;  // the refined UVs, per triangle corner (UVs never move)
    std::vector<Part> parts_;               // a cage of scalps at levels of their own; empty: one plan
};

// The levels each scalp of `cage` (ScalpMesh::pieces) is refined by when the whole is refined by
// `levels`: its own (ScalpPiece::subdivision), else `levels`. Empty when the cage holds one scalp
// or every scalp takes `levels` (one plan refines the whole).
std::vector<int> pieceSubdivisionLevels(const ScalpMesh& cage, int levels);

// The cage refined `levels` times (subdivisionLevel() set); the cage itself for 0 or an empty mesh.
MeshPtr subdivideScalp(const MeshPtr& cage, int levels);
// The same, from the last result: when `cage` differs from `previousCage` only by its points
// (ScalpMesh::samePointsLayout), `previous` takes the new refined points (withPoints: the
// closest-point tree refits); otherwise as above.
MeshPtr subdivideScalp(const MeshPtr& cage, int levels, const MeshPtr& previousCage, const MeshPtr& previous);

// The plan `refined` was made from `cage` by (its subdivisionLevel() levels), or null when
// `refined` is not subdivided or not this cage's (its counts differ): what carries data stored on
// the cage (parts, flow) onto the refined scalp and back (refinedRoot, cageRoot).
std::shared_ptr<const SubdivisionPlan> refinementPlan(const ScalpMesh& cage, const ScalpMesh& refined);

// A scalp and the cage it was refined from (ScalpMesh::cage), for what must not change with the
// scalp's subdivision (smooth preview on or off, another level, match render or off): roots are
// placed on the cage and carried onto the scalp (Scatter), maps are read on the cage (ScalarField),
// and choices by position (Clump's centers, which center a strand clumps to) key
// on where a root sits on the cage. Only where the strands sit changes: on the surface drawn.
// For a cage, or a mesh refined from a cage it does not know, the cage is the scalp itself and
// every answer is the one the scalp gives (to the bit).
class CageBinding {
public:
    CageBinding() = default;
    explicit CageBinding(const ScalpMesh* scalp);

    const ScalpMesh* scalp() const { return scalp_; }
    // The cage (the scalp itself when it is not refined from a known cage); null without a scalp.
    const ScalpMesh* cage() const { return cage_ ? cage_.get() : scalp_; }
    bool refined() const { return plan_ != nullptr; }
    // A root of the cage on the scalp, and back (SubdivisionPlan::refinedRoot, cageRoot).
    StrandRoot toScalp(const StrandRoot& onCage) const { return plan_ ? plan_->refinedRoot(onCage) : onCage; }
    StrandRoot toCage(const StrandRoot& onScalp) const { return plan_ ? plan_->cageRoot(onScalp) : onScalp; }
    // Where a root of the scalp sits on the cage's rest pose (its points without one): what a
    // choice that must not change with the subdivision keys on. `fallback` without a scalp or for
    // a root off it. On a cage, restRootOr(scalp, root, fallback).
    Vec3f stablePosition(const StrandRoot& onScalp, const Vec3f& fallback) const;
    // The same for a root of the cage.
    Vec3f stableCagePosition(const StrandRoot& onCage, const Vec3f& fallback) const;

private:
    const ScalpMesh* scalp_ = nullptr;
    std::shared_ptr<const ScalpMesh> cage_;
    std::shared_ptr<const SubdivisionPlan> plan_;
};

}  // namespace groomlab
