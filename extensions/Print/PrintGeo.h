#pragma once

#include <lila/Mesh.h>

#include <glm/glm.hpp>

#include <array>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

/**
 * 3D print preparation on plain meshes: printability checks, wall thickness and overhang analysis, orientation,
 * hollowing, drain holes and plane splits with registration pegs. Lengths are scene units (cm); meshes are taken in
 * the space the analysis means (world space for anything that depends on the build direction or scale).
 */
namespace print
{
using lila::geo::Index;
using lila::geo::Mesh;

inline constexpr double kInf = std::numeric_limits<double>::infinity();

// Printers

struct Printer
{
    std::string id;
    std::string label;
    /** Build volume, mm: width, depth, height. */
    glm::dvec3 volume{200.0};
    double nozzle = 0.4; // mm; 0 for resin
    double minWall = 0.8; // mm
    double overhang = 45.0; // degrees from vertical that print without support
};

/** The presets Preferences > Print offers, "custom" last (its values come from the prefs). */
const std::vector<Printer>& printers();
const Printer* findPrinter(std::string_view id);

// Checks

enum class Check : uint8_t
{
    NonManifold,       // edges with three or more faces, points whose faces form several fans
    Holes,             // open border edges
    Flipped,           // faces wound against their shell
    ZeroArea,          // faces with no area
    SelfIntersections, // faces cutting through other faces of the mesh
    Shells,            // more than one separate piece
    ThinWalls,         // points where the wall is thinner than the minimum
    Count
};
inline constexpr size_t kCheckCount = size_t(Check::Count);

/** Lower camel case, as the command flags: "nonManifold", "selfIntersections". */
const char* checkName(Check c);
/** Plural words for messages: "non-manifold edges". */
const char* checkLabel(Check c);

struct CheckOptions
{
    std::array<bool, kCheckCount> checks{true, true, true, true, true, true, true};
    /** Thinnest wall that prints, scene units. */
    double minWall = 0.08;
    /** Coincidence and degeneracy distance; negative scales with the mesh. */
    double tolerance = -1.0;
};

struct CheckResult
{
    std::vector<Index> points, edges, faces;
    /** What the message counts: elements, or pieces for Shells. */
    size_t count = 0;
};

struct CheckReport
{
    std::array<CheckResult, kCheckCount> results;
    const CheckResult& of(Check c) const { return results[size_t(c)]; }
    bool clean() const;
};

CheckReport checkPrintability(const Mesh& mesh, const CheckOptions& opts = {});

/** Faces of each connected piece (pieces join across edges), largest first. */
std::vector<std::vector<Index>> shells(const Mesh& mesh);
/** Faces that cross a face they share no point with. Sorted. */
std::vector<Index> selfIntersectingFaces(const Mesh& mesh);

// Analysis

/**
 * Wall thickness per point (by point capacity): the distance to the surface across the inside, along the point's
 * inward normal, and no more than its faces' own (taken from their centres the same way). kInf where the ray leaves
 * an open mesh.
 */
std::vector<double> wallThickness(const Mesh& mesh);
/**
 * Overhang per face (by face capacity), degrees past vertical a down-facing face leans (0 for up-facing and vertical
 * faces, 90 flat underneath). Faces resting on the bed, the lowest level along up, are 0.
 */
std::vector<double> overhangAngles(const Mesh& mesh, const glm::dvec3& up);
/** Area under faces leaning past threshold degrees, projected onto the bed: what support has to hold. */
double supportArea(const Mesh& mesh, const glm::dvec3& up, double threshold);
/** Enclosed volume (divergence theorem); negative when the mesh is wound inside out. */
double volume(const Mesh& mesh);
double surfaceArea(const Mesh& mesh);

/** A heatmap colour, t 0 (red, bad) through yellow to 1 (green, fine). */
glm::vec4 heatColor(double t);

// Orientation

struct Orientation
{
    /** Turns the mesh so its best side faces down along -up. */
    glm::dmat3 rotation{1.0};
    double supportArea = 0.0;
    /** The support area as the mesh stands, for comparison. */
    double currentArea = 0.0;
};

/** The rotation that needs least support among the axis sides and the largest flat faces, ties to the lowest height. */
Orientation bestOrientation(const Mesh& mesh, const glm::dvec3& up, double threshold);

// Modelling

/** A closed box and a closed cylinder (axis from base along dir), outward wound. */
Mesh box(const glm::dvec3& min, const glm::dvec3& max);
Mesh cylinder(const glm::dvec3& base, const glm::dvec3& dir, double radius, double length, int segments = 24);

/**
 * The solid with a cavity: an inner copy of the surface offset inward by thickness (each point moved so its faces'
 * planes keep that distance), wound inward and added as a second shell.
 */
Mesh hollow(const Mesh& mesh, double thickness);

/** The lowest point along up and the face normal there (outward), for an automatic drain hole. */
struct SurfacePoint
{
    glm::dvec3 position{0.0};
    glm::dvec3 normal{0.0};
};
SurfacePoint lowestPoint(const Mesh& mesh, const glm::dvec3& up);

/** Drills a cylinder of radius through the wall at p, along -normal, depth deep (into the cavity). Empty error on success. */
std::string drainHole(Mesh& mesh, const SurfacePoint& at, double radius, double depth);

struct PegOptions
{
    int count = 1;     // 0 for none
    double radius = 0.25;
    double length = 0.6; // how far the peg stands out of the cut
    double tolerance = 0.02; // clearance added to the hole's radius and depth
};

struct SplitResult
{
    /** Above the plane (along its normal) and below; the pegs stand on below and the holes sink into above. */
    Mesh above, below;
    int pegs = 0;
    std::string error;
    bool ok() const { return error.empty(); }
};

/** Cuts a closed mesh by a plane into two capped, closed parts, with registration pegs across the cut. */
SplitResult splitByPlane(const Mesh& mesh, const glm::dvec3& point, const glm::dvec3& normal, const PegOptions& pegs = {});
} // namespace print
