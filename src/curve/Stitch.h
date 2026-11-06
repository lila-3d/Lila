#pragma once

#include "core/Export.h"
#include "curve/Cable.h"
#include "geo/Mesh.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * Sewn thread as geometry: stitches laid out along a path on a surface, each arching out of one hole and into the next,
 * as one or more twisted plies. The path is abstract (s along it, x across it, both cm) so the same builder serves a curve
 * on a mesh and a seam on a garment's flat pattern.
 */
namespace lila::curve
{
enum class StitchPattern : uint8_t
{
    Running, // dashes along the line (saddle stitch with rotation)
    Zigzag,  // continuous diagonals from side to side
    Cross,   // an X per stitch
    Blanket, // legs across the line joined by a run along it, as round a hem
};

/** Where extra rows go relative to the line. */
enum class StitchAlign : uint8_t
{
    Center,
    Left,  // toward +x
    Right, // toward -x
};

LILA_SCENE_API const char* stitchPatternName(StitchPattern p);

struct StitchOptions
{
    StitchPattern pattern = StitchPattern::Running;
    /** Each stitch's length along the line, cm. */
    double length = 0.4;
    /** Between stitches, cm. */
    double gap = 0.25;
    /** Running: each stitch turned about its middle in the surface, degrees. */
    double rotation = 0.0;
    /** Zigzag, cross and blanket: how far across the line the stitch reaches, cm. */
    double width = 0.4;
    int rows = 1;
    /** Between rows, cm. */
    double rowSpacing = 0.45;
    StitchAlign align = StitchAlign::Center;
    /** The whole layout moved across the line, cm (+x). */
    double offset = 0.0;
    /** Thread thickness, cm. */
    double diameter = 0.08;
    /** 0 round, toward 1 flat ribbon (pressed into the cloth). */
    double flatten = 0.2;
    /** Strands twisted into the thread: 1, 2 or 3. */
    int plies = 1;
    /** Turns of the plies per cm of thread. */
    double twist = 2.0;
    /** How far the thread's underside sits off the surface, cm; negative sinks it in. */
    double restHeight = 0.0;
    /** How deep the thread dives into its holes, cm. */
    double inset = 0.05;
    /** Sides round each ply. */
    int sides = 6;
    /** Rings along each stitch. */
    int segments = 8;
    /** Hand-sewn wobble: 0 is machine-even. */
    double jitter = 0.0;
    uint32_t seed = 1;
    /** v runs on along the path from stitch to stitch; off, each stitch starts at v 0. */
    bool continuousUv = true;
    /** UV repeats per thread circumference along v. */
    double uvScale = 1.0;
    /** Each stitch's v shifted by up to this much at random. */
    double uvRandom = 0.0;
};

/** Where a path coordinate lands: s cm along the path, x cm across it. Nullopt where there is no surface. */
using StitchSurface = std::function<std::optional<SurfacePoint>(double s, double x)>;

struct StitchResult
{
    geo::Mesh mesh;
    int stitches = 0;
};

/** Stitches along a path of the given length; closed paths get a whole number of stitches round. */
LILA_SCENE_API StitchResult buildStitches(double pathLength, bool closed, const StitchSurface& surface, const StitchOptions& o);

/**
 * A path along polyline points with surface normals (one per point): x moves along normal x tangent. snap, when set, puts
 * every point onto a mesh and takes its normal there.
 */
LILA_SCENE_API StitchSurface polylineSurface(std::vector<glm::dvec3> points, std::vector<glm::dvec3> normals, bool closed, ClosestSurface snap = {});

LILA_SCENE_API const std::vector<std::string>& stitchPresetNames();
/** A preset's options, or nullopt for an unknown name. */
LILA_SCENE_API std::optional<StitchOptions> stitchPreset(std::string_view name);
} // namespace lila::curve
