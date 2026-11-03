#pragma once

#include "core/Export.h"
#include "curve/Tessellate.h"
#include "geo/Mesh.h"

#include <array>
#include <functional>
#include <span>

/**
 * Curves to quad meshes: sweep (extrude along a path), revolve, loft and patches. All output quads where the input
 * allows (triangles only at revolve poles), "uv0", "hard" and "seam" edges, and a face "part" id on sweeps.
 */
namespace lila::curve
{
/** A cross-section in the path frame's coordinates: x along the frame normal, y along the binormal, z along the tangent. */
struct LILA_SCENE_API Profile
{
    std::vector<glm::dvec3> points;
    bool closed = true;
    /** Per point (may be empty): a crease runs along the sweep from this point. */
    std::vector<uint8_t> hard;
    /**
     * Where each point sits around the profile, 0 to 1 across the UV strip (may be empty: arc length, normalised). A
     * closed profile has one more, for its first point again at the end of the loop.
     */
    std::vector<double> around;

    size_t size() const { return points.size(); }
    /** Signed area in the xy plane; positive is counter-clockwise seen from +z. */
    double signedArea() const;
};

/** A regular polygon of `sides` points, radius cm, starting on +x. */
LILA_SCENE_API Profile circleProfile(int sides, double radius);
/** A rectangle width x height cm centred on the path, with hard corners. */
LILA_SCENE_API Profile rectProfile(double width, double height);

/**
 * A profile from a tessellated curve. alignToPath: the profile's plane is turned to face along the path's first frame and
 * its centre put on the path start (rotated so its own up stays the frame normal). Otherwise the curve keeps its place
 * relative to the start frame (it is swept from where it lies). Hard points come from the polyline's kinks.
 */
LILA_SCENE_API Profile profileFromCurve(const Polyline& profile, const glm::dvec3& pathStart, const Frame& startFrame, bool alignToPath);

/** A path to sweep along: points with frames, arc length and per-point scale (from the spline's radius). */
struct LILA_SCENE_API Path
{
    std::vector<glm::dvec3> points;
    std::vector<Frame> frames;
    std::vector<double> s;
    std::vector<double> scale;
    std::vector<uint8_t> kink;
    std::vector<glm::dvec3> tangentIn, tangentOut;
    bool closed = false;
    double length = 0.0;

    size_t size() const { return points.size(); }
};

LILA_SCENE_API Path makePath(const Polyline& pl, const std::vector<Frame>& frames, std::span<const double> scale = {});
/** Tessellates the spline, builds its frames (with tilt) and takes its radius values as scale. */
LILA_SCENE_API Path pathFrom(const Spline& s, const TessOptions& tess = {}, const FrameOptions& frames = {});

enum class CapMode : uint8_t
{
    None,
    Ngon,  // one face
    Grid,  // quads (a Coons grid over the end loop; an n-gon when the profile has an odd point count)
    Round, // a dome of rings closed by a grid
};
LILA_SCENE_API const char* capModeName(CapMode m);
LILA_SCENE_API bool parseCapMode(std::string_view name, CapMode* out);

enum class UvLayout : uint8_t
{
    Around, // U around the profile, V along the path
    Along,  // U along the path, V around the profile: the trim strip convention
};
LILA_SCENE_API const char* uvLayoutName(UvLayout l);

/** A rectangle of UV space, (min u, min v) to (max u, max v). */
struct UvRect
{
    glm::dvec2 lo{0.0, 0.0};
    glm::dvec2 hi{1.0, 1.0};

