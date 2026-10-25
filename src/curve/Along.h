#pragma once

#include "core/Export.h"
#include "curve/Tessellate.h"
#include "geo/Mesh.h"

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>

/**
 * Geometry laid along curves and surfaces: a mesh bent along a curve (Maya's Curve Warp, Blender's Curve modifier, Max's
 * Path Deform), copies arrayed along a curve, and geometry flowed from a flat reference onto a surface (Rhino's
 * FlowAlongSrf). Everything works in one space: callers bring the curve and the meshes into it first.
 */
namespace lila::curve
{
/** A mesh axis laid along the curve. */
enum class AlongAxis : uint8_t
{
    PosX,
    NegX,
    PosY,
    NegY,
    PosZ,
    NegZ,
};

/** How the frame turns about the curve. */
enum class UpMode : uint8_t
{
    CurveFrames, // rotation minimising, carrying the curve's tilt; planar curves keep the up vector where it lies across them
    AutoNormals, // the frame normal leans toward the up vector everywhere (tilt still applies)
    AimCurve,    // the frame normal points at the nearest point of a second curve
};

/**
 * Mesh axes for a forward axis: forward, up (laid on the frame normal) and side = forward x up (on the binormal). X and Z
 * run with Y up; Y runs with Z up.
 */
LILA_SCENE_API void alongBasis(AlongAxis forward, glm::dvec3& f, glm::dvec3& up, glm::dvec3& side);

struct RailOptions
{
    UpMode up = UpMode::CurveFrames;
    glm::dvec3 upVector{0.0, 1.0, 0.0};
    /** AimCurve: the curve the normal aims at, same space. */
    std::optional<Spline> aim;
    TessOptions tess{.mode = TessMode::Adaptive, .chordError = 0.002, .maxAngle = 0.035};
};

/** A curve by arc length, with a frame and the spline's radius at every station. */
class LILA_SCENE_API Rail
{
public:
    struct Station
    {
        glm::dvec3 p{0.0};
        Frame f;
        double scale = 1.0;
    };

    Rail() = default;
    explicit Rail(const Spline& s, const RailOptions& o = {});

    bool valid() const { return mLength > 1e-9; }
    double length() const { return mLength; }
    bool closed() const { return mClosed; }
    /**
     * The station at arc length s. Closed curves wrap (unless wrap is off, when they act open at their start); past the
     * ends of an open one the curve carries on straight along the end tangent with the end frame (extrapolate), or stays
     * at the end.
     */
    Station at(double s, bool extrapolate = true, bool wrap = true) const;

private:
    Station inside(double s) const;

