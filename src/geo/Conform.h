#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"
#include "geo/Topology.h"

#include <glm/glm.hpp>

#include <optional>
#include <optional>
#include <span>
#include <vector>

/**
 * Shape conform: fits a component selection to a line, circle, rectangle, sphere, cylinder or plane, or to another
 * mesh. Each op only works out where the points go (object space); the caller blends, mirrors and writes them.
 */
namespace lila::geo
{
/** A selection seen as point chains: edge chains, the borders of face regions, or chains through selected points. */
struct ShapeSelection
{
    std::vector<EdgePath> chains;
    /** Inside selected face regions, off their borders: they follow the border's moves. */
    std::vector<Index> interior;
    /** Selected points on no chain and in no region: fitted as a point set. */
    std::vector<Index> loose;
};

LILA_SCENE_API ShapeSelection shapeOfEdges(const Mesh& mesh, std::span<const Index> edges);
/** Each face region's border loops, with its other points as interior. */
LILA_SCENE_API ShapeSelection shapeOfFaces(const Mesh& mesh, std::span<const Index> faces);
/** Points covering whole faces act as those faces; the rest chain along the edges joining selected points. */
LILA_SCENE_API ShapeSelection shapeOfPoints(const Mesh& mesh, std::span<const Index> points);
/** Every point of the selection, once. */
LILA_SCENE_API std::vector<Index> shapePoints(const ShapeSelection& sel);

struct PointMove
{
    Index point = kInvalid;
    glm::dvec3 position{0.0};
};
using PointMoves = std::vector<PointMove>;

enum class Spacing : uint8_t
{
    Keep,    // as far along the shape as the point was along its chain
    Even,    // equal steps
    Project, // the nearest point of the shape
};

struct StraightenOptions
{
    Spacing spacing = Spacing::Keep;
    /** Through all the chain's points rather than between its ends. */
    bool fitLine = false;
};
/** Open chains onto the line between their ends; loose points onto their best-fit line. Closed chains are left. */
LILA_SCENE_API PointMoves straighten(const Mesh& mesh, const ShapeSelection& sel, const StraightenOptions& opts = {});

struct CircleOptions
{
    Spacing spacing = Spacing::Even;
    double radius = 0.0;   // 0: the best fit's
    double rotation = 0.0; // degrees about the circle's normal, past the least-movement alignment
    int sides = 0;         // 3 or more: a regular polygon with its corners on the circle
    bool flatten = true;   // false keeps each point's height off the circle's plane
};
/**
 * Closed chains onto their best-fit circle, aligned to move the points least. Open chains become an arc between their
 * ends; loose points are taken in angle order as one loop. `radii`, when given, receives each circle's radius used.
 */
LILA_SCENE_API PointMoves circularize(const Mesh& mesh, const ShapeSelection& sel, const CircleOptions& opts = {},
                                      std::vector<double>* radii = nullptr);

struct RectangleOptions
{
    Spacing spacing = Spacing::Keep;
    bool square = false;
    double rotation = 0.0; // degrees
    bool flatten = true;
};
/**
 * Closed chains onto their smallest oriented bounding rectangle in their plane. The four points nearest its corners
 * become the corners; the rest spread along the sides between them.
 */
LILA_SCENE_API PointMoves rectangularize(const Mesh& mesh, const ShapeSelection& sel, const RectangleOptions& opts = {});

enum class SphereFit : uint8_t
{
    Best,   // least-squares sphere through the points
    Center, // centred on their centroid
};

enum class SphereRadius : uint8_t
{
    Fit,      // the least-squares sphere's radius (the mean distance when centred on the centroid)
    Average,  // the points' mean distance from the centre
    Min,      // the nearest point's distance
    Max,      // the farthest point's distance
    Explicit, // SpherizeOptions::radius
};

struct SpherizeOptions
{
    SphereFit fit = SphereFit::Best;
    SphereRadius radiusMode = SphereRadius::Average;
    /** How far each point goes toward the sphere, 0 to 1. */
    double amount = 1.0;
    /** Above 0 this radius is used whatever radiusMode says. */
    double radius = 0.0;
    /** Object space; unset takes the fitted centre. */
    std::optional<glm::dvec3> center;
    /** Ellipsoid semi-axes relative to the radius, along the columns of frame. */
    glm::dvec3 scale{1.0};
    /** Orthonormal ellipsoid axes in object space, one per column. */
    glm::dmat3 frame{1.0};
    /** Each point slides along its own normal to the sphere instead of straight toward the centre. */
    bool alongNormals = false;
    /** Per-point normals (object space, indexed by point) for alongNormals; unset takes the area-weighted ones. */
    const std::vector<glm::dvec3>* normals = nullptr;
};

/** The sphere (or ellipsoid, with SpherizeOptions::scale) the points were fitted to. */
struct SpherizeResult
{
    glm::dvec3 center{0.0};
    /** In the ellipsoid's unit space; the semi-axes are radius * scale. */
    double radius = 0.0;
};
/**
 * Points moved onto a sphere or ellipsoid. A point at the centre stays put. With alongNormals a point whose normal line
 * misses the sphere is pushed radially instead. `fitted`, when given, receives the sphere used.
 */
LILA_SCENE_API PointMoves spherize(const Mesh& mesh, std::span<const Index> points, const SpherizeOptions& opts = {}, SpherizeResult* fitted = nullptr);

/** Points pushed radially onto a cylinder. axis -1 fits the axis; 0..2 runs it along object X, Y or Z. */
LILA_SCENE_API PointMoves cylinderize(const Mesh& mesh, std::span<const Index> points, int axis = -1, double radius = 0.0);

/** Points onto their best-fit plane. */
LILA_SCENE_API PointMoves flattenToPlane(const Mesh& mesh, std::span<const Index> points);
/** Points onto their best-fit line. */
LILA_SCENE_API PointMoves alignToLine(const Mesh& mesh, std::span<const Index> points);

enum class ConformMode : uint8_t
{
    Loop,    // closed chains one to one onto the target's nearest closed loop
    Surface, // every point to the nearest point of the target
    Vertex,  // every point to the nearest target vertex
};

struct ConformOptions
{
    ConformMode mode = ConformMode::Loop;
    /** Loop mode, when the counts differ: how the points spread along the target loop. */
    Spacing spacing = Spacing::Even;
};
/**
 * Selection onto `target`, given in the selection's object space. A target without faces is used through its edges.
 * In loop mode equal counts land one to one on the loop's vertices, in order, at the rotation that moves them least.
 */
LILA_SCENE_API PointMoves conformToMesh(const Mesh& mesh, const ShapeSelection& sel, const Mesh& target, const ConformOptions& opts = {});

/** Closed loops a ring could land on: borders, closed edge loops, outlines of faces with five or more sides, and closed chains of loose edges. */
LILA_SCENE_API std::vector<EdgePath> candidateLoops(const Mesh& target);

/** Adds moves for the interior points: the moved points' displacement spread smoothly (harmonic) through them. */
LILA_SCENE_API void followInterior(const Mesh& mesh, std::span<const Index> interior, PointMoves& moves);
} // namespace lila::geo