    /** The point at fractions (u, v) across the rectangle. */
    glm::vec2 at(double u, double v) const { return glm::vec2(lo + (hi - lo) * glm::dvec2(u, v)); }
};

/**
 * Where a swept surface's UVs land. The coordinate around the profile fills its side of `rect`; the one along the path
 * fills the other side, or with `perCm` starts at its low edge and runs on at world scale (a trim strip tiling along
 * its length). Caps are mapped flat into `caps`.
 */
struct StripUv
{
    UvLayout layout = UvLayout::Around;
    UvRect rect;
    /** UV units per cm along the path; 0 fits each part to the rect. */
    double perCm = 0.0;
    UvRect caps{{1.1, 0.0}, {2.1, 1.0}};
};

struct SweepOptions
{
    /** Profile scale at arc length s (cm along the path); null is 1. Multiplies the path's own scale. */
    std::function<double(double s)> scale;
    /** Twist in radians at arc length s; null is none. */
    std::function<double(double s)> twist;
    CapMode startCap = CapMode::None;
    CapMode endCap = CapMode::None;
    int roundSegments = 4;
    /** At kinks, set the profile in the bisector plane and stretch it across the bend so walls keep their width. */
    bool miter = true;
    /** Rings at kinks sharper than this (radians) get hard edges. */
    double hardKinkAngle = 0.5236;

    /** Around the profile is its arc length, normalised. */
    StripUv uv;

    /** Arc lengths where the body splits into parts: each starts a new UV island and part id. */
    std::vector<double> splits;
    /** Also split the geometry at those rings (separate shells). */
    bool detachParts = false;
    /** Face "material" per part (index into the mesh's material names); empty leaves materials alone. */
    std::vector<int> partMaterials;
    /** Flip every face. */
    bool flip = false;
};

struct SweepRing
{
    double s = 0.0;
    glm::dvec3 origin{0.0};
    Frame frame;
    double scale = 1.0;
    int part = 0;
    geo::Index firstPoint = 0; // the ring's profile points follow in order
};

struct SweepResult
{
    geo::Mesh mesh;
    std::vector<SweepRing> rings;
    int parts = 1;
};

/** Sweeps the profile along the path. Profile points are swept as given; closed profiles are made counter-clockwise. */
LILA_SCENE_API SweepResult sweep(const Profile& profile, const Path& path, const SweepOptions& o = {});

/**
 * Fills a closed loop of points with quads: a Coons grid (sides a and b with 2(a + b) = loop size) when the loop has an
 * even size, else one n-gon. lift moves interior points (a dome cap). Returns the faces made. flip reverses them.
 */
LILA_SCENE_API std::vector<geo::Index> gridFill(geo::Mesh& m, std::span<const geo::Index> loop, bool flip,
                                                const std::function<glm::dvec3(const glm::dvec3& p, double u, double v)>& lift = {});

struct RevolveOptions
{
    glm::dvec3 origin{0.0};
    glm::dvec3 axis{0.0, 1.0, 0.0};
    double startAngle = 0.0; // radians
    double endAngle = 6.283185307179586;
    int segments = 16;
    /** Partial sweeps of closed profiles: close the two open ends. */
    bool caps = true;
    /** Profile points this close to the axis (cm) merge into one pole. */
    double poleTolerance = 1e-3;
    bool flip = false;
};
/** Revolves a profile around an axis. Points on the axis become poles (triangle fans). */
LILA_SCENE_API geo::Mesh revolve(const Polyline& profile, const RevolveOptions& o = {});

struct LoftOptions
{
    /** Points across each section; 0 matches the sections' own points, with their kinks lined up. */
    int samples = 0;
    /** Segments along between consecutive sections. */
    int spansBetween = 1;
    /** Cubic through the sections (centripetal Catmull-Rom) rather than straight between them. */
    bool smooth = true;
    /** Join the last section back to the first. */
    bool closeLoft = false;
    /** Cap the open ends when the sections are closed. */
    bool caps = false;
    /** Turn and reverse closed sections so each starts nearest the previous one's start and runs the same way. */
    bool autoAlign = true;
    TessOptions tess;
    bool flip = false;
};
/**
 * Lofts quads through the sections. One rail moves each in-between section so its first point follows the rail; two
 * rails also scale and turn it so the point nearest the second rail follows that one.
 */
LILA_SCENE_API geo::Mesh loft(const std::vector<Spline>& sections, const LoftOptions& o = {}, const std::vector<Spline>& rails = {});

/** Coons patch over four boundary curves running around the patch (each ends where the next starts), nu x nv quads. */
LILA_SCENE_API geo::Mesh coonsPatch(const std::array<Spline, 4>& sides, int nu, int nv);
} // namespace lila::curve
