#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace lila::geo
{
/** Points p with dot(normal, p) == offset. */
struct SymmetryPlane
{
    glm::dvec3 normal{1.0, 0.0, 0.0};
    double offset = 0.0;

    double distance(const glm::dvec3& p) const { return glm::dot(normal, p) - offset; }
    glm::dvec3 reflect(const glm::dvec3& p) const { return p - 2.0 * distance(p) * normal; }
    glm::dvec3 project(const glm::dvec3& p) const { return p - distance(p) * normal; }
};

/**
 * Plane normal to axis 0, 1 or 2 at offset along it (an object's pivot). Reflecting across it maps one coordinate v to
 * 2 * offset - v, which is exact at offset 0.
 */
LILA_SCENE_API SymmetryPlane axisPlane(int axis, double offset = 0.0);

/**
 * Point int layer that keeps the symmetry pairs for an axis on the mesh, so they survive compaction, saving, undo and
 * any amount of position drift. Partners hold +k and -k (k > 0, the sign is the side), on-plane points hold
 * kSymmetryOnPlane, 0 is unpaired.
 */
LILA_SCENE_API std::string symmetryLayerName(int axis);
inline constexpr int32_t kSymmetryOnPlane = INT32_MIN;

/** Zeroes every axis's stored pair key from firstPoint on: what an op copied onto the points it created means nothing. */
LILA_SCENE_API void clearSymmetryKeys(Mesh& mesh, Index firstPoint);

/** How SymmetryMap::persistent resolved the points. */
struct SymmetrySync
{
    Index stored = 0;      // paired or on-plane from the stored layer
    Index topological = 0; // had no stored partner, matched through their paired neighbours
    Index byPosition = 0;  // had no stored partner, matched by position within tolerance
    Index unmatched = 0;
};

struct SymmetryCheck
{
    /**
     * Largest distance between a point and the reflection of its partner, of an on-plane point from the plane, or of an
     * unmatched point's reflection from the surface.
     */
    double maxAsymmetry = 0.0;
    Index worstPoint = kInvalid;
    Index pairs = 0;
    Index onPlane = 0;
    Index unmatched = 0;
};

/**
 * Mirror partner of every point across a plane. Points on the plane map to themselves. Ids refer to
 * the mesh the map was built from; rebuild after topology changes.
 */
class LILA_SCENE_API SymmetryMap
{
public:
    /** Matches each point to the nearest point at its reflection, within tolerance. */
    static SymmetryMap build(const Mesh& mesh, const SymmetryPlane& plane = {}, double tolerance = 1e-3);

    /**
     * Matches by topology, walking outward from seedEdge, which must lie on the plane with two faces.
     * Works on meshes whose positions are no longer symmetric. Empty map if the topology is not symmetric.
     */
    static SymmetryMap buildTopological(const Mesh& mesh, Index seedEdge, const SymmetryPlane& plane = {});

    /**
     * Map from the pairs stored on the mesh for an axis plane. Points without a valid stored partner are matched among
     * themselves, first through their paired neighbours, then by position within tolerance, and the new pairs are
     * stored back. A mesh without the layer gets it built here.
     */
    static SymmetryMap persistent(Mesh& mesh, int axis, double tolerance, SymmetrySync* sync = nullptr, double offset = 0.0);

    /** Writes this map into the mesh's layer for axis, replacing what was there. */
    void store(Mesh& mesh, int axis) const;

    /**
     * Sets each given point's follower to the exact reflection of its partner on the driving side (drive > 0: the
     * positive side), and on-plane points exactly onto the plane.
     */
    void enforce(Mesh& mesh, std::span<const Index> points, int drive) const;
    /** enforce() over every point. */
    void symmetrize(Mesh& mesh, int drive) const;
    SymmetryCheck check(const Mesh& mesh) const;

    /** +1 or -1 for the side a paired point belongs to, 0 on the plane or unpaired. */
    int side(Index p) const;
    /** The side most of the points are on, +1 when tied. */
    int majoritySide(std::span<const Index> points) const;

    const SymmetryPlane& plane() const { return mPlane; }
    /** 0, 1 or 2 for a plane normal to an axis, -1 for any other plane. */
    int axis() const { return mAxis; }
    double tolerance() const { return mTolerance; }
    bool empty() const { return mMirror.empty(); }

    Index mirrorPoint(Index p) const { return p < mMirror.size() ? mMirror[p] : kInvalid; }
    bool onPlane(Index p) const { return p < mOnPlane.size() && mOnPlane[p]; }
    /** Points without a partner. */
    Index unmatchedCount() const { return mUnmatched; }

    Index mirrorEdge(const Mesh& mesh, Index e) const;
    Index mirrorFace(const Mesh& mesh, Index f) const;

    /** The selection plus its mirror, sorted and unique. */
    std::vector<Index> expandPoints(std::span<const Index> points) const;
    std::vector<Index> expandEdges(const Mesh& mesh, std::span<const Index> edges) const;
    std::vector<Index> expandFaces(const Mesh& mesh, std::span<const Index> faces) const;

    /** Just the mirror images, in input order; kInvalid where there is none. */
    std::vector<Index> mirrorPoints(std::span<const Index> points) const;
    std::vector<Index> mirrorEdges(const Mesh& mesh, std::span<const Index> edges) const;
    std::vector<Index> mirrorFaces(const Mesh& mesh, std::span<const Index> faces) const;

    /** Snaps the given points (or all points of the mesh from firstPoint on) within tolerance onto the plane. */
    void snapToPlane(Mesh& mesh, std::span<const Index> points) const;
    void snapNewPoints(Mesh& mesh, Index firstPoint) const;

private:
    glm::dvec3 reflected(const glm::dvec3& p) const;

    SymmetryPlane mPlane;
    int mAxis = -1;
    double mTolerance = 1e-3;
    std::vector<Index> mMirror;
    std::vector<uint8_t> mOnPlane;
    std::vector<int8_t> mSide;
    Index mUnmatched = 0;
};

/** Runs after an op: snaps new points and the old on-plane points it moved back onto the plane. */
LILA_SCENE_API void applySymmetrySnap(Mesh& mesh, const SymmetryMap* symmetry, Index firstNewPoint);
} // namespace lila::geo