    Spline mSpline;
    Polyline mPl;
    std::vector<Frame> mFrames;
    std::vector<double> mScale;
    double mLength = 0.0;
    bool mClosed = false;
};

struct DeformAlongOptions
{
    AlongAxis forward = AlongAxis::PosX;
    /** Turn of the cross-section about the forward axis before it is laid on the frame, radians. */
    double roll = 0.0;
    /** Where the mesh starts along the curve: cm plus a fraction of the curve's length. */
    double offset = 0.0;
    double offsetFraction = 0.0;
    /** Stretch the mesh's forward extent over `range` (default from the offset to the curve's end) instead of keeping its length. */
    bool stretch = false;
    std::optional<std::pair<double, double>> range;
    /** The forward extent that maps onto the curve; unset is the mesh's bounds. */
    std::optional<std::pair<double, double>> extent;
    /** Cross-section scale and twist (radians) at a fraction of the curve's length; null is 1 and none. */
    std::function<double(double t)> scale;
    std::function<double(double t)> twist;
    /** Closed curves: wrap past the seam. Off, the closed curve is treated as open at its start. */
    bool loop = true;
    /** Past the ends of an open curve, carry on straight along the end tangent; off, points pile up at the end. */
    bool extrapolate = true;
    /** 0 leaves the mesh, 1 lays it fully along the curve. */
    double envelope = 1.0;
};

/**
 * Lays the mesh along the rail: its forward axis runs along the curve, the cross-section rides the frame. Stored corner
 * normals and tangents turn with the frame. Throws std::invalid_argument when the rail has no length.
 */
LILA_SCENE_API void deformAlongCurve(geo::Mesh& m, const Rail& rail, const DeformAlongOptions& o);

/** How copies fill the curve. */
enum class ArrayFill : uint8_t
{
    Count,
    Spacing,
};

struct ArrayAlongOptions
{
    ArrayFill fill = ArrayFill::Count;
    /** Copies (Count), start and end pieces included. */
    int count = 5;
    /** Step between copies, cm (Spacing); 0 is the piece's own length, so copies butt together. */
    double spacing = 0.0;
    /** Spacing: round the count and even the step so the copies run exactly from start to end. */
    bool fitToLength = true;
    /** Room left free at each end, cm. */
    double startOffset = 0.0;
    double endOffset = 0.0;
    AlongAxis forward = AlongAxis::PosX;
    double roll = 0.0;
    /** Turn copies to follow the curve; off, they keep the piece's orientation and are only moved. */
    bool align = true;
    /** Bend each copy along its stretch of the curve (trims, rails) instead of placing it rigidly. */
    bool deform = false;
    /** Deform: merge the points where neighbouring copies meet. */
    bool weld = true;
    /** Scale and twist (radians) along the curve as for deformAlongCurve. */
    std::function<double(double t)> scale;
    std::function<double(double t)> twist;
    /** Per-copy random rotation (radians about the piece's axes, plus or minus), uniform scale (a fraction) and offset (cm). */
    glm::dvec3 randomRotate{0.0};
    double randomScale = 0.0;
    glm::dvec3 randomOffset{0.0};
    uint32_t seed = 1;
};

/** Which mesh a copy uses. */
enum class ArrayPiece : uint8_t
{
    Middle,
    Start,
    End,
};

struct ArrayPlacement
{
    /** Piece space to curve space for a rigid copy (the random jitter included). */
    glm::dmat4 transform{1.0};
    /** The random jitter alone, piece space. */
    glm::dmat4 jitter{1.0};
    ArrayPiece piece = ArrayPiece::Middle;
    /** The stretch of curve the copy covers (deform) or its station (rigid: s0 == s1). */
    double s0 = 0.0, s1 = 0.0;
};

/**
 * Where the copies go. Piece lengths are along the forward axis; a negative start or end length means there is no such
 * piece. Throws std::invalid_argument on a rail without length, offsets that leave no room, or more than 100000 copies.
 */
LILA_SCENE_API std::vector<ArrayPlacement> arrayPlacements(const Rail& rail, const ArrayAlongOptions& o, double pieceLength, double startLength = -1.0,
                                                           double endLength = -1.0);

/** The copies merged into one mesh. start and end may be null. placements receives where each copy went. */
LILA_SCENE_API geo::Mesh arrayAlongCurve(const geo::Mesh& piece, const geo::Mesh* start, const geo::Mesh* end, const Rail& rail,
                                         const ArrayAlongOptions& o, std::vector<ArrayPlacement>* placements = nullptr);

/** Extent of a mesh along a forward axis (min, max). */
LILA_SCENE_API std::pair<double, double> forwardExtent(const geo::Mesh& m, AlongAxis forward);

/** How flow finds surface coordinates on the target. */
enum class FlowParam : uint8_t
{
    Uv,   // the target's UVs
    Grid, // the rows and columns of a rectangular quad patch
};

struct FlowOptions
{
    FlowParam param = FlowParam::Uv;
    std::string uvSet = "uv0";
    /** Rectangle of target coordinates (min u, min v, max u, max v) the source footprint maps onto. Grid coordinates run 0 to 1. */
    glm::dvec4 rect{0.0, 0.0, 1.0, 1.0};
    /** Target faces to flow onto; empty is all. Grid needs them to form one rectangular grid of quads. */
    std::vector<geo::Index> faces;
    /** Source footprint in its XZ plane (min x, min z, max x, max z); unset is its bounds. */
    std::optional<glm::dvec4> footprint;
    /** Swap the footprint's x and z before mapping. */
    bool swapUv = false;
    /** Height off the surface per unit of source Y. */
    double heightScale = 1.0;
    /** Measure height from Y = 0 (the construction plane) rather than the source's lowest point. */
    bool baseAtOrigin = true;
    double envelope = 1.0;
};

/**
 * Flows the source onto the target: its XZ footprint maps onto the target's coordinate rectangle and its Y becomes height
 * along the surface normal. Points beyond the target's coordinates extrapolate from the nearest face. Throws
 * std::invalid_argument when the target has no usable faces or coordinates.
 */
LILA_SCENE_API void flowAlongSurface(geo::Mesh& source, const geo::Mesh& target, const FlowOptions& o);

/**
 * Coordinates 0 to 1 per point of a rectangular grid of quads (point capacity; unused points get -1). Throws
 * std::invalid_argument naming the problem when the faces are not quads, not connected, or not a regular grid (a pole
 * inside, or a patch closing on itself).
 */
LILA_SCENE_API std::vector<glm::dvec2> gridParam(const geo::Mesh& m, std::span<const geo::Index> faces);
} // namespace lila::curve
