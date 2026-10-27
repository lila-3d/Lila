#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"
#include "geo/Symmetry.h"

#include <glm/glm.hpp>

#include <optional>
#include <span>
#include <vector>

/**
 * Knife / multi-cut: cuts faces along a path of points on the surface, splitting the edges it crosses. The path between
 * two points follows the surface under the straight screen line joining them (Maya Multi-Cut, Blender Knife), or with
 * `through` cuts every face under that line, back faces included.
 */
namespace lila::geo
{
/** A spot on the surface: a point, a spot along an edge, or a spot inside a face. */
struct KnifePoint
{
    enum class Kind : uint8_t
    {
        Point,
        Edge,
        Face,
    };
    Kind kind = Kind::Face;
    Index id = kInvalid;
    /** Edge: 0 at edgePoint(id, 0), 1 at edgePoint(id, 1). */
    double t = 0.0;
    /** Face: the spot, on the face. Filled in for every kind by knifePlan. */
    glm::dvec3 position{0.0};

    static KnifePoint point(Index p) { return {Kind::Point, p, 0.0, glm::dvec3(0.0)}; }
    static KnifePoint edge(Index e, double t) { return {Kind::Edge, e, t, glm::dvec3(0.0)}; }
    static KnifePoint face(Index f, const glm::dvec3& p) { return {Kind::Face, f, 0.0, p}; }
};

struct KnifeOptions
{
    /** Eye of a perspective view, in mesh space: the cut between two points lies in the plane through it and both. */
    std::optional<glm::dvec3> eye;
    /** View direction of an orthographic view, used without an eye. Neither: the plane holds the points' average normal. */
    glm::dvec3 viewDir{0.0};
    /** Also cut from the last point back to the first. */
    bool closed = false;
    /** Cut every face under each screen line, back faces too, instead of walking the surface between the points. */
    bool through = false;
    /** Also make the mirror image of the cut. */
    const SymmetryMap* symmetry = nullptr;
};

/** One straight piece of the cut across one face of the mesh as it was planned on. */
struct KnifeSegment
{
    Index face = kInvalid;
    KnifePoint a;
    KnifePoint b;
    bool mirrored = false;
};

struct KnifePlan
{
    std::vector<KnifeSegment> segments;
    /** Indices of the point pairs (i, i+1) that no path along the surface joins. */
    std::vector<int> failed;
};

/** Resolves a knife point's position on the mesh. */
LILA_SCENE_API glm::dvec3 knifePosition(const Mesh& mesh, const KnifePoint& p);

/** The pieces a cut through the points would make, without changing the mesh: for previews. */
LILA_SCENE_API KnifePlan knifePlan(const Mesh& mesh, std::span<const KnifePoint> points, const KnifeOptions& opts = {});

/** Plan of an edge loop across the quad ring through edge, at t along it (and its mirror with symmetry). */
LILA_SCENE_API KnifePlan knifeLoopPlan(const Mesh& mesh, Index edge, double t, const SymmetryMap* symmetry = nullptr);

/** Adds each segment's image under every other symmetry element, skipping segments with no image. */
LILA_SCENE_API void addSymmetricImages(const Mesh& mesh, KnifePlan& plan, const SymmetryMap& symmetry);

/**
 * Applies a plan made on this mesh. Pieces joined only through face-inside spots that do not reach the face's border
 * at both ends are dropped, as are pieces along existing edges. Returns the new edges.
 */
LILA_SCENE_API std::vector<Index> knifeApply(Mesh& mesh, const KnifePlan& plan);

/** knifePlan then knifeApply. */
LILA_SCENE_API std::vector<Index> knifeCut(Mesh& mesh, std::span<const KnifePoint> points, const KnifeOptions& opts = {});
} // namespace lila::geo
