#pragma once

#include "core/Export.h"
#include "curve/Sweep.h"

#include <string>

/**
 * Pipes and hoses: a round body swept along a path with rounded bends, an optional wall, and parts placed along it
 * (couplings, flanges, clamps, elbow collars, bolt rings, or any mesh), aligned to the rotation-minimising frame.
 */
namespace lila::curve
{
enum class CouplingKind : uint8_t
{
    None,
    Flange,      // a mating pair of flanges with hubs
    Collar,      // a chamfered sleeve
    Clamp,       // a band with two ears
    ElbowCollar, // a sleeve following the bend; a collar on straight runs
    Mesh,        // the given mesh
};
LILA_SCENE_API const char* couplingKindName(CouplingKind k);

enum class FastenerKind : uint8_t
{
    None,
    BoltAndNut,
    Bolt,
    Nut,
    Mesh,
};
LILA_SCENE_API const char* fastenerKindName(FastenerKind k);

/** Face attr "pipePart" values: what each face of a pipe mesh belongs to. */
enum class PipePart : int32_t
{
    Body = 0,
    Coupling = 1,
    Fastener = 2,
};

struct PipeOptions
{
    double radius = 5.0;
    int sides = 12;
    TessOptions tess{.mode = TessMode::Game};
    FrameOptions frames;
    /** Radius multiplier at a fraction (0 to 1) of the length; null is 1. */
    std::function<double(double t)> radiusRamp;
    /** 0 is a solid body; otherwise the body is a tube with an inner surface and rims at its open ends. */
    double wall = 0.0;
    CapMode startCap = CapMode::None;
    CapMode endCap = CapMode::None;
    int roundSegments = 3;
    bool flip = false;
    double vPerCm = 0.0;

    /** Round the path's corners (tangent breaks) with arcs of bendRadius (0: twice the radius). */
    bool autoFillet = true;
    double bendRadius = 0.0;
    /** Bends tighter than this (0: the radius) are reported in warnings. */
    double minBendRadius = 0.0;

    /** Segments: cut every this many cm, at the bends, and at these fractions of the length. */
    double splitEvery = 0.0;
    bool splitAtBends = false;
    std::vector<double> splitAt;
    /** Couplings at the two ends of an open path as well. */
    bool partsAtEnds = false;
    /** Segments alternate between materials 0 and 1, parts take material 2. */
    bool segmentMaterials = false;

    CouplingKind coupling = CouplingKind::None;
    /** Coupling outer radius as a multiple of the pipe radius, and its length along the path (cm). */
    double couplingScale = 1.4;
    double couplingLength = 4.0;
    int couplingSides = 0; // 0: the body's
    /** Parts follow the local radius (tapers, ramps); off sizes them by the base radius. */
    bool scaleToRadius = true;
    /** A user part mesh, its long axis (0 x, 1 y, 2 z) laid along the path, scaled by meshScale (and the radius ratio). */
    const geo::Mesh* couplingMesh = nullptr;
    int meshAxis = 1;
    double meshScale = 1.0;

    FastenerKind fasteners = FastenerKind::None;
    int boltCount = 6;
    /** Bolt circle radius (0: between the hub and the rim) and the first bolt's angle from the frame normal, radians. */
    double boltCircle = 0.0;
    double boltPhase = 0.0;
    /** Bolt shank diameter (0: from the radius). */
    double boltSize = 0.0;
    const geo::Mesh* boltMesh = nullptr;

    /** Hose corrugation: radius dips by depth (a fraction of it) once per period cm, rings per period along it. */
    double corrugationDepth = 0.0;
    double corrugationPeriod = 2.0;
    int corrugationRings = 4;

    /**
     * A loop of points to use as the first ring (a face's border, say). The profile takes its shape, it is used exactly at
     * the start, and the radius becomes its mean distance from its centre.
     */
    std::vector<glm::dvec3> startLoop;
};

/** Where a part sits: the station's frame and the radius it was sized for. */
struct PipePlacement
{
    double s = 0.0;
    glm::dvec3 origin{0.0};
    Frame frame;
    double radius = 0.0;
    PipePart kind = PipePart::Coupling;
    bool bend = false;
};

struct PipeResult
{
    geo::Mesh mesh;
    std::vector<PipePlacement> placements;
    /** Arc lengths where segments start (the first is 0). */
    std::vector<double> segmentStarts;
    /** Body rings along the path. */
    size_t rings = 0;
    double length = 0.0;
    /** Tightest bend radius of the centreline (cm); infinite when straight. */
    double tightestBend = 0.0;
    /** Mesh point of each startLoop point, in its order. */
    std::vector<geo::Index> startRing;
    std::vector<std::string> warnings;
};

/** Builds the pipe along one path spline (world space in, world space out). */
LILA_SCENE_API PipeResult makePipe(const Spline& path, const PipeOptions& o);

/** Radius ramp keys "t:value t:value ..." (t 0 to 1 along the path), smoothly interpolated; empty for none. */
LILA_SCENE_API std::function<double(double)> parseRamp(std::string_view keys);
/** Fractions "0.25 0.5, 0.8" (spaces or commas). */
LILA_SCENE_API std::vector<double> parseNumbers(std::string_view list);

// Built-in parts, local space: the path tangent is +z, the station at z = 0.

/** Solid of revolution about z from (r, z) points (closed: a ring; open: ends with r near 0 become flat caps). Outward faces, uv0. */
LILA_SCENE_API geo::Mesh lathe(const std::vector<glm::dvec2>& rz, bool closed, int sides, double phase = 0.0);
LILA_SCENE_API geo::Mesh flangePart(double pipeRadius, double outerRadius, double length, int sides);
LILA_SCENE_API geo::Mesh collarPart(double pipeRadius, double outerRadius, double length, int sides);
/** A band and two ears on +x; with bolt, a bolt through the ears along y. */
LILA_SCENE_API geo::Mesh clampPart(double pipeRadius, double outerRadius, double length, int sides, bool bolt);
/** Along +z from z = 0: a hex head under z = 0 and a shank of the given length above. size is the shank diameter. */
LILA_SCENE_API geo::Mesh hexBolt(double size, double length);
/** A hex nut centred on z = 0, threaded along z. */
LILA_SCENE_API geo::Mesh hexNut(double size);
} // namespace lila::curve
